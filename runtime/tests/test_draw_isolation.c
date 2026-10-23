#include "draw_isolation.h"
#include "cpu_state.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint64_t ram[256], scratch[128];
uint32_t g_psx_ram_size = sizeof ram;
uint32_t g_psx_ram_mask = sizeof ram - 1;
uint32_t g_debug_last_store_pc;
void (*g_psx_ram_write_observer)(uint32_t, uint32_t);
void (*g_psx_exception_edge_observer)(int);
void (*g_psx_mmio_observer)(uint32_t, int);
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return (uint8_t *)scratch; }

int main(void) {
    CPUState cpu;
    PsxDrawIsoStats stats;
    memset(&cpu, 0, sizeof cpu);
    ram[0] = 1;
    cpu.gte_data[0] = 3;
    assert(psx_draw_iso_begin(&cpu, 0));
    assert(psx_draw_iso_enter(&cpu));
    ram[0] = 2;
    cpu.gte_data[0] = 4;
    assert(psx_draw_iso_suspend(&cpu));
    assert(!psx_draw_iso_live());
    assert(ram[0] == 1 && cpu.gte_data[0] == 3);
    assert(!psx_draw_iso_enter(&cpu));
    assert(!psx_draw_iso_suspend(&cpu));
    /* An interrupt inside the main-side call must not resume the draw. */
    g_psx_exception_edge_observer(1);
    g_psx_exception_edge_observer(0);
    assert(!psx_draw_iso_live());
    ram[1] = 8;
    g_psx_ram_write_observer(8, 4);
    cpu.gte_data[0] = 5;
    psx_draw_iso_resume(&cpu);
    assert(psx_draw_iso_live());
    assert(ram[0] == 2 && ram[1] == 8 && cpu.gte_data[0] == 4);
    ram[2] = 9;
    psx_draw_iso_leave(&cpu);
    assert(ram[0] == 1 && ram[1] == 8 && ram[2] == 0);
    assert(cpu.gte_data[0] == 5);
    assert(psx_draw_iso_read_word(0) == 2);
    assert(psx_draw_iso_read_word(16) == 9);
    psx_draw_iso_get_stats(&stats);
    assert(stats.spans == 1 && stats.exception_swaps == 0);
    psx_draw_iso_end();
    assert(ram[0] == 2 && ram[1] == 8 && ram[2] == 9);
    return 0;
}
