/* Execute the production GPU accessor, including snapshots' direct register changes. */
#define main gpu_fixture_main
#include "test_gpu_textured_dot_nw_shift_exec.c"
#undef main
#include <time.h>
static uint32_t rng=17;
static uint32_t next(void) { rng=rng*1664525u+1013904223u; return rng; }
static void equal(void) {
    GpuDisplayInfo cached,raw,hit;
    gpu_get_display_info(&cached); gpu_display_info_compute(&raw);
    gpu_get_display_info(&hit);
    assert(!memcmp(&cached,&raw,sizeof raw)); assert(!memcmp(&hit,&raw,sizeof raw));
}
int main(void) {
    fixture_native_start();
    uint32_t *fields[]={&display_area_x,&display_area_y,&display_depth,&display_disabled,
        &hres1,&hres2,&h_display_x1,&h_display_x2,&video_mode,&v_display_y1,&v_display_y2,&vres};
    const uint32_t masks[]={1023,511,1,1,3,1,4095,4095,1,1023,1023,1};
    for(unsigned i=0;i<20000;i++) {
        unsigned field=i%12;
        *fields[field]=next()&masks[field]; gpu_display_info_changed(); equal();
    }
    for(unsigned command=3;command<=8;command++) {
        gpu_write_gp1((command<<24)|0x00081234u); equal();
    }
    uint8_t snapshot[8192]; assert(gpu_snapshot_bytes()<=sizeof snapshot);
    gpu_snapshot_write(snapshot);
    gpu_write_gp1(0x08000000u); equal();
    PstR reader; pst_r_init(&reader,snapshot,gpu_snapshot_bytes());
    assert(gpu_snap_parse(&reader)); equal();
    gpu_set_power_on_video_mode(1); equal();
    gpu_write_gp1(0); equal();
    /* Off switch and toggling back cannot serve the prior state's cache. */
    s_display_info_cache.enabled=0; h_display_x2=3100; gpu_display_info_changed(); equal();
    s_display_info_cache.enabled=1; equal();
    volatile uint64_t digest=0; GpuDisplayInfo out;
    double elapsed[2];
    for(int enabled=0;enabled<2;enabled++) {
        s_display_info_cache.enabled=enabled; clock_t start=clock();
        for(unsigned i=0;i<2000000;i++) { gpu_get_display_info(&out); digest+=out.width; }
        elapsed[enabled]=(double)(clock()-start)/CLOCKS_PER_SEC;
    }
    printf("display cache: PASS; uncached=%.3fms cached=%.3fms checksum=%llu\n",
        elapsed[0]*1000,elapsed[1]*1000,(unsigned long long)digest);
    return 0;
}
