#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

Status CompileHlslToDxil(
    const std::string& hlsl_source,
    const std::string& entry_point,
    const std::string& target_profile,
    std::vector<uint8_t>& out_container,
    bool enable_16bit_types = false);

Status ValidateDxilContainer(const std::vector<uint8_t>& container);

// Stable identity of the live DXC toolchain: compiler version (via
// IDxcVersionInfo) folded with the fixed flag set CompileHlslToDxil passes to
// IDxcCompiler3::Compile. The shader cache mixes this value into its key so
// DXIL produced by a different dxcompiler.dll (or a changed flag vector) is
// never reused from cache. Returns 0 when DXC support is unavailable.
uint64_t DxcCompileIdentity();

}  // namespace scarlettx
