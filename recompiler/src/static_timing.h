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
// Base plus worst load wait/fudge. Cache refills and other observations
// publish first, so their additional latency cannot escape this bound.
inline uint32_t bound(uint32_t w) {
    const uint32_t op = w >> 26;
    return (op >= 0x20 && op <= 0x26) ? 8u : op == 0x32 ? 7u : 1u;
}
inline bool pure(uint32_t w) {
    uint32_t op = w >> 26, fn = w & 63u;
    if (op == 0) return fn == 0 || fn == 2 || fn == 3 || fn == 4 || fn == 6 || fn == 7 ||
        fn == 0x21 || fn == 0x23 || (fn >= 0x24 && fn <= 0x27) || fn == 0x2a || fn == 0x2b;
    return op == 9 || (op >= 0xa && op <= 0xf);
}
inline std::string emit(const std::vector<uint32_t>& words,
                        const std::vector<std::string>& precise) {
    uint32_t mask[4] = {};
    for (size_t i = 0; i < words.size(); ++i) mask[i] = psx_cyc_dep_res_mask(words[i]);
    // Keep the instruction/PGXP body once. Only the timing updates differ at
    // a nearby device deadline; duplicating the bodies bloats hot functions.
    std::string out = fmt::format(
        "    {{\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n"
        "    const int _psx_segment = psx_cyc_segment_ready({}u);\n"
        "    if (_psx_segment) psx_cyc_segment(cpu, {}u, 0x{:X}u, 0x{:X}u, 0x{:X}u, 0x{:X}u);\n",
        words.size(), words.size(), mask[0], mask[1], mask[2], mask[3]);
    out += "#endif\n";
    for (const auto& instruction : precise) {
        std::string guarded = instruction;
        const auto at = guarded.find("psx_cyc_step(");
        if (at == std::string::npos) std::abort();
        const auto end = guarded.find('\n', at);
        if (end == std::string::npos) std::abort();
        guarded.insert(end + 1, "#endif\n");
        guarded.insert(at, "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    if (!_psx_segment) ");
        const auto cosim = guarded.find("#ifdef PSX_COSIM");
        if (cosim != std::string::npos)
            guarded.replace(cosim, std::string("#ifdef PSX_COSIM").size(),
                            "#if defined(PSX_COSIM) && defined(PSX_ENABLE_BLOCK_CYCLES)");
        out += guarded;
    }
    out += "    }\n";
    return out;
}
}
