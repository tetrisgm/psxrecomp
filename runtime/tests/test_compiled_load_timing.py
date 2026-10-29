#!/usr/bin/env python3
"""Differential load timing/value proof against memory.c's existing slow path."""
import pathlib
import tempfile
import argparse
import os
import test_memory_debug_trace_guard as fixture
parser=argparse.ArgumentParser()
parser.add_argument("--cc", default=fixture.default_cc())
compiler=fixture.normalize_cc(parser.parse_args().cc)
os.environ["PATH"]=str(pathlib.Path(compiler).parent)+os.pathsep+os.environ.get("PATH", "")
fixture.STUBS_C += r"""
uint64_t psx_next_service_cycle = 1000;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
int g_psx_cyc_bb_defer, psx_in_device_service;
uint32_t *g_psx_cyc_local_acc;
uint32_t g_psx_gcs_recip_q16 = 65536, g_psx_gcs_frac;
int g_psx_gcs_batch = 1;
unsigned services;
void psx_devices_service_to_now(void) {
    ++services;
    /* A deadline can change the value being read: timing must precede load. */
    g_psx_ram[0x11000]++;
    psx_next_service_cycle = psx_cycle_count + 100;
}
void psx_advance_cycles_slow(uint32_t n) { psx_cycle_count += n; }
"""
fixture.DRIVER_C = r"""
#undef NDEBUG
#include "harness_common.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern unsigned services;
static uint32_t seed = 0x12345678;
static uint32_t next(void) { seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5; return seed; }
static void reset(unsigned scale, uint32_t frac, unsigned deadline) {
    psx_cycle_count = g_psx_cyc_batch = g_psx_cyc_batch_limit = 0;
    g_psx_gcs_recip_q16 = psx_gcs_recip_for(scale); g_psx_gcs_frac = frac;
    psx_next_service_cycle = deadline; services = 0;
    g_psx_ram[0x11000] = 0x47;
}
int main(void) {
    const uint32_t addresses[] = {0x11000,0x80211000,0xA0611000,
        0x1F800000,0x9F800010,0x1FC00000,0xBFC00200,0xC0011000};
    for (unsigned trial=0; trial<30000; ++trial) {
        CPUState a={0}, b;
        for (unsigned r=0;r<33;r++) a.read_absorb[r]=next()%10;
        a.read_absorb_which=next()%32; a.ld_which_t=next()%33;
        a.ld_absorb=next()%10; a.read_fudge=next()%33; b=a;
        uint32_t addr=addresses[next()%8], rt=next()%32, mask=next();
        unsigned width=next()%4, scale=1+next()%64, deadline=1+next()%16;
        uint32_t frac=next()&65535;
        reset(scale,frac,deadline);
        uint32_t v = width==0 ? psx_cyc_load_byte_slow(&a,addr,rt,mask) :
            width==1 ? psx_cyc_load_half_slow(&a,addr,rt,mask) : width==2 ? psx_cyc_load_word_slow(&a,addr,rt,mask) : psx_cyc_lwc2_read_slow(&a,addr);
        uint64_t clock=psx_cycle_count+g_psx_cyc_batch;
        uint32_t remainder=g_psx_gcs_frac; unsigned events=services;
        reset(scale,frac,deadline);
        uint32_t w = width==0 ? psx_cyc_load_byte(&b,addr,rt,mask) :
            width==1 ? psx_cyc_load_half(&b,addr,rt,mask) : width==2 ? psx_cyc_load_word(&b,addr,rt,mask) : psx_cyc_lwc2_read(&b,addr);
        assert(v==w && memcmp(&a,&b,sizeof a)==0);
        assert(clock==psx_cycle_count+g_psx_cyc_batch && remainder==g_psx_gcs_frac);
        assert(events==services);
    }
    puts("PASS: 30000 load cases preserve RAM/scratch/ROM values, pipeline, cycles and deadline ordering");
}
"""
with tempfile.TemporaryDirectory(prefix="psx-compiled-load-") as td:
    directory=pathlib.Path(td)
    exe=fixture.build(compiler,directory,fixture.MEMORY_C,"loads",("PSX_ENABLE_BLOCK_CYCLES",),"trace_counting.c")
    import subprocess
    subprocess.run([str(exe)],check=True)
