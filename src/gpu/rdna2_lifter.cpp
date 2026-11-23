#include "scarlettx/gpu/rdna2_lifter.h"

#include "scarlettx/gpu/dxil_backend.h"
#include "core/logger.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace scarlettx {
namespace {

constexpr uint32_t kNoValue = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kVop3Prefix = 0b110101;
constexpr uint32_t kVop3pPrefix = 0b110011;
constexpr uint32_t kInlineLiteral = 255;
constexpr uint32_t kSdwaSource = 249;

uint32_t ReadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

Rdna2Encoding Classify(uint32_t word) {
    if ((word >> 23) == 0x17f) return Rdna2Encoding::Sopp;
    if ((word >> 23) == 0x17d) return Rdna2Encoding::Sop1;
    if ((word >> 23) == 0x17e) return Rdna2Encoding::Sopc;
    if ((word >> 28) == 0xbu) return Rdna2Encoding::Sopk;
    if ((word >> 30) == 0x2u) return Rdna2Encoding::Sop2;
    if ((word >> 25) == 0x3fu) return Rdna2Encoding::Vop1;
    if ((word >> 25) == 0x3eu) return Rdna2Encoding::Vopc;
    if ((word >> 31) == 0 && ((word >> 25) & 0x3fu) < 0x3eu) {
        return Rdna2Encoding::Vop2;
    }

    switch (word >> 26) {
        case kVop3Prefix:
        case kVop3pPrefix: return Rdna2Encoding::Vop3;
        case 0b110010: return Rdna2Encoding::Vinterp;
        case 0b110110: return Rdna2Encoding::Ds;
        case 0b111000: return Rdna2Encoding::Mubuf;
        case 0b111010: return Rdna2Encoding::Mtbuf;
        case 0b111100: return Rdna2Encoding::Mimg;
        case 0b111101: return Rdna2Encoding::Smem;
        case 0b110111: return Rdna2Encoding::Flat;
        case 0b111110: return Rdna2Encoding::Exp;
        default: return Rdna2Encoding::Unknown;
    }
}

uint32_t OpcodeFor(Rdna2Encoding encoding, uint32_t word) {
    switch (encoding) {
        case Rdna2Encoding::Sop2: return (word >> 23) & 0x7fu;
        case Rdna2Encoding::Sop1: return (word >> 8) & 0xffu;
        case Rdna2Encoding::Sopk: return (word >> 23) & 0x1fu;
        case Rdna2Encoding::Sopc: return (word >> 16) & 0x7fu;
        case Rdna2Encoding::Sopp: return (word >> 16) & 0x7fu;
        case Rdna2Encoding::Smem: return (word >> 18) & 0xffu;
        case Rdna2Encoding::Vop2: return (word >> 25) & 0x3fu;
        case Rdna2Encoding::Vop1: return (word >> 9) & 0xffu;
        case Rdna2Encoding::Vopc: return (word >> 17) & 0xffu;
        case Rdna2Encoding::Vop3:
            return (word >> 26) == kVop3pPrefix
                       ? (0x400u | ((word >> 16) & 0x7fu))
                       : ((word >> 16) & 0x3ffu);
        case Rdna2Encoding::Vinterp: return (word >> 16) & 0x3u;
        case Rdna2Encoding::Mubuf: return (word >> 18) & 0xffu;
        case Rdna2Encoding::Mtbuf: return (word >> 16) & 0x7u;
        case Rdna2Encoding::Mimg: return ((word >> 18) & 0x7fu) | ((word & 1u) << 7);
        default: return (word >> 18) & 0xffu;
    }
}

std::string MnemonicFor(Rdna2Encoding encoding, uint32_t opcode) {
    switch (encoding) {
        case Rdna2Encoding::Sop2:
            switch (opcode) {
                case 0x00: return "s_add_u32";
                case 0x01: return "s_sub_u32";
                case 0x0a: return "s_cselect_b32";
                case 0x0b: return "s_cselect_b64";
                case 0x0e: return "s_and_b32";
                case 0x0f: return "s_and_b64";
                case 0x10: return "s_or_b32";
                case 0x11: return "s_or_b64";
                case 0x12: return "s_xor_b32";
                case 0x13: return "s_xor_b64";
                case 0x15: return "s_andn2_b64";
                case 0x17: return "s_orn2_b64";
                default: break;
            }
            break;
        case Rdna2Encoding::Sop1:
            if (opcode == 0x03) return "s_mov_b32";
            if (opcode == 0x04) return "s_mov_b64";
            if (opcode == 0x08) return "s_not_b64";
            if (opcode == 0x24) return "s_and_saveexec_b64";
            if (opcode == 0x25) return "s_or_saveexec_b64";
            if (opcode == 0x26) return "s_xor_saveexec_b64";
            if (opcode == 0x27) return "s_andn2_saveexec_b64";
            break;
        case Rdna2Encoding::Sopk:
            if (opcode == 0x00) return "s_movk_i32";
            if (opcode == 0x0e) return "s_addk_i32";
            if (opcode == 0x0f) return "s_mulk_i32";
            break;
        case Rdna2Encoding::Sopp:
            switch (opcode) {
                case 0x00: return "s_nop";
                case 0x01: return "s_endpgm";
                case 0x02: return "s_branch";
                case 0x04: return "s_cbranch_scc0";
                case 0x05: return "s_cbranch_scc1";
                case 0x06: return "s_cbranch_vccz";
                case 0x07: return "s_cbranch_vccnz";
                case 0x08: return "s_cbranch_execz";
                case 0x09: return "s_cbranch_execnz";
                case 0x0a: return "s_barrier";
                case 0x0c: return "s_waitcnt";
                case 0x18: return "s_sendmsg";
                default: break;
            }
            break;
        case Rdna2Encoding::Ds:
            switch (opcode) {
                case 0x36: return "ds_read_b32";
                case 0x76: return "ds_read_b64";
                case 0xff: return "ds_read_b128";
                case 0x0d: return "ds_write_b32";
                case 0x4d: return "ds_write_b64";
                case 0xdf: return "ds_write_b128";
                case 0x00: return "ds_add_u32";
                default: break;
            }
            break;
        case Rdna2Encoding::Sopc:
            switch (opcode) {
                case 0x00: return "s_cmp_eq_i32";
                case 0x01: return "s_cmp_lg_i32";
                case 0x04: return "s_cmp_lt_i32";
                case 0x06: return "s_cmp_eq_u32";
                case 0x07: return "s_cmp_lg_u32";
                case 0x0a: return "s_cmp_lt_u32";
                default: break;
            }
            break;
        case Rdna2Encoding::Vop2:
            switch (opcode) {
                case 0x01: return "v_cndmask_b32";
                case 0x03: return "v_add_f32";
                case 0x04: return "v_sub_f32";
                case 0x08: return "v_mul_f32";
                case 0x25: return "v_add_nc_u32";
                case 0x26: return "v_sub_nc_u32";
                case 0x1b: return "v_and_b32";
                case 0x1c: return "v_or_b32";
                case 0x1d: return "v_xor_b32";
                default: break;
            }
            break;
        case Rdna2Encoding::Vop1:
            if (opcode == 0x01) return "v_mov_b32";
            if (opcode == 0x0a) return "v_cvt_f16_f32";
            if (opcode == 0x0b) return "v_cvt_f32_f16";
            break;
        case Rdna2Encoding::Vopc:
            switch (opcode) {
                case 0x01: return "v_cmp_lt_f32";
                case 0x02: return "v_cmp_eq_f32";
                case 0x03: return "v_cmp_le_f32";
                case 0x04: return "v_cmp_gt_f32";
                case 0x05: return "v_cmp_lg_f32";
                case 0x06: return "v_cmp_ge_f32";
                case 0x21: return "v_cmp_lt_i32";
                case 0x22: return "v_cmp_eq_i32";
                case 0x23: return "v_cmp_le_i32";
                case 0x24: return "v_cmp_gt_i32";
                case 0x25: return "v_cmp_ne_i32";
                case 0x26: return "v_cmp_ge_i32";
                case 0x29: return "v_cmp_lt_u32";
                case 0x2a: return "v_cmp_eq_u32";
                case 0x2b: return "v_cmp_le_u32";
                case 0x2c: return "v_cmp_gt_u32";
                case 0x2d: return "v_cmp_ne_u32";
                case 0x2e: return "v_cmp_ge_u32";
                default: break;
            }
            break;
        case Rdna2Encoding::Vop3:
            if (opcode == 0x148) return "v_bfe_u32";
            if (opcode == 0x14a) return "v_bfi_b32";
            if (opcode == 0x14b) return "v_fma_f32";
            if (opcode == 0x14e) return "v_alignbit_b32";
            if (opcode == 0x151) return "v_min3_f32";
            if (opcode == 0x154) return "v_max3_f32";
            if (opcode == 0x157) return "v_med3_f32";
            if (opcode == 0x40e) return "v_pk_fma_f16";
            if (opcode == 0x40f) return "v_pk_add_f16";
            if (opcode == 0x410) return "v_pk_mul_f16";
            break;
        case Rdna2Encoding::Vinterp:
            if (opcode == 0) return "v_interp_p1_f32";
            if (opcode == 1) return "v_interp_p2_f32";
            if (opcode == 2 || opcode == 3) return "v_interp_mov_f32";
            break;
        case Rdna2Encoding::Mubuf:
            if (opcode == 0x0c) return "buffer_load_dword";
            if (opcode == 0x1c) return "buffer_store_dword";
            if (opcode == 0x31) return "buffer_atomic_cmpswap";
            if (opcode == 0x32) return "buffer_atomic_add";
            break;
        case Rdna2Encoding::Mimg:
            if (opcode == 0x00) return "image_load";
            if (opcode == 0x01) return "image_load_mip";
            if (opcode == 0x20) return "image_sample";
            if (opcode == 0x22) return "image_sample_d";
            if (opcode == 0x24) return "image_sample_l";
            if (opcode == 0x25) return "image_sample_b";
            if (opcode == 0xe6) return "image_bvh_intersect_ray";
            if (opcode == 0xe7) return "image_bvh64_intersect_ray";
            if (opcode >= 0x21 && opcode < 0x40) return "unsupported_image_sample_variant";
            if (opcode >= 0x40 && opcode < 0x60) return "image_gather4";
            break;
        case Rdna2Encoding::Exp: return "exp";
        default: break;
    }

    std::ostringstream name;
    name << "unknown_" << static_cast<unsigned>(encoding) << "_0x"
         << std::hex << opcode;
    return name.str();
}

bool HasTrailingLiteral(Rdna2Encoding encoding, uint32_t word, uint32_t next) {
    switch (encoding) {
        case Rdna2Encoding::Sop2:
            return (word & 0xffu) == kInlineLiteral || ((word >> 8) & 0xffu) == kInlineLiteral;
        case Rdna2Encoding::Sop1:
            return (word & 0xffu) == kInlineLiteral;
        case Rdna2Encoding::Sopc:
            return (word & 0xffu) == kInlineLiteral || ((word >> 8) & 0xffu) == kInlineLiteral;
        case Rdna2Encoding::Sopk:
            return ((word >> 23) & 0x1fu) == 0x15;
        case Rdna2Encoding::Vop2: {
            const uint32_t src0 = word & 0x1ffu;
            const uint32_t opcode = (word >> 25) & 0x3fu;
            return src0 == kInlineLiteral || src0 == kSdwaSource ||
                   src0 == 233 || src0 == 234 || src0 == 250 ||
                   opcode == 0x20 || opcode == 0x21 || opcode == 0x2c ||
                   opcode == 0x2d || opcode == 0x37 || opcode == 0x38;
        }
        case Rdna2Encoding::Vop1:
        case Rdna2Encoding::Vopc: {
            const uint32_t src0 = word & 0x1ffu;
            return src0 == kInlineLiteral || src0 == kSdwaSource ||
                   src0 == 233 || src0 == 234 || src0 == 250;
        }
        case Rdna2Encoding::Vop3: {
            const uint32_t src0 = next & 0x1ffu;
            const uint32_t src1 = (next >> 9) & 0x1ffu;
            const uint32_t src2 = (next >> 18) & 0x1ffu;
            return src0 == kInlineLiteral || src1 == kInlineLiteral ||
                   src2 == kInlineLiteral;
        }
        default: return false;
    }
}

Rdna2Operand ScalarSource(uint32_t code, uint32_t literal) {
    Rdna2Operand operand{};
    if (code <= 105) {
        operand.bank = Rdna2RegisterBank::Scalar;
        operand.index = static_cast<uint16_t>(code);
    } else if (code == 106 || code == 107) {
        operand.bank = Rdna2RegisterBank::Vcc;
        operand.index = static_cast<uint16_t>(code - 106);
    } else if (code == 124) {
        operand.bank = Rdna2RegisterBank::M0;
    } else if (code == 125) {
        operand.bank = Rdna2RegisterBank::Null;
    } else if (code == 126 || code == 127) {
        operand.bank = Rdna2RegisterBank::Exec;
        operand.index = static_cast<uint16_t>(code - 126);
    } else if (code == 128) {
        operand.bank = Rdna2RegisterBank::Immediate;
        operand.bits = 0;
    } else if (code <= 192 && code >= 129) {
        operand.bank = Rdna2RegisterBank::Immediate;
        operand.bits = code - 128;
    } else if (code <= 208 && code >= 193) {
        operand.bank = Rdna2RegisterBank::Immediate;
        operand.bits = static_cast<uint32_t>(-static_cast<int32_t>(code - 192));
    } else if (code >= 240 && code <= 248) {
        constexpr std::array<uint32_t, 9> kFloatConstants = {
            0x3f000000u, 0xbf000000u, 0x3f800000u, 0xbf800000u,
            0x40000000u, 0xc0000000u, 0x40800000u, 0xc0800000u,
            0x3e22f983u,
        };
        operand.bank = Rdna2RegisterBank::FloatImmediate;
        operand.bits = kFloatConstants[code - 240];
    } else if (code == 253) {
        operand.bank = Rdna2RegisterBank::Scc;
    } else if (code == 255) {
        operand.bank = Rdna2RegisterBank::Literal;
        operand.bits = literal;
    } else {
        operand.bank = Rdna2RegisterBank::Unknown;
        operand.index = static_cast<uint16_t>(code);
    }
    return operand;
}

Rdna2Operand VectorSource(uint32_t code, uint32_t literal) {
    if (code >= 256 && code <= 511) {
        return {Rdna2RegisterBank::Vector, static_cast<uint16_t>(code - 256), 0};
    }
    return ScalarSource(code, literal);
}

uint32_t RegisterKey(const Rdna2Operand& operand) {
    return (static_cast<uint32_t>(operand.bank) << 16) | operand.index;
}

std::string ValueName(uint32_t id) {
    return "sx_v" + std::to_string(id);
}

Status UnsupportedInstruction(const Rdna2Instruction& instruction) {
    std::ostringstream message;
    message << "RDNA2 lifting does not support " << instruction.mnemonic
            << " at byte offset 0x" << std::hex << instruction.byte_offset;
    return Status::Error(StatusCode::UnsupportedFormat, message.str());
}

bool IsConditionalBranch(const std::string& mnemonic) {
    return mnemonic == "s_cbranch_scc0" || mnemonic == "s_cbranch_scc1" ||
           mnemonic == "s_cbranch_vccz" || mnemonic == "s_cbranch_vccnz" ||
           mnemonic == "s_cbranch_execz" || mnemonic == "s_cbranch_execnz";
}

bool IsBranch(const std::string& mnemonic) {
    return mnemonic == "s_branch" || IsConditionalBranch(mnemonic);
}

bool IsNativeF16Instruction(const std::string& mnemonic) {
    return mnemonic == "v_cvt_f16_f32" ||
           mnemonic == "v_cvt_f32_f16" ||
           mnemonic == "v_pk_add_f16" ||
           mnemonic == "v_pk_mul_f16" ||
           mnemonic == "v_pk_fma_f16";
}

Status BuildControlFlow(
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2IrModule& module) {
    if (instructions.empty()) {
        return Status::Error(StatusCode::ShaderError, "RDNA2 control-flow program is empty");
    }

    std::unordered_map<uint32_t, size_t> instruction_at_offset;
    for (size_t i = 0; i < instructions.size(); ++i) {
        instruction_at_offset.emplace(
            static_cast<uint32_t>(instructions[i].byte_offset), i);
    }

    std::set<size_t> leaders = {0};
    std::unordered_map<size_t, uint32_t> branch_target_offsets;
    bool has_end = false;
    bool has_conditional_branch = false;
    bool has_group_barrier = false;
    for (size_t i = 0; i < instructions.size(); ++i) {
        const Rdna2Instruction& instruction = instructions[i];
        has_conditional_branch |= IsConditionalBranch(instruction.mnemonic);
        has_group_barrier |= instruction.mnemonic == "s_barrier";
        if (instruction.mnemonic.find("_b64") != std::string::npos ||
            instruction.mnemonic.find("_saveexec_b64") != std::string::npos) {
            for (const Rdna2Operand& operand : instruction.sources) {
                if (operand.bank == Rdna2RegisterBank::Scalar && operand.index >= 105) {
                    return Status::Error(
                        StatusCode::UnsupportedFormat,
                        "64-bit scalar source pair exceeds the RDNA2 SGPR range");
                }
            }
            for (const Rdna2Operand& operand : instruction.destinations) {
                if (operand.bank == Rdna2RegisterBank::Scalar && operand.index >= 105) {
                    return Status::Error(
                        StatusCode::UnsupportedFormat,
                        "64-bit scalar destination pair exceeds the RDNA2 SGPR range");
                }
            }
        }
        if (instruction.mnemonic == "s_endpgm") {
            has_end = true;
            if (i + 1 < instructions.size()) leaders.insert(i + 1);
        }
        if (!IsBranch(instruction.mnemonic)) continue;
        if (i + 1 < instructions.size()) leaders.insert(i + 1);

        const int64_t next_offset = static_cast<int64_t>(instruction.byte_offset) + 4;
        const int64_t target =
            next_offset + static_cast<int64_t>(instruction.immediate) * 4;
        if (target < 0 || target > std::numeric_limits<uint32_t>::max()) {
            return Status::Error(StatusCode::ShaderError, "RDNA2 branch target is out of range");
        }
        const auto target_instruction =
            instruction_at_offset.find(static_cast<uint32_t>(target));
        if (target_instruction == instruction_at_offset.end()) {
            return Status::Error(
                StatusCode::ShaderError,
                "RDNA2 branch target is not an instruction boundary");
        }
        leaders.insert(target_instruction->second);
        branch_target_offsets.emplace(i, static_cast<uint32_t>(target));
    }
    if (!has_end) {
        return Status::Error(StatusCode::ShaderError, "RDNA2 program has no s_endpgm terminator");
    }
    if (has_group_barrier && has_conditional_branch) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "s_barrier inside conditional control flow cannot be guaranteed group-uniform");
    }

    std::vector<size_t> block_leaders(leaders.begin(), leaders.end());
    std::unordered_map<size_t, uint32_t> block_for_instruction;
    module.basic_blocks.clear();
    module.basic_blocks.reserve(block_leaders.size());
    for (size_t block_index = 0; block_index < block_leaders.size(); ++block_index) {
        const size_t first = block_leaders[block_index];
        const size_t end = block_index + 1 < block_leaders.size()
                               ? block_leaders[block_index + 1]
                               : instructions.size();
        Rdna2BasicBlock block{};
        block.byte_offset = static_cast<uint32_t>(instructions[first].byte_offset);
        block.first_instruction = first;
        block.instruction_count = end - first;
        module.basic_blocks.push_back(block);
        for (size_t i = first; i < end; ++i) {
            block_for_instruction.emplace(i, static_cast<uint32_t>(block_index));
        }
    }

    std::vector<std::vector<uint32_t>> successors(module.basic_blocks.size());
    std::vector<std::vector<uint32_t>> predecessors(module.basic_blocks.size());
    for (uint32_t block_index = 0; block_index < module.basic_blocks.size(); ++block_index) {
        Rdna2BasicBlock& block = module.basic_blocks[block_index];
        const size_t last_index = block.first_instruction + block.instruction_count - 1;
        const Rdna2Instruction& last = instructions[last_index];
        const uint32_t next_block = block_index + 1 < module.basic_blocks.size()
                                        ? block_index + 1
                                        : 0xffffffffu;
        if (IsBranch(last.mnemonic)) {
            const auto target_offset = branch_target_offsets.find(last_index);
            if (target_offset == branch_target_offsets.end()) {
                return Status::Error(StatusCode::ShaderError, "RDNA2 branch has no decoded target");
            }
            const size_t target_instruction =
                instruction_at_offset.at(target_offset->second);
            block.branch_target = block_for_instruction.at(target_instruction);
            block.conditional_branch = IsConditionalBranch(last.mnemonic);
            block.unconditional_branch = !block.conditional_branch;
            successors[block_index].push_back(block.branch_target);
            if (block.conditional_branch) {
                if (next_block == 0xffffffffu) {
                    return Status::Error(StatusCode::ShaderError, "Conditional branch falls off the program");
                }
                block.fallthrough_target = next_block;
                successors[block_index].push_back(next_block);
            }
        } else if (last.mnemonic == "s_endpgm") {
            block.terminates = true;
        } else {
            if (next_block == 0xffffffffu) {
                return Status::Error(StatusCode::ShaderError, "RDNA2 block falls off the end of the program");
            }
            block.fallthrough_target = next_block;
            successors[block_index].push_back(next_block);
        }
        for (uint32_t successor : successors[block_index]) {
            predecessors[successor].push_back(block_index);
        }
    }

    std::vector<bool> reachable(module.basic_blocks.size(), false);
    std::vector<uint32_t> worklist = {0};
    while (!worklist.empty()) {
        const uint32_t block = worklist.back();
        worklist.pop_back();
        if (reachable[block]) continue;
        reachable[block] = true;
        worklist.insert(worklist.end(), successors[block].begin(), successors[block].end());
    }
    if (std::find(reachable.begin(), reachable.end(), false) != reachable.end()) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "RDNA2 CFG contains unreachable instruction blocks");
    }

    const size_t block_count = module.basic_blocks.size();
    std::vector<std::set<uint32_t>> dominators(block_count);
    for (uint32_t block = 0; block < block_count; ++block) {
        if (block == 0) dominators[block].insert(0);
        else {
            for (uint32_t candidate = 0; candidate < block_count; ++candidate) {
                dominators[block].insert(candidate);
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t block = 1; block < block_count; ++block) {
            std::set<uint32_t> next = dominators[block];
            if (predecessors[block].empty()) {
                next.clear();
            } else {
                next = dominators[predecessors[block][0]];
                for (size_t p = 1; p < predecessors[block].size(); ++p) {
                    std::set<uint32_t> intersection;
                    const auto& other = dominators[predecessors[block][p]];
                    std::set_intersection(
                        next.begin(), next.end(), other.begin(), other.end(),
                        std::inserter(intersection, intersection.end()));
                    next = std::move(intersection);
                }
            }
            next.insert(block);
            if (next != dominators[block]) {
                dominators[block] = std::move(next);
                changed = true;
            }
        }
    }

    std::vector<int32_t> discovery(block_count, -1);
    std::vector<int32_t> low(block_count, -1);
    std::vector<bool> on_stack(block_count, false);
    std::vector<uint32_t> stack;
    int32_t next_discovery = 0;
    std::vector<std::vector<uint32_t>> components;
    auto visit = [&](auto&& self, uint32_t block) -> void {
        discovery[block] = low[block] = next_discovery++;
        stack.push_back(block);
        on_stack[block] = true;
        for (uint32_t successor : successors[block]) {
            if (discovery[successor] == -1) {
                self(self, successor);
                low[block] = std::min(low[block], low[successor]);
            } else if (on_stack[successor]) {
                low[block] = std::min(low[block], discovery[successor]);
            }
        }
        if (low[block] != discovery[block]) return;
        components.emplace_back();
        for (;;) {
            const uint32_t member = stack.back();
            stack.pop_back();
            on_stack[member] = false;
            components.back().push_back(member);
            if (member == block) break;
        }
    };
    visit(visit, 0);

    for (const auto& component : components) {
        const bool cyclic = component.size() > 1 ||
            std::find(successors[component[0]].begin(),
                      successors[component[0]].end(), component[0]) !=
                successors[component[0]].end();
        if (!cyclic) continue;
        std::set<uint32_t> entries;
        if (std::find(component.begin(), component.end(), 0u) != component.end()) {
            entries.insert(0u);
        }
        for (uint32_t member : component) {
            for (uint32_t predecessor : predecessors[member]) {
                if (std::find(component.begin(), component.end(), predecessor) == component.end()) {
                    entries.insert(member);
                }
            }
        }
        if (entries.size() != 1) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "RDNA2 CFG has an irreducible loop with multiple entries");
        }
        const uint32_t header = *entries.begin();
        for (uint32_t member : component) {
            if (dominators[member].count(header) == 0) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "RDNA2 loop header does not dominate its body");
            }
        }
    }

    module.decoded_program = instructions;
    module.has_control_flow = true;
    for (const Rdna2Instruction& instruction : instructions) {
        if (instruction.mnemonic.find("_b64") != std::string::npos ||
            instruction.mnemonic.find("_saveexec_b64") != std::string::npos ||
            instruction.mnemonic == "s_cbranch_vccz" ||
            instruction.mnemonic == "s_cbranch_vccnz" ||
            instruction.mnemonic == "s_cbranch_execz" ||
            instruction.mnemonic == "s_cbranch_execnz" ||
            instruction.encoding == Rdna2Encoding::Vopc) {
            module.requires_wave64 = true;
        }
    }
    return Status::Ok();
}

}  // namespace

Status DecodeRdna2Instructions(
    const std::vector<uint8_t>& program,
    std::vector<Rdna2Instruction>& out_instructions) {
    out_instructions.clear();
    if (program.empty() || (program.size() & 3u) != 0) {
        return Status::Error(StatusCode::ShaderError, "RDNA2 program must contain aligned dwords");
    }

    const size_t word_count = program.size() / 4;
    size_t cursor = 0;
    while (cursor < word_count) {
        const uint32_t word = ReadLe32(program.data() + cursor * 4);
        const Rdna2Encoding encoding = Classify(word);
        if (encoding == Rdna2Encoding::Unknown) {
            std::ostringstream message;
            message << "Unknown RDNA2 encoding prefix at byte offset 0x"
                    << std::hex << cursor * 4;
            return Status::Error(StatusCode::UnsupportedFormat, message.str());
        }

        const uint32_t next = cursor + 1 < word_count
                                  ? ReadLe32(program.data() + (cursor + 1) * 4)
                                  : 0;
        size_t length = 1;
        switch (encoding) {
            case Rdna2Encoding::Smem:
            case Rdna2Encoding::Ds:
            case Rdna2Encoding::Mubuf:
            case Rdna2Encoding::Mtbuf:
            case Rdna2Encoding::Flat:
            case Rdna2Encoding::Exp:
                length = 2;
                break;
            case Rdna2Encoding::Vop3:
                length = 2;
                break;
            case Rdna2Encoding::Vinterp:
                length = 1;
                break;
            case Rdna2Encoding::Mimg:
                length = 2 + ((word >> 1) & 0x3u);
                break;
            default:
                break;
        }

        const bool trailing_literal = HasTrailingLiteral(encoding, word, next);
        if (trailing_literal) {
            ++length;
        }
        if (length > word_count - cursor) {
            std::ostringstream message;
            message << "Truncated RDNA2 instruction at byte offset 0x"
                    << std::hex << cursor * 4;
            return Status::Error(StatusCode::ShaderError, message.str());
        }

        Rdna2Instruction instruction{};
        instruction.byte_offset = cursor * 4;
        instruction.encoding = encoding;
        instruction.opcode = OpcodeFor(encoding, word);
        instruction.mnemonic = MnemonicFor(encoding, instruction.opcode);
        instruction.raw_dword0 = word;
        instruction.raw_dword1 = length > 1 ? next : 0;
        instruction.immediate = static_cast<int16_t>(word & 0xffffu);
        const uint32_t literal =
            trailing_literal && length > (encoding == Rdna2Encoding::Vop3 ? 2u : 1u)
                ? ReadLe32(program.data() + (cursor + length - 1) * 4)
                : 0;

        switch (encoding) {
            case Rdna2Encoding::Sop2:
                instruction.destinations.push_back(
                    ScalarSource((word >> 16) & 0x7fu, 0));
                instruction.sources.push_back(ScalarSource(word & 0xffu, literal));
                instruction.sources.push_back(ScalarSource((word >> 8) & 0xffu, literal));
                break;
            case Rdna2Encoding::Sop1:
                instruction.destinations.push_back(
                    ScalarSource((word >> 16) & 0x7fu, 0));
                instruction.sources.push_back(ScalarSource(word & 0xffu, literal));
                break;
            case Rdna2Encoding::Sopk:
                instruction.destinations.push_back(
                    ScalarSource((word >> 16) & 0x7fu, 0));
                if (instruction.opcode == 0x00) {
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Immediate, 0,
                         static_cast<uint32_t>(instruction.immediate)});
                } else if (instruction.opcode == 0x0e ||
                           instruction.opcode == 0x0f) {
                    instruction.sources.push_back(
                        ScalarSource((word >> 16) & 0x7fu, 0));
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Immediate, 0,
                         static_cast<uint32_t>(instruction.immediate)});
                }
                break;
            case Rdna2Encoding::Sopc:
                instruction.destinations.push_back({Rdna2RegisterBank::Scc, 0, 0});
                instruction.sources.push_back(ScalarSource(word & 0xffu, literal));
                instruction.sources.push_back(ScalarSource((word >> 8) & 0xffu, literal));
                break;
            case Rdna2Encoding::Sopp:
                if (instruction.mnemonic == "s_sendmsg") {
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::M0, 0, 0});
                }
                break;
            case Rdna2Encoding::Vop2:
                instruction.destinations.push_back(
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>((word >> 17) & 0xffu), 0});
                instruction.sources.push_back(VectorSource(word & 0x1ffu, literal));
                instruction.sources.push_back(
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>((word >> 9) & 0xffu), 0});
                break;
            case Rdna2Encoding::Vop1:
                instruction.destinations.push_back(
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>((word >> 17) & 0xffu), 0});
                instruction.sources.push_back(VectorSource(word & 0x1ffu, literal));
                break;
            case Rdna2Encoding::Vopc:
                instruction.destinations.push_back({Rdna2RegisterBank::Vcc, 0, 0});
                instruction.sources.push_back(VectorSource(word & 0x1ffu, literal));
                instruction.sources.push_back(
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>((word >> 9) & 0xffu), 0});
                break;
            case Rdna2Encoding::Vop3: {
                instruction.destinations.push_back(
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>(word & 0xffu), 0});
                const uint32_t src0 = instruction.raw_dword1 & 0x1ffu;
                const uint32_t src1 = (instruction.raw_dword1 >> 9) & 0x1ffu;
                const uint32_t src2 = (instruction.raw_dword1 >> 18) & 0x1ffu;
                instruction.sources.push_back(VectorSource(src0, literal));
                instruction.sources.push_back(VectorSource(src1, literal));
                instruction.sources.push_back(VectorSource(src2, literal));
                break;
            }
            case Rdna2Encoding::Vinterp:
                instruction.destinations.push_back(
                    {Rdna2RegisterBank::Vector,
                     static_cast<uint16_t>((word >> 18) & 0xffu), 0});
                instruction.sources.push_back(
                    {Rdna2RegisterBank::Vector,
                     static_cast<uint16_t>(word & 0xffu), 0});
                instruction.interpolation_attribute =
                    static_cast<uint8_t>((word >> 10) & 0x3fu);
                instruction.interpolation_channel =
                    static_cast<uint8_t>((word >> 8) & 0x3u);
                break;
            case Rdna2Encoding::Mubuf: {
                const uint32_t data = (next >> 8) & 0xffu;
                const uint32_t address = next & 0xffu;
                instruction.resource_sgpr =
                    static_cast<uint16_t>(((next >> 16) & 0x1fu) * 4u);
                instruction.offen = ((word >> 12) & 1u) != 0;
                instruction.idxen = ((word >> 13) & 1u) != 0;
                instruction.glc = ((word >> 14) & 1u) != 0;
                instruction.scalar_offset_code = static_cast<uint8_t>((next >> 24) & 0xffu);
                instruction.immediate = static_cast<int32_t>(word & 0xfffu);
                if (instruction.opcode == 0x0c) {
                    instruction.destinations.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(data), 0});
                    if (instruction.offen) {
                        instruction.sources.push_back(
                            {Rdna2RegisterBank::Vector, static_cast<uint16_t>(address), 0});
                    }
                } else if (instruction.opcode == 0x1c) {
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(data), 0});
                    if (instruction.offen) {
                        instruction.sources.push_back(
                            {Rdna2RegisterBank::Vector, static_cast<uint16_t>(address), 0});
                    }
                } else if (instruction.opcode == 0x32 ||
                           instruction.opcode == 0x31) {
                    if (instruction.glc) {
                        instruction.destinations.push_back(
                            {Rdna2RegisterBank::Vector, static_cast<uint16_t>(data), 0});
                    }
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(data), 0});
                    if (instruction.opcode == 0x31) {
                        instruction.sources.push_back(
                            {Rdna2RegisterBank::Vector,
                             static_cast<uint16_t>(data + 1u), 0});
                    }
                    if (instruction.offen) {
                        instruction.sources.push_back(
                            {Rdna2RegisterBank::Vector, static_cast<uint16_t>(address), 0});
                    }
                }
                break;
            }
            case Rdna2Encoding::Ds: {
                instruction.ds_offset0 = static_cast<uint16_t>(word & 0xffu);
                instruction.ds_offset1 = static_cast<uint16_t>((word >> 8) & 0xffu);
                const uint32_t address = next & 0xffu;
                const uint32_t data0 = (next >> 8) & 0xffu;
                const uint32_t destination = (next >> 24) & 0xffu;
                if (instruction.mnemonic == "ds_add_u32") {
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(address), 0});
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(data0), 0});
                } else if (instruction.mnemonic == "ds_read_b32" ||
                    instruction.mnemonic == "ds_read_b64" ||
                    instruction.mnemonic == "ds_read_b128") {
                    instruction.destinations.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(destination), 0});
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(address), 0});
                } else if (instruction.mnemonic == "ds_write_b32" ||
                           instruction.mnemonic == "ds_write_b64" ||
                           instruction.mnemonic == "ds_write_b128") {
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(address), 0});
                    instruction.sources.push_back(
                        {Rdna2RegisterBank::Vector, static_cast<uint16_t>(data0), 0});
                }
                break;
            }
            case Rdna2Encoding::Mimg: {
                const uint32_t address = next & 0xffu;
                instruction.destinations.push_back(
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>((next >> 8) & 0xffu), 0});
                instruction.resource_sgpr =
                    static_cast<uint16_t>(((next >> 16) & 0x1fu) * 4u);
                instruction.sampler_sgpr =
                    static_cast<uint16_t>(((next >> 21) & 0x1fu) * 4u);
                instruction.texture_dimension = static_cast<uint8_t>((word >> 3) & 0x7u);
                instruction.component_mask = static_cast<uint8_t>((word >> 8) & 0xfu);
                instruction.nsa_dwords = static_cast<uint8_t>((word >> 1) & 0x3u);
                instruction.sources.push_back(
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>(address), 0});
                break;
            }
            case Rdna2Encoding::Exp: {
                instruction.immediate = static_cast<int32_t>((word >> 4) & 0x3fu);
                instruction.component_mask = static_cast<uint8_t>(word & 0xfu);
                instruction.sources = {
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>(next & 0xffu), 0},
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>((next >> 8) & 0xffu), 0},
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>((next >> 16) & 0xffu), 0},
                    {Rdna2RegisterBank::Vector, static_cast<uint16_t>((next >> 24) & 0xffu), 0},
                };
                break;
            }
            default:
                break;
        }

        out_instructions.push_back(std::move(instruction));
        cursor += length;
    }
    return Status::Ok();
}

Status BuildRdna2Ir(
    const std::vector<Rdna2Instruction>& instructions,
    const Rdna2TranslationOptions& options,
    Rdna2IrModule& out_module) {
    out_module = Rdna2IrModule{};
    out_module.stage = options.stage;
    out_module.root_cbv_count = options.root_cbv_count;
    out_module.root_srv_count = options.root_srv_count;
    out_module.root_sampler_count = options.root_sampler_count;
    out_module.root_uav_count = options.root_uav_count;
    out_module.first_cbv_register = options.first_cbv_register;
    out_module.use_bindless_resources = options.use_bindless_resources;
    out_module.use_bindless_samplers = options.use_bindless_samplers;
    out_module.thread_group_size_x = std::max(1u, options.thread_group_size_x);
    out_module.thread_group_size_y = std::max(1u, options.thread_group_size_y);
    out_module.thread_group_size_z = std::max(1u, options.thread_group_size_z);
    out_module.mesh_max_vertices = options.mesh_max_vertices;
    out_module.mesh_max_primitives = options.mesh_max_primitives;
    const uint64_t thread_count =
        static_cast<uint64_t>(out_module.thread_group_size_x) *
        out_module.thread_group_size_y * out_module.thread_group_size_z;
    if (options.stage == ShaderStage::Mesh &&
        (thread_count == 0 || thread_count > 128 ||
         options.mesh_max_vertices == 0 || options.mesh_max_vertices > 256 ||
         options.mesh_max_primitives == 0 || options.mesh_max_primitives > 256)) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Mesh shader thread or output limits exceed the supported D3D12 mesh limits");
    }
    if (options.stage == ShaderStage::Amplification) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Xbox amplification shaders require the guest mesh/payload ring ABI, which is not yet mapped");
    }
    for (const Rdna2Instruction& instruction : instructions) {
        if (instruction.encoding == Rdna2Encoding::Vinterp) {
            if (options.stage != ShaderStage::Pixel) {
                return UnsupportedInstruction(instruction);
            }
            out_module.has_input_vgpr = true;
            const uint32_t input_vgpr =
                4u + static_cast<uint32_t>(instruction.interpolation_attribute) * 4u +
                instruction.interpolation_channel;
            out_module.max_input_vgpr =
                std::max(out_module.max_input_vgpr, input_vgpr);
        }
    }
    if (std::any_of(instructions.begin(), instructions.end(), [&options](const Rdna2Instruction& instruction) {
            return IsBranch(instruction.mnemonic) ||
                   IsNativeF16Instruction(instruction.mnemonic) ||
                   instruction.mnemonic.find("_b64") != std::string::npos ||
                   instruction.mnemonic.find("_saveexec_b64") != std::string::npos ||
                   instruction.encoding == Rdna2Encoding::Vopc ||
                   instruction.encoding == Rdna2Encoding::Ds ||
                   instruction.mnemonic == "s_barrier" ||
                   instruction.mnemonic == "buffer_atomic_add" ||
                   instruction.mnemonic == "buffer_atomic_cmpswap" ||
                   ((options.use_bindless_resources &&
                     (instruction.encoding == Rdna2Encoding::Mubuf ||
                      instruction.encoding == Rdna2Encoding::Mimg)) ||
                    (options.use_bindless_samplers &&
                     instruction.encoding == Rdna2Encoding::Mimg));
        })) {
        return BuildControlFlow(instructions, out_module);
    }

    std::unordered_map<uint32_t, uint32_t> current_values;
    uint32_t next_value = 0;
    bool saw_end = false;
    std::set<uint32_t> written_uavs;

    auto add_value = [&](Rdna2IrInstruction node) {
        node.result = next_value++;
        const uint32_t result = node.result;
        out_module.instructions.push_back(std::move(node));
        return result;
    };
    auto read_operand = [&](const Rdna2Operand& operand, uint32_t& result) -> Status {
        if (operand.bank == Rdna2RegisterBank::Immediate ||
            operand.bank == Rdna2RegisterBank::FloatImmediate ||
            operand.bank == Rdna2RegisterBank::Literal) {
            Rdna2IrInstruction constant{};
            constant.opcode = Rdna2IrOpcode::Constant;
            constant.immediate_bits = operand.bits;
            result = add_value(std::move(constant));
            return Status::Ok();
        }
        if (operand.bank != Rdna2RegisterBank::Scalar &&
            operand.bank != Rdna2RegisterBank::Vector &&
            operand.bank != Rdna2RegisterBank::Vcc &&
            operand.bank != Rdna2RegisterBank::M0) {
            return Status::Error(StatusCode::UnsupportedFormat, "RDNA2 operand uses an unsupported special register");
        }

        const uint32_t key = RegisterKey(operand);
        const auto found = current_values.find(key);
        if (found != current_values.end()) {
            result = found->second;
            return Status::Ok();
        }

        Rdna2IrInstruction input{};
        input.opcode = Rdna2IrOpcode::InputRegister;
        input.input_register = operand;
        result = add_value(std::move(input));
        current_values.emplace(key, result);
        if (operand.bank == Rdna2RegisterBank::Scalar &&
            !(options.stage == ShaderStage::Mesh && operand.index == 3)) {
            out_module.uses_scalar_input = true;
        } else if (operand.bank == Rdna2RegisterBank::Vector) {
            out_module.has_input_vgpr = true;
            out_module.max_input_vgpr =
                std::max(out_module.max_input_vgpr, static_cast<uint32_t>(operand.index));
        }
        return Status::Ok();
    };
    auto write_operation = [&](Rdna2IrOpcode opcode,
                               const Rdna2Operand& destination,
                               const std::vector<Rdna2Operand>& sources) -> Status {
        Rdna2IrInstruction operation{};
        operation.opcode = opcode;
        for (const Rdna2Operand& source : sources) {
            uint32_t value = kNoValue;
            Status status = read_operand(source, value);
            if (!status.ok()) return status;
            operation.operands.push_back(value);
        }
        const uint32_t result = add_value(std::move(operation));
        current_values[RegisterKey(destination)] = result;
        return Status::Ok();
    };
    auto write_side_effect = [&](Rdna2IrInstruction operation) {
        operation.result = kNoValue;
        out_module.instructions.push_back(std::move(operation));
    };

    for (const Rdna2Instruction& instruction : instructions) {
        if (instruction.mnemonic == "s_nop") continue;
        if (instruction.mnemonic == "s_endpgm") {
            Rdna2IrInstruction end{};
            end.opcode = Rdna2IrOpcode::End;
            write_side_effect(std::move(end));
            saw_end = true;
            continue;
        }
        if (instruction.mnemonic == "s_waitcnt") {
            Rdna2IrInstruction wait{};
            wait.opcode = Rdna2IrOpcode::Wait;
            write_side_effect(std::move(wait));
            continue;
        }
        if (instruction.mnemonic == "s_barrier") {
            if (options.stage != ShaderStage::Compute &&
                options.stage != ShaderStage::Mesh) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "s_barrier is only supported in compute and mesh shaders");
            }
            Rdna2IrInstruction barrier{};
            barrier.opcode = Rdna2IrOpcode::Barrier;
            write_side_effect(std::move(barrier));
            continue;
        }
        if (instruction.mnemonic == "s_sendmsg") {
            if (options.stage != ShaderStage::Mesh ||
                (static_cast<uint32_t>(instruction.immediate) & 0xfu) != 9u ||
                instruction.sources.empty()) {
                return UnsupportedInstruction(instruction);
            }
            Rdna2IrInstruction allocate{};
            allocate.opcode = Rdna2IrOpcode::MeshAllocate;
            uint32_t count_value = kNoValue;
            Status status = read_operand(instruction.sources[0], count_value);
            if (!status.ok()) return status;
            allocate.operands.push_back(count_value);
            write_side_effect(std::move(allocate));
            continue;
        }
        if (instruction.mnemonic.rfind("s_cbranch_", 0) == 0 ||
            instruction.mnemonic == "s_branch") {
            return UnsupportedInstruction(instruction);
        }

        if (instruction.mnemonic == "s_mov_b32" ||
            instruction.mnemonic == "s_movk_i32" ||
            instruction.mnemonic == "v_mov_b32") {
            if (instruction.destinations.empty() || instruction.sources.empty()) {
                return UnsupportedInstruction(instruction);
            }
            Status status = write_operation(
                Rdna2IrOpcode::Move, instruction.destinations[0], {instruction.sources[0]});
            if (!status.ok()) return status;
            continue;
        }
        if (instruction.mnemonic == "s_addk_i32" ||
            instruction.mnemonic == "s_mulk_i32") {
            if (instruction.destinations.empty() || instruction.sources.size() < 2) {
                return UnsupportedInstruction(instruction);
            }
            Status status = write_operation(
                instruction.mnemonic == "s_addk_i32"
                    ? Rdna2IrOpcode::AddU32
                    : Rdna2IrOpcode::MulU32,
                instruction.destinations[0],
                {instruction.sources[0], instruction.sources[1]});
            if (!status.ok()) return status;
            continue;
        }
        if (instruction.mnemonic == "s_add_u32" || instruction.mnemonic == "v_add_nc_u32" ||
            instruction.mnemonic == "s_sub_u32" || instruction.mnemonic == "v_sub_nc_u32" ||
            instruction.mnemonic == "s_and_b32" || instruction.mnemonic == "v_and_b32" ||
            instruction.mnemonic == "s_or_b32" || instruction.mnemonic == "v_or_b32" ||
            instruction.mnemonic == "s_xor_b32" || instruction.mnemonic == "v_xor_b32") {
            Rdna2IrOpcode opcode = Rdna2IrOpcode::AddU32;
            if (instruction.mnemonic.find("sub_") != std::string::npos) opcode = Rdna2IrOpcode::SubU32;
            else if (instruction.mnemonic.find("and_") != std::string::npos) opcode = Rdna2IrOpcode::AndU32;
            else if (instruction.mnemonic.find("or_") != std::string::npos) opcode = Rdna2IrOpcode::OrU32;
            else if (instruction.mnemonic.find("xor_") != std::string::npos) opcode = Rdna2IrOpcode::XorU32;
            if (instruction.destinations.empty() || instruction.sources.size() < 2) {
                return UnsupportedInstruction(instruction);
            }
            Status status = write_operation(
                opcode, instruction.destinations[0],
                {instruction.sources[0], instruction.sources[1]});
            if (!status.ok()) return status;
            continue;
        }
        if (instruction.mnemonic == "v_add_f32" || instruction.mnemonic == "v_sub_f32" ||
            instruction.mnemonic == "v_mul_f32") {
            if (instruction.destinations.empty() || instruction.sources.size() < 2) {
                return UnsupportedInstruction(instruction);
            }
            const Rdna2IrOpcode opcode =
                instruction.mnemonic == "v_add_f32" ? Rdna2IrOpcode::AddF32 :
                instruction.mnemonic == "v_sub_f32" ? Rdna2IrOpcode::SubF32 :
                                                       Rdna2IrOpcode::MulF32;
            Status status = write_operation(
                opcode, instruction.destinations[0],
                {instruction.sources[0], instruction.sources[1]});
            if (!status.ok()) return status;
            continue;
        }
        if (instruction.mnemonic == "v_fma_f32") {
            if (instruction.destinations.empty() || instruction.sources.size() < 3) {
                return UnsupportedInstruction(instruction);
            }
            Status status = write_operation(
                Rdna2IrOpcode::FmaF32, instruction.destinations[0],
                {instruction.sources[0], instruction.sources[1], instruction.sources[2]});
            if (!status.ok()) return status;
            continue;
        }
        if (instruction.mnemonic == "v_bfe_u32" ||
            instruction.mnemonic == "v_bfi_b32" ||
            instruction.mnemonic == "v_alignbit_b32" ||
            instruction.mnemonic == "v_min3_f32" ||
            instruction.mnemonic == "v_max3_f32" ||
            instruction.mnemonic == "v_med3_f32") {
            if (instruction.destinations.empty() || instruction.sources.size() < 3) {
                return UnsupportedInstruction(instruction);
            }
            Rdna2IrOpcode opcode = Rdna2IrOpcode::BitfieldExtractU32;
            if (instruction.mnemonic == "v_bfi_b32") {
                opcode = Rdna2IrOpcode::BitfieldInsertU32;
            } else if (instruction.mnemonic == "v_alignbit_b32") {
                opcode = Rdna2IrOpcode::AlignBitU32;
            } else if (instruction.mnemonic == "v_min3_f32") {
                opcode = Rdna2IrOpcode::Min3F32;
            } else if (instruction.mnemonic == "v_max3_f32") {
                opcode = Rdna2IrOpcode::Max3F32;
            } else if (instruction.mnemonic == "v_med3_f32") {
                opcode = Rdna2IrOpcode::Med3F32;
            }
            Status status = write_operation(
                opcode, instruction.destinations[0],
                {instruction.sources[0], instruction.sources[1],
                 instruction.sources[2]});
            if (!status.ok()) return status;
            continue;
        }
        if (instruction.encoding == Rdna2Encoding::Vinterp) {
            if (instruction.destinations.empty()) {
                return UnsupportedInstruction(instruction);
            }
            Rdna2IrInstruction interpolation{};
            interpolation.opcode = Rdna2IrOpcode::PixelInterpolate;
            interpolation.interpolation_attribute =
                instruction.interpolation_attribute;
            interpolation.interpolation_channel =
                instruction.interpolation_channel;
            interpolation.interpolation_flat =
                instruction.mnemonic == "v_interp_mov_f32";
            const uint32_t result = add_value(std::move(interpolation));
            current_values[RegisterKey(instruction.destinations[0])] = result;
            continue;
        }
        if (instruction.mnemonic == "v_cmp_eq_f32" || instruction.mnemonic == "v_cmp_lt_f32" ||
            instruction.mnemonic == "v_cmp_gt_f32") {
            if (instruction.destinations.empty() || instruction.sources.size() < 2) {
                return UnsupportedInstruction(instruction);
            }
            const Rdna2IrOpcode opcode =
                instruction.mnemonic == "v_cmp_eq_f32" ? Rdna2IrOpcode::CompareEqF32 :
                instruction.mnemonic == "v_cmp_lt_f32" ? Rdna2IrOpcode::CompareLtF32 :
                                                         Rdna2IrOpcode::CompareLtF32;
            std::vector<Rdna2Operand> sources = {
                instruction.sources[0], instruction.sources[1]};
            if (instruction.mnemonic == "v_cmp_gt_f32") {
                std::swap(sources[0], sources[1]);
            }
            Status status = write_operation(opcode, instruction.destinations[0], sources);
            if (!status.ok()) return status;
            continue;
        }
        if (instruction.mnemonic == "v_cndmask_b32") {
            if (instruction.destinations.empty() || instruction.sources.size() < 2) {
                return UnsupportedInstruction(instruction);
            }
            std::vector<Rdna2Operand> sources = {
                instruction.sources[0], instruction.sources[1],
                {Rdna2RegisterBank::Vcc, 0, 0}};
            Status status = write_operation(
                Rdna2IrOpcode::ConditionalSelect, instruction.destinations[0], sources);
            if (!status.ok()) return status;
            continue;
        }
        if (instruction.mnemonic == "image_bvh_intersect_ray" ||
            instruction.mnemonic == "image_bvh64_intersect_ray") {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "AMD BVH traversal-step opcode is decoded, but lowering requires a guest-address-space view; substituting RayQuery would change its semantics");
        }
        const bool image_load =
            instruction.mnemonic == "image_load" || instruction.mnemonic == "image_load_mip";
        const bool image_sample =
            instruction.mnemonic == "image_sample" ||
            instruction.mnemonic == "image_sample_l" ||
            instruction.mnemonic == "image_sample_b" ||
            instruction.mnemonic == "image_sample_d";
        if (image_load || image_sample) {
            if (options.stage == ShaderStage::Unknown ||
                instruction.texture_dimension != 1 || instruction.nsa_dwords != 0 ||
                instruction.destinations.empty() || instruction.sources.empty()) {
                return UnsupportedInstruction(instruction);
            }
            const uint32_t slot = instruction.resource_sgpr / 4;
            if (slot >= options.root_srv_count) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "RDNA2 image load references an SRV outside the root-signature SRV range");
            }
            uint32_t source_count = 0;
            Rdna2TextureSampleMode sample_mode = Rdna2TextureSampleMode::ImplicitLod;
            if (image_sample) {
                const uint32_t sampler_slot = instruction.sampler_sgpr / 4;
                if (sampler_slot >= options.root_sampler_count) {
                    return Status::Error(
                        StatusCode::UnsupportedFormat,
                        "RDNA2 image sample references a sampler outside the root-signature range");
                }
                if (instruction.mnemonic == "image_sample_l") {
                    sample_mode = Rdna2TextureSampleMode::ExplicitLod;
                    source_count = 3;
                } else if (instruction.mnemonic == "image_sample_b") {
                    sample_mode = Rdna2TextureSampleMode::Bias;
                    source_count = 3;
                } else if (instruction.mnemonic == "image_sample_d") {
                    sample_mode = Rdna2TextureSampleMode::Gradients;
                    source_count = 6;
                } else {
                    source_count = 2;
                }
            } else {
                source_count = instruction.mnemonic == "image_load_mip" ? 3u : 2u;
            }
            const uint32_t address = instruction.sources[0].index;
            if (address + source_count > 256u ||
                instruction.destinations[0].index + 4u > 256u) {
                return UnsupportedInstruction(instruction);
            }
            std::vector<uint32_t> sample_operands;
            for (uint32_t component = 0; component < source_count; ++component) {
                uint32_t value = kNoValue;
                Status status = read_operand(
                    {Rdna2RegisterBank::Vector,
                     static_cast<uint16_t>(address + component), 0},
                    value);
                if (!status.ok()) return status;
                sample_operands.push_back(value);
            }
            uint32_t destination_component = 0;
            for (uint32_t component = 0; component < 4; ++component) {
                if ((instruction.component_mask & (1u << component)) == 0) continue;
                Rdna2IrInstruction load{};
                load.opcode = image_sample
                                  ? Rdna2IrOpcode::TextureSampleComponent
                                  : Rdna2IrOpcode::TextureLoadComponent;
                load.operands = sample_operands;
                load.resource_slot = slot;
                load.sampler_slot = instruction.sampler_sgpr / 4;
                load.texture_sample_mode = sample_mode;
                load.component = static_cast<uint8_t>(component);
                load.result = next_value++;
                current_values[RegisterKey({
                    Rdna2RegisterBank::Vector,
                    static_cast<uint16_t>(instruction.destinations[0].index + destination_component),
                    0})] = load.result;
                out_module.instructions.push_back(std::move(load));
                ++destination_component;
            }
            if (destination_component == 0) {
                return UnsupportedInstruction(instruction);
            }
            out_module.uses_texture = true;
            continue;
        }
        if (instruction.mnemonic == "buffer_load_dword" ||
            instruction.mnemonic == "buffer_store_dword") {
            if (instruction.idxen || instruction.resource_sgpr % 4 != 0 ||
                (instruction.scalar_offset_code != 125 &&
                 instruction.scalar_offset_code != 128)) {
                return UnsupportedInstruction(instruction);
            }
            const uint32_t slot = instruction.resource_sgpr / 4;
            const bool is_store = instruction.mnemonic == "buffer_store_dword";
            if ((is_store && instruction.sources.empty()) ||
                (!is_store && instruction.destinations.empty())) {
                return UnsupportedInstruction(instruction);
            }
            if ((is_store && slot >= options.root_uav_count) ||
                (!is_store && slot >= options.root_srv_count)) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "RDNA2 buffer instruction references a resource outside the root-signature range");
            }
            uint32_t address_value = kNoValue;
            if (instruction.offen) {
                const Rdna2Operand* address = nullptr;
                if (is_store) {
                    if (instruction.sources.size() < 2) return UnsupportedInstruction(instruction);
                    address = &instruction.sources[1];
                } else if (!instruction.sources.empty()) {
                    address = &instruction.sources[0];
                }
                if (!address) return UnsupportedInstruction(instruction);
                Status status = read_operand(*address, address_value);
                if (!status.ok()) return status;
            } else {
                Rdna2IrInstruction constant{};
                constant.opcode = Rdna2IrOpcode::Constant;
                constant.immediate_bits = 0;
                address_value = add_value(std::move(constant));
            }

            if (is_store) {
                if (instruction.sources.empty()) return UnsupportedInstruction(instruction);
                uint32_t data_value = kNoValue;
                Status status = read_operand(instruction.sources[0], data_value);
                if (!status.ok()) return status;
                Rdna2IrInstruction store{};
                store.opcode = Rdna2IrOpcode::BufferStore;
                store.operands = {address_value, data_value};
                store.resource_slot = slot;
                store.byte_offset = instruction.immediate;
                write_side_effect(std::move(store));
                written_uavs.insert(slot);
            } else {
                Rdna2IrInstruction load{};
                load.opcode = Rdna2IrOpcode::BufferLoad;
                load.operands = {address_value};
                load.resource_slot = slot;
                load.byte_offset = instruction.immediate;
                const uint32_t result = add_value(std::move(load));
                current_values[RegisterKey(instruction.destinations[0])] = result;
            }
            continue;
        }
        if (instruction.mnemonic == "exp") {
            if (options.stage == ShaderStage::Mesh) {
                const uint32_t target = static_cast<uint32_t>(instruction.immediate);
                if (target != 12u && target != 20u &&
                    (target < 32u || target > 63u)) {
                    return UnsupportedInstruction(instruction);
                }
                Rdna2IrInstruction export_instruction{};
                export_instruction.opcode = Rdna2IrOpcode::Export;
                export_instruction.export_target =
                    static_cast<uint8_t>(target);
                export_instruction.component_mask = instruction.component_mask;
                for (uint32_t component = 0; component < 4; ++component) {
                    uint32_t value = kNoValue;
                    if ((instruction.component_mask & (1u << component)) != 0) {
                        Status status = read_operand(
                            instruction.sources[component], value);
                        if (!status.ok()) return status;
                    }
                    export_instruction.operands.push_back(value);
                }
                write_side_effect(std::move(export_instruction));
                continue;
            }
            if (options.stage != ShaderStage::Pixel && options.stage != ShaderStage::Vertex) {
                return UnsupportedInstruction(instruction);
            }
            const int32_t expected_target = options.stage == ShaderStage::Pixel ? 0 : 12;
            if (instruction.immediate != expected_target || instruction.sources.size() != 4) {
                return UnsupportedInstruction(instruction);
            }
            out_module.export_target = static_cast<uint8_t>(instruction.immediate);
            out_module.exports.assign(4, kNoValue);
            for (uint32_t component = 0; component < 4; ++component) {
                if ((instruction.component_mask & (1u << component)) == 0) {
                    Rdna2IrInstruction zero{};
                    zero.opcode = Rdna2IrOpcode::Constant;
                    zero.immediate_bits = 0;
                    out_module.exports[component] = add_value(std::move(zero));
                    continue;
                }
                Status status = read_operand(instruction.sources[component], out_module.exports[component]);
                if (!status.ok()) return status;
            }
            continue;
        }

        return UnsupportedInstruction(instruction);
    }

    if (!saw_end) {
        return Status::Error(StatusCode::ShaderError, "RDNA2 program has no s_endpgm terminator");
    }
    if (options.stage == ShaderStage::Pixel && out_module.export_target != 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Pixel shader has no MRT0 export");
    }
    if (options.stage == ShaderStage::Vertex && out_module.export_target != 12) {
        return Status::Error(StatusCode::UnsupportedFormat, "Vertex shader has no position export");
    }
    if (options.stage == ShaderStage::Compute && written_uavs.empty()) {
        return Status::Error(StatusCode::UnsupportedFormat, "Compute shader has no supported UAV store");
    }
    if (options.stage != ShaderStage::Compute &&
        options.stage != ShaderStage::Pixel &&
        options.stage != ShaderStage::Vertex &&
        options.stage != ShaderStage::Mesh) {
        return Status::Error(StatusCode::UnsupportedFormat, "RDNA2 shader stage is not supported by this lifter");
    }
    if (options.stage == ShaderStage::Mesh) {
        const bool has_allocation = std::any_of(
            out_module.instructions.begin(), out_module.instructions.end(),
            [](const Rdna2IrInstruction& instruction) {
                return instruction.opcode == Rdna2IrOpcode::MeshAllocate;
            });
        const bool has_position = std::any_of(
            out_module.instructions.begin(), out_module.instructions.end(),
            [](const Rdna2IrInstruction& instruction) {
                return instruction.opcode == Rdna2IrOpcode::Export &&
                       instruction.export_target == 12;
            });
        const bool has_primitive = std::any_of(
            out_module.instructions.begin(), out_module.instructions.end(),
            [](const Rdna2IrInstruction& instruction) {
                return instruction.opcode == Rdna2IrOpcode::Export &&
                       instruction.export_target == 20;
            });
        if (!has_allocation || !has_position || !has_primitive) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "Mesh shader requires GS_ALLOC_REQ, position, and primitive exports");
        }
    }
    if (out_module.uses_scalar_input && options.root_cbv_count == 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Scalar input registers require a root CBV binding");
    }
    return Status::Ok();
}

namespace {

Status GenerateControlFlowHlsl(const Rdna2IrModule& module, std::string& out_hlsl) {
    std::set<uint32_t> scalar_registers;
    std::set<uint32_t> vector_registers;
    std::set<uint32_t> input_scalar_registers;
    std::set<uint32_t> input_vector_registers;
    std::set<uint32_t> seen_scalar_definitions;
    std::set<uint32_t> seen_vector_definitions;
    std::set<uint32_t> texture_slots;
    std::set<uint32_t> sampler_slots;
    std::set<uint32_t> srv_slots;
    std::set<uint32_t> uav_slots;
    std::set<uint32_t> flat_pixel_attributes;
    bool uses_vcc = false;
    bool uses_exec = false;
    bool uses_m0 = false;
    bool has_expected_export = false;
    bool uses_lds = false;
    bool has_mesh_primitive = false;
    bool has_mesh_allocation = false;
    std::set<uint32_t> mesh_parameter_targets;
    uint8_t expected_export = module.stage == ShaderStage::Pixel ? 0u : 12u;
    uint32_t max_input_vgpr = 0;
    bool has_input_vgpr = false;

    auto record_operand = [&](const Rdna2Operand& operand) {
        switch (operand.bank) {
            case Rdna2RegisterBank::Scalar:
                scalar_registers.insert(operand.index);
                break;
            case Rdna2RegisterBank::Vector:
                vector_registers.insert(operand.index);
                break;
            case Rdna2RegisterBank::Vcc: uses_vcc = true; break;
            case Rdna2RegisterBank::Exec: uses_exec = true; break;
            case Rdna2RegisterBank::M0: uses_m0 = true; break;
            case Rdna2RegisterBank::Scc: break;
            default: break;
        }
    };

    for (const Rdna2Instruction& instruction : module.decoded_program) {
        uses_lds |= instruction.encoding == Rdna2Encoding::Ds;
        if (instruction.encoding == Rdna2Encoding::Vinterp) {
            has_input_vgpr = true;
            max_input_vgpr = std::max(
                max_input_vgpr,
                4u + static_cast<uint32_t>(instruction.interpolation_attribute) * 4u +
                    instruction.interpolation_channel);
            if (instruction.mnemonic == "v_interp_mov_f32") {
                flat_pixel_attributes.insert(instruction.interpolation_attribute);
            }
        }
        for (const Rdna2Operand& source : instruction.sources) {
            if (instruction.encoding == Rdna2Encoding::Vinterp) continue;
            record_operand(source);
            if (source.bank == Rdna2RegisterBank::Scalar &&
                !(module.stage == ShaderStage::Mesh && source.index == 3) &&
                seen_scalar_definitions.count(source.index) == 0) {
                input_scalar_registers.insert(source.index);
            } else if (source.bank == Rdna2RegisterBank::Vector &&
                       seen_vector_definitions.count(source.index) == 0) {
                input_vector_registers.insert(source.index);
            }
        }
        for (const Rdna2Operand& destination : instruction.destinations) {
            record_operand(destination);
            if (destination.bank == Rdna2RegisterBank::M0) {
                uses_m0 = true;
            }
        }
        if (instruction.encoding == Rdna2Encoding::Ds) {
            const uint32_t width =
                instruction.mnemonic.find("b128") != std::string::npos ? 4u :
                instruction.mnemonic.find("b64") != std::string::npos ? 2u : 1u;
            if (instruction.mnemonic.find("ds_read_") == 0 &&
                !instruction.destinations.empty()) {
                for (uint32_t i = 1; i < width; ++i) {
                    const uint32_t reg = instruction.destinations[0].index + i;
                    vector_registers.insert(reg);
                }
            } else if (instruction.mnemonic.find("ds_write_") == 0 &&
                       instruction.sources.size() > 1) {
                for (uint32_t i = 1; i < width; ++i) {
                    const uint32_t reg = instruction.sources[1].index + i;
                    vector_registers.insert(reg);
                    if (seen_vector_definitions.count(reg) == 0) {
                        input_vector_registers.insert(reg);
                    }
                }
            }
        }
        if (instruction.encoding == Rdna2Encoding::Mimg) {
            const uint32_t address = instruction.sources.empty() ? 0u : instruction.sources[0].index;
            const uint32_t address_count =
                instruction.mnemonic == "image_sample_d" ? 6u :
                instruction.mnemonic == "image_load_mip" ||
                        instruction.mnemonic == "image_sample_l" ||
                        instruction.mnemonic == "image_sample_b" ? 3u :
                instruction.mnemonic == "image_load" ||
                        instruction.mnemonic == "image_sample" ? 2u : 0u;
            for (uint32_t i = 0; i < address_count; ++i) {
                vector_registers.insert(address + i);
                if (seen_vector_definitions.count(address + i) == 0) {
                    input_vector_registers.insert(address + i);
                }
            }
            if (!instruction.destinations.empty()) {
                uint32_t written_components = 0;
                for (uint32_t component = 0; component < 4; ++component) {
                    if ((instruction.component_mask & (1u << component)) == 0) continue;
                    vector_registers.insert(
                        instruction.destinations[0].index + written_components);
                    ++written_components;
                }
            }
        }
        if (instruction.mnemonic.find("_b64") != std::string::npos ||
            instruction.mnemonic.find("_saveexec_b64") != std::string::npos) {
            for (const Rdna2Operand& operand : instruction.sources) {
                if (operand.bank == Rdna2RegisterBank::Scalar) {
                    const uint32_t high = static_cast<uint32_t>(operand.index) + 1u;
                    scalar_registers.insert(high);
                    if (seen_scalar_definitions.count(high) == 0) {
                        input_scalar_registers.insert(high);
                    }
                }
            }
            for (const Rdna2Operand& operand : instruction.destinations) {
                if (operand.bank == Rdna2RegisterBank::Scalar) {
                    const uint32_t high = static_cast<uint32_t>(operand.index) + 1u;
                    scalar_registers.insert(high);
                }
            }
        }
        for (const Rdna2Operand& destination : instruction.destinations) {
            if (destination.bank == Rdna2RegisterBank::Scalar) {
                seen_scalar_definitions.insert(destination.index);
            } else if (destination.bank == Rdna2RegisterBank::Vector) {
                seen_vector_definitions.insert(destination.index);
                if (instruction.encoding == Rdna2Encoding::Ds) {
                    const uint32_t width =
                        instruction.mnemonic.find("b128") != std::string::npos ? 4u :
                        instruction.mnemonic.find("b64") != std::string::npos ? 2u : 1u;
                    for (uint32_t i = 1; i < width; ++i) {
                        seen_vector_definitions.insert(
                            static_cast<uint32_t>(destination.index) + i);
                    }
                }
            }
        }
        if (instruction.encoding == Rdna2Encoding::Mimg &&
            !instruction.destinations.empty()) {
            uint32_t written_components = 0;
            for (uint32_t component = 0; component < 4; ++component) {
                if ((instruction.component_mask & (1u << component)) == 0) continue;
                seen_vector_definitions.insert(
                    instruction.destinations[0].index + written_components++);
            }
        }
        if ((instruction.mnemonic.find("_b64") != std::string::npos ||
             instruction.mnemonic.find("_saveexec_b64") != std::string::npos)) {
            for (const Rdna2Operand& destination : instruction.destinations) {
                if (destination.bank == Rdna2RegisterBank::Scalar) {
                    seen_scalar_definitions.insert(
                        static_cast<uint32_t>(destination.index) + 1u);
                }
            }
        }
        if (instruction.mnemonic == "v_cndmask_b32") uses_vcc = true;
        if (instruction.mnemonic == "s_cbranch_vccz" ||
            instruction.mnemonic == "s_cbranch_vccnz") uses_vcc = true;
        if (instruction.mnemonic == "s_cbranch_execz" ||
            instruction.mnemonic == "s_cbranch_execnz") uses_exec = true;
        if (instruction.mnemonic.find("saveexec") != std::string::npos) uses_exec = true;
        if (instruction.encoding == Rdna2Encoding::Vopc) uses_vcc = true;
        if (instruction.encoding == Rdna2Encoding::Vopc) uses_exec = true;

        if (module.use_bindless_resources &&
            (instruction.encoding == Rdna2Encoding::Mubuf ||
             instruction.encoding == Rdna2Encoding::Mimg)) {
            const uint32_t resource_register = instruction.resource_sgpr;
            scalar_registers.insert(resource_register);
            if (seen_scalar_definitions.count(resource_register) == 0) {
                input_scalar_registers.insert(resource_register);
            }
            if (module.use_bindless_samplers &&
                instruction.encoding == Rdna2Encoding::Mimg &&
                (instruction.mnemonic == "image_sample" ||
                 instruction.mnemonic == "image_sample_l" ||
                 instruction.mnemonic == "image_sample_b" ||
                 instruction.mnemonic == "image_sample_d")) {
                const uint32_t sampler_register = instruction.sampler_sgpr;
                scalar_registers.insert(sampler_register);
                if (seen_scalar_definitions.count(sampler_register) == 0) {
                    input_scalar_registers.insert(sampler_register);
                }
            }
        } else if (module.use_bindless_samplers &&
                   instruction.encoding == Rdna2Encoding::Mimg &&
                   (instruction.mnemonic == "image_sample" ||
                    instruction.mnemonic == "image_sample_l" ||
                    instruction.mnemonic == "image_sample_b" ||
                    instruction.mnemonic == "image_sample_d")) {
            const uint32_t sampler_register = instruction.sampler_sgpr;
            scalar_registers.insert(sampler_register);
            if (seen_scalar_definitions.count(sampler_register) == 0) {
                input_scalar_registers.insert(sampler_register);
            }
        }

        const bool buffer_atomic =
            instruction.mnemonic == "buffer_atomic_add" ||
            instruction.mnemonic == "buffer_atomic_cmpswap";
        if (instruction.mnemonic == "buffer_load_dword" ||
            instruction.mnemonic == "buffer_store_dword" ||
            buffer_atomic) {
            const uint32_t slot = instruction.resource_sgpr / 4;
            if (instruction.mnemonic == "buffer_store_dword" || buffer_atomic) {
                uav_slots.insert(slot);
            } else {
                if (!module.use_bindless_resources) srv_slots.insert(slot);
            }
            if (module.use_bindless_resources) {
                uav_slots.erase(slot);
            }
        }
        if (instruction.encoding == Rdna2Encoding::Mimg) {
            const uint32_t slot = instruction.resource_sgpr / 4;
            if (!module.use_bindless_resources &&
                (instruction.mnemonic == "image_load" ||
                instruction.mnemonic == "image_load_mip" ||
                instruction.mnemonic == "image_sample" ||
                instruction.mnemonic == "image_sample_l" ||
                instruction.mnemonic == "image_sample_b" ||
                instruction.mnemonic == "image_sample_d")) {
                texture_slots.insert(slot);
            }
            if (!module.use_bindless_samplers &&
                (instruction.mnemonic == "image_sample" ||
                instruction.mnemonic == "image_sample_l" ||
                instruction.mnemonic == "image_sample_b" ||
                instruction.mnemonic == "image_sample_d")) {
                sampler_slots.insert(instruction.sampler_sgpr / 4);
            }
        }
        if (instruction.mnemonic == "exp" &&
            static_cast<uint8_t>(instruction.immediate) == expected_export) {
            has_expected_export = true;
        }
        if (module.stage == ShaderStage::Mesh &&
            instruction.mnemonic == "exp") {
            const uint32_t target = static_cast<uint32_t>(instruction.immediate);
            if (target == 20u) {
                has_mesh_primitive = true;
            } else if (target >= 32u && target <= 63u) {
                mesh_parameter_targets.insert(target - 32u);
            }
        }
        if (module.stage == ShaderStage::Mesh &&
            instruction.mnemonic == "s_sendmsg" &&
            (static_cast<uint32_t>(instruction.immediate) & 0xfu) == 9u) {
            has_mesh_allocation = true;
        }
    }

    for (uint32_t slot : texture_slots) {
        if (srv_slots.count(slot) != 0) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "A single root SRV slot is used as both a texture and a byte-address buffer");
        }
        if (!module.use_bindless_resources && slot >= module.root_srv_count) {
            return Status::Error(StatusCode::UnsupportedFormat, "Texture SRV index exceeds the root-signature range");
        }
    }
    for (uint32_t slot : srv_slots) {
        if (slot >= module.root_srv_count) {
            return Status::Error(StatusCode::UnsupportedFormat, "Buffer SRV index exceeds the root-signature range");
        }
    }
    for (uint32_t slot : sampler_slots) {
        if (!module.use_bindless_samplers && slot >= module.root_sampler_count) {
            return Status::Error(StatusCode::UnsupportedFormat, "Sampler index exceeds the root-signature range");
        }
    }
    for (uint32_t slot : uav_slots) {
        if (!module.use_bindless_resources && slot >= module.root_uav_count) {
            return Status::Error(StatusCode::UnsupportedFormat, "UAV index exceeds the root-signature range");
        }
    }
    if (module.stage != ShaderStage::Compute && !has_expected_export) {
        return Status::Error(StatusCode::UnsupportedFormat, "Graphics shader is missing its required export");
    }
    if (module.stage == ShaderStage::Mesh &&
        (!has_mesh_primitive || !has_mesh_allocation)) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Mesh shader requires GS_ALLOC_REQ and primitive exports");
    }

    for (uint32_t reg : input_vector_registers) {
        has_input_vgpr = true;
        max_input_vgpr = std::max(max_input_vgpr, reg);
    }
    if ((module.stage == ShaderStage::Pixel || module.stage == ShaderStage::Vertex) &&
        has_input_vgpr && max_input_vgpr > 63) {
        return Status::Error(StatusCode::UnsupportedFormat, "Graphics shader input VGPR exceeds the supported signature range");
    }

    bool needs_user_data = false;
    needs_user_data = !input_scalar_registers.empty();
    if (needs_user_data && module.root_cbv_count == 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Scalar input registers require a root CBV binding");
    }

    std::ostringstream hlsl;
    if (module.stage == ShaderStage::Pixel && has_input_vgpr) {
        hlsl << "struct SXPixelInput { float4 position : SV_Position;\n";
        if (max_input_vgpr >= 4) {
            const uint32_t count = (max_input_vgpr - 4) / 4 + 1;
            for (uint32_t i = 0; i < count; ++i) {
                hlsl << (flat_pixel_attributes.count(i) ? "nointerpolation " : "")
                     << "float4 attr" << i << " : TEXCOORD" << i << ";\n";
            }
        }
        hlsl << "};\n";
    } else if (module.stage == ShaderStage::Vertex && has_input_vgpr) {
        hlsl << "struct SXVertexInput { uint vertex_id : SV_VertexID; "
                "uint instance_id : SV_InstanceID;\n";
        if (max_input_vgpr >= 2) {
            const uint32_t count = (max_input_vgpr - 2) / 4 + 1;
            for (uint32_t i = 0; i < count; ++i) {
                hlsl << "float4 attr" << i << " : TEXCOORD" << i << ";\n";
            }
        }
        hlsl << "};\n";
    } else if (module.stage != ShaderStage::Compute &&
               module.stage != ShaderStage::Pixel &&
               module.stage != ShaderStage::Vertex &&
               module.stage != ShaderStage::Mesh) {
        return Status::Error(StatusCode::UnsupportedFormat, "HLSL generation does not support this shader stage");
    }

    if (needs_user_data) {
        hlsl << "cbuffer SXUserData : register(b" << module.first_cbv_register
             << ") { uint4 sx_user_data[64]; };\n";
    }
    for (uint32_t slot : srv_slots) {
        hlsl << "ByteAddressBuffer sx_srv" << slot << " : register(t" << slot << ");\n";
    }
    for (uint32_t slot : texture_slots) {
        hlsl << "Texture2D<float4> sx_texture" << slot << " : register(t" << slot << ");\n";
    }
    for (uint32_t slot : sampler_slots) {
        hlsl << "SamplerState sx_sampler" << slot << " : register(s" << slot << ");\n";
    }
    for (uint32_t slot : uav_slots) {
        hlsl << "RWByteAddressBuffer sx_uav" << slot << " : register(u" << slot << ");\n";
    }
    if (uses_lds) hlsl << "groupshared uint sx_lds[16384];\n";
    if (module.stage == ShaderStage::Mesh) {
        hlsl << "struct SXMeshVertex { float4 position : SV_Position;\n";
        for (uint32_t target : mesh_parameter_targets) {
            hlsl << "float4 attr" << target << " : TEXCOORD" << target << ";\n";
        }
        hlsl << "};\n"
                "struct SXMeshPrimitive { bool cull : SV_CullPrimitive; };\n"
                "groupshared uint sx_mesh_counts;\n";
    }
    if (module.requires_wave64) hlsl << "[WaveSize(64)]\n";
    if (module.stage == ShaderStage::Compute) {
        hlsl << "[numthreads(" << module.thread_group_size_x << ", "
             << module.thread_group_size_y << ", " << module.thread_group_size_z << ")]\n";
        hlsl << "void main(uint3 dispatch_id : SV_DispatchThreadID, "
                "uint3 group_thread_id : SV_GroupThreadID, "
                "uint3 group_id : SV_GroupID) {\n";
    } else if (module.stage == ShaderStage::Pixel) {
        hlsl << "float4 main(" << (has_input_vgpr ? "SXPixelInput input" : "")
             << ") : SV_Target0 {\n";
    } else if (module.stage == ShaderStage::Mesh) {
        hlsl << "[numthreads(" << module.thread_group_size_x << ", "
             << module.thread_group_size_y << ", " << module.thread_group_size_z
             << ")]\n[outputtopology(\"triangle\")]\n"
                "void main(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID, "
                "out vertices SXMeshVertex sx_vertices["
             << module.mesh_max_vertices << "], out indices uint3 sx_triangles["
             << module.mesh_max_primitives << "], out primitives SXMeshPrimitive sx_primitives["
             << module.mesh_max_primitives << "]) {\n";
    } else {
        hlsl << "float4 main(" << (has_input_vgpr ? "SXVertexInput input" : "")
             << ") : SV_Position {\n";
    }
    if (module.stage == ShaderStage::Mesh) {
        hlsl << "if (gi == 0u) sx_mesh_counts = 0u;\n"
                "GroupMemoryBarrierWithGroupSync();\n"
                "SXMeshVertex sx_mesh_output = (SXMeshVertex)0;\n"
                "uint sx_mesh_primitive = 0u;\n";
    }

    auto input_expression = [&](uint32_t index, std::string& expression) -> Status {
        if (module.stage == ShaderStage::Mesh) {
            if (index != 5u) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "Mesh VGPR input has no system-value mapping");
            }
            expression = "gid.x * " +
                         std::to_string(module.thread_group_size_x) +
                         "u + gi";
        } else if (module.stage == ShaderStage::Compute) {
            if (index > 8) {
                return Status::Error(StatusCode::UnsupportedFormat, "Compute VGPR input has no system-value mapping");
            }
            constexpr std::array<const char*, 3> axes = {"x", "y", "z"};
            if (index < 3) {
                expression = "asuint(dispatch_id." + std::string(axes[index]) + ")";
            } else if (index < 6) {
                expression = "asuint(group_thread_id." + std::string(axes[index - 3]) + ")";
            } else {
                expression = "asuint(group_id." + std::string(axes[index - 6]) + ")";
            }
        } else if (module.stage == ShaderStage::Pixel) {
            if (index < 4) {
                constexpr std::array<const char*, 4> components = {"x", "y", "z", "w"};
                expression = "asuint(input.position." + std::string(components[index]) + ")";
            } else {
                constexpr std::array<const char*, 4> components = {"x", "y", "z", "w"};
                expression = "asuint(input.attr" + std::to_string((index - 4) / 4) + "." +
                             components[(index - 4) % 4] + ")";
            }
        } else {
            if (index == 0) expression = "input.vertex_id";
            else if (index == 1) expression = "input.instance_id";
            else {
                constexpr std::array<const char*, 4> components = {"x", "y", "z", "w"};
                expression = "asuint(input.attr" + std::to_string((index - 2) / 4) + "." +
                             components[(index - 2) % 4] + ")";
            }
        }
        return Status::Ok();
    };

    for (uint32_t reg : scalar_registers) {
        hlsl << "uint sx_s" << reg << " = ";
        if (module.stage == ShaderStage::Mesh && reg == 3u) {
            hlsl << "((gi / 64u) << 24u)";
        } else if (input_scalar_registers.count(reg) == 0) hlsl << "0u";
        else hlsl << "sx_user_data[" << reg / 4 << "][" << reg % 4 << "]";
        hlsl << ";\n";
    }
    if (uses_m0) hlsl << "uint sx_m0 = 0u;\n";
    for (uint32_t reg : vector_registers) {
        hlsl << "uint sx_v" << reg << " = ";
        if (input_vector_registers.count(reg) != 0) {
            std::string expression;
            Status status = input_expression(reg, expression);
            if (!status.ok()) return status;
            hlsl << expression;
        } else hlsl << "0u";
        hlsl << ";\n";
    }
    hlsl << "uint sx_scc = 0u;\n";
    if (uses_vcc) hlsl << "uint2 sx_vcc = uint2(0u, 0u);\n";
    if (uses_exec) hlsl << "uint2 sx_exec = WaveActiveBallot(true).xy;\n";
    if (uses_exec || uses_vcc) {
        hlsl << "#define SX_MASK_BIT(mask) (WaveGetLaneIndex() < 32u ? "
                "(((mask).x >> WaveGetLaneIndex()) & 1u) : "
                "(((mask).y >> (WaveGetLaneIndex() - 32u)) & 1u))\n";
    }
    hlsl << "uint sx_out0 = 0u, sx_out1 = 0u, sx_out2 = 0u, sx_out3 = 0u;\n";

    auto expression_for = [&](const Rdna2Operand& operand, std::string& expression) -> Status {
        switch (operand.bank) {
            case Rdna2RegisterBank::Scalar:
                expression = "sx_s" + std::to_string(operand.index);
                return Status::Ok();
            case Rdna2RegisterBank::Vector:
                expression = "sx_v" + std::to_string(operand.index);
                return Status::Ok();
            case Rdna2RegisterBank::Vcc:
                expression = operand.index == 0 ? "sx_vcc.x" : "sx_vcc.y";
                return Status::Ok();
            case Rdna2RegisterBank::Exec:
                expression = operand.index == 0 ? "sx_exec.x" : "sx_exec.y";
                return Status::Ok();
            case Rdna2RegisterBank::Scc:
                expression = "sx_scc";
                return Status::Ok();
            case Rdna2RegisterBank::M0:
                expression = "sx_m0";
                return Status::Ok();
            case Rdna2RegisterBank::Immediate:
            case Rdna2RegisterBank::FloatImmediate:
            case Rdna2RegisterBank::Literal: {
                std::ostringstream value;
                value << "0x" << std::hex << operand.bits << "u";
                expression = value.str();
                return Status::Ok();
            }
            case Rdna2RegisterBank::Null:
                expression = "0u";
                return Status::Ok();
            default:
                return Status::Error(StatusCode::UnsupportedFormat, "CFG instruction uses an unmapped RDNA2 operand");
        }
    };
    auto pair_expression = [&](const Rdna2Operand& operand, std::string& expression) -> Status {
        if (operand.bank == Rdna2RegisterBank::Scalar) {
            expression = "uint2(sx_s" + std::to_string(operand.index) + ", sx_s" +
                         std::to_string(static_cast<uint32_t>(operand.index) + 1u) + ")";
            return Status::Ok();
        }
        if (operand.bank == Rdna2RegisterBank::Vcc) {
            if (operand.index != 0) {
                return Status::Error(StatusCode::UnsupportedFormat, "64-bit source starts at VCC high half");
            }
            expression = "sx_vcc";
            return Status::Ok();
        }
        if (operand.bank == Rdna2RegisterBank::Exec) {
            if (operand.index != 0) {
                return Status::Error(StatusCode::UnsupportedFormat, "64-bit source starts at EXEC high half");
            }
            expression = "sx_exec";
            return Status::Ok();
        }
        if (operand.bank == Rdna2RegisterBank::Immediate ||
            operand.bank == Rdna2RegisterBank::FloatImmediate ||
            operand.bank == Rdna2RegisterBank::Literal) {
            const uint32_t high =
                operand.bank == Rdna2RegisterBank::Immediate &&
                        (operand.bits & 0x80000000u) != 0
                    ? 0xffffffffu
                    : 0u;
            std::ostringstream value;
            value << "uint2(0x" << std::hex << operand.bits << "u, 0x"
                  << high << "u)";
            expression = value.str();
            return Status::Ok();
        }
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported 64-bit scalar source");
    };
    auto emit_pair_store = [&](const Rdna2Operand& destination, const std::string& value) -> Status {
        if (destination.bank == Rdna2RegisterBank::Scalar) {
            hlsl << "sx_s" << destination.index << " = " << value << ".x;\n"
                 << "sx_s" << (static_cast<uint32_t>(destination.index) + 1u)
                 << " = " << value << ".y;\n";
        } else if (destination.bank == Rdna2RegisterBank::Vcc) {
            if (destination.index != 0) {
                return Status::Error(StatusCode::UnsupportedFormat, "64-bit destination starts at VCC high half");
            }
            hlsl << "sx_vcc = " << value << ";\n";
        } else if (destination.bank == Rdna2RegisterBank::Exec) {
            if (destination.index != 0) {
                return Status::Error(StatusCode::UnsupportedFormat, "64-bit destination starts at EXEC high half");
            }
            hlsl << "sx_exec = " << value << ";\n";
        } else {
            return Status::Error(StatusCode::UnsupportedFormat, "Unsupported 64-bit scalar destination");
        }
        return Status::Ok();
    };
    auto lane_guard = [&]() {
        return uses_exec ? "if (SX_MASK_BIT(sx_exec) != 0u) " : "";
    };
    auto source_at = [&](const Rdna2Instruction& instruction, size_t index,
                         std::string& expression) -> Status {
        if (index >= instruction.sources.size()) {
            return Status::Error(StatusCode::ShaderError, "RDNA2 instruction has too few decoded sources");
        }
        return expression_for(instruction.sources[index], expression);
    };
    auto packed_half_source = [&](const Rdna2Instruction& instruction,
                                  size_t source_index,
                                  uint32_t result_half,
                                  std::string& expression) -> Status {
        if (source_index >= instruction.sources.size() || result_half > 1) {
            return UnsupportedInstruction(instruction);
        }
        const Rdna2Operand& operand = instruction.sources[source_index];
        const uint32_t selected_half = result_half == 0
            ? ((instruction.raw_dword0 >> (11u + source_index)) & 1u)
            : (source_index < 2
                   ? ((instruction.raw_dword1 >> (27u + source_index)) & 1u)
                   : ((instruction.raw_dword0 >> 14u) & 1u));
        const uint32_t negate = result_half == 0
            ? ((instruction.raw_dword1 >> (29u + source_index)) & 1u)
            : ((instruction.raw_dword0 >> (8u + source_index)) & 1u);

        if (operand.bank == Rdna2RegisterBank::FloatImmediate) {
            expression = selected_half == 0
                ? "(half)asfloat(" + std::to_string(operand.bits) + "u)"
                : "half(0.0f)";
        } else if (operand.bank == Rdna2RegisterBank::Immediate) {
            expression = selected_half == 0
                ? "half(" +
                      std::to_string(static_cast<int32_t>(operand.bits)) + ".0f)"
                : "half(0.0f)";
        } else if (operand.bank == Rdna2RegisterBank::Null) {
            expression = "half(0.0f)";
        } else {
            std::string packed;
            Status status = source_at(instruction, source_index, packed);
            if (!status.ok()) return status;
            const uint32_t shift = selected_half * 16u;
            expression = "(half)f16tof32((" + packed + " >> " +
                         std::to_string(shift) +
                         "u) & 0xffffu)";
        }
        if (negate != 0) expression = "-(" + expression + ")";
        return Status::Ok();
    };
    auto bindless_resource_expression = [&](uint32_t sgpr, const std::string& name,
                                            const char* type, const char* heap) -> Status {
        if (sgpr > 127u || scalar_registers.count(sgpr) == 0) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "Bindless resource index SGPR is outside the translated register set");
        }
        hlsl << type << " " << name << " = " << heap
             << "[NonUniformResourceIndex(sx_s"
             << sgpr << ")];\n";
        return Status::Ok();
    };
    auto append_instruction = [&](const Rdna2Instruction& instruction) -> Status {
        std::string a, b, c;
        const std::string& mnemonic = instruction.mnemonic;
        if (mnemonic == "s_nop") return Status::Ok();
        if (mnemonic == "s_endpgm" || IsBranch(mnemonic)) return Status::Ok();
        if (mnemonic == "s_waitcnt") {
            hlsl << "DeviceMemoryBarrier();\n";
            return Status::Ok();
        }
        if (mnemonic == "s_sendmsg") {
            if (module.stage != ShaderStage::Mesh ||
                (static_cast<uint32_t>(instruction.immediate) & 0xfu) != 9u ||
                instruction.sources.empty()) {
                return UnsupportedInstruction(instruction);
            }
            Status status = source_at(instruction, 0, a);
            if (!status.ok()) return status;
            hlsl << "if (gi == 0u) sx_mesh_counts = " << a << ";\n";
            return Status::Ok();
        }
        if (mnemonic == "s_barrier") {
            if (module.stage != ShaderStage::Compute &&
                module.stage != ShaderStage::Mesh) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "s_barrier is only supported in compute and mesh shaders");
            }
            hlsl << "GroupMemoryBarrierWithGroupSync();\n";
            return Status::Ok();
        }
        if (instruction.encoding == Rdna2Encoding::Vinterp) {
            if (module.stage != ShaderStage::Pixel ||
                instruction.destinations.empty()) {
                return UnsupportedInstruction(instruction);
            }
            constexpr std::array<const char*, 4> components = {"x", "y", "z", "w"};
            hlsl << lane_guard() << "sx_v"
                 << instruction.destinations[0].index << " = asuint(input.attr"
                 << static_cast<uint32_t>(instruction.interpolation_attribute)
                 << "." << components[instruction.interpolation_channel] << ");\n";
            return Status::Ok();
        }
        if (instruction.encoding == Rdna2Encoding::Ds) {
            const bool load = mnemonic.rfind("ds_read_", 0) == 0;
            const bool store = mnemonic.rfind("ds_write_", 0) == 0;
            const bool atomic_add = mnemonic == "ds_add_u32";
            const uint32_t width =
                mnemonic.find("b128") != std::string::npos ? 4u :
                mnemonic.find("b64") != std::string::npos ? 2u : 1u;
            if ((!load && !store && !atomic_add) ||
                module.stage != ShaderStage::Compute ||
                instruction.sources.empty() ||
                (load && instruction.destinations.empty()) ||
                ((store || atomic_add) && instruction.sources.size() < 2)) {
                return UnsupportedInstruction(instruction);
            }
            const uint32_t address_register = instruction.sources[0].index;
            const uint32_t data_register =
                load ? instruction.destinations[0].index : instruction.sources[1].index;
            if (address_register >= 256u || data_register + width > 256u) {
                return UnsupportedInstruction(instruction);
            }
            for (uint32_t component = 0; component < width; ++component) {
                const std::string index_name =
                    "sx_lds_index_" + std::to_string(instruction.byte_offset) +
                    "_" + std::to_string(component);
                hlsl << "uint " << index_name << " = (sx_v" << address_register
                     << " + " << (static_cast<uint32_t>(instruction.ds_offset0) +
                                      component * 4u)
                     << "u) >> 2u;\n";
                if (load) {
                    hlsl << lane_guard() << "sx_v" << (data_register + component)
                         << " = " << index_name << " < 16384u ? sx_lds["
                         << index_name << "] : 0u;\n";
                } else if (atomic_add) {
                    hlsl << lane_guard() << "if (" << index_name
                         << " < 16384u) InterlockedAdd(sx_lds[" << index_name
                         << "], sx_v" << data_register << ");\n";
                } else {
                    hlsl << lane_guard() << "if (" << index_name
                         << " < 16384u) sx_lds[" << index_name << "] = sx_v"
                         << (data_register + component) << ";\n";
                }
            }
            return Status::Ok();
        }
        if (mnemonic == "s_mov_b64" || mnemonic == "s_not_b64" ||
            mnemonic == "s_cselect_b64" ||
            mnemonic == "s_and_b64" || mnemonic == "s_or_b64" ||
            mnemonic == "s_xor_b64" || mnemonic == "s_andn2_b64" ||
            mnemonic == "s_orn2_b64" || mnemonic == "s_and_saveexec_b64" ||
            mnemonic == "s_or_saveexec_b64" || mnemonic == "s_xor_saveexec_b64" ||
            mnemonic == "s_andn2_saveexec_b64") {
            if (instruction.destinations.empty() || instruction.sources.empty()) {
                return UnsupportedInstruction(instruction);
            }
            const std::string temporary = "sx_mask_tmp" + std::to_string(instruction.byte_offset);
            if (mnemonic.find("saveexec") != std::string::npos) {
                std::string source;
                Status status = pair_expression(instruction.sources[0], source);
                if (!status.ok()) return status;
                const char* op = mnemonic.find("andn2") != std::string::npos ? "& ~" :
                                 mnemonic.find("and") != std::string::npos ? "& " :
                                 mnemonic.find("or") != std::string::npos ? "| " : "^ ";
                hlsl << "uint2 " << temporary << " = sx_exec " << op << source << ";\n";
                status = emit_pair_store(instruction.destinations[0], "sx_exec");
                if (!status.ok()) return status;
                hlsl << "sx_exec = " << temporary << ";\n";
                return Status::Ok();
            }
            if (mnemonic == "s_cselect_b64") {
                if (instruction.sources.size() < 2) return UnsupportedInstruction(instruction);
                std::string when_set, when_clear;
                Status status = pair_expression(instruction.sources[0], when_set);
                if (!status.ok()) return status;
                status = pair_expression(instruction.sources[1], when_clear);
                if (!status.ok()) return status;
                hlsl << "uint2 " << temporary << " = sx_scc != 0u ? "
                     << when_set << " : " << when_clear << ";\n";
                return emit_pair_store(instruction.destinations[0], temporary);
            }
            std::string lhs;
            Status status = pair_expression(instruction.sources[0], lhs);
            if (!status.ok()) return status;
            if (mnemonic == "s_mov_b64") {
                hlsl << "uint2 " << temporary << " = " << lhs << ";\n";
                status = emit_pair_store(instruction.destinations[0], temporary);
                return status;
            }
            std::string rhs;
            if (mnemonic != "s_not_b64") {
                if (instruction.sources.size() < 2) return UnsupportedInstruction(instruction);
                status = pair_expression(instruction.sources[1], rhs);
                if (!status.ok()) return status;
            }
            hlsl << "uint2 " << temporary << " = ";
            if (mnemonic == "s_not_b64") hlsl << "~" << lhs;
            else hlsl << lhs << " "
                      << (mnemonic == "s_and_b64" ? "&" :
                          mnemonic == "s_or_b64" || mnemonic == "s_orn2_b64" ? "|" :
                          mnemonic == "s_xor_b64" ? "^" : "&")
                      << (mnemonic == "s_andn2_b64" || mnemonic == "s_orn2_b64" ? " ~" : " ")
                      << rhs;
            hlsl << ";\n";
            status = emit_pair_store(instruction.destinations[0], temporary);
            if (!status.ok()) return status;
            hlsl << "sx_scc = any(" << temporary << " != uint2(0u, 0u)) ? 1u : 0u;\n";
            return Status::Ok();
        }
        if (mnemonic == "s_cmp_eq_i32" || mnemonic == "s_cmp_eq_u32" ||
            mnemonic == "s_cmp_lg_i32" || mnemonic == "s_cmp_lg_u32" ||
            mnemonic == "s_cmp_lt_i32" || mnemonic == "s_cmp_lt_u32") {
            Status status = source_at(instruction, 0, a);
            if (!status.ok()) return status;
            status = source_at(instruction, 1, b);
            if (!status.ok()) return status;
            const bool signed_compare = mnemonic.find("_i32") != std::string::npos;
            const char* op = mnemonic.find("eq") != std::string::npos ? "==" :
                             mnemonic.find("lg") != std::string::npos ? "!=" : "<";
            const std::string left = signed_compare ? "asint(" + a + ")" : a;
            const std::string right = signed_compare ? "asint(" + b + ")" : b;
            hlsl << "sx_scc = (" << left << " " << op << " " << right
                 << ") ? 1u : 0u;\n";
            return Status::Ok();
        }
        if (mnemonic.rfind("v_cmp_", 0) == 0 &&
            (mnemonic.find("_f32") != std::string::npos ||
             mnemonic.find("_i32") != std::string::npos ||
             mnemonic.find("_u32") != std::string::npos) &&
            (mnemonic.find("_eq_") != std::string::npos ||
             mnemonic.find("_ne_") != std::string::npos ||
             mnemonic.find("_lg_") != std::string::npos ||
             mnemonic.find("_lt_") != std::string::npos ||
             mnemonic.find("_le_") != std::string::npos ||
             mnemonic.find("_gt_") != std::string::npos ||
             mnemonic.find("_ge_") != std::string::npos)) {
            Status status = source_at(instruction, 0, a);
            if (!status.ok()) return status;
            status = source_at(instruction, 1, b);
            if (!status.ok()) return status;
            const char* op = mnemonic.find("_eq_") != std::string::npos ? "==" :
                             mnemonic.find("_ne_") != std::string::npos ||
                                     mnemonic.find("_lg_") != std::string::npos ? "!=" :
                             mnemonic.find("_lt_") != std::string::npos ? "<" :
                             mnemonic.find("_le_") != std::string::npos ? "<=" :
                             mnemonic.find("_gt_") != std::string::npos ? ">" : ">=";
            const bool floating = mnemonic.find("_f32") != std::string::npos;
            const bool signed_integer = mnemonic.find("_i32") != std::string::npos;
            const std::string left = floating ? "asfloat(" + a + ")" :
                                     signed_integer ? "asint(" + a + ")" : a;
            const std::string right = floating ? "asfloat(" + b + ")" :
                                      signed_integer ? "asint(" + b + ")" : b;
            hlsl << "sx_vcc = WaveActiveBallot(SX_MASK_BIT(sx_exec) != 0u && "
                 << left << " " << op << " " << right << ").xy;\n";
            return Status::Ok();
        }
        if (mnemonic == "v_cndmask_b32") {
            if (instruction.destinations.empty()) return UnsupportedInstruction(instruction);
            Status status = source_at(instruction, 0, a);
            if (!status.ok()) return status;
            status = source_at(instruction, 1, b);
            if (!status.ok()) return status;
            hlsl << lane_guard() << "sx_v" << instruction.destinations[0].index
                 << " = SX_MASK_BIT(sx_vcc) != 0u ? " << b << " : " << a << ";\n";
            return Status::Ok();
        }
        if (mnemonic == "v_mov_b32" || mnemonic == "s_mov_b32" ||
            mnemonic == "s_movk_i32" || mnemonic == "s_addk_i32" ||
            mnemonic == "s_mulk_i32" || mnemonic == "s_cselect_b32" ||
            mnemonic == "v_add_f32" ||
            mnemonic == "v_sub_f32" || mnemonic == "v_mul_f32" ||
            mnemonic == "v_add_nc_u32" || mnemonic == "v_sub_nc_u32" ||
            mnemonic == "v_and_b32" || mnemonic == "v_or_b32" ||
            mnemonic == "v_xor_b32" || mnemonic == "s_add_u32" ||
            mnemonic == "s_sub_u32" || mnemonic == "s_and_b32" ||
            mnemonic == "s_or_b32" || mnemonic == "s_xor_b32") {
            if (instruction.destinations.empty()) return UnsupportedInstruction(instruction);
            Status status = source_at(instruction, 0, a);
            if (!status.ok()) return status;
            std::string expression = a;
            if (instruction.sources.size() > 1) {
                status = source_at(instruction, 1, b);
                if (!status.ok()) return status;
                const bool floating = mnemonic == "v_add_f32" ||
                                      mnemonic == "v_sub_f32" ||
                                      mnemonic == "v_mul_f32";
                const char* op = mnemonic.find("add") != std::string::npos ? "+" :
                                 mnemonic.find("sub") != std::string::npos ? "-" :
                                 mnemonic.find("and") != std::string::npos ? "&" :
                                 mnemonic.find("or") != std::string::npos ? "|" :
                                 mnemonic.find("xor") != std::string::npos ? "^" : "*";
                expression = mnemonic == "s_cselect_b32"
                    ? "(sx_scc != 0u ? " + a + " : " + b + ")"
                    : floating
                        ? "asuint(asfloat(" + a + ") " + op + " asfloat(" + b + "))"
                        : "(" + a + " " + op + " " + b + ")";
            }
            const Rdna2Operand& destination = instruction.destinations[0];
            if (destination.bank == Rdna2RegisterBank::Vector) {
                hlsl << lane_guard() << "sx_v" << destination.index << " = " << expression << ";\n";
            } else if (destination.bank == Rdna2RegisterBank::Scalar) {
                hlsl << "sx_s" << destination.index << " = " << expression << ";\n";
            } else if (destination.bank == Rdna2RegisterBank::M0) {
                hlsl << "sx_m0 = " << expression << ";\n";
            } else {
                return UnsupportedInstruction(instruction);
            }
            return Status::Ok();
        }
        if (mnemonic == "v_fma_f32") {
            if (instruction.destinations.empty()) return UnsupportedInstruction(instruction);
            Status status = source_at(instruction, 0, a);
            if (!status.ok()) return status;
            status = source_at(instruction, 1, b);
            if (!status.ok()) return status;
            status = source_at(instruction, 2, c);
            if (!status.ok()) return status;
            hlsl << lane_guard() << "sx_v" << instruction.destinations[0].index
                 << " = asuint(mad(asfloat(" << a << "), asfloat(" << b
                 << "), asfloat(" << c << ")));\n";
            return Status::Ok();
        }
        if (mnemonic == "v_bfe_u32" || mnemonic == "v_bfi_b32" ||
            mnemonic == "v_alignbit_b32" || mnemonic == "v_min3_f32" ||
            mnemonic == "v_max3_f32" || mnemonic == "v_med3_f32") {
            if (instruction.destinations.empty()) {
                return UnsupportedInstruction(instruction);
            }
            Status status = source_at(instruction, 0, a);
            if (!status.ok()) return status;
            status = source_at(instruction, 1, b);
            if (!status.ok()) return status;
            status = source_at(instruction, 2, c);
            if (!status.ok()) return status;
            std::string expression;
            if (mnemonic == "v_bfe_u32") {
                expression = "((" + a + " >> (" + b +
                    " & 31u)) & (" + c + " >= 32u ? 0xffffffffu : ((1u << (" +
                    c + " & 31u)) - 1u)))";
            } else if (mnemonic == "v_bfi_b32") {
                expression = "((" + b + " & " + a + ") | (" + c +
                    " & ~" + a + "))";
            } else if (mnemonic == "v_alignbit_b32") {
                expression = "((" + c + " & 31u) == 0u ? " + b +
                    " : ((" + a + " << (32u - (" + c +
                    " & 31u))) | (" + b + " >> (" + c + " & 31u))))";
            } else {
                const std::string fa = "asfloat(" + a + ")";
                const std::string fb = "asfloat(" + b + ")";
                const std::string fc = "asfloat(" + c + ")";
                if (mnemonic == "v_min3_f32") {
                    expression = "asuint(min(min(" + fa + ", " + fb + "), " + fc + "))";
                } else if (mnemonic == "v_max3_f32") {
                    expression = "asuint(max(max(" + fa + ", " + fb + "), " + fc + "))";
                } else {
                    expression = "asuint(max(min(" + fa + ", " + fb +
                        "), min(max(" + fa + ", " + fb + "), " + fc + ")))";
                }
            }
            hlsl << lane_guard() << "sx_v"
                 << instruction.destinations[0].index << " = "
                 << expression << ";\n";
            return Status::Ok();
        }
        if (instruction.encoding == Rdna2Encoding::Mimg) {
            const bool load = mnemonic == "image_load" || mnemonic == "image_load_mip";
            const bool sample = mnemonic == "image_sample" || mnemonic == "image_sample_l" ||
                                mnemonic == "image_sample_b" || mnemonic == "image_sample_d";
            if ((!load && !sample) || instruction.texture_dimension != 1 ||
                instruction.nsa_dwords != 0 || instruction.destinations.empty()) {
                return UnsupportedInstruction(instruction);
            }
            const uint32_t address = instruction.sources[0].index;
            uint32_t source_count = load
                ? (mnemonic == "image_load_mip" ? 3u : 2u)
                : mnemonic == "image_sample_d" ? 6u
                  : (mnemonic == "image_sample_l" || mnemonic == "image_sample_b") ? 3u : 2u;
            if (address + source_count > 256u) return UnsupportedInstruction(instruction);
            std::vector<std::string> coordinates;
            for (uint32_t i = 0; i < source_count; ++i) {
                coordinates.push_back("sx_v" + std::to_string(address + i));
            }
            const uint32_t texture_slot = instruction.resource_sgpr / 4;
            const uint32_t sampler_slot = instruction.sampler_sgpr / 4;
            std::string texture_name = "sx_texture" + std::to_string(texture_slot);
            std::string sampler_name = "sx_sampler" + std::to_string(sampler_slot);
            if (module.use_bindless_resources) {
                texture_name = "sx_texture_heap_" +
                               std::to_string(instruction.byte_offset);
                Status status = bindless_resource_expression(
                    instruction.resource_sgpr, texture_name,
                    "Texture2D<float4>", "ResourceDescriptorHeap");
                if (!status.ok()) return status;
            }
            if (sample && module.use_bindless_samplers) {
                sampler_name = "sx_sampler_heap_" +
                               std::to_string(instruction.byte_offset);
                Status status = bindless_resource_expression(
                    instruction.sampler_sgpr, sampler_name,
                    "SamplerState", "SamplerDescriptorHeap");
                if (!status.ok()) return status;
            }
            if ((mnemonic == "image_sample" || mnemonic == "image_sample_b") &&
                module.stage == ShaderStage::Compute) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "Implicit-LOD and bias sampling are unavailable in compute shaders");
            }
            for (uint32_t component = 0, dest_component = 0; component < 4; ++component) {
                if ((instruction.component_mask & (1u << component)) == 0) continue;
                std::string value;
                if (load) {
                    value = "asuint(" + texture_name +
                            ".Load(int3(int2(asint(" + coordinates[0] + "), asint(" +
                            coordinates[1] + ")), " +
                            (source_count > 2 ? "asint(" + coordinates[2] + ")" : "0") +
                            "))." + std::string(1, "xyzw"[component]) + ")";
                } else {
                    const size_t uv = mnemonic == "image_sample_d" ? 4u : 0u;
                    std::string method = mnemonic == "image_sample" ? "Sample" :
                                         mnemonic == "image_sample_l" ? "SampleLevel" :
                                         mnemonic == "image_sample_b" ? "SampleBias" : "SampleGrad";
                    std::string args = sampler_name +
                        ", float2(asfloat(" + coordinates[uv] + "), asfloat(" +
                        coordinates[uv + 1] + "))";
                    if (mnemonic == "image_sample_l" || mnemonic == "image_sample_b") {
                        args += ", asfloat(" + coordinates[2] + ")";
                    } else if (mnemonic == "image_sample_d") {
                        args += ", float2(asfloat(" + coordinates[0] + "), asfloat(" +
                                coordinates[1] + ")), float2(asfloat(" + coordinates[2] +
                                "), asfloat(" + coordinates[3] + "))";
                    }
                    value = "asuint(" + texture_name + "." +
                            method + "(" + args + ")." +
                            std::string(1, "xyzw"[component]) + ")";
                }
                hlsl << lane_guard() << "sx_v"
                     << (instruction.destinations[0].index + dest_component++)
                     << " = " << value << ";\n";
            }
            return Status::Ok();
        }
        if (mnemonic == "buffer_load_dword" || mnemonic == "buffer_store_dword" ||
            mnemonic == "buffer_atomic_add" || mnemonic == "buffer_atomic_cmpswap") {
            const uint32_t slot = instruction.resource_sgpr / 4;
            const bool store = mnemonic == "buffer_store_dword";
            const bool atomic_add = mnemonic == "buffer_atomic_add";
            const bool atomic_cmpswap = mnemonic == "buffer_atomic_cmpswap";
            const bool atomic = atomic_add || atomic_cmpswap;
            if (instruction.idxen || instruction.resource_sgpr % 4 != 0 ||
                (instruction.scalar_offset_code != 125 &&
                 instruction.scalar_offset_code != 128)) {
                return UnsupportedInstruction(instruction);
            }
            if ((!store && !atomic && instruction.destinations.empty()) ||
                (store && instruction.sources.empty()) ||
                (atomic && instruction.sources.size() <
                               (atomic_cmpswap ? 2u : 1u))) {
                return UnsupportedInstruction(instruction);
            }
            const std::string resource_name =
                module.use_bindless_resources
                    ? "sx_buffer_heap_" + std::to_string(instruction.byte_offset)
                    : (store || atomic
                           ? "sx_uav" + std::to_string(slot)
                           : "sx_srv" + std::to_string(slot));
            if (module.use_bindless_resources) {
                Status status = bindless_resource_expression(
                    instruction.resource_sgpr, resource_name,
                    (store || atomic) ? "RWByteAddressBuffer" : "ByteAddressBuffer",
                    "ResourceDescriptorHeap");
                if (!status.ok()) return status;
            }
            std::string address = "0u";
            if (instruction.offen) {
                const size_t address_index = atomic
                    ? instruction.sources.size() - 1
                    : store ? 1u : 0u;
                Status status = source_at(instruction, address_index, address);
                if (!status.ok()) return status;
            }
            const std::string offset = std::to_string(instruction.immediate) + "u";
            const std::string byte_address =
                "(" + address + " + " + offset + ")";
            if (atomic_add || atomic_cmpswap) {
                Status status = source_at(instruction, 0, a);
                if (!status.ok()) return status;
                if (atomic_cmpswap) {
                    status = source_at(instruction, 1, b);
                    if (!status.ok()) return status;
                }
                const std::string original_name =
                    "sx_atomic_old_" + std::to_string(instruction.byte_offset);
                hlsl << "uint " << original_name << " = 0u;\n"
                     << lane_guard();
                if (atomic_add) {
                    hlsl << resource_name << ".InterlockedAdd(" << byte_address
                         << ", " << a << ", " << original_name << ");\n";
                } else {
                    hlsl << resource_name << ".InterlockedCompareExchange("
                         << byte_address << ", " << a << ", " << b << ", "
                         << original_name << ");\n";
                }
                if (!instruction.destinations.empty()) {
                    hlsl << lane_guard() << "sx_v"
                         << instruction.destinations[0].index << " = "
                         << original_name << ";\n";
                }
            } else if (store) {
                Status status = source_at(instruction, 0, a);
                if (!status.ok()) return status;
                hlsl << lane_guard() << resource_name << ".Store("
                     << byte_address << ", " << a << ");\n";
            } else {
                hlsl << lane_guard() << "sx_v" << instruction.destinations[0].index
                     << " = " << resource_name << ".Load("
                     << byte_address << ");\n";
            }
            return Status::Ok();
        }
        if (mnemonic == "v_cvt_f16_f32" || mnemonic == "v_cvt_f32_f16") {
            if (instruction.destinations.empty() ||
                instruction.destinations[0].bank != Rdna2RegisterBank::Vector ||
                instruction.sources.empty()) {
                return UnsupportedInstruction(instruction);
            }
            Status status = source_at(instruction, 0, a);
            if (!status.ok()) return status;
            const uint32_t destination = instruction.destinations[0].index;
            if (mnemonic == "v_cvt_f16_f32") {
                hlsl << lane_guard() << "sx_v" << destination
                     << " = (sx_v" << destination
                     << " & 0xffff0000u) | (f32tof16(asfloat(" << a
                     << ")) & 0xffffu);\n";
            } else {
                hlsl << lane_guard() << "sx_v" << destination
                     << " = asuint(f16tof32((" << a << ") & 0xffffu));\n";
            }
            return Status::Ok();
        }
        if (mnemonic == "v_pk_add_f16" || mnemonic == "v_pk_mul_f16" ||
            mnemonic == "v_pk_fma_f16") {
            const size_t source_count = mnemonic == "v_pk_fma_f16" ? 3u : 2u;
            if (instruction.destinations.empty() ||
                instruction.destinations[0].bank != Rdna2RegisterBank::Vector ||
                instruction.sources.size() < source_count) {
                return UnsupportedInstruction(instruction);
            }
            const std::string suffix = std::to_string(instruction.byte_offset);
            std::array<std::string, 3> source_names{};
            for (size_t source_index = 0; source_index < source_count; ++source_index) {
                std::string low, high;
                Status status = packed_half_source(
                    instruction, source_index, 0, low);
                if (!status.ok()) return status;
                status = packed_half_source(
                    instruction, source_index, 1, high);
                if (!status.ok()) return status;
                source_names[source_index] =
                    "sx_f16_src_" + suffix + "_" + std::to_string(source_index);
                hlsl << "half2 " << source_names[source_index] << " = half2("
                     << low << ", " << high << ");\n";
            }
            const std::string result_name = "sx_f16_result_" + suffix;
            hlsl << "half2 " << result_name << " = ";
            if (mnemonic == "v_pk_add_f16") {
                hlsl << source_names[0] << " + " << source_names[1];
            } else if (mnemonic == "v_pk_mul_f16") {
                hlsl << source_names[0] << " * " << source_names[1];
            } else {
                hlsl << "mad(" << source_names[0] << ", " << source_names[1]
                     << ", " << source_names[2] << ")";
            }
            hlsl << ";\n";
            const std::string packed_result = "sx_f16_packed_" + suffix;
            hlsl << "uint " << packed_result
                 << " = (f32tof16((float)" << result_name
                 << ".x) & 0xffffu) | ((f32tof16((float)" << result_name
                 << ".y) & 0xffffu) << 16u);\n"
                 << lane_guard() << "sx_v"
                 << instruction.destinations[0].index << " = "
                 << packed_result << ";\n";
            return Status::Ok();
        }
        if (mnemonic == "exp") {
            if (module.stage == ShaderStage::Mesh) {
                const uint32_t target = static_cast<uint32_t>(instruction.immediate);
                if (target == 20u) {
                    if ((instruction.component_mask & 1u) == 0) {
                        return Status::Ok();
                    }
                    Status status = source_at(instruction, 0, a);
                    if (!status.ok()) return status;
                    hlsl << lane_guard() << "sx_mesh_primitive = " << a << ";\n";
                    return Status::Ok();
                }
                const std::string object =
                    target == 12u
                        ? "sx_mesh_output.position"
                        : target >= 32u && target <= 63u
                              ? "sx_mesh_output.attr" + std::to_string(target - 32u)
                              : std::string{};
                if (object.empty()) return UnsupportedInstruction(instruction);
                for (uint32_t component = 0; component < 4; ++component) {
                    if ((instruction.component_mask & (1u << component)) == 0) continue;
                    Status status = source_at(instruction, component, a);
                    if (!status.ok()) return status;
                    hlsl << lane_guard() << object << "." << "xyzw"[component]
                         << " = asfloat(" << a << ");\n";
                }
                return Status::Ok();
            }
            if (instruction.immediate != expected_export) return UnsupportedInstruction(instruction);
            for (uint32_t component = 0; component < 4; ++component) {
                if ((instruction.component_mask & (1u << component)) == 0) continue;
                Status status = source_at(instruction, component, a);
                if (!status.ok()) return status;
                hlsl << lane_guard() << "sx_out" << component << " = " << a << ";\n";
            }
            return Status::Ok();
        }
        return UnsupportedInstruction(instruction);
    };

    using BlockSet = std::set<uint32_t>;
    const uint32_t block_count = static_cast<uint32_t>(module.basic_blocks.size());
    std::vector<std::vector<uint32_t>> successors(block_count);
    std::vector<std::vector<uint32_t>> predecessors(block_count);
    auto block_terminator = [&](uint32_t index) -> const Rdna2Instruction& {
        const Rdna2BasicBlock& block = module.basic_blocks[index];
        return module.decoded_program[block.first_instruction + block.instruction_count - 1];
    };
    for (uint32_t block = 0; block < block_count; ++block) {
        const Rdna2BasicBlock& info = module.basic_blocks[block];
        if (info.conditional_branch) {
            successors[block] = {info.branch_target, info.fallthrough_target};
        } else if (info.unconditional_branch) {
            successors[block] = {info.branch_target};
        } else if (!info.terminates) {
            successors[block] = {info.fallthrough_target};
        }
        for (uint32_t successor : successors[block]) {
            if (successor >= block_count) {
                return Status::Error(StatusCode::ShaderError, "CFG successor is outside the block table");
            }
            predecessors[successor].push_back(block);
        }
    }

    std::vector<std::set<uint32_t>> dominators(block_count);
    for (uint32_t block = 0; block < block_count; ++block) {
        if (block == 0) dominators[block].insert(0);
        else {
            for (uint32_t candidate = 0; candidate < block_count; ++candidate) {
                dominators[block].insert(candidate);
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t block = 1; block < block_count; ++block) {
            if (predecessors[block].empty()) continue;
            BlockSet next = dominators[predecessors[block][0]];
            for (size_t i = 1; i < predecessors[block].size(); ++i) {
                BlockSet intersection;
                const BlockSet& other = dominators[predecessors[block][i]];
                std::set_intersection(
                    next.begin(), next.end(), other.begin(), other.end(),
                    std::inserter(intersection, intersection.end()));
                next = std::move(intersection);
            }
            next.insert(block);
            if (next != dominators[block]) {
                dominators[block] = std::move(next);
                changed = true;
            }
        }
    }

    std::map<uint32_t, BlockSet> natural_loops;
    std::set<std::pair<uint32_t, uint32_t>> back_edges;
    for (uint32_t source = 0; source < block_count; ++source) {
        for (uint32_t target : successors[source]) {
            if (dominators[source].count(target) == 0) continue;
            back_edges.emplace(source, target);
            BlockSet& loop = natural_loops[target];
            loop.insert(target);
            loop.insert(source);
            std::vector<uint32_t> work = {source};
            while (!work.empty()) {
                const uint32_t member = work.back();
                work.pop_back();
                if (member == target) continue;
                for (uint32_t predecessor : predecessors[member]) {
                    if (dominators[predecessor].count(target) == 0) continue;
                    if (loop.insert(predecessor).second) work.push_back(predecessor);
                }
            }
        }
    }
    for (auto first = natural_loops.begin(); first != natural_loops.end(); ++first) {
        for (auto second = std::next(first); second != natural_loops.end(); ++second) {
            bool overlaps = false;
            for (uint32_t block : first->second) {
                overlaps |= second->second.count(block) != 0;
            }
            const bool first_contains_second =
                std::includes(first->second.begin(), first->second.end(),
                              second->second.begin(), second->second.end());
            const bool second_contains_first =
                std::includes(second->second.begin(), second->second.end(),
                              first->second.begin(), first->second.end());
            if (overlaps && !first_contains_second && !second_contains_first) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "Overlapping natural loops cannot be represented as structured HLSL");
            }
        }
    }

    std::vector<BlockSet> postdominators(block_count);
    for (uint32_t block = 0; block < block_count; ++block) {
        for (uint32_t candidate = 0; candidate < block_count; ++candidate) {
            postdominators[block].insert(candidate);
        }
    }
    changed = true;
    while (changed) {
        changed = false;
        for (uint32_t block = 0; block < block_count; ++block) {
            BlockSet next;
            bool has_successor = false;
            for (uint32_t successor : successors[block]) {
                if (back_edges.count({block, successor}) != 0) continue;
                if (!has_successor) {
                    next = postdominators[successor];
                    has_successor = true;
                } else {
                    BlockSet intersection;
                    std::set_intersection(
                        next.begin(), next.end(),
                        postdominators[successor].begin(),
                        postdominators[successor].end(),
                        std::inserter(intersection, intersection.end()));
                    next = std::move(intersection);
                }
            }
            next.insert(block);
            if (next != postdominators[block]) {
                postdominators[block] = std::move(next);
                changed = true;
            }
        }
    }

    std::map<uint32_t, BlockSet> loop_exit_targets;
    for (const auto& [header, loop] : natural_loops) {
        BlockSet exits;
        for (uint32_t member : loop) {
            for (uint32_t successor : successors[member]) {
                if (loop.count(successor) == 0) exits.insert(successor);
            }
        }
        if (exits.empty()) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "Natural loop has no reachable exit");
        }
        loop_exit_targets.emplace(header, std::move(exits));
    }

    std::set<uint32_t> emitted_blocks;
    auto emit_body = [&](uint32_t index) -> Status {
        const Rdna2BasicBlock& block = module.basic_blocks[index];
        size_t end = block.first_instruction + block.instruction_count;
        const Rdna2Instruction& terminator = block_terminator(index);
        if (IsBranch(terminator.mnemonic) || terminator.mnemonic == "s_endpgm") --end;
        for (size_t i = block.first_instruction; i < end; ++i) {
            Status status = append_instruction(module.decoded_program[i]);
            if (!status.ok()) return status;
        }
        return Status::Ok();
    };
    auto branch_condition = [&](const Rdna2Instruction& terminator) {
        if (terminator.mnemonic == "s_cbranch_scc0") return std::string("sx_scc == 0u");
        if (terminator.mnemonic == "s_cbranch_scc1") return std::string("sx_scc != 0u");
        if (terminator.mnemonic == "s_cbranch_vccz") return std::string("all(sx_vcc == uint2(0u, 0u))");
        if (terminator.mnemonic == "s_cbranch_vccnz") return std::string("any(sx_vcc != uint2(0u, 0u))");
        if (terminator.mnemonic == "s_cbranch_execz") return std::string("all(sx_exec == uint2(0u, 0u))");
        return std::string("any(sx_exec != uint2(0u, 0u))");
    };
    auto find_join = [&](uint32_t first, uint32_t second) {
        BlockSet common;
        std::set_intersection(
            postdominators[first].begin(), postdominators[first].end(),
            postdominators[second].begin(), postdominators[second].end(),
            std::inserter(common, common.end()));
        uint32_t join = 0xffffffffu;
        size_t depth = 0;
        for (uint32_t candidate : common) {
            if (join == 0xffffffffu || postdominators[candidate].size() > depth) {
                join = candidate;
                depth = postdominators[candidate].size();
            }
        }
        return join;
    };

    auto find_exit_join = [&](const BlockSet& exits) {
        BlockSet common;
        bool first = true;
        for (uint32_t exit : exits) {
            if (first) {
                common = postdominators[exit];
                first = false;
                continue;
            }
            BlockSet intersection;
            std::set_intersection(
                common.begin(), common.end(),
                postdominators[exit].begin(), postdominators[exit].end(),
                std::inserter(intersection, intersection.end()));
            common = std::move(intersection);
        }
        uint32_t join = 0xffffffffu;
        size_t depth = 0;
        for (uint32_t candidate : common) {
            if (join == 0xffffffffu ||
                postdominators[candidate].size() > depth) {
                join = candidate;
                depth = postdominators[candidate].size();
            }
        }
        return join;
    };
    auto emit_loop_exit_value = [&](uint32_t header, uint32_t target) {
        const BlockSet& exits = loop_exit_targets.at(header);
        if (exits.size() <= 1) return;
        const auto found = exits.find(target);
        const uint32_t value = found == exits.end()
            ? 0xffffffffu
            : static_cast<uint32_t>(std::distance(exits.begin(), found));
        hlsl << "sx_loop_exit_" << header << " = " << value << "u;\n";
    };

    auto emit_sequence = [&](auto&& self, uint32_t current, uint32_t stop,
                             uint32_t active_header, bool loop_entry) -> Status {
        while (current != stop && current < block_count) {
            if (active_header != 0xffffffffu) {
                const BlockSet& active_loop = natural_loops.at(active_header);
                if (current == active_header && !loop_entry) {
                    hlsl << "continue;\n";
                    return Status::Ok();
                }
                if (active_loop.count(current) == 0) {
                    emit_loop_exit_value(active_header, current);
                    hlsl << "break;\n";
                    return Status::Ok();
                }
            }
            loop_entry = false;
            const auto natural_loop = natural_loops.find(current);
            if (natural_loop != natural_loops.end() && current != active_header) {
                const BlockSet& exits = loop_exit_targets.at(current);
                if (exits.size() > 1) {
                    hlsl << "uint sx_loop_exit_" << current
                         << " = 0xffffffffu;\n";
                }
                hlsl << "[loop] while (true) {\n";
                Status body_status = self(self, current, 0xffffffffu, current, true);
                if (!body_status.ok()) return body_status;
                hlsl << "}\n";
                if (exits.size() == 1) {
                    current = *exits.begin();
                    continue;
                }

                const uint32_t join = find_exit_join(exits);
                const uint32_t arm_stop =
                    join == 0xffffffffu ? 0xffffffffu : join;
                size_t exit_index = 0;
                for (uint32_t exit : exits) {
                    hlsl << (exit_index == 0 ? "if (" : "else if (")
                         << "sx_loop_exit_" << current << " == "
                         << exit_index << "u) {\n";
                    body_status = self(
                        self, exit, arm_stop, active_header, false);
                    if (!body_status.ok()) return body_status;
                    hlsl << "}\n";
                    ++exit_index;
                }
                if (join == 0xffffffffu) return Status::Ok();
                current = join;
                continue;
            }
            if (emitted_blocks.count(current) != 0) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "CFG region re-enters an emitted block outside a natural loop");
            }
            emitted_blocks.insert(current);
            Status body_status = emit_body(current);
            if (!body_status.ok()) return body_status;

            const Rdna2BasicBlock& block = module.basic_blocks[current];
            const Rdna2Instruction& terminator = block_terminator(current);
            if (terminator.mnemonic == "s_endpgm") {
                if (module.stage == ShaderStage::Compute) {
                    hlsl << "return;\n";
                } else if (module.stage != ShaderStage::Mesh) {
                    hlsl << "return float4(asfloat(sx_out0), asfloat(sx_out1), "
                            "asfloat(sx_out2), asfloat(sx_out3));\n";
                }
                return Status::Ok();
            }
            if (block.unconditional_branch) {
                const uint32_t target = block.branch_target;
                if (active_header != 0xffffffffu) {
                    const BlockSet& loop = natural_loops.at(active_header);
                    if (target == active_header) {
                        hlsl << "continue;\n";
                        return Status::Ok();
                    }
                    if (loop.count(target) == 0) {
                        emit_loop_exit_value(active_header, target);
                        hlsl << "break;\n";
                        return Status::Ok();
                    }
                }
                current = target;
                continue;
            }
            if (!block.conditional_branch) {
                current = block.fallthrough_target;
                continue;
            }

            const uint32_t taken = block.branch_target;
            const uint32_t fallthrough = block.fallthrough_target;
            const std::string condition = branch_condition(terminator);
            if (active_header != 0xffffffffu) {
                const BlockSet& loop = natural_loops.at(active_header);
                const bool taken_exits = loop.count(taken) == 0;
                const bool fallthrough_exits = loop.count(fallthrough) == 0;
                if (taken_exits && fallthrough_exits) {
                    if (taken == fallthrough) {
                        emit_loop_exit_value(active_header, taken);
                        hlsl << "break;\n";
                    } else {
                        hlsl << "if (" << condition << ") {\n";
                        emit_loop_exit_value(active_header, taken);
                        hlsl << "break;\n} else {\n";
                        emit_loop_exit_value(active_header, fallthrough);
                        hlsl << "break;\n}\n";
                    }
                    return Status::Ok();
                }
                if (taken_exits != fallthrough_exits) {
                    const uint32_t exit_target = taken_exits ? taken : fallthrough;
                    hlsl << "if (" << (taken_exits ? condition : "!(" + condition)
                         << (taken_exits ? ") {\n" : ")) {\n");
                    emit_loop_exit_value(active_header, exit_target);
                    hlsl << "break;\n}\n";
                    current = taken_exits ? fallthrough : taken;
                    continue;
                }
                if (taken == active_header || fallthrough == active_header) {
                    const bool taken_back = taken == active_header;
                    hlsl << "if (" << (taken_back ? condition : "!(" + condition)
                         << (taken_back ? ") continue;\n" : ")) continue;\n");
                    current = taken_back ? fallthrough : taken;
                    continue;
                }
            }

            const uint32_t join = find_join(taken, fallthrough);
            hlsl << "if (" << condition << ") {\n";
            const uint32_t arm_stop =
                join == 0xffffffffu || join == current ? 0xffffffffu : join;
            body_status = self(self, taken, arm_stop, active_header, false);
            if (!body_status.ok()) return body_status;
            hlsl << "} else {\n";
            body_status = self(self, fallthrough, arm_stop, active_header, false);
            if (!body_status.ok()) return body_status;
            hlsl << "}\n";
            if (arm_stop == 0xffffffffu) return Status::Ok();
            current = join;
        }
        return Status::Ok();
    };
    Status structured_status = emit_sequence(
        emit_sequence, 0, 0xffffffffu, 0xffffffffu, false);
    if (!structured_status.ok()) return structured_status;
    if (module.stage == ShaderStage::Mesh) {
        hlsl << "GroupMemoryBarrierWithGroupSync();\n"
             << "uint sx_mesh_nv = min(sx_mesh_counts & 0xfffu, "
             << module.mesh_max_vertices << "u);\n"
             << "uint sx_mesh_np = min((sx_mesh_counts >> 12u) & 0xfffu, "
             << module.mesh_max_primitives << "u);\n"
                "SetMeshOutputCounts(sx_mesh_nv, sx_mesh_np);\n"
                "if (gi < sx_mesh_nv) sx_vertices[gi] = sx_mesh_output;\n"
                "if (gi < sx_mesh_np) {\n"
                "sx_triangles[gi] = uint3(sx_mesh_primitive & 0x1ffu, "
                "(sx_mesh_primitive >> 10u) & 0x1ffu, "
                "(sx_mesh_primitive >> 20u) & 0x1ffu);\n"
                "sx_primitives[gi].cull = (sx_mesh_primitive & 0x80000000u) != 0u;\n"
                "}\n";
    } else if (module.stage == ShaderStage::Pixel ||
               module.stage == ShaderStage::Vertex) {
        hlsl << "return float4(asfloat(sx_out0), asfloat(sx_out1), "
                "asfloat(sx_out2), asfloat(sx_out3));\n";
    }
    hlsl << "}\n";
    out_hlsl = hlsl.str();
    return Status::Ok();
}

}  // namespace

Status GenerateRdna2Hlsl(const Rdna2IrModule& module, std::string& out_hlsl) {
    out_hlsl.clear();
    if (module.has_control_flow) {
        return GenerateControlFlowHlsl(module, out_hlsl);
    }
    if ((module.stage == ShaderStage::Pixel || module.stage == ShaderStage::Vertex) &&
        module.has_input_vgpr && module.max_input_vgpr > 63) {
        return Status::Error(StatusCode::UnsupportedFormat, "Graphics shader input VGPR exceeds the supported signature range");
    }

    std::set<uint32_t> texture_slots;
    std::set<uint32_t> sampler_slots;
    std::set<uint32_t> srv_slots;
    std::set<uint32_t> uav_slots;
    std::set<uint32_t> flat_pixel_attributes;
    std::set<uint32_t> mesh_parameter_targets;
    for (const Rdna2IrInstruction& instruction : module.instructions) {
        if (instruction.opcode == Rdna2IrOpcode::PixelInterpolate &&
            instruction.interpolation_flat) {
            flat_pixel_attributes.insert(instruction.interpolation_attribute);
        } else if (instruction.opcode == Rdna2IrOpcode::Export &&
                   instruction.export_target >= 32u &&
                   instruction.export_target <= 63u) {
            mesh_parameter_targets.insert(instruction.export_target - 32u);
        } else if (instruction.opcode == Rdna2IrOpcode::TextureLoadComponent) {
            texture_slots.insert(instruction.resource_slot);
        } else if (instruction.opcode == Rdna2IrOpcode::TextureSampleComponent) {
            texture_slots.insert(instruction.resource_slot);
            sampler_slots.insert(instruction.sampler_slot);
        } else if (instruction.opcode == Rdna2IrOpcode::BufferLoad) {
            srv_slots.insert(instruction.resource_slot);
        } else if (instruction.opcode == Rdna2IrOpcode::BufferStore) {
            uav_slots.insert(instruction.resource_slot);
        }
    }
    for (uint32_t slot : texture_slots) {
        if (srv_slots.count(slot) != 0) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                "A single root SRV slot is used as both a texture and a byte-address buffer");
        }
    }
    if (!sampler_slots.empty() && *sampler_slots.rbegin() >= module.root_sampler_count) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Generated HLSL references a sampler outside the root-signature range");
    }

    std::ostringstream hlsl;
    if (module.uses_scalar_input) {
        if (module.root_cbv_count == 0) {
            return Status::Error(StatusCode::UnsupportedFormat, "No root CBV is available for scalar inputs");
        }
        hlsl << "cbuffer SXUserData : register(b" << module.first_cbv_register
             << ") { uint4 sx_user_data[64]; };\n";
    }
    for (uint32_t slot : srv_slots) {
        hlsl << "ByteAddressBuffer sx_srv" << slot << " : register(t" << slot << ");\n";
    }
    for (uint32_t slot : texture_slots) {
        hlsl << "Texture2D<float4> sx_texture" << slot
             << " : register(t" << slot << ");\n";
    }
    for (uint32_t slot : sampler_slots) {
        hlsl << "SamplerState sx_sampler" << slot
             << " : register(s" << slot << ");\n";
    }
    for (uint32_t slot : uav_slots) {
        hlsl << "RWByteAddressBuffer sx_uav" << slot << " : register(u" << slot << ");\n";
    }

    bool has_input_parameter = module.has_input_vgpr;
    if (module.stage == ShaderStage::Pixel && has_input_parameter) {
        hlsl << "struct SXPixelInput { float4 position : SV_Position;\n";
        if (module.max_input_vgpr >= 4) {
            const uint32_t count = (module.max_input_vgpr - 4) / 4 + 1;
            for (uint32_t i = 0; i < count; ++i) {
                hlsl << (flat_pixel_attributes.count(i) ? "nointerpolation " : "")
                     << "float4 attr" << i << " : TEXCOORD" << i << ";\n";
            }
        }
        hlsl << "};\n";
    } else if (module.stage == ShaderStage::Vertex && has_input_parameter) {
        hlsl << "struct SXVertexInput { uint vertex_id : SV_VertexID; "
                "uint instance_id : SV_InstanceID;\n";
        if (module.max_input_vgpr >= 2) {
            const uint32_t count = (module.max_input_vgpr - 2) / 4 + 1;
            for (uint32_t i = 0; i < count; ++i) {
                hlsl << "float4 attr" << i << " : TEXCOORD" << i << ";\n";
            }
        }
        hlsl << "};\n";
    }
    if (module.stage == ShaderStage::Mesh) {
        hlsl << "struct SXMeshVertex { float4 position : SV_Position;\n";
        for (uint32_t target : mesh_parameter_targets) {
            hlsl << "float4 attr" << target << " : TEXCOORD" << target << ";\n";
        }
        hlsl << "};\n"
                "struct SXMeshPrimitive { bool cull : SV_CullPrimitive; };\n"
                "groupshared uint sx_mesh_counts;\n";
    }

    switch (module.stage) {
        case ShaderStage::Pixel:
            hlsl << "float4 main(";
            if (has_input_parameter) hlsl << "SXPixelInput input";
            hlsl << ") : SV_Target0 {\n";
            break;
        case ShaderStage::Vertex:
            hlsl << "float4 main(";
            if (has_input_parameter) hlsl << "SXVertexInput input";
            hlsl << ") : SV_Position {\n";
            break;
        case ShaderStage::Compute:
            hlsl << "[numthreads(" << module.thread_group_size_x << ", "
                 << module.thread_group_size_y << ", " << module.thread_group_size_z
                 << ")]\nvoid main(uint3 dispatch_id : SV_DispatchThreadID, "
                    "uint3 group_thread_id : SV_GroupThreadID, "
                    "uint3 group_id : SV_GroupID) {\n";
            break;
        case ShaderStage::Mesh:
            hlsl << "[numthreads(" << module.thread_group_size_x << ", "
                 << module.thread_group_size_y << ", " << module.thread_group_size_z
                 << ")]\n[outputtopology(\"triangle\")]\n"
                    "void main(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID, "
                    "out vertices SXMeshVertex sx_vertices["
                 << module.mesh_max_vertices << "], out indices uint3 sx_triangles["
                 << module.mesh_max_primitives << "], out primitives SXMeshPrimitive sx_primitives["
                 << module.mesh_max_primitives << "]) {\n"
                    "if (gi == 0u) sx_mesh_counts = 0u;\n"
                    "GroupMemoryBarrierWithGroupSync();\n"
                    "SXMeshVertex sx_mesh_output = (SXMeshVertex)0;\n"
                    "uint sx_mesh_primitive = 0u;\n";
            break;
        default:
            return Status::Error(StatusCode::UnsupportedFormat, "HLSL generation does not support this shader stage");
    }

    std::map<uint32_t, std::string> values;
    auto input_expression = [&](const Rdna2Operand& input, std::string& expression) -> Status {
        if (input.bank == Rdna2RegisterBank::M0) {
            expression = "sx_m0";
            return Status::Ok();
        }
        if (input.bank == Rdna2RegisterBank::Scalar) {
            if (module.stage == ShaderStage::Mesh && input.index == 3u) {
                expression = "((gi / 64u) << 24u)";
                return Status::Ok();
            }
            if (module.root_cbv_count == 0) {
                return Status::Error(StatusCode::UnsupportedFormat, "Unbound scalar input register");
            }
            expression = "sx_user_data[" + std::to_string(input.index / 4) + "][" +
                         std::to_string(input.index % 4) + "]";
            return Status::Ok();
        }
        if (input.bank != Rdna2RegisterBank::Vector) {
            return Status::Error(StatusCode::UnsupportedFormat, "Input special register has no HLSL mapping");
        }
        const uint32_t index = input.index;
        if (module.stage == ShaderStage::Mesh) {
            if (index != 5u) {
                return Status::Error(
                    StatusCode::UnsupportedFormat,
                    "Mesh VGPR input has no system-value mapping");
            }
            expression = "gid.x * " +
                         std::to_string(module.thread_group_size_x) +
                         "u + gi";
        } else if (module.stage == ShaderStage::Compute) {
            if (index > 8) {
                return Status::Error(StatusCode::UnsupportedFormat, "Compute VGPR input has no system-value mapping");
            }
            constexpr std::array<const char*, 3> axes = {"x", "y", "z"};
            if (index < 3) {
                expression = "asuint(dispatch_id." + std::string(axes[index]) + ")";
            } else if (index < 6) {
                expression = "asuint(group_thread_id." + std::string(axes[index - 3]) + ")";
            } else {
                expression = "asuint(group_id." + std::string(axes[index - 6]) + ")";
            }
        } else if (module.stage == ShaderStage::Pixel) {
            if (index < 4) {
                constexpr std::array<const char*, 4> components = {"x", "y", "z", "w"};
                expression = "asuint(input.position." + std::string(components[index]) + ")";
            } else {
                const uint32_t attribute = (index - 4) / 4;
                constexpr std::array<const char*, 4> components = {"x", "y", "z", "w"};
                expression = "asuint(input.attr" + std::to_string(attribute) + "." +
                             components[(index - 4) % 4] + ")";
            }
        } else {
            if (index == 0) {
                expression = "input.vertex_id";
            } else if (index == 1) {
                expression = "input.instance_id";
            } else {
                const uint32_t attribute = (index - 2) / 4;
                constexpr std::array<const char*, 4> components = {"x", "y", "z", "w"};
                expression = "asuint(input.attr" + std::to_string(attribute) + "." +
                             components[(index - 2) % 4] + ")";
            }
        }
        return Status::Ok();
    };

    if (module.stage == ShaderStage::Mesh) hlsl << "uint sx_m0 = 0u;\n";
    for (const Rdna2IrInstruction& instruction : module.instructions) {
        if (instruction.opcode == Rdna2IrOpcode::End) {
            continue;
        }
        if (instruction.opcode == Rdna2IrOpcode::Wait) {
            hlsl << "DeviceMemoryBarrier();\n";
            continue;
        }
        if (instruction.opcode == Rdna2IrOpcode::Barrier) {
            hlsl << "GroupMemoryBarrierWithGroupSync();\n";
            continue;
        }
        if (instruction.opcode == Rdna2IrOpcode::Export) {
            if (module.stage == ShaderStage::Mesh) {
                const uint32_t target = instruction.export_target;
                if (target == 20u) {
                    if ((instruction.component_mask & 1u) != 0) {
                        const auto found = values.find(instruction.operands[0]);
                        if (found == values.end()) {
                            return Status::Error(
                                StatusCode::ShaderError,
                                "Mesh primitive export references a missing SSA value");
                        }
                        hlsl << "sx_mesh_primitive = " << found->second << ";\n";
                    }
                    continue;
                }
                const std::string object =
                    target == 12u
                        ? "sx_mesh_output.position"
                        : target >= 32u && target <= 63u
                              ? "sx_mesh_output.attr" + std::to_string(target - 32u)
                              : std::string{};
                if (object.empty()) {
                    return Status::Error(
                        StatusCode::UnsupportedFormat,
                        "Mesh export target has no output mapping");
                }
                for (uint32_t component = 0; component < 4; ++component) {
                    if ((instruction.component_mask & (1u << component)) == 0) continue;
                    if (component >= instruction.operands.size()) {
                        return Status::Error(
                            StatusCode::ShaderError,
                            "Mesh vertex export has an invalid operand list");
                    }
                    const auto found = values.find(instruction.operands[component]);
                    if (found == values.end()) {
                        return Status::Error(
                            StatusCode::ShaderError,
                            "Mesh vertex export references a missing SSA value");
                    }
                    hlsl << object << "." << "xyzw"[component]
                         << " = asfloat(" << found->second << ");\n";
                }
                continue;
            }
            continue;
        }

        std::ostringstream expression;
        auto operand = [&](size_t index) -> const std::string* {
            if (index >= instruction.operands.size()) return nullptr;
            const auto found = values.find(instruction.operands[index]);
            return found == values.end() ? nullptr : &found->second;
        };
        const std::string result_name = ValueName(instruction.result);
        if (instruction.opcode == Rdna2IrOpcode::MeshAllocate) {
            if (module.stage != ShaderStage::Mesh || !operand(0)) {
                return Status::Error(
                    StatusCode::ShaderError,
                    "Mesh allocation references an unavailable count value");
            }
            hlsl << "if (gi == 0u) sx_mesh_counts = " << *operand(0) << ";\n";
            continue;
        }
        switch (instruction.opcode) {
            case Rdna2IrOpcode::InputRegister: {
                std::string input;
                Status status = input_expression(instruction.input_register, input);
                if (!status.ok()) return status;
                expression << input;
                break;
            }
            case Rdna2IrOpcode::Constant:
                expression << "0x" << std::hex << instruction.immediate_bits << "u";
                break;
            case Rdna2IrOpcode::Move:
                if (!operand(0)) return Status::Error(StatusCode::ShaderError, "IR move references a missing SSA value");
                expression << *operand(0);
                break;
            case Rdna2IrOpcode::AddF32:
            case Rdna2IrOpcode::SubF32:
            case Rdna2IrOpcode::MulF32:
                if (!operand(0) || !operand(1)) return Status::Error(StatusCode::ShaderError, "IR arithmetic references a missing SSA value");
                expression << "asuint(asfloat(" << *operand(0) << ") "
                          << (instruction.opcode == Rdna2IrOpcode::AddF32 ? "+" :
                              instruction.opcode == Rdna2IrOpcode::SubF32 ? "-" : "*")
                          << " asfloat(" << *operand(1) << "))";
                break;
            case Rdna2IrOpcode::FmaF32:
                if (!operand(0) || !operand(1) || !operand(2)) return Status::Error(StatusCode::ShaderError, "IR FMA references a missing SSA value");
                expression << "asuint(mad(asfloat(" << *operand(0) << "), asfloat("
                          << *operand(1) << "), asfloat(" << *operand(2) << ")))";
                break;
            case Rdna2IrOpcode::AddU32:
            case Rdna2IrOpcode::SubU32:
            case Rdna2IrOpcode::MulU32:
            case Rdna2IrOpcode::AndU32:
            case Rdna2IrOpcode::OrU32:
            case Rdna2IrOpcode::XorU32:
                if (!operand(0) || !operand(1)) return Status::Error(StatusCode::ShaderError, "IR integer operation references a missing SSA value");
                expression << *operand(0) << " "
                          << (instruction.opcode == Rdna2IrOpcode::AddU32 ? "+" :
                              instruction.opcode == Rdna2IrOpcode::SubU32 ? "-" :
                          instruction.opcode == Rdna2IrOpcode::MulU32 ? "*" :
                              instruction.opcode == Rdna2IrOpcode::AndU32 ? "&" :
                              instruction.opcode == Rdna2IrOpcode::OrU32 ? "|" : "^")
                          << " " << *operand(1);
                break;
            case Rdna2IrOpcode::BitfieldExtractU32:
                if (!operand(0) || !operand(1) || !operand(2)) return Status::Error(StatusCode::ShaderError, "IR bitfield extract references a missing SSA value");
                expression << "((" << *operand(0) << " >> (" << *operand(1)
                          << " & 31u)) & (" << *operand(2)
                          << " >= 32u ? 0xffffffffu : ((1u << (" << *operand(2)
                          << " & 31u)) - 1u)))";
                break;
            case Rdna2IrOpcode::BitfieldInsertU32:
                if (!operand(0) || !operand(1) || !operand(2)) return Status::Error(StatusCode::ShaderError, "IR bitfield insert references a missing SSA value");
                expression << "((" << *operand(1) << " & " << *operand(0)
                          << ") | (" << *operand(2) << " & ~" << *operand(0) << "))";
                break;
            case Rdna2IrOpcode::AlignBitU32:
                if (!operand(0) || !operand(1) || !operand(2)) return Status::Error(StatusCode::ShaderError, "IR alignbit references a missing SSA value");
                expression << "((" << *operand(2) << " & 31u) == 0u ? "
                          << *operand(1) << " : ((" << *operand(0)
                          << " << (32u - (" << *operand(2)
                          << " & 31u))) | (" << *operand(1) << " >> ("
                          << *operand(2) << " & 31u))))";
                break;
            case Rdna2IrOpcode::Min3F32:
            case Rdna2IrOpcode::Max3F32:
            case Rdna2IrOpcode::Med3F32:
                if (!operand(0) || !operand(1) || !operand(2)) return Status::Error(StatusCode::ShaderError, "IR three-way min/max references a missing SSA value");
                if (instruction.opcode == Rdna2IrOpcode::Min3F32) {
                    expression << "asuint(min(min(asfloat(" << *operand(0)
                          << "), asfloat(" << *operand(1) << ")), asfloat("
                          << *operand(2) << ")))";
                } else if (instruction.opcode == Rdna2IrOpcode::Max3F32) {
                    expression << "asuint(max(max(asfloat(" << *operand(0)
                          << "), asfloat(" << *operand(1) << ")), asfloat("
                          << *operand(2) << ")))";
                } else {
                    expression << "asuint(max(min(asfloat(" << *operand(0)
                          << "), asfloat(" << *operand(1)
                          << ")), min(max(asfloat(" << *operand(0)
                          << "), asfloat(" << *operand(1) << ")), asfloat("
                          << *operand(2) << "))))";
                }
                break;
            case Rdna2IrOpcode::PixelInterpolate: {
                if (module.stage != ShaderStage::Pixel) {
                    return Status::Error(StatusCode::UnsupportedFormat, "Pixel interpolation requires a pixel shader input");
                }
                constexpr std::array<const char*, 4> components = {"x", "y", "z", "w"};
                expression << "asuint(input.attr"
                          << static_cast<uint32_t>(instruction.interpolation_attribute)
                          << "." << components[instruction.interpolation_channel] << ")";
                break;
            }
            case Rdna2IrOpcode::CompareEqF32:
            case Rdna2IrOpcode::CompareLtF32:
                if (!operand(0) || !operand(1)) return Status::Error(StatusCode::ShaderError, "IR comparison references a missing SSA value");
                expression << "(asfloat(" << *operand(0) << ") "
                          << (instruction.opcode == Rdna2IrOpcode::CompareEqF32 ? "==" : "<")
                          << " asfloat(" << *operand(1) << ") ? 0xffffffffu : 0u)";
                break;
            case Rdna2IrOpcode::ConditionalSelect:
                if (!operand(0) || !operand(1) || !operand(2)) return Status::Error(StatusCode::ShaderError, "IR conditional select references a missing SSA value");
                expression << "(" << *operand(2) << " != 0u ? " << *operand(1)
                          << " : " << *operand(0) << ")";
                break;
            case Rdna2IrOpcode::BufferLoad:
                if (!operand(0)) return Status::Error(StatusCode::ShaderError, "IR buffer load references a missing SSA value");
                expression << "sx_srv" << instruction.resource_slot
                          << ".Load(" << *operand(0) << " + "
                          << instruction.byte_offset << "u)";
                break;
            case Rdna2IrOpcode::TextureLoadComponent: {
                if (!operand(0) || !operand(1)) return Status::Error(StatusCode::ShaderError, "IR texture load references a missing coordinate");
                const std::string mip = instruction.operands.size() > 2
                                            ? (operand(2) ? *operand(2) : std::string{})
                                            : "0u";
                if (mip.empty()) {
                    return Status::Error(StatusCode::ShaderError, "IR texture load references a missing mip value");
                }
                constexpr std::array<const char*, 4> channels = {"x", "y", "z", "w"};
                expression << "asuint(sx_texture" << instruction.resource_slot
                          << ".Load(int3(int2(asint(" << *operand(0) << "), asint("
                          << *operand(1) << ")), asint(" << mip << ")))."
                          << channels[instruction.component] << ")";
                break;
            }
            case Rdna2IrOpcode::TextureSampleComponent: {
                const size_t required_operands =
                    instruction.texture_sample_mode == Rdna2TextureSampleMode::Gradients
                        ? 6u
                        : instruction.texture_sample_mode == Rdna2TextureSampleMode::ImplicitLod
                              ? 2u
                              : 3u;
                if (instruction.operands.size() != required_operands) {
                    return Status::Error(StatusCode::ShaderError, "IR texture sample has an invalid operand count");
                }
                for (size_t i = 0; i < required_operands; ++i) {
                    if (!operand(i)) {
                        return Status::Error(StatusCode::ShaderError, "IR texture sample references a missing SSA value");
                    }
                }
                if (module.stage == ShaderStage::Compute &&
                    instruction.texture_sample_mode == Rdna2TextureSampleMode::ImplicitLod) {
                    return Status::Error(
                        StatusCode::UnsupportedFormat,
                        "Implicit-LOD texture sampling is unavailable in compute shaders");
                }
                const size_t uv_index =
                    instruction.texture_sample_mode == Rdna2TextureSampleMode::Gradients
                        ? 4u
                        : 0u;
                const std::string uv =
                    "float2(asfloat(" + *operand(uv_index) + "), asfloat(" +
                    *operand(uv_index + 1) + "))";
                std::string method;
                std::string arguments = "sx_sampler" + std::to_string(instruction.sampler_slot) + ", " + uv;
                switch (instruction.texture_sample_mode) {
                    case Rdna2TextureSampleMode::ImplicitLod:
                        method = "Sample";
                        break;
                    case Rdna2TextureSampleMode::ExplicitLod:
                        method = "SampleLevel";
                        arguments += ", asfloat(" + *operand(2) + ")";
                        break;
                    case Rdna2TextureSampleMode::Bias:
                        method = "SampleBias";
                        arguments += ", asfloat(" + *operand(2) + ")";
                        break;
                    case Rdna2TextureSampleMode::Gradients:
                        method = "SampleGrad";
                        arguments += ", float2(asfloat(" + *operand(0) + "), asfloat(" +
                                     *operand(1) + ")), float2(asfloat(" + *operand(2) +
                                     "), asfloat(" + *operand(3) + "))";
                        break;
                }
                constexpr std::array<const char*, 4> channels = {"x", "y", "z", "w"};
                expression << "asuint(sx_texture" << instruction.resource_slot
                          << "." << method << "(" << arguments << ")."
                          << channels[instruction.component] << ")";
                break;
            }
            case Rdna2IrOpcode::BufferStore:
                if (!operand(0) || !operand(1)) return Status::Error(StatusCode::ShaderError, "IR buffer store references a missing SSA value");
                hlsl << "sx_uav" << instruction.resource_slot << ".Store("
                     << *operand(0) << " + " << instruction.byte_offset << "u, "
                     << *operand(1) << ");\n";
                continue;
            case Rdna2IrOpcode::Barrier:
            case Rdna2IrOpcode::Wait:
            case Rdna2IrOpcode::MeshAllocate:
            case Rdna2IrOpcode::Export:
            case Rdna2IrOpcode::End:
                continue;
        }
        if (instruction.result == kNoValue) {
            return Status::Error(StatusCode::ShaderError, "Value-producing IR instruction has no SSA result");
        }
        hlsl << "uint " << result_name << " = " << expression.str() << ";\n";
        values.emplace(instruction.result, result_name);
    }

    if (module.stage == ShaderStage::Mesh) {
        hlsl << "GroupMemoryBarrierWithGroupSync();\n"
             << "uint sx_mesh_nv = min(sx_mesh_counts & 0xfffu, "
             << module.mesh_max_vertices << "u);\n"
             << "uint sx_mesh_np = min((sx_mesh_counts >> 12u) & 0xfffu, "
             << module.mesh_max_primitives << "u);\n"
                "SetMeshOutputCounts(sx_mesh_nv, sx_mesh_np);\n"
                "if (gi < sx_mesh_nv) sx_vertices[gi] = sx_mesh_output;\n"
                "if (gi < sx_mesh_np) {\n"
                "sx_triangles[gi] = uint3(sx_mesh_primitive & 0x1ffu, "
                "(sx_mesh_primitive >> 10u) & 0x1ffu, "
                "(sx_mesh_primitive >> 20u) & 0x1ffu);\n"
                "sx_primitives[gi].cull = (sx_mesh_primitive & 0x80000000u) != 0u;\n"
                "}\n";
    } else if (module.stage == ShaderStage::Pixel ||
               module.stage == ShaderStage::Vertex) {
        if (module.exports.size() != 4) {
            return Status::Error(StatusCode::UnsupportedFormat, "Graphics shader is missing a four-component export");
        }
        hlsl << "return float4(";
        for (size_t i = 0; i < module.exports.size(); ++i) {
            const auto found = values.find(module.exports[i]);
            if (found == values.end()) {
                return Status::Error(StatusCode::ShaderError, "Shader export references a missing SSA value");
            }
            if (i != 0) hlsl << ", ";
            hlsl << "asfloat(" << found->second << ")";
        }
        hlsl << ");\n";
    }
    hlsl << "}\n";
    out_hlsl = hlsl.str();
    return Status::Ok();
}

Status TranslateRdna2ToHlsl(
    const std::vector<uint8_t>& program,
    const Rdna2TranslationOptions& options,
    std::string& out_hlsl,
    bool* out_requires_wave64) {
    out_hlsl.clear();
    if (out_requires_wave64) *out_requires_wave64 = false;
    std::vector<Rdna2Instruction> decoded;
    Status status = DecodeRdna2Instructions(program, decoded);
    if (!status.ok()) return status;
    if (std::any_of(
            decoded.begin(), decoded.end(),
            [](const Rdna2Instruction& instruction) {
                return instruction.mnemonic == "image_bvh_intersect_ray" ||
                       instruction.mnemonic == "image_bvh64_intersect_ray";
            })) {
        LogMessage(
            LogLevel::Warning, "shader",
            "AMD BVH traversal-step instruction detected; it is not replaced by a full RayQuery.");
    }

    Rdna2IrModule module{};
    status = BuildRdna2Ir(decoded, options, module);
    if (!status.ok()) return status;

    status = GenerateRdna2Hlsl(module, out_hlsl);
    if (!status.ok()) return status;
    if (out_requires_wave64) *out_requires_wave64 = module.requires_wave64;
    return Status::Ok();
}

Status TranslateRdna2ToDxil(
    const std::vector<uint8_t>& program,
    const Rdna2TranslationOptions& options,
    std::vector<uint8_t>& out_dxil,
    std::string* out_hlsl) {
    out_dxil.clear();
    if (out_hlsl) out_hlsl->clear();

    std::string hlsl;
    Status status = TranslateRdna2ToHlsl(program, options, hlsl);
    if (!status.ok()) return status;

    const char* profile = nullptr;
    switch (options.stage) {
        case ShaderStage::Vertex: profile = "vs_6_6"; break;
        case ShaderStage::Pixel: profile = "ps_6_6"; break;
        case ShaderStage::Compute: profile = "cs_6_6"; break;
        case ShaderStage::Mesh: profile = "ms_6_6"; break;
        case ShaderStage::Amplification: profile = "as_6_6"; break;
        default:
            return Status::Error(StatusCode::UnsupportedFormat, "RDNA2-to-DXIL stage is unsupported");
    }
    status = CompileHlslToDxil(hlsl, "main", profile, out_dxil, true);
    if (!status.ok()) return status;
    if (out_hlsl) *out_hlsl = std::move(hlsl);
    return Status::Ok();
}

}  // namespace scarlettx
