#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vicewindev {

struct ShaderCase {
    std::string id;          // stable short id, latin/underscores, e.g. "deferred_lighting"
    std::string description; // single menu line
    std::string profile;     // "ps_6_6", "cs_6_6", "ms_6_6", "as_6_6", "vs_6_6"
    std::string entry;       // always "main"
    std::string hlsl;        // self-contained source, no #include, no external assets
    bool needs_16bit = false;
};

const std::vector<ShaderCase>& Gta6ShaderCorpus();

// Returns a copy of base with id + "_v<N>" and with "#define SX_VARIANT <N>\n"
// inserted at the top of hlsl (so the compiler cannot fold variants together).
ShaderCase MakeVariant(const ShaderCase& base, uint32_t variant);

}  // namespace vicewindev
