/* The untracked-store fixture drives memory.c with the DMA ownership flag.
 * It never executes a DMA device or a dynamically loaded BIOS/overlay. */
#include "psx_bios_image.h"
#include "dma.h"
#include <stdlib.h>
int g_dma_exec_depth, g_dma_cur_ch = -1;
uint32_t g_dma_cur_madr, g_dma_cur_bcr, g_dma_initiator_pc;
uint32_t g_parity_trace_armed;
volatile int g_card_data_write_armed;
uint32_t g_overlay_region_floor = 0x200000u, g_text_image_lo = 0x10000u;
const PsxKernelBody *psx_bios_kernel_bodies;
uint32_t psx_bios_kernel_body_count;
const PsxKernelPatchRange *psx_bios_kernel_patch_ranges;
uint32_t psx_bios_kernel_patch_range_count;
PsxBiosImageInfo psx_bios_image;
void overlay_loader_active_write_check(uint32_t address, uint32_t size) { (void)address; (void)size; }
uint32_t dma_read(uint32_t address) { (void)address; abort(); }
void dma_write(uint32_t address, uint32_t value) { (void)address; (void)value; abort(); }
void dma_write_masked(uint32_t address, uint32_t value, uint32_t mask) { (void)address; (void)value; (void)mask; abort(); }
