#ifndef PSX_MEM_INLINE_H
#define PSX_MEM_INLINE_H
#include "psx_memory.h"
#include <string.h>
#if defined(_MSC_VER)
#define PSX_MEM_INLINE static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_MEM_INLINE static inline __attribute__((always_inline))
#else
#define PSX_MEM_INLINE static inline
#endif
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t *g_psx_ram, *g_psx_scratchpad, *g_psx_bios_rom;
extern int g_ls_mode, g_ram_read_watch_active;
extern volatile int g_ds_recording;
extern int g_psx_render_pass_active, g_dma_exec_depth, g_host_store_depth;
extern int g_psx_memory_tomb_protect;
extern uint64_t g_guest_store_count;
extern int g_psx_track_last_store;
extern uint32_t g_guest_last_store_addr, g_guest_last_store_val;
extern uint32_t g_debug_write_hooks;
extern uint32_t g_parity_trace_armed;
extern volatile int g_card_data_write_armed;
extern uint32_t g_debug_last_store_pc, g_debug_current_func_addr;
extern uint8_t *g_psx_text_ref_image;
extern uint32_t g_psx_text_ref_lo, g_psx_text_ref_hi;
extern uint32_t g_psx_text_modified_bitmap[], g_psx_overlay_watch_bitmap[];
extern uint32_t g_dirty_ram_exec_page_bitmap[];
extern void (*g_overlay_flush_pending_cycles)(void);
extern uint32_t g_psx_dirty_ram_bitmap[], g_dirty_ram_code_gen, g_psx_memory_word_probe;
void psx_memory_store_observers(uint32_t addr, uint32_t phys, const uint8_t *p, uint32_t value, uint32_t width);
void psx_memory_kernel_write(uint32_t phys);
void psx_memory_code_write(uint32_t phys, uint32_t width);
void debug_server_trace_write_check(uint32_t phys, uint32_t old, uint32_t value, uint8_t width);
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t value);
void parity_trace_note_write(uint32_t phys, uint32_t width, uint32_t pc);
int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width);

/* Returns only contiguous ordinary storage. Decode KSEG2 before alias folding;
 * MMIO, expansion devices and malformed boundary-spanning accesses stay slow. */
PSX_MEM_INLINE uint8_t *psx_mem_pointer(uint32_t addr, uint32_t width) {
    if (addr >= 0xC0000000u) return NULL;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < PSX_MAIN_RAM_WINDOW_BYTES) {
        uint32_t off = phys & g_psx_ram_mask;
        return off <= g_psx_ram_size - width ? g_psx_ram + off : NULL;
    }
    if (phys >= 0x1F800000u && phys <= 0x1F800400u - width)
        return g_psx_scratchpad + (phys - 0x1F800000u);
    if (phys >= 0x1FC00000u && phys <= 0x1FC80000u - width)
        return g_psx_bios_rom + (phys - 0x1FC00000u);
    return NULL;
}

PSX_MEM_INLINE uint32_t psx_mem_read_word(CPUState *cpu, uint32_t addr) {
#if !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_DRAW_CTX_BUILD) && !defined(PSX_COSIM)
    if (!g_ls_mode && !g_ds_recording) {
        uint8_t *p = psx_mem_pointer(addr, 4u);
        if (p) {
            uint32_t value; memcpy(&value, p, sizeof(value));
            uint32_t phys = addr & 0x1FFFFFFFu;
            if (phys < PSX_MAIN_RAM_WINDOW_BYTES && g_ram_read_watch_active)
                debug_server_trace_ram_read_watch(phys & g_psx_ram_mask, value);
            return value;
        }
    }
#endif
    return cpu->read_word(addr);
}
PSX_MEM_INLINE uint16_t psx_mem_read_half(CPUState *cpu, uint32_t addr) {
#if !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_DRAW_CTX_BUILD) && !defined(PSX_COSIM)
    if (!g_ls_mode && !g_ds_recording) {
        uint8_t *p = psx_mem_pointer(addr, 2u);
        if (p) {
            uint16_t value; memcpy(&value, p, sizeof(value));
            return value;
        }
    }
#endif
    return cpu->read_half(addr);
}
PSX_MEM_INLINE uint8_t psx_mem_read_byte(CPUState *cpu, uint32_t addr) {
#if !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_DRAW_CTX_BUILD) && !defined(PSX_COSIM)
    if (!g_ls_mode && !g_ds_recording) {
        uint8_t *p = psx_mem_pointer(addr, 1u);
        if (p) {
            uint8_t value; memcpy(&value, p, sizeof(value));
            return value;
        }
    }
#endif
    return cpu->read_byte(addr);
}

/* A compiled CPU store is not a DMA/host store. Special execution modes and
 * cache isolation keep their existing handlers. Ordinary RAM updates perform
 * text dirty marking inline; only actual watched code needs loader callbacks. */
PSX_MEM_INLINE int psx_mem_store_fast(CPUState *cpu, uint32_t addr,
                                     uint32_t value, uint32_t width) {
#if defined(PSX_OVERLAY_DLL_BUILD) || defined(PSX_DRAW_CTX_BUILD) || defined(PSX_COSIM)
    (void)cpu; (void)addr; (void)value; (void)width; return 0;
#else
    if (g_ls_mode || g_ds_recording || g_psx_render_pass_active ||
        g_dma_exec_depth || g_host_store_depth || (cpu->cop0[12] & 0x10000u) ||
        addr >= 0xC0000000u || g_psx_memory_tomb_protect) return 0;
    uint32_t phys = addr & 0x1FFFFFFFu;
    /* Preserve legacy vector-store compatibility and the configured word probe. */
    if (phys < PSX_MAIN_RAM_WINDOW_BYTES) phys &= g_psx_ram_mask;
    if (width == 4u && (phys < 0x10u || phys == g_psx_memory_word_probe)) return 0;
    uint8_t *p = psx_mem_pointer(addr, width);
    if (!p) return 0;
    if (g_overlay_flush_pending_cycles) g_overlay_flush_pending_cycles();
    ++g_guest_store_count;
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        if (g_psx_track_last_store) {
            g_guest_last_store_addr = addr; g_guest_last_store_val = value;
        }
        return 1; /* ROM */
    }
    if (g_psx_track_last_store || g_debug_write_hooks ||
        g_parity_trace_armed || g_card_data_write_armed)
        psx_memory_store_observers(addr, phys, p, value, width);
    if (phys < PSX_MAIN_RAM_WINDOW_BYTES) {
        uint32_t page = phys >> 12, bit = 1u << (page & 31u);
        if (phys < 0x10000u) {
            if (!(g_psx_dirty_ram_bitmap[page >> 5] & bit)) ++g_dirty_ram_code_gen;
            g_psx_dirty_ram_bitmap[page >> 5] |= bit;
            psx_memory_kernel_write(phys);
        }
        if (g_psx_text_ref_image && phys >= g_psx_text_ref_lo &&
            phys + width <= g_psx_text_ref_hi &&
            memcmp(g_psx_text_ref_image + phys - g_psx_text_ref_lo, &value, width))
            g_psx_text_modified_bitmap[page >> 5] |= bit;
        if ((g_psx_overlay_watch_bitmap[page >> 5] |
             g_dirty_ram_exec_page_bitmap[page >> 5]) & bit)
            psx_memory_code_write(phys, width);
    }
    memcpy(p, &value, width);
    return 1;
#endif
}
PSX_MEM_INLINE void psx_mem_write_word(CPUState *cpu, uint32_t addr, uint32_t value) {
    if (!psx_mem_store_fast(cpu, addr, value, 4u)) cpu->write_word(addr, value);
}
PSX_MEM_INLINE void psx_mem_write_half(CPUState *cpu, uint32_t addr, uint16_t value) {
    if (!psx_mem_store_fast(cpu, addr, value, 2u)) cpu->write_half(addr, value);
}
PSX_MEM_INLINE void psx_mem_write_byte(CPUState *cpu, uint32_t addr, uint8_t value) {
    if (!psx_mem_store_fast(cpu, addr, value, 1u)) cpu->write_byte(addr, value);
}
#ifdef __cplusplus
}
#endif
#endif
