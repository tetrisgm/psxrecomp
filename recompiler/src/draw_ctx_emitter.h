#pragma once

// Lean draw-context clones (<stem>_draw.c, docs/THREADED_CONTEXTS.md).
//
// From the [threaded_draw] roots of game.toml, take the closure of every
// compiled function they can reach statically: direct C calls between split
// pieces and alias bodies, and every constant CPS transfer (jal / j / branch
// to a split piece). Each function in the closure is emitted a second time as
// dctx_func_XXXXXXXX: the same body text, with its symbol references renamed
// into the clone set, guest stores routed to the draw context's memory and the
// store-PC breadcrumb removed. The file compiles without
// PSX_ENABLE_BLOCK_CYCLES / PSX_COSIM and with PSX_DRAW_CTX_BUILD, so the
// cycle, I-cache, slice and cosim code the emitter guards vanishes and the
// remaining runtime calls resolve through psx_dctx_codegen.h. A sorted row
// table (entries and CPS continuations of the clones, with their code ranges)
// registers itself with the runtime's draw-context trampoline.
//
// The main-context output (<stem>_full_NN.c, <stem>_dispatch.c) is not
// touched by any of this.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "code_generator.h"
#include "game_dispatch_emitter.h"

namespace PSXRecomp {

struct DrawCtxConfig {
    std::vector<uint32_t> roots;
    std::vector<uint32_t> exclude;
    uint32_t max_functions = 4096;
};

struct DrawCtxReport {
    size_t functions = 0;          // clone bodies
    size_t rows = 0;               // trampoline rows (entries + continuations)
    size_t lines = 0;
    std::vector<uint32_t> unresolved;   // constant CPS targets with no compiled owner
    std::vector<uint32_t> excluded_hits; // excluded functions the closure reached
};

// Emits the draw file into `out`. An empty roots list emits a valid file with
// no clones (the runtime then reports PSX_CTX_FAULT_NO_TABLE). Returns false
// with `error` set when a root has no compiled entry or the closure exceeds
// max_functions.
bool emit_draw_ctx(const GameDispatchUnit& unit,
                   const std::vector<GeneratedFunction>& funcs,
                   const std::string& ranges_manifest,
                   const std::string& decls_basename,
                   const std::string& stem,
                   const DrawCtxConfig& config,
                   std::string& out, DrawCtxReport& report, std::string& error);

// Clone-body transform, exposed for tests: renames func_/psx_alias_body_
// references that are in `names`, routes cpu->write_*( to the context
// stores and drops the store-PC breadcrumb.
std::string draw_ctx_clone_text(const std::string& code,
                                const std::set<std::string>& names);

}  // namespace PSXRecomp
