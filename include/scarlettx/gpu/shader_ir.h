#pragma once

#include <cstdint>
#include <mutex>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

enum class ShaderContainerKind : uint32_t {
    Unknown = 0,
    Dxbc = 1,
    Dxil = 2,
    ScarlettRdna2 = 3,
};

enum class ShaderStage : uint32_t {
    Unknown = 0,
    Vertex = 1,
    Pixel = 2,
    Compute = 3,
    Geometry = 4,
    Hull = 5,
    Domain = 6,
    Library = 7,  // DXR raytracing library (lib_6_3+)
    Mesh = 8,
    Amplification = 9,
};

struct ShaderModuleInfo {
    ShaderContainerKind kind = ShaderContainerKind::Unknown;
    ShaderStage stage = ShaderStage::Unknown;
    uint32_t byte_size = 0;
    uint32_t chunk_count = 0;
    uint32_t input_param_count = 0;
    uint32_t output_param_count = 0;
    uint32_t shader_model_major = 0;
    uint32_t shader_model_minor = 0;
    uint64_t content_hash = 0;
    bool has_raytracing_exports = false;
    bool has_embedded_root_signature = false;
    bool has_xbox_private_chunks = false;
    bool has_xbox_native_shader = false;
    bool has_missing_dxil_bitcode = false;
    std::vector<std::string> chunk_fourccs;
};

struct ShaderTranslationResult {
    ShaderModuleInfo source_info;
    std::vector<uint8_t> pc_bytecode;
    uint64_t translated_hash = 0;
    uint32_t stripped_xbox_chunks = 0;
};

struct XboxRdna2Program {
    std::vector<uint8_t> fssm_blob;
    std::vector<uint8_t> instruction_bytes;
    uint32_t thread_group_size_x = 1;
    uint32_t thread_group_size_y = 1;
    uint32_t thread_group_size_z = 1;
    bool has_thread_group_size = false;
};

struct ShaderTranslationOptions {
    ShaderStage stage_override = ShaderStage::Unknown;
    uint32_t root_cbv_count = 0;
    uint32_t root_srv_count = 0;
    uint32_t root_sampler_count = 0;
    uint32_t root_uav_count = 0;
    uint32_t first_cbv_register = 0;
    bool use_bindless_resources = false;
    bool use_bindless_samplers = false;
    bool bypass_shader_cache = false;
};

uint64_t HashShaderBytes(const uint8_t* data, size_t size);

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info);

// Extracts the compressed FSSM payload and the aligned RDNA2 instruction range
// from an Xbox DXIL/XDAT container. Does not translate the instructions.
Status ExtractXboxRdna2Program(
    const std::vector<uint8_t>& bytecode,
    XboxRdna2Program& out_program);

// Translates a Scarlett/Xbox DXBC or DXIL container into a PC-compatible DXBC/DXIL
// container by stripping Xbox-private chunks (XBOX, XS2M, XDBG) and rebuilding the
// DXBC chunk offset table. Embedded Xbox RDNA2 FSSM payloads, including NOBC
// containers, are lifted to DXIL for the supported instruction subset; unsupported
// encodings fail explicitly rather than reaching the host driver.
Status TranslateXboxShaderToPc(
    const std::vector<uint8_t>& xbox_bytecode,
    ShaderTranslationResult& out_result);
Status TranslateXboxShaderToPc(
    const std::vector<uint8_t>& xbox_bytecode,
    const ShaderTranslationOptions& options,
    ShaderTranslationResult& out_result);

// Compiles HLSL source to DXBC bytecode using d3dcompiler (Windows).
// target examples: "vs_5_0", "ps_5_0", "cs_5_0".
Status CompileHlslToDxbc(
    const std::string& hlsl_source,
    const std::string& entry_point,
    const std::string& target,
    std::vector<uint8_t>& out_bytecode);

// Validates a textual GCN/RDNA2 disassembly before it is handed to the
// translator. Unknown opcodes are reported instead of crashing or guessing
// at an encoding.
Status ValidateRdna2Disassembly(const std::string& disassembly);

// Reports whether Xbox/RDNA2-specific instruction families are present.
struct Rdna2InstructionFeatures {
    bool wave64 = false;
    bool texture_xbox = false;
};
Rdna2InstructionFeatures InspectRdna2Disassembly(const std::string& disassembly) noexcept;

// Thread-safe in-memory shader/PSO blob cache keyed by 64-bit hash.
class ShaderCache {
public:
    ShaderCache();
    explicit ShaderCache(std::filesystem::path disk_directory);

    bool Lookup(uint64_t key, std::vector<uint8_t>& out_blob);
    void Store(uint64_t key, std::vector<uint8_t> blob);
    void Clear();

    // Drops one entry from memory and disk. Used when a cache hit fails
    // re-validation, so the next lookup recompiles instead of serving the
    // rejected blob again.
    void Evict(uint64_t key);

    uint64_t hits() const;
    uint64_t misses() const;
    size_t entry_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> entries_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    std::filesystem::path disk_directory_;
};

}  // namespace scarlettx
