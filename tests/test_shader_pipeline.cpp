#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/dxil_backend.h"
#include "scarlettx/gpu/rdna2_lifter.h"
#include "scarlettx/gpu/shader_ir.h"
#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_shader_pipeline] FAILED: " #cond                \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#if SCARLETTX_HAS_ZLIB
#include <zlib.h>
#endif

namespace {

void AppendLe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void WriteLe32(std::vector<uint8_t>& buf, size_t offset, uint32_t v) {
    buf[offset] = static_cast<uint8_t>(v & 0xFFu);
    buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    buf[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

// Builds a valid synthetic DXBC container with custom chunks (for testing Xbox stripping & DXIL inspection).
std::vector<uint8_t> BuildSyntheticDxbc(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& chunks) {
    const uint32_t n = static_cast<uint32_t>(chunks.size());
    std::vector<uint32_t> offsets(n, 0);
    uint32_t cur = 32u + n * 4u;
    for (uint32_t i = 0; i < n; ++i) {
        offsets[i] = cur;
        cur += 8u + static_cast<uint32_t>(chunks[i].second.size());
    }

    std::vector<uint8_t> out;
    out.reserve(cur);
    out.insert(out.end(), {'D', 'X', 'B', 'C'});
    out.insert(out.end(), 16, 0u);  // MD5 placeholder
    AppendLe32(out, 1u);            // version
    AppendLe32(out, cur);           // total size
    AppendLe32(out, n);             // chunk count
    for (uint32_t off : offsets) AppendLe32(out, off);
    for (const auto& c : chunks) {
        out.insert(out.end(), c.first.begin(), c.first.begin() + 4);
        AppendLe32(out, static_cast<uint32_t>(c.second.size()));
        out.insert(out.end(), c.second.begin(), c.second.end());
    }
    return out;
}

std::vector<uint8_t> BuildXboxNativeShaderContainer() {
    std::vector<uint8_t> fssm(0x12Cu, 0);
    std::memcpy(fssm.data(), "FSSM", 4);
    fssm[4] = 1;
    fssm[5] = 1;
    WriteLe32(fssm, 0xC0, 0x100);
    WriteLe32(fssm, 0x128, 0xBF810000u);

#if SCARLETTX_HAS_ZLIB
    uLongf compressed_size = compressBound(static_cast<uLong>(fssm.size()));
    std::vector<uint8_t> compressed(compressed_size);
    const int compress_status = compress2(
        compressed.data(),
        &compressed_size,
        fssm.data(),
        static_cast<uLong>(fssm.size()),
        Z_BEST_SPEED);
    if (compress_status != Z_OK) {
        return {};
    }
    compressed.resize(compressed_size);
#else
    std::vector<uint8_t> compressed = {0x78, 0x01, 0x00};
#endif

    std::vector<uint8_t> xbch(24 + compressed.size(), 0);
    std::memcpy(xbch.data(), "XBCH", 4);
    WriteLe32(xbch, 8, static_cast<uint32_t>(compressed.size()));
    WriteLe32(xbch, 16, static_cast<uint32_t>(fssm.size()));
    std::memcpy(xbch.data() + 24, compressed.data(), compressed.size());

    constexpr size_t xdat_offset = 24;
    constexpr size_t xdat_header_size = 36;
    constexpr size_t no_bc_offset = xdat_offset + xdat_header_size;
    constexpr size_t xbch_offset = no_bc_offset + 16;
    std::vector<uint8_t> dxil(xbch_offset + xbch.size(), 0);
    const uint32_t program_version = (5u << 16) | (6u << 4) | 6u;
    std::memcpy(dxil.data(), &program_version, sizeof(program_version));
    std::memcpy(dxil.data() + xdat_offset, "XDAT", 4);
    WriteLe32(dxil, xdat_offset + 4, 2);
    WriteLe32(dxil, xdat_offset + 8, xdat_header_size);
    WriteLe32(dxil, xdat_offset + 12, 0x07);
    WriteLe32(dxil, xdat_offset + 16, static_cast<uint32_t>(no_bc_offset));
    WriteLe32(dxil, xdat_offset + 20, 16);
    WriteLe32(dxil, xdat_offset + 24, 0x0D);
    WriteLe32(dxil, xdat_offset + 28, static_cast<uint32_t>(xbch_offset));
    WriteLe32(dxil, xdat_offset + 32, static_cast<uint32_t>(xbch.size()));
    std::memcpy(dxil.data() + no_bc_offset + 8, "NOBC", 4);
    std::memcpy(dxil.data() + xbch_offset, xbch.data(), xbch.size());

    return BuildSyntheticDxbc({{"DXIL", dxil}});
}

}  // namespace

int main() {
    using namespace scarlettx;

    // 1. Synthetic DXIL raytracing library with Xbox private chunks + embedded root signature
    {
        std::vector<uint8_t> dxil_payload(24, 0);
        // DXIL header: program version token at offset 0: (type=7 Library << 16) | (major=6 << 4) | minor=5
        const uint32_t ver_token = (7u << 16) | (6u << 4) | 5u;
        std::memcpy(dxil_payload.data(), &ver_token, 4);

        auto xbox_dxil = BuildSyntheticDxbc({
            {"DXIL", dxil_payload},
            {"RDAT", {1, 2, 3, 4}},
            {"RTS0", {5, 6, 7, 8}},
            {"XBOX", {0xAA, 0xBB, 0xCC, 0xDD}},
            {"XS2M", {0x11, 0x22, 0x33, 0x44}},
        });

        ShaderModuleInfo info{};
        SX_CHECK(InspectShaderContainer(xbox_dxil, info).ok());
        SX_CHECK(info.kind == ShaderContainerKind::Dxil);
        SX_CHECK(info.stage == ShaderStage::Library);
        SX_CHECK(info.shader_model_major == 6);
        SX_CHECK(info.shader_model_minor == 5);
        SX_CHECK(info.has_raytracing_exports);
        SX_CHECK(info.has_embedded_root_signature);
        SX_CHECK(info.has_xbox_private_chunks);
        SX_CHECK(info.chunk_count == 5);

        ShaderTranslationResult translated{};
        SX_CHECK(TranslateXboxShaderToPc(xbox_dxil, translated).ok());
        SX_CHECK(translated.stripped_xbox_chunks == 2);

        ShaderModuleInfo pc_info{};
        SX_CHECK(InspectShaderContainer(translated.pc_bytecode, pc_info).ok());
        SX_CHECK(pc_info.kind == ShaderContainerKind::Dxil);
        SX_CHECK(!pc_info.has_xbox_private_chunks);
        SX_CHECK(pc_info.chunk_count == 3);
        SX_CHECK(pc_info.has_raytracing_exports);
        SX_CHECK(pc_info.has_embedded_root_signature);
    }

    // 1a. DXIL program types for Mesh and Amplification are preserved as distinct stages.
    {
        for (const auto& [program_type, expected_stage] : {
                 std::pair<uint32_t, ShaderStage>{13u, ShaderStage::Mesh},
                 std::pair<uint32_t, ShaderStage>{14u, ShaderStage::Amplification},
             }) {
            std::vector<uint8_t> dxil_payload(24, 0);
            const uint32_t version =
                (program_type << 16) | (6u << 4) | 6u;
            std::memcpy(dxil_payload.data(), &version, sizeof(version));
            const auto shader = BuildSyntheticDxbc({{"DXIL", dxil_payload}});
            ShaderModuleInfo info{};
            SX_CHECK(InspectShaderContainer(shader, info).ok());
            SX_CHECK(info.stage == expected_stage);
        }
    }

    // 2. Reject malformed DXBC sizes, offsets, and overlapping chunks.
    {
        ShaderModuleInfo info{};
        auto bad_size = BuildSyntheticDxbc({{"DXIL", {1, 2, 3, 4}}});
        bad_size[24] = static_cast<uint8_t>(bad_size.size() - 1);
        SX_CHECK(!InspectShaderContainer(bad_size, info).ok());

        auto bad_chunk = BuildSyntheticDxbc({{"DXIL", {1, 2, 3, 4}}});
        bad_chunk[40] = 0xFF;
        bad_chunk[41] = 0xFF;
        bad_chunk[42] = 0xFF;
        bad_chunk[43] = 0x7F;
        SX_CHECK(!InspectShaderContainer(bad_chunk, info).ok());

        auto overlapping = BuildSyntheticDxbc({
            {"DXIL", {1, 2, 3, 4}},
            {"RDAT", {5, 6, 7, 8}},
        });
        overlapping[36] = overlapping[32];
        overlapping[37] = overlapping[33];
        overlapping[38] = overlapping[34];
        overlapping[39] = overlapping[35];
        SX_CHECK(!InspectShaderContainer(overlapping, info).ok());
    }

    // 3. Scarlett precompiled RDNA2 ISA container detection
    {
        std::vector<uint8_t> rdna2 = {'X', 'S', '2', '\0', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        ShaderModuleInfo info{};
        SX_CHECK(InspectShaderContainer(rdna2, info).ok());
        SX_CHECK(info.kind == ShaderContainerKind::ScarlettRdna2);
        ShaderTranslationResult tr{};
        Status st = TranslateXboxShaderToPc(rdna2, tr);
        SX_CHECK(!st.ok());
        SX_CHECK(st.code == StatusCode::UnsupportedFormat);
    }

    // 4. Xbox XDAT/XBCH extraction and explicit NOBC classification.
    {
        const auto native_shader = BuildXboxNativeShaderContainer();
        SX_CHECK(!native_shader.empty());

        ShaderModuleInfo info{};
        SX_CHECK(InspectShaderContainer(native_shader, info).ok());
        SX_CHECK(info.has_xbox_native_shader);
        SX_CHECK(info.has_missing_dxil_bitcode);

        ShaderTranslationResult translated{};
        Status translate_status = TranslateXboxShaderToPc(native_shader, translated);
        SX_CHECK(!translate_status.ok());
        SX_CHECK(translate_status.code == StatusCode::UnsupportedFormat);

        XboxRdna2Program program{};
#if SCARLETTX_HAS_ZLIB
        SX_CHECK(ExtractXboxRdna2Program(native_shader, program).ok());
        SX_CHECK(program.fssm_blob.size() == 0x12Cu);
        SX_CHECK(program.instruction_bytes.size() == 4);
        SX_CHECK(program.instruction_bytes[0] == 0x00);
        SX_CHECK(program.instruction_bytes[1] == 0x00);
        SX_CHECK(program.instruction_bytes[2] == 0x81);
        SX_CHECK(program.instruction_bytes[3] == 0xBF);
#else
        Status extract_status = ExtractXboxRdna2Program(native_shader, program);
        SX_CHECK(!extract_status.ok());
        SX_CHECK(extract_status.code == StatusCode::NotImplemented);
#endif
    }

    // 5. Binary RDNA2 decode, SSA lift, and stage-specific HLSL generation.
    {
        std::vector<uint8_t> program;
        uint32_t mov_v0_literal = 0x7e0002ffu;
        const uint32_t pixel_export = 0xf800000fu;
        for (uint32_t bits : {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u}) {
            AppendLe32(program, mov_v0_literal);
            AppendLe32(program, bits);
            mov_v0_literal += (1u << 17);
        }
        AppendLe32(program, pixel_export);
        AppendLe32(program, 0x03020100u);
        AppendLe32(program, 0xbf810000u);

        std::vector<Rdna2Instruction> decoded;
        SX_CHECK(DecodeRdna2Instructions(program, decoded).ok());
        SX_CHECK(decoded.size() == 6);
        SX_CHECK(decoded[0].mnemonic == "v_mov_b32");
        SX_CHECK(decoded[4].mnemonic == "exp");

        Rdna2TranslationOptions options{};
        options.stage = ShaderStage::Pixel;
        Rdna2IrModule ir{};
        SX_CHECK(BuildRdna2Ir(decoded, options, ir).ok());
        std::string hlsl;
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("float4 main() : SV_Target0") != std::string::npos);
        SX_CHECK(hlsl.find("asfloat(sx_v") != std::string::npos);

        std::vector<uint8_t> mesh_program;
        AppendLe32(
            mesh_program,
            (0x17du << 23) | (0x03u << 8) | (124u << 16) | 0xffu);
        AppendLe32(mesh_program, 0x00001003u);  // m0 = 1 primitive, 3 vertices
        uint32_t mesh_move = 0x7e0002ffu;
        for (uint32_t bits : {
                 0x3f800000u, 0x40000000u, 0x3f800000u,
                 0x3f800000u, 0x00000801u}) {
            AppendLe32(mesh_program, mesh_move);
            AppendLe32(mesh_program, bits);
            mesh_move += 1u << 17;
        }
        AppendLe32(mesh_program, 0xbf980009u);  // s_sendmsg GS_ALLOC_REQ
        AppendLe32(mesh_program, 0xf80000cfu);  // exp pos0
        AppendLe32(mesh_program, 0x03020100u);
        AppendLe32(mesh_program, 0xf8000141u);  // exp prim
        AppendLe32(mesh_program, 0x00000004u);  // primitive data from v4
        AppendLe32(mesh_program, 0xbf810000u);

        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(mesh_program, decoded).ok());
        SX_CHECK(decoded.front().destinations[0].bank == Rdna2RegisterBank::M0);
        SX_CHECK(std::any_of(
            decoded.begin(), decoded.end(),
            [](const Rdna2Instruction& instruction) {
                return instruction.mnemonic == "s_sendmsg" &&
                       (static_cast<uint32_t>(instruction.immediate) & 0xfu) == 9u;
            }));
        Rdna2TranslationOptions mesh_options{};
        mesh_options.stage = ShaderStage::Mesh;
        mesh_options.thread_group_size_x = 4;
        mesh_options.thread_group_size_y = 1;
        mesh_options.thread_group_size_z = 1;
        mesh_options.mesh_max_vertices = 4;
        mesh_options.mesh_max_primitives = 4;
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, mesh_options, ir).ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("[outputtopology(\"triangle\")]") != std::string::npos);
        SX_CHECK(hlsl.find("out vertices SXMeshVertex") != std::string::npos);
        SX_CHECK(hlsl.find("out indices uint3") != std::string::npos);
        SX_CHECK(hlsl.find("SetMeshOutputCounts(sx_mesh_nv, sx_mesh_np)") !=
                 std::string::npos);
        SX_CHECK(hlsl.find("sx_mesh_output.position.x = asfloat(") !=
                 std::string::npos);
        SX_CHECK(hlsl.find("sx_mesh_primitive >> 20u") != std::string::npos);

        for (const uint32_t bvh_word : {0xf1980f01u, 0xf19c0f01u}) {
            std::vector<uint8_t> bvh_program;
            AppendLe32(bvh_program, bvh_word);
            AppendLe32(bvh_program, 0x00001401u);
            AppendLe32(bvh_program, 0xbf810000u);
            decoded.clear();
            SX_CHECK(DecodeRdna2Instructions(bvh_program, decoded).ok());
            SX_CHECK(decoded[0].mnemonic ==
                     (bvh_word == 0xf1980f01u
                          ? "image_bvh_intersect_ray"
                          : "image_bvh64_intersect_ray"));
            SX_CHECK(decoded[0].destinations[0].index == 20u);
            Rdna2TranslationOptions bvh_options{};
            bvh_options.stage = ShaderStage::Compute;
            ir = Rdna2IrModule{};
            const Status bvh_status =
                BuildRdna2Ir(decoded, bvh_options, ir);
            SX_CHECK(!bvh_status.ok());
            SX_CHECK(bvh_status.message.find("RayQuery") != std::string::npos);
        }

        std::vector<uint8_t> f16_program;
        auto append_vop1 = [&](uint32_t opcode, uint32_t destination, uint32_t source) {
            AppendLe32(
                f16_program,
                (0x3fu << 25) | (opcode << 9) | (destination << 17) |
                    (0x100u + source));
        };
        auto append_vop3p = [&](uint32_t opcode, uint32_t destination,
                                uint32_t source0, uint32_t source1,
                                uint32_t source2, bool has_third_source) {
            const uint32_t word =
                (0x33u << 26) | (opcode << 16) | destination |
                (has_third_source ? (1u << 14) : 0u);
            const uint32_t sources =
                source0 | (source1 << 9) | (source2 << 18) |
                (1u << 27) | (1u << 28);
            AppendLe32(f16_program, word);
            AppendLe32(f16_program, sources);
        };
        append_vop3p(0x0fu, 8u, 0x100u, 0x101u, 125u, false);
        append_vop3p(0x10u, 9u, 0x101u, 0x102u, 125u, false);
        append_vop3p(0x0eu, 10u, 0x100u, 0x101u, 0x102u, true);
        append_vop1(0x0au, 4u, 3u);
        append_vop1(0x0bu, 5u, 4u);
        AppendLe32(f16_program, 0xf8000001u);  // export MRT0.X
        AppendLe32(f16_program, 0x00000105u);  // v5
        AppendLe32(f16_program, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(f16_program, decoded).ok());
        SX_CHECK(decoded[0].mnemonic == "v_pk_add_f16");
        SX_CHECK(decoded[1].mnemonic == "v_pk_mul_f16");
        SX_CHECK(decoded[2].mnemonic == "v_pk_fma_f16");
        SX_CHECK(decoded[3].mnemonic == "v_cvt_f16_f32");
        SX_CHECK(decoded[4].mnemonic == "v_cvt_f32_f16");
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, options, ir).ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("half2 sx_f16_src_") != std::string::npos);
        SX_CHECK(hlsl.find("mad(sx_f16_src_") != std::string::npos);
        SX_CHECK(hlsl.find("f32tof16(asfloat(") != std::string::npos);
        SX_CHECK(hlsl.find("f16tof32((") != std::string::npos);

        std::vector<uint8_t> instruction_coverage;
        auto append_vop3 = [&](uint32_t opcode, uint32_t destination) {
            const uint32_t word =
                (0x35u << 26) | (opcode << 16) | destination;
            const uint32_t sources = 4u | (5u << 9) | (6u << 18);
            AppendLe32(instruction_coverage, word);
            AppendLe32(instruction_coverage, sources);
        };
        AppendLe32(instruction_coverage, (0xbu << 28) | (0x00u << 23) | 7u);
        AppendLe32(instruction_coverage, (0xbu << 28) | (0x0eu << 23) | 3u);
        AppendLe32(instruction_coverage, (0xbu << 28) | (0x0fu << 23) | 2u);
        append_vop3(0x148u, 8u);
        append_vop3(0x14au, 9u);
        append_vop3(0x14eu, 10u);
        append_vop3(0x151u, 11u);
        append_vop3(0x154u, 12u);
        append_vop3(0x157u, 13u);
        AppendLe32(instruction_coverage, 0xf800000fu);
        AppendLe32(instruction_coverage, 0x0b0a0908u);
        AppendLe32(instruction_coverage, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(instruction_coverage, decoded).ok());
        SX_CHECK(decoded[0].mnemonic == "s_movk_i32");
        SX_CHECK(decoded[1].mnemonic == "s_addk_i32");
        SX_CHECK(decoded[2].mnemonic == "s_mulk_i32");
        SX_CHECK(decoded[3].mnemonic == "v_bfe_u32");
        SX_CHECK(decoded[4].mnemonic == "v_bfi_b32");
        SX_CHECK(decoded[5].mnemonic == "v_alignbit_b32");
        SX_CHECK(decoded[6].mnemonic == "v_min3_f32");
        SX_CHECK(decoded[7].mnemonic == "v_max3_f32");
        SX_CHECK(decoded[8].mnemonic == "v_med3_f32");
        ir = Rdna2IrModule{};
        Rdna2TranslationOptions coverage_options = options;
        coverage_options.root_cbv_count = 1;
        const Status coverage_status =
            BuildRdna2Ir(decoded, coverage_options, ir);
        if (!coverage_status.ok()) {
            std::fprintf(
                stderr, "Instruction coverage IR build failed: %s\n",
                coverage_status.message.c_str());
        }
        SX_CHECK(coverage_status.ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("1u << (") != std::string::npos);
        SX_CHECK(hlsl.find("asuint(min(min(") != std::string::npos);
        SX_CHECK(hlsl.find("asuint(max(min(") != std::string::npos);
        SX_CHECK(hlsl.find("0x7u") != std::string::npos);
        SX_CHECK(hlsl.find(" * ") != std::string::npos);

        std::vector<uint8_t> interpolation_program;
        auto append_interpolation = [&](uint32_t opcode, uint32_t destination,
                                       uint32_t attribute, uint32_t channel) {
            AppendLe32(
                interpolation_program,
                (0x32u << 26) | (opcode << 16) | (destination << 18) |
                    (attribute << 10) | (channel << 8));
        };
        append_interpolation(0u, 0u, 1u, 0u);
        append_interpolation(1u, 1u, 1u, 1u);
        append_interpolation(2u, 2u, 2u, 2u);
        append_interpolation(3u, 3u, 2u, 3u);
        AppendLe32(interpolation_program, 0xf800000fu);
        AppendLe32(interpolation_program, 0x03020100u);
        AppendLe32(interpolation_program, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(interpolation_program, decoded).ok());
        SX_CHECK(decoded[0].encoding == Rdna2Encoding::Vinterp);
        SX_CHECK(decoded[0].mnemonic == "v_interp_p1_f32");
        SX_CHECK(decoded[1].mnemonic == "v_interp_p2_f32");
        SX_CHECK(decoded[2].mnemonic == "v_interp_mov_f32");
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, options, ir).ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("input.attr1.x") != std::string::npos);
        SX_CHECK(hlsl.find("input.attr1.y") != std::string::npos);
        SX_CHECK(hlsl.find("nointerpolation float4 attr2") != std::string::npos);

        std::vector<uint8_t> texture_program;
        AppendLe32(texture_program, 0xf0000108u);  // 2D image_load, component X
        AppendLe32(texture_program, 0x00000400u);  // vaddr=v0, vdata=v4, resource s[0:3]
        AppendLe32(texture_program, 0xf8000001u);  // export MRT0.X
        AppendLe32(texture_program, 0x00000004u);  // v4
        AppendLe32(texture_program, 0xbf810000u);

        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(texture_program, decoded).ok());
        options.root_srv_count = 1;
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, options, ir).ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("Texture2D<float4> sx_texture0 : register(t0)") != std::string::npos);
        SX_CHECK(hlsl.find(".Load(int3(") != std::string::npos);

        struct SampleCase {
            uint32_t opcode;
            const char* method;
        };
        for (const SampleCase sample : {
                 SampleCase{0x20u, ".Sample("},
                 SampleCase{0x24u, ".SampleLevel("},
                 SampleCase{0x25u, ".SampleBias("},
                 SampleCase{0x22u, ".SampleGrad("},
             }) {
            std::vector<uint8_t> sample_program;
            AppendLe32(sample_program, 0xf0000108u | (sample.opcode << 18));
            AppendLe32(sample_program, 0x00000400u);  // vaddr=v0, vdata=v4, T#0, S#0
            AppendLe32(sample_program, 0xf8000001u);
            AppendLe32(sample_program, 0x00000004u);
            AppendLe32(sample_program, 0xbf810000u);

            decoded.clear();
            SX_CHECK(DecodeRdna2Instructions(sample_program, decoded).ok());
            options.root_sampler_count = 1;
            ir = Rdna2IrModule{};
            SX_CHECK(BuildRdna2Ir(decoded, options, ir).ok());
            hlsl.clear();
            SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
            SX_CHECK(hlsl.find("SamplerState sx_sampler0 : register(s0)") != std::string::npos);
            SX_CHECK(hlsl.find(sample.method) != std::string::npos);
        }

        std::vector<uint8_t> bindless_sample;
        AppendLe32(bindless_sample, 0xf0000108u | (0x20u << 18));
        AppendLe32(bindless_sample, 0x00000400u);
        AppendLe32(bindless_sample, 0xf8000001u);
        AppendLe32(bindless_sample, 0x00000004u);
        AppendLe32(bindless_sample, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(bindless_sample, decoded).ok());
        Rdna2TranslationOptions bindless_options = options;
        bindless_options.root_cbv_count = 1;
        bindless_options.use_bindless_resources = true;
        bindless_options.use_bindless_samplers = true;
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, bindless_options, ir).ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("ResourceDescriptorHeap[NonUniformResourceIndex(sx_s0)]") !=
                 std::string::npos);
        SX_CHECK(hlsl.find("SamplerDescriptorHeap[NonUniformResourceIndex(sx_s0)]") !=
                 std::string::npos);

        std::vector<uint8_t> branch_program;
        AppendLe32(branch_program, 0x7c040300u);  // v_cmp_eq_f32 vcc, v0, v1
        AppendLe32(branch_program, 0xbf860003u);  // s_cbranch_vccz -> byte offset 20
        AppendLe32(branch_program, 0x7e0802ffu);  // v_mov_b32 v4, literal
        AppendLe32(branch_program, 0x3f800000u);
        AppendLe32(branch_program, 0xbf820002u);  // s_branch -> byte offset 28
        AppendLe32(branch_program, 0x7e0802ffu);  // alternate v_mov_b32 v4, literal
        AppendLe32(branch_program, 0x40000000u);
        AppendLe32(branch_program, 0xf8000001u);  // exp MRT0.X
        AppendLe32(branch_program, 0x00000004u);
        AppendLe32(branch_program, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(branch_program, decoded).ok());
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, options, ir).ok());
        SX_CHECK(ir.has_control_flow);
        SX_CHECK(ir.basic_blocks.size() == 4);
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("[WaveSize(64)]") != std::string::npos);
        SX_CHECK(hlsl.find("if (all(sx_vcc == uint2(0u, 0u)))") != std::string::npos);
        SX_CHECK(hlsl.find("sx_pc") == std::string::npos);
        SX_CHECK(hlsl.find("switch (") == std::string::npos);
        SX_CHECK(hlsl.find("if (") != std::string::npos);

        std::vector<uint8_t> loop_program;
        AppendLe32(loop_program, 0x7c040300u);  // v_cmp_eq_f32 vcc, v0, v1
        AppendLe32(loop_program, 0xbf860001u);  // exit loop when VCC is zero
        AppendLe32(loop_program, 0xbf82fffdu);  // s_branch -> byte offset 0
        AppendLe32(loop_program, 0xf800000fu);  // exp MRT0
        AppendLe32(loop_program, 0x03020100u);
        AppendLe32(loop_program, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(loop_program, decoded).ok());
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, options, ir).ok());
        SX_CHECK(ir.basic_blocks.size() == 3);
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("[loop] while (true)") != std::string::npos);
        SX_CHECK(hlsl.find("break;") != std::string::npos);
        SX_CHECK(hlsl.find("continue;") != std::string::npos);

        std::vector<uint8_t> multi_exit_loop;
        AppendLe32(multi_exit_loop, 0x7c040300u);  // v_cmp_eq_f32 vcc, v0, v1
        AppendLe32(multi_exit_loop, 0xbf860003u);  // exit A at byte offset 20
        AppendLe32(multi_exit_loop, 0x7c040300u);  // v_cmp_eq_f32 vcc, v0, v1
        AppendLe32(multi_exit_loop, 0xbf860002u);  // exit B at byte offset 24
        AppendLe32(multi_exit_loop, 0xbf82fffbu);  // s_branch -> byte offset 0
        AppendLe32(multi_exit_loop, 0xbf820001u);  // exit A -> common block
        AppendLe32(multi_exit_loop, 0xbf820000u);  // exit B -> common block
        AppendLe32(multi_exit_loop, 0xf800000fu);  // exp MRT0
        AppendLe32(multi_exit_loop, 0x03020100u);
        AppendLe32(multi_exit_loop, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(multi_exit_loop, decoded).ok());
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, options, ir).ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("sx_loop_exit_0 = 0u") != std::string::npos);
        SX_CHECK(hlsl.find("sx_loop_exit_0 = 1u") != std::string::npos);
        SX_CHECK(hlsl.find("sx_pc") == std::string::npos);

        std::vector<uint8_t> lds_program;
        AppendLe32(lds_program, 0xd8d80000u);  // ds_read_b32 v10, v0
        AppendLe32(lds_program, 0x0a000000u);
        AppendLe32(lds_program, 0xd8d80000u);  // ds_read_b32 v11, v3
        AppendLe32(lds_program, 0x0b000003u);
        AppendLe32(lds_program, 0xd8d80000u);  // ds_read_b32 v12, v6
        AppendLe32(lds_program, 0x0c000006u);
        AppendLe32(lds_program, 0xbf8a0000u);  // s_barrier
        AppendLe32(lds_program, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(lds_program, decoded).ok());
        SX_CHECK(decoded[0].mnemonic == "ds_read_b32");
        Rdna2TranslationOptions compute_options = options;
        compute_options.stage = ShaderStage::Compute;
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, compute_options, ir).ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("groupshared uint sx_lds[16384]") != std::string::npos);
        SX_CHECK(hlsl.find("GroupMemoryBarrierWithGroupSync()") != std::string::npos);
        SX_CHECK(hlsl.find("SV_DispatchThreadID") != std::string::npos);
        SX_CHECK(hlsl.find("SV_GroupThreadID") != std::string::npos);
        SX_CHECK(hlsl.find("SV_GroupID") != std::string::npos);
        SX_CHECK(hlsl.find("sx_v3 = asuint(group_thread_id.x)") != std::string::npos);
        SX_CHECK(hlsl.find("sx_v6 = asuint(group_id.x)") != std::string::npos);

        struct DsCase {
            uint32_t opcode;
            const char* mnemonic;
            bool store;
        };
        for (const DsCase ds_case : {
                 DsCase{0x36u, "ds_read_b32", false},
                 DsCase{0x76u, "ds_read_b64", false},
                 DsCase{0xffu, "ds_read_b128", false},
                 DsCase{0x0du, "ds_write_b32", true},
                 DsCase{0x4du, "ds_write_b64", true},
                 DsCase{0xdfu, "ds_write_b128", true},
             }) {
            std::vector<uint8_t> ds_program;
            AppendLe32(ds_program, 0xd8000000u | (ds_case.opcode << 18));
            AppendLe32(ds_program, ds_case.store ? 0x00000400u : 0x04000000u);
            AppendLe32(ds_program, 0xbf810000u);
            decoded.clear();
            SX_CHECK(DecodeRdna2Instructions(ds_program, decoded).ok());
            SX_CHECK(decoded[0].mnemonic == ds_case.mnemonic);
            ir = Rdna2IrModule{};
            SX_CHECK(BuildRdna2Ir(decoded, compute_options, ir).ok());
            hlsl.clear();
            SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
            SX_CHECK(hlsl.find("sx_lds[") != std::string::npos);
        }

        std::vector<uint8_t> ds_atomic_program;
        AppendLe32(ds_atomic_program, 0xd8000000u);  // ds_add_u32 vaddr=v0, vdata=v4
        AppendLe32(ds_atomic_program, 0x00000400u);
        AppendLe32(ds_atomic_program, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(ds_atomic_program, decoded).ok());
        SX_CHECK(decoded[0].mnemonic == "ds_add_u32");
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, compute_options, ir).ok());
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("InterlockedAdd(sx_lds[") != std::string::npos);

        for (const auto& atomic_case : {
                 std::pair<uint32_t, const char*>{0x32u, "InterlockedAdd"},
                 std::pair<uint32_t, const char*>{0x31u, "InterlockedCompareExchange"},
             }) {
            std::vector<uint8_t> atomic_program;
            AppendLe32(atomic_program, 0xe0000000u |
                                           (atomic_case.first << 18) |
                                           (1u << 14));  // glc return enabled
            AppendLe32(atomic_program, 0x7d000400u);      // vdata=v4, resource=s[0:3]
            AppendLe32(atomic_program, 0xbf810000u);
            decoded.clear();
            SX_CHECK(DecodeRdna2Instructions(atomic_program, decoded).ok());
            SX_CHECK(decoded[0].mnemonic ==
                     (atomic_case.first == 0x32u
                          ? "buffer_atomic_add"
                          : "buffer_atomic_cmpswap"));
            Rdna2TranslationOptions atomic_options = compute_options;
            atomic_options.root_cbv_count = 1;
            atomic_options.use_bindless_resources = true;
            ir = Rdna2IrModule{};
            SX_CHECK(BuildRdna2Ir(decoded, atomic_options, ir).ok());
            hlsl.clear();
            SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
            SX_CHECK(hlsl.find(atomic_case.second) != std::string::npos);
            SX_CHECK(hlsl.find(
                         "ResourceDescriptorHeap[NonUniformResourceIndex(sx_s0)]") !=
                     std::string::npos);
        }

        std::vector<uint8_t> wave64_program;
        AppendLe32(wave64_program, 0xbefe0400u);  // s_mov_b64 exec, s[0:1]
        AppendLe32(wave64_program, 0xbe822404u);  // s_and_saveexec_b64 s[2:3], s[4:5]
        AppendLe32(wave64_program, 0xf800000fu);
        AppendLe32(wave64_program, 0x03020100u);
        AppendLe32(wave64_program, 0xbf810000u);
        decoded.clear();
        SX_CHECK(DecodeRdna2Instructions(wave64_program, decoded).ok());
        Rdna2TranslationOptions wave_options = options;
        wave_options.root_cbv_count = 1;
        ir = Rdna2IrModule{};
        SX_CHECK(BuildRdna2Ir(decoded, wave_options, ir).ok());
        SX_CHECK(ir.requires_wave64);
        hlsl.clear();
        SX_CHECK(GenerateRdna2Hlsl(ir, hlsl).ok());
        SX_CHECK(hlsl.find("[WaveSize(64)]") != std::string::npos);
        SX_CHECK(hlsl.find("sx_exec = sx_mask_tmp0") != std::string::npos);
    }

    // 6. ShaderCache hit/miss tracking and corrupt-file recovery
    {
        const std::filesystem::path cache_dir =
            std::filesystem::temp_directory_path() / "scarlettx_test_shader_disk_cache";
        std::error_code ec;
        std::filesystem::remove_all(cache_dir, ec);
        ShaderCache cache(cache_dir);
        std::vector<uint8_t> out;
        SX_CHECK(!cache.Lookup(0x1234ull, out));
        SX_CHECK(cache.misses() == 1);
        cache.Store(0x1234ull, {1, 2, 3, 4});
        SX_CHECK(cache.Lookup(0x1234ull, out));
        SX_CHECK(cache.hits() == 1);
        SX_CHECK(out.size() == 4 && out[3] == 4);

        {
            ShaderCache disk_cache(cache_dir);
            disk_cache.Store(0xCAFEull, {5, 6, 7, 8});
        }
        ShaderCache reopened(cache_dir);
        SX_CHECK(reopened.Lookup(0xCAFEull, out));
        SX_CHECK(out.size() == 4 && out[0] == 5 && out[3] == 8);
        reopened.Clear();
        SX_CHECK(!reopened.Lookup(0xCAFEull, out));

        const auto cache_file = cache_dir / "sx000000000000cafe.sxbc";
        {
            std::ofstream corrupt(cache_file, std::ios::binary | std::ios::trunc);
            corrupt.put('X');
        }
        ShaderCache corrupted(cache_dir);
        SX_CHECK(!corrupted.Lookup(0xCAFEull, out));
        SX_CHECK(corrupted.misses() == 1);
        corrupted.Store(0xCAFEull, {9, 8, 7});
        ShaderCache recovered(cache_dir);
        SX_CHECK(recovered.Lookup(0xCAFEull, out));
        SX_CHECK(out.size() == 3 && out[0] == 9 && out[2] == 7);
        std::filesystem::remove_all(cache_dir, ec);
    }

    // 6. Live D3D12 WARP shader compile + root signature + descriptor heap + graphics/compute/DXR PSO + cache
#if SCARLETTX_HAS_DXC
    {
        const std::string source =
            "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { }";
        std::vector<uint8_t> dxil;
        Status compile_status = CompileHlslToDxil(source, "main", "cs_6_6", dxil);
        if (compile_status.code == StatusCode::NotFound) {
            // DXC is compiled in (SCARLETTX_HAS_DXC=1) but no dxcompiler.dll
            // was reachable through the cascade. A green suite here would hide
            // a dead SM 6.6 path behind zero executed assertions, so fail
            // loudly instead of skipping.
            std::cerr << "[test_shader_pipeline] DXC is compiled in but the runtime "
                         "is not loadable; SM 6.6 compilation is NOT verified: "
                      << compile_status.message << "\n";
            SX_CHECK(false);
        }
        SX_CHECK(compile_status.ok());
        SX_CHECK(!dxil.empty());
        ShaderModuleInfo info{};
        SX_CHECK(InspectShaderContainer(dxil, info).ok());
        SX_CHECK(info.kind == ShaderContainerKind::Dxil);
        SX_CHECK(ValidateDxilContainer(dxil).ok());

        const std::string half_source =
            "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { "
            "half2 value = half2(0.5h, 1.0h); value = mad(value, value, value); }";
        std::vector<uint8_t> half_dxil;
        Status half_compile_status = CompileHlslToDxil(
            half_source, "main", "cs_6_6", half_dxil, true);
        SX_CHECK(half_compile_status.ok());
        SX_CHECK(!half_dxil.empty());
    }
#endif

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.PreferWarp = 1;
    uint64_t dev = 0;
    SX_CHECK(D3D12XboxCreateSyntheticDevice(&dev_params, &dev) == 0);
    SX_CHECK(dev != 0);

    // Descriptor heaps
    D3D12XBOX_DESCRIPTOR_HEAP_DESC cbv_heap_desc{};
    cbv_heap_desc.Type = D3D12XBOX_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_heap_desc.NumDescriptors = 16;
    cbv_heap_desc.ShaderVisible = 1;
    uint64_t cbv_heap = 0;
    SX_CHECK(D3D12XboxCreateDescriptorHeap(dev, &cbv_heap_desc, &cbv_heap) == 0);
    SX_CHECK(cbv_heap != 0);
    SX_CHECK(D3D12XboxGetDescriptorHandleIncrementSize(
                 dev, D3D12XBOX_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) > 0);

    // Root signatures (graphics: empty; compute: 2 root constants + 1 UAV)
    D3D12XBOX_ROOT_SIGNATURE_DESC gfx_rs_desc{};
    uint64_t gfx_rs = 0;
    SX_CHECK(D3D12XboxCreateRootSignature(dev, &gfx_rs_desc, &gfx_rs) == 0);

    D3D12XBOX_ROOT_SIGNATURE_DESC cs_rs_desc{};
    cs_rs_desc.NumRootConstants32Bit = 2;
    cs_rs_desc.NumUavs = 1;
    uint64_t cs_rs = 0;
    SX_CHECK(D3D12XboxCreateRootSignature(dev, &cs_rs_desc, &cs_rs) == 0);

    D3D12XBOX_ROOT_SIGNATURE_DESC_EX sampler_rs_desc{};
    sampler_rs_desc.NumSamplers = 1;
    uint64_t sampler_rs = 0;
    SX_CHECK(D3D12XboxCreateRootSignatureEx(dev, &sampler_rs_desc, &sampler_rs) == 0);
    SX_CHECK(D3D12XboxDestroyRootSignature(dev, sampler_rs) == 0);

    // Compile real HLSL shaders to DXBC
    const std::string vs_src = R"(
        struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR0; };
        VSOut main(uint vid : SV_VertexID) {
            float2 p[3] = { float2(0.0, 0.7), float2(0.7, -0.7), float2(-0.7, -0.7) };
            float4 c[3] = { float4(0.8, 0.4, 1.0, 1.0), float4(0.4, 0.2, 0.9, 1.0), float4(0.2, 0.8, 0.9, 1.0) };
            VSOut o;
            o.pos = float4(p[vid], 0.0, 1.0);
            o.col = c[vid];
            return o;
        }
    )";
    const std::string ps_src = R"(
        struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR0; };
        float4 main(PSIn i) : SV_TARGET { return i.col; }
    )";
    const std::string cs_src = R"(
        cbuffer Params : register(b0) { uint multiplier; uint bias; };
        RWByteAddressBuffer OutBuf : register(u0);
        [numthreads(16, 1, 1)]
        void main(uint3 tid : SV_DispatchThreadID) {
            uint val = tid.x * multiplier + bias;
            OutBuf.Store(tid.x * 4, val);
        }
    )";

    std::vector<uint8_t> vs_dxbc, ps_dxbc, cs_dxbc;
    SX_CHECK(CompileHlslToDxbc(vs_src, "main", "vs_5_0", vs_dxbc).ok());
    SX_CHECK(CompileHlslToDxbc(ps_src, "main", "ps_5_0", ps_dxbc).ok());
    SX_CHECK(CompileHlslToDxbc(cs_src, "main", "cs_5_0", cs_dxbc).ok());

    // Inspect compiled shaders through the d3d12_xs C ABI
    D3D12XBOX_SHADER_BYTECODE cs_bc{cs_dxbc.data(), cs_dxbc.size()};
    D3D12XBOX_SHADER_INSPECTION cs_insp{};
    SX_CHECK(D3D12XboxInspectShader(&cs_bc, &cs_insp) == 0);
    SX_CHECK(cs_insp.ContainerKind == static_cast<uint32_t>(ShaderContainerKind::Dxbc));
    SX_CHECK(cs_insp.Stage == static_cast<uint32_t>(ShaderStage::Compute));
    SX_CHECK(cs_insp.ShaderModelMajor == 5);

    // Create Graphics PSO twice to exercise PSO cache hit
    D3D12XBOX_GRAPHICS_PSO_DESC gfx_pso_desc{};
    gfx_pso_desc.RootSignatureHandle = gfx_rs;
    gfx_pso_desc.VS = {vs_dxbc.data(), vs_dxbc.size()};
    gfx_pso_desc.PS = {ps_dxbc.data(), ps_dxbc.size()};
    gfx_pso_desc.RtvFormat = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    uint64_t gfx_pso1 = 0, gfx_pso2 = 0;
    SX_CHECK(D3D12XboxCreateGraphicsPipelineState(dev, &gfx_pso_desc, &gfx_pso1) == 0);
    SX_CHECK(D3D12XboxCreateGraphicsPipelineState(dev, &gfx_pso_desc, &gfx_pso2) == 0);
    SX_CHECK(gfx_pso1 != 0 && gfx_pso2 != 0 && gfx_pso1 != gfx_pso2);

    // Render using the custom Graphics PSO and verify deterministic pixel hash
    const std::string ppm_path = std::string(SCARLETTX_BINARY_DIR) + "/shader_pso_frame.ppm";
    uint64_t hash1 = 0, hash2 = 0;
    SX_CHECK(D3D12XboxRenderWithPipelineState(dev, gfx_pso1, 128, 128, 3, ppm_path.c_str(), &hash1) == 0);
    SX_CHECK(D3D12XboxRenderWithPipelineState(dev, gfx_pso2, 128, 128, 3, nullptr, &hash2) == 0);
    SX_CHECK(hash1 != 0);
    SX_CHECK(hash1 == hash2);
    SX_CHECK(std::filesystem::exists(ppm_path));

    // Create Compute PSO & dispatch on WARP, then verify UAV readback values!
    D3D12XBOX_COMPUTE_PSO_DESC cs_pso_desc{};
    cs_pso_desc.RootSignatureHandle = cs_rs;
    cs_pso_desc.CS = cs_bc;
    uint64_t cs_pso = 0;
    SX_CHECK(D3D12XboxCreateComputePipelineState(dev, &cs_pso_desc, &cs_pso) == 0);

    const uint32_t root_consts[2] = {7u, 100u};
    uint32_t uav_out[16] = {};
    SX_CHECK(D3D12XboxDispatchCompute(
                 dev, cs_pso, 1, 1, 1, root_consts, 2, uav_out, sizeof(uav_out)) == 0);
    for (uint32_t i = 0; i < 16; ++i) {
        SX_CHECK(uav_out[i] == i * 7u + 100u);
    }

    // Raytracing PSO from Xbox DXIL library (auto-strips Xbox-private chunks)
    std::vector<uint8_t> rt_dxil_payload(24, 0);
    const uint32_t rt_ver_token = (7u << 16) | (6u << 4) | 5u;  // lib_6_5
    std::memcpy(rt_dxil_payload.data(), &rt_ver_token, 4);
    auto rt_xbox_dxil = BuildSyntheticDxbc({
        {"DXIL", rt_dxil_payload},
        {"RDAT", {1, 2, 3, 4}},
        {"XBOX", {0xAA, 0xBB, 0xCC, 0xDD}},
    });
    D3D12XBOX_RAYTRACING_PSO_DESC rt_pso_desc{};
    rt_pso_desc.GlobalRootSignatureHandle = gfx_rs;
    rt_pso_desc.DxilLibrary.Bytecode = rt_xbox_dxil.data();
    rt_pso_desc.DxilLibrary.BytecodeLength = rt_xbox_dxil.size();
    rt_pso_desc.MaxPayloadSizeInBytes = 32;
    rt_pso_desc.MaxAttributeSizeInBytes = 8;
    rt_pso_desc.MaxTraceRecursionDepth = 2;
    uint64_t rt_pso = 0;
    SX_CHECK(D3D12XboxCreateRaytracingPipelineState(dev, &rt_pso_desc, &rt_pso) == 0);
    SX_CHECK(rt_pso != 0);

    // Check pipeline cache stats (1 graphics hit + 3 misses: 1 gfx + 1 cs + 1 rt)
    D3D12XBOX_PIPELINE_CACHE_STATS stats{};
    SX_CHECK(D3D12XboxGetPipelineCacheStats(dev, &stats) == 0);
    SX_CHECK(stats.CacheHits == 1);
    SX_CHECK(stats.CacheMisses == 3);
    SX_CHECK(stats.LiveRootSignatures == 2);
    SX_CHECK(stats.LiveDescriptorHeaps == 1);
    SX_CHECK(stats.LiveGraphicsPsos == 2);
    SX_CHECK(stats.LiveComputePsos == 1);
    SX_CHECK(stats.LiveRaytracingPsos == 1);

    SX_CHECK(D3D12XboxDestroyPipelineState(dev, gfx_pso1) == 0);
    SX_CHECK(D3D12XboxDestroyPipelineState(dev, gfx_pso2) == 0);
    SX_CHECK(D3D12XboxDestroyPipelineState(dev, cs_pso) == 0);
    SX_CHECK(D3D12XboxDestroyPipelineState(dev, rt_pso) == 0);
    SX_CHECK(D3D12XboxDestroyDescriptorHeap(dev, cbv_heap) == 0);
    SX_CHECK(D3D12XboxDestroyRootSignature(dev, gfx_rs) == 0);
    SX_CHECK(D3D12XboxDestroyRootSignature(dev, cs_rs) == 0);
    SX_CHECK(D3D12XboxDestroyDevice(dev) == 0);

    std::cout << "[PASS] test_shader_pipeline (DXIL/Xbox strip + WARP graphics PSO + compute UAV readback)\n";
    return 0;
}
