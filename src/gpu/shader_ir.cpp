#include "scarlettx/gpu/shader_ir.h"
#include "scarlettx/gpu/rdna2_lifter.h"
#include "scarlettx/gpu/dxil_backend.h"
#include "core/logger.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <sstream>
#include <iomanip>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <d3dcompiler.h>
#endif

#if SCARLETTX_HAS_ZLIB
#include <zlib.h>
#endif

namespace scarlettx {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint32_t kXdatShaderTag = 0x0Du;
constexpr uint32_t kXdatProgramHeaderTag = 0x07u;
constexpr uint32_t kMaxXdatEntries = 65536;
constexpr size_t kMaxFssmSize = 64u * 1024u * 1024u;
// Bumped to 2 when the RDNA2 cache key gained the DXC toolchain identity
// (compiler version + flag set): every entry written by an older build is
// stale by construction and is evicted on first read.
constexpr uint32_t kShaderCacheVersion = 2;
constexpr uint32_t kRdna2LifterVersion = 7;
constexpr size_t kShaderCacheHeaderSize = 32;
constexpr std::array<uint8_t, 4> kShaderCacheMagic = {'S', 'X', 'S', 'C'};

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t ReadU64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void WriteU32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

void WriteU64(uint8_t* p, uint64_t v) {
    std::memcpy(p, &v, sizeof(v));
}

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

void BuildShaderCacheHeader(
    uint8_t* header, uint64_t key, uint64_t payload_size, uint64_t payload_hash) {
    std::memcpy(header, kShaderCacheMagic.data(), kShaderCacheMagic.size());
    WriteU32(header + 4, kShaderCacheVersion);
    WriteU64(header + 8, key);
    WriteU64(header + 16, payload_size);
    WriteU64(header + 24, payload_hash);
}

bool IsXboxPrivateFourCC(const std::string& fourcc) {
    return fourcc == "XBOX" || fourcc == "XS2M" || fourcc == "XDBG" || fourcc == "XBIN";
}

ShaderStage DecodeDxbcProgramType(uint16_t program_type) {
    switch (program_type) {
        case 0: return ShaderStage::Pixel;
        case 1: return ShaderStage::Vertex;
        case 2: return ShaderStage::Geometry;
        case 3: return ShaderStage::Hull;
        case 4: return ShaderStage::Domain;
        case 5: return ShaderStage::Compute;
        case 6: return ShaderStage::Library;
        case 13: return ShaderStage::Mesh;
        case 14: return ShaderStage::Amplification;
        default: return ShaderStage::Unknown;
    }
}

struct XdatInfo {
    bool has_native_shader = false;
    bool has_no_bitcode_marker = false;
    size_t xbch_offset = 0;
    size_t xbch_size = 0;
};

XdatInfo InspectXdat(const uint8_t* data, size_t size) {
    XdatInfo result{};
    if (!data || size < 12) {
        return result;
    }

    for (size_t marker = 0; marker <= size - 12; ++marker) {
        if (std::memcmp(data + marker, "XDAT", 4) != 0) {
            continue;
        }

        const uint32_t count = ReadU32(data + marker + 4);
        const uint64_t table_end =
            static_cast<uint64_t>(marker) + 12ull + static_cast<uint64_t>(count) * 12ull;
        if (count > kMaxXdatEntries || table_end > size) {
            continue;
        }

        XdatInfo candidate{};
        bool entries_valid = true;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t entry = marker + 12 + static_cast<size_t>(i) * 12;
            const uint32_t tag = ReadU32(data + entry);
            const uint32_t offset = ReadU32(data + entry + 4);
            const uint32_t entry_size = ReadU32(data + entry + 8);
            const uint64_t entry_end = static_cast<uint64_t>(offset) + entry_size;
            if (offset < table_end || entry_end > size) {
                entries_valid = false;
                break;
            }

            if (tag == kXdatShaderTag && entry_size >= 4 &&
                std::memcmp(data + offset, "XBCH", 4) == 0) {
                candidate.has_native_shader = true;
                candidate.xbch_offset = offset;
                candidate.xbch_size = entry_size;
            } else if (tag == kXdatProgramHeaderTag && entry_size >= 12 &&
                       ReadU32(data + offset + 8) == FourCC('N', 'O', 'B', 'C')) {
                candidate.has_no_bitcode_marker = true;
            }
        }

        if (entries_valid) {
            return candidate;
        }
    }
    return result;
}

bool FindXboxXbchPayload(
    const std::vector<uint8_t>& bytecode, size_t& out_offset, size_t& out_size) {
    const uint32_t chunk_count = ReadU32(bytecode.data() + 28);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        const uint32_t chunk_offset = ReadU32(bytecode.data() + 32 + i * 4);
        const uint32_t chunk_size = ReadU32(bytecode.data() + chunk_offset + 4);
        if (std::memcmp(bytecode.data() + chunk_offset, "DXIL", 4) != 0) {
            continue;
        }
        const size_t data_offset = static_cast<size_t>(chunk_offset) + 8;
        const XdatInfo xdat = InspectXdat(bytecode.data() + data_offset, chunk_size);
        if (xdat.has_native_shader) {
            out_offset = data_offset + xdat.xbch_offset;
            out_size = xdat.xbch_size;
            return true;
        }
    }
    return false;
}

}  // namespace

uint64_t HashShaderBytes(const uint8_t* data, size_t size) {
    uint64_t h = kFnvOffset;
    if (!data) {
        return h;
    }
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnvPrime;
    }
    return h;
}

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info) {
    out_info = ShaderModuleInfo{};
    if (bytecode.size() > std::numeric_limits<uint32_t>::max()) {
        return Status::Error(StatusCode::ShaderError, "Shader container exceeds the DXBC size limit");
    }
    out_info.byte_size = static_cast<uint32_t>(bytecode.size());
    out_info.content_hash = HashShaderBytes(bytecode.data(), bytecode.size());

    if (bytecode.size() >= 4 && std::memcmp(bytecode.data(), "XS2\0", 4) == 0) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
        return Status::Ok();
    }

    if (bytecode.size() < 32) {
        return Status::Error(StatusCode::ShaderError, "Shader container too small (< 32 bytes)");
    }

    if (std::memcmp(bytecode.data(), "DXBC", 4) == 0) {
        const uint32_t declared_size = ReadU32(bytecode.data() + 24);
        const uint32_t chunk_count = ReadU32(bytecode.data() + 28);
        if (declared_size != bytecode.size()) {
            return Status::Error(StatusCode::ShaderError, "DXBC declared size does not match buffer size");
        }

        out_info.kind = ShaderContainerKind::Dxbc;

        const size_t offsets_pos = 32;
        const uint64_t table_end =
            static_cast<uint64_t>(offsets_pos) + static_cast<uint64_t>(chunk_count) * 4;
        if (table_end > bytecode.size()) {
            return Status::Error(StatusCode::ShaderError, "DXBC chunk offset table truncated");
        }
        out_info.chunk_count = chunk_count;
        std::vector<std::pair<size_t, size_t>> chunk_ranges;
        chunk_ranges.reserve(chunk_count);

        for (uint32_t i = 0; i < chunk_count; ++i) {
            const uint32_t chunk_off = ReadU32(bytecode.data() + offsets_pos + i * 4);
            if (chunk_off < table_end || static_cast<uint64_t>(chunk_off) + 8 > bytecode.size()) {
                return Status::Error(StatusCode::ShaderError, "DXBC chunk offset is outside the data region");
            }
            std::string fourcc(reinterpret_cast<const char*>(bytecode.data() + chunk_off), 4);
            const uint32_t chunk_size = ReadU32(bytecode.data() + chunk_off + 4);
            const size_t data_off = static_cast<size_t>(chunk_off) + 8;
            const uint64_t chunk_end = static_cast<uint64_t>(data_off) + chunk_size;
            if (chunk_end > bytecode.size()) {
                return Status::Error(StatusCode::ShaderError, "DXBC chunk extends past end of buffer");
            }
            chunk_ranges.emplace_back(chunk_off, static_cast<size_t>(chunk_end));

            if (fourcc == "DXIL" || fourcc == "ILDB") {
                out_info.kind = ShaderContainerKind::Dxil;
                if (data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                    const uint32_t ver_token = ReadU32(bytecode.data() + data_off);
                    const uint16_t prog_type = static_cast<uint16_t>((ver_token >> 16) & 0xFFFFu);
                    out_info.stage = DecodeDxbcProgramType(prog_type);
                    out_info.shader_model_major = (ver_token >> 4) & 0xFu;
                    out_info.shader_model_minor = ver_token & 0xFu;
                }
                if (fourcc == "DXIL") {
                    const XdatInfo xdat = InspectXdat(bytecode.data() + data_off, chunk_size);
                    out_info.has_xbox_native_shader |= xdat.has_native_shader;
                    out_info.has_missing_dxil_bitcode |= xdat.has_no_bitcode_marker;
                }
            } else if ((fourcc == "SHEX" || fourcc == "SHDR") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                const uint32_t ver_token = ReadU32(bytecode.data() + data_off);
                const uint16_t prog_type = static_cast<uint16_t>((ver_token >> 16) & 0xFFFFu);
                out_info.stage = DecodeDxbcProgramType(prog_type);
                out_info.shader_model_major = (ver_token >> 4) & 0xFu;
                out_info.shader_model_minor = ver_token & 0xFu;
            } else if ((fourcc == "ISGN" || fourcc == "ISG1") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                out_info.input_param_count = ReadU32(bytecode.data() + data_off);
            } else if ((fourcc == "OSGN" || fourcc == "OSG1") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                out_info.output_param_count = ReadU32(bytecode.data() + data_off);
            } else if (fourcc == "RDAT") {
                out_info.has_raytracing_exports = true;
                if (out_info.stage == ShaderStage::Unknown) {
                    out_info.stage = ShaderStage::Library;
                }
            } else if (fourcc == "RTS0") {
                out_info.has_embedded_root_signature = true;
            }

            if (IsXboxPrivateFourCC(fourcc)) {
                out_info.has_xbox_private_chunks = true;
            }

            out_info.chunk_fourccs.push_back(std::move(fourcc));
        }
        std::sort(chunk_ranges.begin(), chunk_ranges.end());
        for (size_t i = 1; i < chunk_ranges.size(); ++i) {
            if (chunk_ranges[i].first < chunk_ranges[i - 1].second) {
                return Status::Error(StatusCode::ShaderError, "DXBC chunks overlap");
            }
        }
        return Status::Ok();
    }

    if (std::memcmp(bytecode.data(), "XS2\0", 4) == 0) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
        return Status::Ok();
    }

    return Status::Error(StatusCode::UnsupportedFormat, "Unrecognized shader container magic");
}

Status TranslateXboxShaderToPc(
    const std::vector<uint8_t>& xbox_bytecode,
    ShaderTranslationResult& out_result) {
    return TranslateXboxShaderToPc(
        xbox_bytecode, ShaderTranslationOptions{}, out_result);
}

Status TranslateXboxShaderToPc(
    const std::vector<uint8_t>& xbox_bytecode,
    const ShaderTranslationOptions& options,
    ShaderTranslationResult& out_result) {
    out_result = ShaderTranslationResult{};
    Status st = InspectShaderContainer(xbox_bytecode, out_result.source_info);
    if (!st.ok()) {
        return st;
    }

    if (out_result.source_info.kind == ShaderContainerKind::ScarlettRdna2) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "XS2 raw shader payload has no FSSM metadata; provide an Xbox DXIL/XDAT container");
    }

    static ShaderCache disk_cache;
    if (out_result.source_info.has_missing_dxil_bitcode ||
        out_result.source_info.has_xbox_native_shader) {
        const ShaderStage stage = options.stage_override != ShaderStage::Unknown
                                      ? options.stage_override
                                      : out_result.source_info.stage;
        if (stage == ShaderStage::Unknown) {
            return Status::Error(StatusCode::UnsupportedFormat, "NOBC shader stage is unknown");
        }

        XboxRdna2Program program{};
        st = ExtractXboxRdna2Program(xbox_bytecode, program);
        if (!st.ok()) return st;
        const bool requires_group_size =
            stage == ShaderStage::Compute || stage == ShaderStage::Mesh ||
            stage == ShaderStage::Amplification;
        if (requires_group_size && !program.has_thread_group_size) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "FSSM preamble does not contain a valid thread-group size");
        }

        const uint32_t group_x =
            requires_group_size && program.has_thread_group_size
                ? program.thread_group_size_x
                : 1u;
        const uint32_t group_y =
            requires_group_size && program.has_thread_group_size
                ? program.thread_group_size_y
                : 1u;
        const uint32_t group_z =
            requires_group_size && program.has_thread_group_size
                ? program.thread_group_size_z
                : 1u;
        // 56 bytes of translation inputs, plus 8 bytes of DXC toolchain
        // identity (compiler version + fixed flag set) so a different
        // dxcompiler.dll or a changed flag vector can never reuse stale DXIL.
        std::array<uint8_t, 64> key_data{};
        WriteU64(
            key_data.data(),
            HashShaderBytes(program.instruction_bytes.data(),
                            program.instruction_bytes.size()));
        WriteU32(key_data.data() + 8, static_cast<uint32_t>(stage));
        WriteU32(key_data.data() + 12, kRdna2LifterVersion);
        WriteU32(key_data.data() + 16, options.root_cbv_count);
        WriteU32(key_data.data() + 20, options.root_srv_count);
        WriteU32(key_data.data() + 24, options.root_sampler_count);
        WriteU32(key_data.data() + 28, options.root_uav_count);
        WriteU32(key_data.data() + 32, options.first_cbv_register);
        WriteU32(key_data.data() + 36, group_x);
        WriteU32(key_data.data() + 40, group_y);
        WriteU32(key_data.data() + 44, group_z);
        WriteU32(key_data.data() + 48, options.use_bindless_resources ? 1u : 0u);
        WriteU32(key_data.data() + 52, options.use_bindless_samplers ? 1u : 0u);
        WriteU64(key_data.data() + 56, DxcCompileIdentity());
        const uint64_t cache_key = HashShaderBytes(key_data.data(), key_data.size());
        std::vector<uint8_t> cached_dxil;
        if (!options.bypass_shader_cache &&
            disk_cache.Lookup(cache_key, cached_dxil)) {
            ShaderModuleInfo cached_info{};
            if (InspectShaderContainer(cached_dxil, cached_info).ok() &&
                cached_info.kind == ShaderContainerKind::Dxil &&
                !cached_info.has_xbox_private_chunks &&
                !cached_info.has_missing_dxil_bitcode) {
                const auto accept_cached = [&] {
                    out_result.pc_bytecode = std::move(cached_dxil);
                    out_result.translated_hash =
                        HashShaderBytes(out_result.pc_bytecode.data(),
                                        out_result.pc_bytecode.size());
                    out_result.stripped_xbox_chunks =
                        out_result.source_info.chunk_count > cached_info.chunk_count
                            ? out_result.source_info.chunk_count - cached_info.chunk_count
                            : 0;
                    std::ostringstream cache_message;
                    cache_message << "RDNA2 DXIL cache hit, key=0x" << std::hex
                                  << cache_key;
                    LogMessage(LogLevel::Info, "shader", cache_message.str());
                    return Status::Ok();
                };
                // A cached blob was produced by a possibly different DXC
                // build; re-validate through the live validator before it
                // reaches the driver. Validation is far cheaper than
                // recompiling, so it runs on every hit.
                Status revalidation = ValidateDxilContainer(cached_dxil);
                if (revalidation.ok() ||
                    revalidation.code == StatusCode::NotFound) {
                    if (!revalidation.ok()) {
                        // No DXC runtime at all: the compile path would fail
                        // the same way, so serve the cached blob rather than
                        // nothing.
                        LogMessage(
                            LogLevel::Warning, "shader",
                            "RDNA2 DXIL cache hit re-validation skipped: "
                            "DXC runtime not available");
                    }
                    return accept_cached();
                }
                LogMessage(
                    LogLevel::Warning, "shader",
                    "RDNA2 DXIL cache entry failed re-validation; evicting and "
                    "recompiling: " +
                        revalidation.message);
                disk_cache.Evict(cache_key);
                cached_dxil.clear();
            }
        }

        Rdna2TranslationOptions lifter_options{};
        lifter_options.stage = stage;
        lifter_options.root_cbv_count = options.root_cbv_count;
        lifter_options.root_srv_count = options.root_srv_count;
        lifter_options.root_sampler_count = options.root_sampler_count;
        lifter_options.root_uav_count = options.root_uav_count;
        lifter_options.first_cbv_register = options.first_cbv_register;
        lifter_options.use_bindless_resources = options.use_bindless_resources;
        lifter_options.use_bindless_samplers = options.use_bindless_samplers;
        if (program.has_thread_group_size && requires_group_size) {
            lifter_options.thread_group_size_x = group_x;
            lifter_options.thread_group_size_y = group_y;
            lifter_options.thread_group_size_z = group_z;
        }

        std::string hlsl_source;
        st = TranslateRdna2ToHlsl(
            program.instruction_bytes, lifter_options, hlsl_source);
        if (!st.ok()) {
            LogMessage(
                LogLevel::Error, "shader",
                "RDNA2-to-HLSL lifting failed: " + st.message);
            return st;
        }

        const char* target_profile = nullptr;
        switch (stage) {
            case ShaderStage::Vertex: target_profile = "vs_6_6"; break;
            case ShaderStage::Pixel: target_profile = "ps_6_6"; break;
            case ShaderStage::Compute: target_profile = "cs_6_6"; break;
            case ShaderStage::Mesh: target_profile = "ms_6_6"; break;
            case ShaderStage::Amplification: target_profile = "as_6_6"; break;
            default:
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "RDNA2 shader stage has no DXIL target profile");
        }
        st = CompileHlslToDxil(
            hlsl_source, "main", target_profile, out_result.pc_bytecode, true);
        if (!st.ok()) return st;

        ShaderModuleInfo translated_info{};
        st = InspectShaderContainer(out_result.pc_bytecode, translated_info);
        if (!st.ok() || translated_info.kind != ShaderContainerKind::Dxil) {
            out_result.pc_bytecode.clear();
            LogMessage(
                LogLevel::Error, "shader",
                "RDNA2 shader output failed DXIL container validation.");
            return Status::Error(StatusCode::ShaderError, "RDNA2 lifter did not produce a valid DXIL container");
        }
        out_result.translated_hash =
            HashShaderBytes(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
        out_result.stripped_xbox_chunks =
            out_result.source_info.chunk_count > translated_info.chunk_count
                ? out_result.source_info.chunk_count - translated_info.chunk_count
                : 0;
        if (stage == ShaderStage::Mesh) {
            LogMessage(
                LogLevel::Info, "shader",
                "Compiled RDNA2 mesh shader to DXIL (ms_6_6).");
        } else if (stage == ShaderStage::Amplification) {
            LogMessage(
                LogLevel::Info, "shader",
                "Compiled amplification shader to DXIL (as_6_6).");
        }
        if (!options.bypass_shader_cache) {
            disk_cache.Store(cache_key, out_result.pc_bytecode);
            std::ostringstream cache_message;
            cache_message << "Stored RDNA2 DXIL cache entry, key=0x"
                          << std::hex << cache_key;
            LogMessage(LogLevel::Info, "shader", cache_message.str());
        }
        return Status::Ok();
    }

    std::vector<uint8_t> cached_blob;
    if (!options.bypass_shader_cache &&
        disk_cache.Lookup(out_result.source_info.content_hash, cached_blob)) {
        ShaderModuleInfo pc_info{};
        if (InspectShaderContainer(cached_blob, pc_info).ok() &&
            pc_info.kind == out_result.source_info.kind &&
            !pc_info.has_xbox_private_chunks) {
            out_result.pc_bytecode = std::move(cached_blob);
            out_result.translated_hash =
                HashShaderBytes(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
            if (pc_info.chunk_count < out_result.source_info.chunk_count) {
                out_result.stripped_xbox_chunks =
                    out_result.source_info.chunk_count - pc_info.chunk_count;
            }
            LogMessage(LogLevel::Info, "shader", "Shader disk cache hit.");
            return Status::Ok();
        }
    }

    if (!out_result.source_info.has_xbox_private_chunks) {
        out_result.pc_bytecode = xbox_bytecode;
        out_result.translated_hash = out_result.source_info.content_hash;
        out_result.stripped_xbox_chunks = 0;
        if (!options.bypass_shader_cache) {
            disk_cache.Store(
                out_result.source_info.content_hash, out_result.pc_bytecode);
        }
        return Status::Ok();
    }

    // Rebuild DXBC container without Xbox-private chunks while preserving all standard chunks.
    const uint32_t orig_count = ReadU32(xbox_bytecode.data() + 28);
    struct ChunkSlice {
        const uint8_t* ptr;
        size_t total_len;
    };
    std::vector<ChunkSlice> kept;
    kept.reserve(orig_count);

    for (uint32_t i = 0; i < orig_count; ++i) {
        const uint32_t off = ReadU32(xbox_bytecode.data() + 32 + i * 4);
        if (static_cast<size_t>(off) + 8 > xbox_bytecode.size()) {
            continue;
        }
        std::string fourcc(reinterpret_cast<const char*>(xbox_bytecode.data() + off), 4);
        const uint32_t sz = ReadU32(xbox_bytecode.data() + off + 4);
        const size_t total_len = 8ull + static_cast<size_t>(sz);
        if (static_cast<size_t>(off) + total_len > xbox_bytecode.size()) {
            return Status::Error(StatusCode::ShaderError, "DXBC chunk extends past end of buffer");
        }
        if (IsXboxPrivateFourCC(fourcc)) {
            ++out_result.stripped_xbox_chunks;
            continue;
        }
        kept.push_back({xbox_bytecode.data() + off, total_len});
    }

    const size_t header_and_table = 32ull + kept.size() * 4ull;
    size_t total_size = header_and_table;
    for (const auto& c : kept) {
        total_size += c.total_len;
    }

    out_result.pc_bytecode.resize(total_size, 0);
    // Copy DXBC magic + version fields (bytes 0..23), then write updated size + chunk count.
    std::memcpy(out_result.pc_bytecode.data(), xbox_bytecode.data(), 24);
    WriteU32(out_result.pc_bytecode.data() + 24, static_cast<uint32_t>(total_size));
    WriteU32(out_result.pc_bytecode.data() + 28, static_cast<uint32_t>(kept.size()));

    size_t cursor = header_and_table;
    for (size_t i = 0; i < kept.size(); ++i) {
        WriteU32(out_result.pc_bytecode.data() + 32 + i * 4, static_cast<uint32_t>(cursor));
        std::memcpy(out_result.pc_bytecode.data() + cursor, kept[i].ptr, kept[i].total_len);
        cursor += kept[i].total_len;
    }

    out_result.translated_hash =
        HashShaderBytes(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
    if (!options.bypass_shader_cache) {
        disk_cache.Store(
            out_result.source_info.content_hash, out_result.pc_bytecode);
    }
    return Status::Ok();
}

Status ExtractXboxRdna2Program(
    const std::vector<uint8_t>& bytecode,
    XboxRdna2Program& out_program) {
    out_program = XboxRdna2Program{};

    ShaderModuleInfo info{};
    Status status = InspectShaderContainer(bytecode, info);
    if (!status.ok()) {
        return status;
    }
    if (info.kind != ShaderContainerKind::Dxil || !info.has_xbox_native_shader) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Shader container does not contain an Xbox XDAT/XBCH RDNA2 payload");
    }

    size_t xbch_offset = 0;
    size_t xbch_size = 0;
    if (!FindXboxXbchPayload(bytecode, xbch_offset, xbch_size) ||
        xbch_size < 24 ||
        std::memcmp(bytecode.data() + xbch_offset, "XBCH", 4) != 0) {
        return Status::Error(StatusCode::ShaderError, "Xbox XBCH payload is truncated");
    }

    const uint32_t stored_size = ReadU32(bytecode.data() + xbch_offset + 8);
    const uint32_t unpacked_size = ReadU32(bytecode.data() + xbch_offset + 16);
    if (stored_size != xbch_size - 24 || unpacked_size == 0 ||
        unpacked_size > kMaxFssmSize) {
        return Status::Error(StatusCode::ShaderError, "Xbox XBCH size fields are invalid");
    }

#if SCARLETTX_HAS_ZLIB
    out_program.fssm_blob.resize(unpacked_size);
    uLongf actual_size = static_cast<uLongf>(out_program.fssm_blob.size());
    const int zlib_status = uncompress(
        reinterpret_cast<Bytef*>(out_program.fssm_blob.data()),
        &actual_size,
        reinterpret_cast<const Bytef*>(bytecode.data() + xbch_offset + 24),
        static_cast<uLong>(stored_size));
    if (zlib_status != Z_OK || actual_size != unpacked_size) {
        out_program = XboxRdna2Program{};
        return Status::Error(StatusCode::ShaderError, "Xbox XBCH zlib stream is invalid");
    }
#else
    return Status::Error(
        StatusCode::NotImplemented,
        "Xbox XBCH extraction requires zlib; configure with ZLIB available");
#endif

    if (out_program.fssm_blob.size() < 0xC4 ||
        std::memcmp(out_program.fssm_blob.data(), "FSSM", 4) != 0) {
        out_program = XboxRdna2Program{};
        return Status::Error(StatusCode::ShaderError, "Xbox XBCH did not decode to a valid FSSM blob");
    }

    const uint64_t code_start =
        static_cast<uint64_t>(ReadU32(out_program.fssm_blob.data() + 0xC0)) + 0x28ull;
    if (code_start >= out_program.fssm_blob.size() || (code_start & 3u) != 0) {
        out_program = XboxRdna2Program{};
        return Status::Error(StatusCode::ShaderError, "FSSM code offset is invalid");
    }
    if (code_start >= 16) {
        const uint32_t group_x = ReadU32(out_program.fssm_blob.data() + code_start - 12);
        const uint32_t group_y = ReadU32(out_program.fssm_blob.data() + code_start - 8);
        const uint32_t group_z = ReadU32(out_program.fssm_blob.data() + code_start - 4);
        const uint64_t group_size =
            static_cast<uint64_t>(group_x) * group_y * group_z;
        if (group_x > 0 && group_y > 0 && group_z > 0 &&
            group_x <= 1024 && group_y <= 1024 && group_z <= 1024 &&
            group_size <= 1024) {
            out_program.thread_group_size_x = group_x;
            out_program.thread_group_size_y = group_y;
            out_program.thread_group_size_z = group_z;
            out_program.has_thread_group_size = true;
        }
    }

    size_t code_end = 0;
    for (size_t offset = static_cast<size_t>(code_start);
         offset + 4 <= out_program.fssm_blob.size();
         offset += 4) {
        if (ReadU32(out_program.fssm_blob.data() + offset) == 0xBF810000u) {
            code_end = offset + 4;
        }
    }
    if (code_end == 0) {
        out_program = XboxRdna2Program{};
        return Status::Error(StatusCode::ShaderError, "FSSM instruction range has no s_endpgm");
    }

    out_program.instruction_bytes.assign(
        out_program.fssm_blob.begin() + static_cast<std::ptrdiff_t>(code_start),
        out_program.fssm_blob.begin() + static_cast<std::ptrdiff_t>(code_end));
    return Status::Ok();
}

Status CompileHlslToDxbc(
    const std::string& hlsl_source,
    const std::string& entry_point,
    const std::string& target,
    std::vector<uint8_t>& out_bytecode) {
    out_bytecode.clear();
#if defined(_WIN32)
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        hlsl_source.data(),
        hlsl_source.size(),
        "scarlettx_inline.hlsl",
        nullptr,
        nullptr,
        entry_point.c_str(),
        target.c_str(),
        D3DCOMPILE_OPTIMIZATION_LEVEL1,
        0,
        &code,
        &errors);
    if (FAILED(hr) || !code) {
        std::string msg = "D3DCompile failed";
        if (errors && errors->GetBufferPointer() && errors->GetBufferSize() > 0) {
            msg += ": ";
            msg.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        if (errors) errors->Release();
        if (code) code->Release();
        return Status::Error(StatusCode::ShaderError, msg);
    }
    const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
    out_bytecode.assign(bytes, bytes + code->GetBufferSize());
    code->Release();
    if (errors) errors->Release();
    return Status::Ok();
#else
    (void)hlsl_source;
    (void)entry_point;
    (void)target;
    return Status::Error(StatusCode::NotImplemented, "D3DCompile requires Windows");
#endif
}

namespace {

bool StartsWithOneOf(const std::string& text, std::initializer_list<const char*> prefixes) {
    for (const char* prefix : prefixes) {
        if (text.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool IsKnownRdna2Opcode(const std::string& op) {
    std::string base = op;
    for (const char* suffix : {"_e32", "_e64", "_sdwa", "_dpp"}) {
        const std::string s(suffix);
        if (base.size() > s.size() &&
            base.compare(base.size() - s.size(), s.size(), s) == 0) {
            base.resize(base.size() - s.size());
            break;
        }
    }
    static const std::unordered_map<std::string, uint8_t> known = {
        {"s_mov_b32", 1}, {"s_mov_b64", 1}, {"s_movk_i32", 1},
        {"s_addk_i32", 1}, {"s_mulk_i32", 1},
        {"s_add_u32", 1}, {"s_sub_u32", 1}, {"s_and_b32", 1},
        {"s_or_b32", 1}, {"s_xor_b32", 1}, {"s_lshl_b32", 1},
        {"s_lshr_b32", 1}, {"s_ashr_i32", 1}, {"s_mul_i32", 1},
        {"s_lshl_b64", 1}, {"s_lshr_b64", 1}, {"s_ashr_i64", 1},
        {"s_bfm_b32", 1}, {"s_bfm_b64", 1}, {"s_bfe_u32", 1},
        {"s_bfe_i32", 1}, {"s_cmp_eq_u32", 1}, {"s_cmp_lt_u32", 1},
        {"s_cmp_le_u32", 1}, {"s_cmp_gt_u32", 1}, {"s_cmp_ge_u32", 1},
        {"s_cbranch_g_fork", 1}, {"s_cbranch_i_fork", 1}, {"s_cbranch_scc0", 1},
        {"s_cbranch_scc1", 1}, {"s_cbranch_vccz", 1}, {"s_cbranch_vccnz", 1},
        {"s_branch", 1}, {"s_endpgm", 1}, {"s_barrier", 1}, {"s_waitcnt", 1},
        {"s_nop", 1}, {"s_setprio", 1}, {"s_sendmsg", 1}, {"s_getpc_b64", 1},
        {"s_swappc_b64", 1}, {"s_load_dword", 1}, {"s_load_dwordx2", 1},
        {"s_load_dwordx4", 1}, {"s_load_dwordx8", 1}, {"s_load_dwordx16", 1},
        {"s_buffer_load_dword", 1}, {"s_buffer_load_dwordx2", 1},
        {"s_buffer_load_dwordx4", 1}, {"s_buffer_load_dwordx8", 1},
        {"s_buffer_load_dwordx16", 1},
        {"v_mov_b32", 1}, {"v_mov_b64", 1}, {"v_nop", 1}, {"v_add_f32", 1},
        {"v_sub_f32", 1}, {"v_mul_f32", 1}, {"v_fma_f32", 1}, {"v_fmac_f32", 1},
        {"v_mad_f32", 1}, {"v_rcp_f32", 1}, {"v_rsq_f32", 1}, {"v_sqrt_f32", 1},
        {"v_exp_f32", 1}, {"v_log_f32", 1}, {"v_floor_f32", 1},
        {"v_fract_f32", 1}, {"v_ceil_f32", 1}, {"v_trunc_f32", 1},
        {"v_add_nc_u32", 1}, {"v_sub_nc_u32", 1}, {"v_and_b32", 1},
        {"v_or_b32", 1}, {"v_xor_b32", 1}, {"v_lshlrev_b32", 1},
        {"v_lshrrev_b32", 1}, {"v_ashrrev_i32", 1}, {"v_cndmask_b32", 1},
        {"v_mul_lo_u32", 1}, {"v_mul_hi_u32", 1}, {"v_mul_u32_u24", 1},
        {"v_bfe_u32", 1}, {"v_bfi_b32", 1}, {"v_alignbit_b32", 1},
        {"v_min3_f32", 1}, {"v_max3_f32", 1}, {"v_med3_f32", 1},
        {"v_cmp_eq_f32", 1}, {"v_cmp_lt_f32", 1}, {"v_cmp_le_f32", 1},
        {"v_cmp_gt_f32", 1}, {"v_cmp_ge_f32", 1},
        {"v_perm_b32", 1}, {"v_writelane_b32", 1}, {"v_readlane_b32", 1},
        {"v_readfirstlane_b32", 1}, {"v_mbcnt_lo_u32_b32", 1},
        {"v_mbcnt_hi_u32_b32", 1}, {"v_ballot", 1},
        {"ds_swizzle_b32", 1}, {"ds_permute_b32", 1}, {"ds_bpermute_b32", 1},
        {"ds_add_u32", 1}, {"ds_sub_u32", 1}, {"ds_min_u32", 1},
        {"ds_max_u32", 1}, {"ds_and_b32", 1}, {"ds_or_b32", 1},
        {"ds_xor_b32", 1}, {"ds_write_b32", 1}, {"ds_read_b32", 1},
        {"buffer_load_dword", 1}, {"buffer_load_dwordx2", 1},
        {"buffer_load_dwordx3", 1}, {"buffer_load_dwordx4", 1},
        {"buffer_store_dword", 1}, {"buffer_store_dwordx2", 1},
        {"buffer_store_dwordx3", 1}, {"buffer_store_dwordx4", 1},
        {"buffer_atomic_add", 1}, {"buffer_atomic_sub", 1},
        {"tbuffer_load_format_x", 1}, {"tbuffer_load_format_xy", 1},
        {"tbuffer_load_format_xyz", 1}, {"tbuffer_load_format_xyzw", 1},
        {"tbuffer_store_format_x", 1}, {"tbuffer_store_format_xy", 1},
        {"tbuffer_store_format_xyz", 1}, {"tbuffer_store_format_xyzw", 1},
        {"image_load", 1}, {"image_load_mip", 1}, {"image_store", 1},
        {"image_store_mip", 1}, {"image_sample", 1}, {"image_sample_l", 1},
        {"image_sample_lz", 1}, {"image_sample_b", 1}, {"image_sample_b_cl", 1},
        {"image_sample_c", 1}, {"image_sample_c_l", 1},
        {"image_sample_c_lz", 1}, {"image_sample_c_b", 1},
        {"image_sample_o", 1}, {"image_sample_l_o", 1},
        {"image_sample_lz_o", 1}, {"image_sample_b_o", 1},
        {"image_sample_c_o", 1}, {"image_gather4", 1},
        {"image_gather4_l", 1}, {"image_gather4_lz", 1},
        {"image_gather4_b", 1}, {"image_gather4_c", 1},
        {"image_gather4_c_l", 1}, {"image_gather4_c_lz", 1},
        {"flat_load_dword", 1}, {"flat_load_dwordx2", 1},
        {"flat_load_dwordx4", 1}, {"flat_store_dword", 1},
        {"global_load_dword", 1}, {"global_load_dwordx2", 1},
        {"global_load_dwordx4", 1}, {"global_store_dword", 1},
        {"exp", 1}, {"v_interp_p1_f32", 1}, {"v_interp_p2_f32", 1},
        {"v_interp_mov_f32", 1}, {"v_interp_p1_f16", 1},
        {"v_interp_p2_f16_f32", 1},
    };
    auto it = known.find(base);
    return it != known.end();
}

}  // namespace

Rdna2InstructionFeatures InspectRdna2Disassembly(const std::string& disassembly) noexcept {
    Rdna2InstructionFeatures features{};
    std::istringstream input(disassembly);
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("exec_hi") != std::string::npos ||
            line.find("vcc_hi") != std::string::npos ||
            line.find("wave64") != std::string::npos ||
            line.find("_b64") != std::string::npos) {
            features.wave64 = true;
        }
        if (StartsWithOneOf(line,
                            {"image_", "tbuffer_", "s_getresinfo", "s_sample"})) {
            features.texture_xbox = true;
        }
    }
    return features;
}

Status ValidateRdna2Disassembly(const std::string& disassembly) {
    std::istringstream input(disassembly);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '.') {
            continue;
        }
        size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos) {
            continue;
        }
        const size_t end = line.find_first_of(" \t", begin);
        std::string op = line.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (op.find(':') != std::string::npos) {
            continue;
        }
        if (!IsKnownRdna2Opcode(op) &&
            StartsWithOneOf(op, {"s_", "v_", "ds_", "buffer_", "tbuffer_", "image_", "flat_", "global_", "exp"})) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "Unknown RDNA2 opcode encountered: " + op);
        }
    }
    return Status::Ok();
}

ShaderCache::ShaderCache() {
#if defined(_WIN32)
    const char* env_root = std::getenv("SCARLETTX_SHADER_CACHE");
    if (env_root && env_root[0] != '\0') {
        disk_directory_ = std::filesystem::path(env_root);
    } else {
        const char* local_app_data = std::getenv("LOCALAPPDATA");
        // Product path is %LOCALAPPDATA%\ViceWin\shader_cache. Entries left in
        // the pre-release %LOCALAPPDATA%\ScarlettX location are not migrated:
        // the cache format version and key both changed, so they are stale by
        // construction.
        disk_directory_ = local_app_data && local_app_data[0]
                              ? std::filesystem::path(local_app_data) / "ViceWin" / "shader_cache"
                              : std::filesystem::temp_directory_path() / "shader_cache";
    }
#else
    disk_directory_ = std::filesystem::temp_directory_path() / "shader_cache";
#endif
}

ShaderCache::ShaderCache(std::filesystem::path disk_directory)
    : disk_directory_(std::move(disk_directory)) {}

std::filesystem::path ShaderCacheDiskPath(const std::filesystem::path& dir, uint64_t key) {
    std::ostringstream name;
    name << "sx" << std::hex << std::setw(16) << std::setfill('0') << key << ".sxbc";
    return dir / name.str();
}

bool ShaderCache::Lookup(uint64_t key, std::vector<uint8_t>& out_blob) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        ++hits_;
        out_blob = it->second;
        return true;
    }

    if (!disk_directory_.empty()) {
        std::error_code ec;
        const auto path = ShaderCacheDiskPath(disk_directory_, key);
        std::ifstream input(path, std::ios::binary);
        if (input) {
            std::array<uint8_t, kShaderCacheHeaderSize> header{};
            input.read(reinterpret_cast<char*>(header.data()), header.size());
            const uint64_t payload_size = ReadU64(header.data() + 16);
            const uint64_t payload_hash = ReadU64(header.data() + 24);
            const uint64_t file_size = std::filesystem::file_size(path, ec);
            const bool valid_header =
                input.good() &&
                std::equal(kShaderCacheMagic.begin(), kShaderCacheMagic.end(), header.begin()) &&
                ReadU32(header.data() + 4) == kShaderCacheVersion &&
                ReadU64(header.data() + 8) == key &&
                payload_size <= std::numeric_limits<uint32_t>::max() &&
                !ec && file_size == kShaderCacheHeaderSize + payload_size;
            if (valid_header) {
                std::vector<uint8_t> blob(static_cast<size_t>(payload_size));
                if (!blob.empty()) {
                    input.read(reinterpret_cast<char*>(blob.data()),
                               static_cast<std::streamsize>(blob.size()));
                }
                if (input.good() && HashShaderBytes(blob.data(), blob.size()) == payload_hash) {
                    out_blob = std::move(blob);
                    entries_[key] = out_blob;
                    ++hits_;
                    return true;
                }
            }

            input.close();
            std::filesystem::remove(path, ec);
        }
    }
    ++misses_;
    return false;
}

void ShaderCache::Store(uint64_t key, std::vector<uint8_t> blob) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = blob;

    if (!disk_directory_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(disk_directory_, ec);
        if (!ec) {
            const auto path = ShaderCacheDiskPath(disk_directory_, key);
            const auto tmp = path.string() + ".tmp";
            std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
            if (output) {
                std::array<uint8_t, kShaderCacheHeaderSize> header{};
                BuildShaderCacheHeader(
                    header.data(), key, blob.size(), HashShaderBytes(blob.data(), blob.size()));
                output.write(reinterpret_cast<const char*>(header.data()), header.size());
                output.write(reinterpret_cast<const char*>(blob.data()),
                             static_cast<std::streamsize>(blob.size()));
                output.close();
                if (output) {
                    std::filesystem::rename(tmp, path, ec);
                    if (ec) {
                        std::error_code ignored;
                        std::filesystem::remove(path, ignored);
                        std::filesystem::rename(tmp, path, ignored);
                    }
                } else {
                    std::error_code ignored;
                    std::filesystem::remove(tmp, ignored);
                }
            }
        }
    }
}

void ShaderCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    hits_ = 0;
    misses_ = 0;
    if (!disk_directory_.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(disk_directory_, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(disk_directory_, ec)) {
                if (!entry.is_regular_file(ec)) {
                    continue;
                }
                const auto name = entry.path().filename().string();
                if (name.size() >= 5 &&
                    name.compare(name.size() - 5, 5, ".sxbc") == 0) {
                    std::error_code ignored;
                    std::filesystem::remove(entry.path(), ignored);
                }
            }
        }
    }
}

void ShaderCache::Evict(uint64_t key) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(key);
    if (!disk_directory_.empty()) {
        std::error_code ec;
        std::filesystem::remove(ShaderCacheDiskPath(disk_directory_, key), ec);
    }
}

uint64_t ShaderCache::hits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

uint64_t ShaderCache::misses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
}

size_t ShaderCache::entry_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace scarlettx
