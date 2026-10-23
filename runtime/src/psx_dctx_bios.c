#include "psx_dctx_bios.h"
#include "psx_dctx_mem.h"
#include "pgxp_ctx.h"

/* OpenBIOS memcpy, qualified against the actual vector, table dispatcher and
 * routine bytes, not a BIOS name or the caller's title. Reference:
 * generated/OpenBIOS_full.c, BFC085D8..BFC08600. Forward byte-copy semantics
 * preserve overlap, RAM mirrors and scratchpad boundaries. Caller-saved
 * registers and PGXP effects follow that reference as well. Other BIOS
 * implementations and patched vectors fail closed until qualified. */
static int words_match(CPUState *cpu, uint32_t at,
                       const uint32_t *words, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (psx_dctx_lw(cpu, at + i * 4u) != words[i]) return 0;
    return 1;
}

static void alu(CPUState *cpu, unsigned reg, uint32_t instruction,
                uint32_t result, uint32_t a, uint32_t b) {
    cpu->gpr[reg] = result;
    if (cpu->ctx->pgxp_active)
        pgxp_ctx_alu(cpu->ctx->pgxp, cpu, instruction, result, a, b);
}

int psx_dctx_bios_call(CPUState *cpu, uint32_t pc) {
    static const uint32_t vector[] = {0x24082A80u, 0x01000008u, 0u};
    static const uint32_t dispatch[] = {
        0x3C080000u, 0x25080200u, 0x00095080u, 0x01485020u,
        0x8D4A0000u, 0x240800A0u, 0x01400008u, 0u
    };
    static const uint32_t copy[] = {
        0x00801025u, 0x00001825u, 0x14660003u, 0x00A32021u,
        0x03E00008u, 0u, 0x90870000u, 0x00432021u,
        0xA0870000u, 0x1000FFF8u, 0x24630001u
    };
    if ((pc & 0x1FFFFFFFu) != 0xA0u || cpu->gpr[9] != 0x2Au ||
        !words_match(cpu, 0xA0u, vector, 3) ||
        !words_match(cpu, 0x2A80u, dispatch, 8)) return 0;
    const uint32_t target = psx_dctx_lw(cpu, 0x2A8u);
    const uint32_t phys = target & 0x1FFFFFFFu;
    if (target >= 0xC0000000u || (target & 3u) ||
        phys < 0x1FC00000u || phys > 0x1FC80000u - sizeof copy ||
        !words_match(cpu, target, copy, 11)) return 0;

    alu(cpu, 8, 0x24082A80u, 0x2A80u, 0, 0x2A80u);
    alu(cpu, 8, 0x3C080000u, 0, 0, 0);
    alu(cpu, 8, 0x25080200u, 0x200u, 0, 0x200u);
    alu(cpu, 10, 0x00095080u, 0xA8u, 0x2Au, 2);
    alu(cpu, 10, 0x01485020u, 0x2A8u, 0xA8u, 0x200u);
    cpu->gpr[10] = target;
    if (cpu->ctx->pgxp_active)
        pgxp_ctx_load(cpu->ctx->pgxp, cpu, 0x8D4A0000u, 0x2A8u, target);
    alu(cpu, 8, 0x240800A0u, 0xA0u, 0, 0xA0u);
    alu(cpu, 2, 0x00801025u, cpu->gpr[4], cpu->gpr[4], 0);
    alu(cpu, 3, 0x00001825u, 0, 0, 0);
    for (;;) {
        if (!cpu->ctx->block_budget--)
            psx_dctx_fault(cpu, PSX_CTX_FAULT_RUNAWAY, target, "BIOS memcpy budget");
        alu(cpu, 4, 0x00A32021u, cpu->gpr[5] + cpu->gpr[3], cpu->gpr[5], cpu->gpr[3]);
        if (cpu->gpr[3] == cpu->gpr[6]) break;
        if (cpu->gpr[4] >= 0xC0000000u || !psx_dctx_ptr(cpu->ctx, cpu->gpr[4]) ||
            cpu->gpr[2] + cpu->gpr[3] >= 0xC0000000u ||
            !psx_dctx_ptr(cpu->ctx, cpu->gpr[2] + cpu->gpr[3]))
            psx_dctx_fault(cpu, PSX_CTX_FAULT_UNSUPPORTED, target,
                           "BIOS memcpy requires private RAM or scratchpad");
        cpu->gpr[7] = psx_dctx_lb(cpu, cpu->gpr[4]);
        if (cpu->ctx->pgxp_active)
            pgxp_ctx_load(cpu->ctx->pgxp, cpu, 0x90870000u, cpu->gpr[4], cpu->gpr[7]);
        alu(cpu, 4, 0x00432021u, cpu->gpr[2] + cpu->gpr[3], cpu->gpr[2], cpu->gpr[3]);
        psx_dctx_sb(cpu, cpu->gpr[4], (uint8_t)cpu->gpr[7]);
        if (cpu->ctx->pgxp_active)
            pgxp_ctx_store(cpu->ctx->pgxp, cpu, 0xA0870000u, cpu->gpr[4], cpu->gpr[7]);
        alu(cpu, 3, 0x24630001u, cpu->gpr[3] + 1u, cpu->gpr[3], 1);
    }
    cpu->pc = cpu->gpr[31];
    return 1;
}
