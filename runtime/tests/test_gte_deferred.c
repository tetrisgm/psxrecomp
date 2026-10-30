/* Production GTE deadline regressions. No BIOS, disc or copied guest code. */
#include "psx_cyc.h"
#include <assert.h>
#include <stdio.h>
int g_ls_replay_active = 0;
int g_ls_mode = 0;
int g_debug_cycle_hooks = 0;
volatile int g_ds_recording = 0;
int g_precise_mode = 0;
int g_psx_call_bail = 0;
int g_boot_state_cpu_timing_restored = 0; /* no restored boot state in this fixture */
int psx_mod_host_output_scope(void) { return 0; } /* no host-output mod callback */
uint32_t i_mask = 0;
uint64_t g_guest_store_count = 0;
uint64_t g_mmio_access_count = 0;

static uint32_t s_cd_cycles_remaining = 5;
static int s_cd_ready = 0;
static uint32_t s_dma_ready_cycles = 0;

void sio_advance(uint32_t cycles) { (void)cycles; }

void cdrom_advance(uint32_t cycles) {
    if (s_cd_ready) return;
    if (cycles >= s_cd_cycles_remaining) {
        s_cd_cycles_remaining = 0;
        s_cd_ready = 1;
    } else {
        s_cd_cycles_remaining -= cycles;
    }
}

void dma_advance(uint32_t cycles) {
    if (s_cd_ready) s_dma_ready_cycles += cycles;
}

void timers_advance(uint32_t cycles) { (void)cycles; }
void interrupts_advance_cycles(uint32_t cycles) { (void)cycles; }
void interrupts_service_scheduled_events(void) {}

uint32_t interrupts_cycles_to_vblank(void) { return UINT32_MAX; }
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t mask) {
    (void)mask;
    return s_cd_ready ? UINT32_MAX : s_cd_cycles_remaining;
}
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
void source_gpu_runtime_advance(void) {}
uint32_t source_gpu_runtime_cycles_to_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t mask) {
    (void)mask;
    return UINT32_MAX;
}
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
/* SPU sample-event scheduler (golden 1a973806): psx_cycles.c consults it; stub here. */
static uint32_t s_spu_next_sample = UINT32_MAX;
uint32_t psx_spu_sample_event_cycles_to_next(void) { return s_spu_next_sample; }
void psx_spu_sample_event_service(void) {}
int psx_get_in_exception(void) { return 0; }

void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}

int  psx_netplay_active(void) { return 0; }
int  psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}


static uint32_t local;
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
static void reset(unsigned elapsed,unsigned batch,unsigned local_value) {
 psx_cycles_reset_for_boot();s_cd_ready=1;s_spu_next_sample=UINT32_MAX;
 psx_advance_cycles(1000+elapsed);
 psx_next_service_cycle=UINT64_MAX;
 g_psx_cyc_batch=batch;g_psx_cyc_batch_limit=64;g_psx_cyc_bb_defer=1;
 local=local_value;g_psx_cyc_local_acc=local_value?&local:NULL;
}
int main(void) {
 CPUState cpu={0};
 /* Authored timing equivalent of DPCS / ADDIU / SWC2 with two remaining
  * load-overlap credits. Source return(ret-1) gives a seven-cycle deadline.
  * SWC2's one pending base cycle must be part of its stall comparison. */
 reset(0,0,0);cpu.read_absorb_which=23;cpu.read_absorb[23].value=2;
 cpu.ld_which_t=32;cpu.ld_absorb=5;
 psx_cyc_step(&cpu,0);psx_gte_set(&cpu,psx_gte_cmd_latency(0x10));
 psx_cyc_step(&cpu,(1u<<2)|(1u<<14));psx_cyc_step(&cpu,0);
 psx_gte_stall(&cpu);psx_cyc_batch_flush();
 CHECK(psx_cycle_count==1007);
 /* Every command latency, pending source (batch/local/both), and prior
  * command overlap. Deadline ownership is max(published time,old deadline). */
 for(unsigned cmd=0;cmd<64;cmd++)for(unsigned pending=0;pending<=64;pending++)
 for(unsigned mode=0;mode<3;mode++)for(unsigned lead=0;lead<=43;lead+=43) {
  reset(0,mode==1?0:pending,mode==0?0:pending);cpu.gte_ts_done=1000+lead;
  unsigned elapsed=(mode==2?2:1)*pending;
  uint64_t at=1000+(elapsed>lead?elapsed:lead);
  psx_gte_set(&cpu,psx_gte_cmd_latency(cmd));
  CHECK(psx_cycle_count==at && cpu.gte_ts_done==at+psx_gte_cmd_latency(cmd));
  CHECK(g_psx_cyc_batch==0 && local==0);
 }
 /* Reads additionally return only the actual stall as the new load credit.
  * Ordinary writes/stores preserve the existing credit and delayed slot. */
 for(unsigned elapsed=0;elapsed<=64;elapsed++)for(unsigned pending=0;pending<=elapsed;pending++)
 for(unsigned mode=0;mode<2;mode++)for(unsigned read=0;read<2;read++) {
  memset(&cpu,0,sizeof(cpu));cpu.gte_ts_done=1043;cpu.ld_absorb=19;cpu.ld_which_t=32;
  cpu.read_absorb_which=5;cpu.read_absorb[5].value=11;
  reset(elapsed-pending,mode?0:pending,mode?pending:0);
  if(read)psx_gte_read(&cpu,7);else psx_gte_stall(&cpu);
  unsigned stall=elapsed<43?43-elapsed:0;
  CHECK(psx_cycle_count==1000+elapsed+stall);
  CHECK(cpu.ld_absorb==(read?stall:19) && cpu.ld_which_t==(read?7:32));
  CHECK(cpu.read_absorb[5].value==11 && g_psx_cyc_batch==0 && local==0);
 }
 /* Compare the compiled proof path with the publishing reference, including
  * pending raw work, expired/in-flight deadlines, reads and conservative scale. */
 for(unsigned lead=0;lead<=80;lead++)for(unsigned raw=0;raw<=64;raw++)
 for(unsigned read=0;read<2;read++)for(unsigned scale=1;scale<=2;scale++) {
  CPUState a={0},b; a.gte_ts_done=1000+lead;a.ld_absorb=19;a.ld_which_t=32;
  a.read_absorb_which=5;a.read_absorb[5].value=11;b=a;
  reset(0,7,0);psx_guest_cycle_scale_set(scale);g_psx_gcs_frac=0;psx_next_service_cycle=UINT64_MAX;
  psx_cyc_block_begin(512);psx_cyc_charge(raw);
  if(read)psx_gte_read(&a,7);else psx_gte_stall(&a);
  psx_cyc_batch_flush();uint64_t at=psx_cycle_count;uint32_t frac=g_psx_gcs_frac;
  reset(0,7,0);psx_guest_cycle_scale_set(scale);g_psx_gcs_frac=0;psx_next_service_cycle=UINT64_MAX;
  psx_cyc_block_begin(512);psx_cyc_charge(raw);
  if(read)psx_gte_read_compiled(&b,7);else psx_gte_stall_compiled(&b);
  if(scale==1 && lead<=7+raw) CHECK(g_psx_cyc_block_active);
  psx_cyc_batch_flush();
  if (psx_cycle_count!=at || g_psx_gcs_frac!=frac || memcmp(&a,&b,sizeof a)) printf("lead=%u raw=%u read=%u scale=%u clock=%llu/%llu frac=%u/%u absorb=%u/%u\n",lead,raw,read,scale,(unsigned long long)psx_cycle_count,(unsigned long long)at,g_psx_gcs_frac,frac,b.ld_absorb,a.ld_absorb);
  CHECK(psx_cycle_count==at && g_psx_gcs_frac==frac && !memcmp(&a,&b,sizeof a));
 }
 printf("PASS %u production GTE pending-charge/deadline checks\n",checks);return 0;
}
