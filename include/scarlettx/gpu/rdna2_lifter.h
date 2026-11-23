#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/gpu/shader_ir.h"

namespace scarlettx {

enum class Rdna2Encoding : uint8_t {
    Unknown,
    Sop2,
    Sop1,
    Sopk,
    Sopc,
    Sopp,
    Smem,
    Vop2,
    Vop1,
    Vopc,
    Vop3,
    Vinterp,
    Ds,
    Mubuf,
    Mtbuf,
    Mimg,
    Flat,
    Exp,
};

enum class Rdna2RegisterBank : uint8_t {
    Scalar,
    Vector,
    Vcc,
    Exec,
    Scc,
    M0,
    Null,
    Immediate,
    FloatImmediate,
    Literal,
    Unknown,
};

struct Rdna2Operand {
    Rdna2RegisterBank bank = Rdna2RegisterBank::Unknown;
    uint16_t index = 0;
    uint32_t bits = 0;
};

struct Rdna2Instruction {
    size_t byte_offset = 0;
    Rdna2Encoding encoding = Rdna2Encoding::Unknown;
    uint32_t opcode = 0;
    std::string mnemonic;
    std::vector<Rdna2Operand> destinations;
    std::vector<Rdna2Operand> sources;
    int32_t immediate = 0;
    uint32_t raw_dword0 = 0;
    uint32_t raw_dword1 = 0;
    uint16_t resource_sgpr = 0;
    uint16_t sampler_sgpr = 0;
    uint16_t ds_offset0 = 0;
    uint16_t ds_offset1 = 0;
    uint8_t texture_dimension = 0;
    uint8_t interpolation_attribute = 0;
    uint8_t interpolation_channel = 0;
    uint8_t component_mask = 0;
    uint8_t nsa_dwords = 0;
    uint8_t scalar_offset_code = 125;
    bool offen = false;
    bool idxen = false;
    bool glc = false;
};

enum class Rdna2IrOpcode : uint8_t {
    InputRegister,
    Constant,
    Move,
    AddF32,
    SubF32,
    MulF32,
    FmaF32,
    AddU32,
    SubU32,
    MulU32,
    AndU32,
    OrU32,
    XorU32,
    BitfieldExtractU32,
    BitfieldInsertU32,
    AlignBitU32,
    Min3F32,
    Max3F32,
    Med3F32,
    PixelInterpolate,
    CompareEqF32,
    CompareLtF32,
    ConditionalSelect,
    BufferLoad,
    BufferStore,
    TextureLoadComponent,
    TextureSampleComponent,
    Barrier,
    Wait,
    MeshAllocate,
    Export,
    End,
};

enum class Rdna2TextureSampleMode : uint8_t {
    ImplicitLod,
    ExplicitLod,
    Bias,
    Gradients,
};

struct Rdna2IrInstruction {
    Rdna2IrOpcode opcode = Rdna2IrOpcode::End;
    uint32_t result = 0xffffffffu;
    std::vector<uint32_t> operands;
    Rdna2Operand input_register{};
    uint32_t immediate_bits = 0;
    uint32_t resource_slot = 0;
    uint32_t sampler_slot = 0;
    int32_t byte_offset = 0;
    uint8_t component = 0;
    uint8_t component_mask = 0;
    Rdna2TextureSampleMode texture_sample_mode = Rdna2TextureSampleMode::ImplicitLod;
    uint8_t export_target = 0;
    uint8_t interpolation_attribute = 0;
    uint8_t interpolation_channel = 0;
    bool interpolation_flat = false;
};

struct Rdna2BasicBlock {
    uint32_t byte_offset = 0;
    size_t first_instruction = 0;
    size_t instruction_count = 0;
    uint32_t branch_target = 0xffffffffu;
    uint32_t fallthrough_target = 0xffffffffu;
    bool conditional_branch = false;
    bool unconditional_branch = false;
    bool terminates = false;
};

struct Rdna2IrModule {
    ShaderStage stage = ShaderStage::Unknown;
    uint32_t root_cbv_count = 0;
    uint32_t root_srv_count = 0;
    uint32_t root_sampler_count = 0;
    uint32_t root_uav_count = 0;
    uint32_t first_cbv_register = 0;
    uint32_t thread_group_size_x = 1;
    uint32_t thread_group_size_y = 1;
    uint32_t thread_group_size_z = 1;
    uint32_t mesh_max_vertices = 128;
    uint32_t mesh_max_primitives = 128;
    std::vector<Rdna2IrInstruction> instructions;
    std::vector<Rdna2Instruction> decoded_program;
    std::vector<Rdna2BasicBlock> basic_blocks;
    std::vector<uint32_t> exports;
    uint8_t export_target = 0xff;
    uint32_t max_input_vgpr = 0;
    bool use_bindless_resources = false;
    bool use_bindless_samplers = false;
    bool has_input_vgpr = false;
    bool uses_scalar_input = false;
    bool uses_texture = false;
    bool has_control_flow = false;
    bool requires_wave64 = false;
};

struct Rdna2TranslationOptions {
    ShaderStage stage = ShaderStage::Unknown;
    uint32_t root_cbv_count = 0;
    uint32_t root_srv_count = 0;
    uint32_t root_sampler_count = 0;
    uint32_t root_uav_count = 0;
    uint32_t first_cbv_register = 0;
    uint32_t thread_group_size_x = 1;
    uint32_t thread_group_size_y = 1;
    uint32_t thread_group_size_z = 1;
    uint32_t mesh_max_vertices = 128;
    uint32_t mesh_max_primitives = 128;
    bool use_bindless_resources = false;
    bool use_bindless_samplers = false;
};

Status DecodeRdna2Instructions(
    const std::vector<uint8_t>& program,
    std::vector<Rdna2Instruction>& out_instructions);

Status BuildRdna2Ir(
    const std::vector<Rdna2Instruction>& instructions,
    const Rdna2TranslationOptions& options,
    Rdna2IrModule& out_module);

Status GenerateRdna2Hlsl(const Rdna2IrModule& module, std::string& out_hlsl);

Status TranslateRdna2ToDxil(
    const std::vector<uint8_t>& program,
    const Rdna2TranslationOptions& options,
    std::vector<uint8_t>& out_dxil,
    std::string* out_hlsl = nullptr);

Status TranslateRdna2ToHlsl(
    const std::vector<uint8_t>& program,
    const Rdna2TranslationOptions& options,
    std::string& out_hlsl,
    bool* out_requires_wave64 = nullptr);

}  // namespace scarlettx
