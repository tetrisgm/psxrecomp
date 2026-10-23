#include "draw_ctx_emitter.h"
#include <cstdlib>
#include <iostream>
using namespace PSXRecomp;
static void check(bool v, const char *why) { if (!v) { std::cerr << why << '\n'; std::exit(1); } }
int main() {
    const std::string body = "void func_80010000(CPUState* cpu) {\nblock_80010000:\n"
        "cpu->write_word(0x80020000u, 1u); cpu->write_half(2u, 3u); cpu->write_byte(4u, 5u);\n"
        "g_debug_last_store_pc = 0x80010000u;\n"
        "func_80010010(cpu); /* func_80019999(cpu) */\n"
        "cpu->gpr[31] = 0x80010020u; cpu->pc = 0x80010010u; return;\n}\n";
    const auto cloned = draw_ctx_clone_text(body, {"func_80010000", "func_80010010"});
    check(cloned.find("void dctx_func_80010000") != std::string::npos, "clone name");
    check(cloned.find("PSX_DCTX_TICK") != std::string::npos, "bounded internal loops");
    check(cloned.find("cpu->write_") == std::string::npos, "all stores are private");
    check(cloned.find("g_debug_last_store_pc") == std::string::npos, "no shared debug write");
    check(cloned.find("dctx_func_80010010(cpu)") != std::string::npos, "direct callee clone");
    PS1Executable exe{}; CodeGenerator generator(exe); generator.set_code_segment(0x80000000);
    GameDispatchUnit unit{&generator, {0x80010000,0x80010010,0x80010020}};
    std::vector<GeneratedFunction> funcs(3);
    funcs[0].function_name="func_80010000"; funcs[0].full_code=body;
    funcs[1].function_name="func_80010010"; funcs[1].full_code="void func_80010010(CPUState* cpu) { cpu->pc = cpu->gpr[31]; }";
    funcs[2].function_name="func_80010020"; funcs[2].full_code="void func_80010020(CPUState* cpu) { cpu->pc = 0x80017777u; }";
    DrawCtxConfig config; config.roots={0x80010000};
    std::string out,error; DrawCtxReport report;
    const std::string ranges="F 80010000\nR 80010000 10\nF 80010010\nR 80010010 10\nF 80010020\nR 80010020 10\n";
    check(emit_draw_ctx(unit,funcs,ranges,"test.h","test",config,out,report,error),error.c_str());
    check(report.functions==3 && report.rows==3,"closure follows return links as well as call targets");
    check(report.unresolved==std::vector<uint32_t>{0x80017777},"unknown target manifest");
    check(out.find("func_80019999")!=std::string::npos,"comment references ignored");
    check(out.find("#undef PSX_ENABLE_BLOCK_CYCLES")!=std::string::npos,"lean compilation");
    check(out.find("{0x80010020u, 0x00000000u, 2u, 1u,")!=std::string::npos,"return link has verified range");
    config.max_functions=2;
    check(!emit_draw_ctx(unit,funcs,ranges,"test.h","test",config,out,report,error),"bounded closure");
    config.max_functions=4096; config.roots={0x80013333};
    check(!emit_draw_ctx(unit,funcs,ranges,"test.h","test",config,out,report,error),"unknown root fails closed");
    config.roots={};
    check(emit_draw_ctx(unit,funcs,ranges,"test.h","test",config,out,report,error),"empty config");
    check(report.functions==0 && report.rows==0,"empty table");
    std::cout << "draw clone transform and closure passed\n";
}
