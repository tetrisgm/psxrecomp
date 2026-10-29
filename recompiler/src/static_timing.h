#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "../../runtime/include/psx_instr_cost.h"
namespace PSXStaticTiming {
inline bool enabled() {
    const char *v = std::getenv("PSX_CODEGEN_STATIC_BLOCK_TIMING");
    return !v || !*v || *v != '0';
}
inline bool pure(uint32_t w) {
    uint32_t op = w >> 26, fn = w & 63u;
    if (op == 0) return fn == 0 || fn == 2 || fn == 3 || fn == 4 || fn == 6 || fn == 7 ||
        fn == 0x21 || fn == 0x23 || (fn >= 0x24 && fn <= 0x27) || fn == 0x2a || fn == 0x2b;
    return op == 9 || (op >= 0xa && op <= 0xf);
}
inline std::string emit(const std::vector<uint32_t>& words,
                        const std::vector<std::string>& bodies,
                        const std::vector<std::string>& precise) {
    uint32_t mask[4] = {};
    for (size_t i = 0; i < words.size(); ++i) mask[i] = psx_cyc_dep_res_mask(words[i]);
    std::string out = fmt::format(
        "#ifdef PSX_ENABLE_BLOCK_CYCLES\n    if (psx_cyc_segment_ready({}u)) {{\n"
        "        psx_cyc_segment(cpu, {}u, 0x{:X}u, 0x{:X}u, 0x{:X}u, 0x{:X}u);\n",
        words.size(), words.size(), mask[0], mask[1], mask[2], mask[3]);
    for (const auto& body : bodies) out += body + "\n";
    out += "    } else {\n";
    for (size_t i = 0; i < words.size(); ++i) out += precise[i];
    out += "    }\n#else\n";
    for (const auto& body : bodies) out += body + "\n";
    out += "#endif\n";
    return out;
}
}
