#!/usr/bin/env python3
"""Compare compiled memory stores with actual memory.c, including debug observers."""
import pathlib
import tempfile
import argparse
import os
import test_memory_debug_trace_guard as fixture
parser=argparse.ArgumentParser()
parser.add_argument("--cc", default=fixture.default_cc())
compiler=fixture.normalize_cc(parser.parse_args().cc)
os.environ["PATH"]=str(pathlib.Path(compiler).parent)+os.pathsep+os.environ.get("PATH", "")

base = fixture.DRIVER_C
# Exercise ordinary data outside the kernel validation window, all RAM mirrors
# and both scratchpad segments. The same actual memory.c is linked to both.
for a,b in [("0x80001000", "0x80011000"), ("0xA0001004", "0xA0011004"),
            ("0x00001006", "0x00011006"), ("0x100", "0x1100"), ("0xFFFu", "0x10FFFu")]:
    base = base.replace(a,b)
base = base.replace("int main(int argc, char **argv) {", """
static CPUState cpu;
static unsigned callback_calls;
static void callback_word(uint32_t a, uint32_t v) { ++callback_calls; psx_write_word(a,v); }
static void callback_half(uint32_t a, uint16_t v) { ++callback_calls; psx_write_half(a,v); }
static void callback_byte(uint32_t a, uint8_t v) { ++callback_calls; psx_write_byte(a,v); }
int main(int argc, char **argv) {
    cpu.write_word = callback_word; cpu.write_half = callback_half; cpu.write_byte = callback_byte;
""")
# CPU is needed before the helper bodies in the driver.
base = base.replace("static CPUState cpu;", "")
base = base.replace("static void seed_memory", "static CPUState cpu;\nstatic void seed_memory")
fixture.STUBS_C = fixture.STUBS_C.replace(
    "(void)biu; (void)addr; (void)value; abort();",
    "(void)biu; (void)addr; (void)value; extern unsigned isolated; ++isolated;")
fixture.STUBS_C += "\nunsigned isolated;\n"
base = base.replace('#include <stdlib.h>', '#include <stdlib.h>\n#include <assert.h>')
edge_checks = r"""
static void verify_edges(void) {
    extern void memory_set_sr_ptr(const uint32_t *);
    extern unsigned isolated;
    uint8_t *ram=memory_get_ram_ptr();
    memory_set_sr_ptr(&cpu.cop0[12]);
    ram[0x11000]=0x55;
    cpu.cop0[12]=0x10000;
    unsigned before=isolated;
    psx_mem_write_word(&cpu,0x80011000,0x11223344);
    assert(isolated==before+1 && ram[0x11000]==0x55);
    cpu.cop0[12]=0;
    uint8_t old=g_psx_bios_rom[0];
    psx_mem_write_byte(&cpu,0xBFC00000,0xA5);
    assert(g_psx_bios_rom[0]==old);
    psx_mem_write_word(&cpu,0xC0011000,0);
    assert(ram[0x11000]==0x55);
    uint8_t reference[16]={0};
    g_psx_text_ref_image=reference; g_psx_text_ref_lo=0x11000; g_psx_text_ref_hi=0x11010;
    g_psx_text_modified_bitmap[0]=0;
    psx_mem_write_word(&cpu,0x80011000,0x11223344);
    assert(g_psx_text_modified_bitmap[0] & (1u<<17));
    g_psx_text_ref_image=0;
    g_dirty_ram_exec_page_bitmap[0] |= 1u<<17;
    psx_mem_write_half(&cpu,0x80011000,0x1234);
    assert(!(g_dirty_ram_exec_page_bitmap[0] & (1u<<17)));
    uint32_t generation=g_dirty_ram_code_gen;
    g_psx_dirty_ram_bitmap[0] &= ~(1u<<2);
    psx_mem_write_word(&cpu,0x80002000,0x5678);
    assert((g_psx_dirty_ram_bitmap[0] & (1u<<2)) && g_dirty_ram_code_gen==generation+1);
    g_psx_track_last_store=1;
    psx_mem_write_word(&cpu,0x80011000,0x12345678);
    assert(g_guest_last_store_addr==0x80011000 && g_guest_last_store_val==0x12345678);
    psx_mem_write_half(&cpu,0x80011004,0x4321);
    assert(g_guest_last_store_addr==0xffffffffu && g_guest_last_store_val==0x12345678);
    psx_mem_write_byte(&cpu,0x80011006,0x56);
    assert(g_guest_last_store_addr==0xffffffffu && g_guest_last_store_val==0x12345678);
    psx_mem_write_word(&cpu,0x1fc00000,0x89abcdef);
    assert(g_guest_last_store_addr==0x1fc00000 && g_guest_last_store_val==0x89abcdef);
    psx_mem_write_half(&cpu,0x1fc00000,0x4321);
    assert(g_guest_last_store_addr==0xffffffffu && g_guest_last_store_val==0x89abcdef);
    g_psx_track_last_store=0;
    g_psx_ram_size=0x800000; g_psx_ram_mask=0x7FFFFF;
    ram[0x11000]=0x11;
    psx_mem_write_byte(&cpu,0x80611000,0x22);
    assert(ram[0x611000]==0x22 && ram[0x11000]==0x11);
    g_psx_ram_size=0x200000; g_psx_ram_mask=0x1FFFFF;
}
"""
base = base.replace('int main(int argc, char **argv) {', edge_checks+'\nint main(int argc, char **argv) {')
base = base.replace('    return 0;\n}', '    verify_edges();\n    return 0;\n}')
original = base
fast = base
start = fast.index("static void do_six_writes")
end = fast.index("static int check_written_bytes",start)
body = fast[start:end]
for width in ["word","half","byte"]:
    body = body.replace("psx_write_"+width+"(", "psx_mem_write_"+width+"(&cpu, ")
fast = fast[:start]+body+fast[end:]
fast = fast.replace('    printf("state=', '    if (callback_calls) return 7;\n    printf("state=')
with tempfile.TemporaryDirectory(prefix="psx-compiled-memory-") as td:
    root = pathlib.Path(td)
    outputs=[]
    for name,driver in [("reference",original),("inline",fast)]:
        directory=root/name; directory.mkdir()
        fixture.DRIVER_C=driver
        exe=fixture.build(compiler,directory,fixture.MEMORY_C,name,(),"trace_counting.c")
        outputs.append(fixture.run(exe,4))
    assert outputs[0] == outputs[1], outputs
print("PASS: inline RAM/scratch stores match memory.c, all mirrors and observers; zero memory callbacks")
