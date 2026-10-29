/* Complete-frame adapter: bounded replacement, private batch atomicity,
 * permanent context ownership, and durable paused diagnostics. */
#include "render_thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
static _Atomic int entered,unblock,ctx_owner,violations,diagnostics;
static _Atomic uint64_t last,count;
static void yield(void) { struct timespec t={0,1000000};nanosleep(&t,NULL); }
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);abort(); } } while(0)
static void context(void *unused,int current) {
    (void)unused;
    int me=rt_on_render_thread()?2:1,expected=current?0:me;
    if(!atomic_compare_exchange_strong(&ctx_owner,&expected,current?me:0)) ++violations;
}
static void execute(void *unused,const RtCmd *command,const void *payload) {
    (void)unused;
    CHECK(rt_on_render_thread() && atomic_load(&ctx_owner)==2);
    CHECK(command->op==1 && command->payload==sizeof(uint64_t));
    uint64_t value;memcpy(&value,payload,sizeof value);
    if(value==1) { atomic_store(&entered,1);while(!atomic_load(&unblock)) yield(); }
    CHECK(value>atomic_load(&last));atomic_store(&last,value);++count;
}
static void diagnostic(const void *payload) {
    uint64_t expected;memcpy(&expected,payload,sizeof expected);
    CHECK(rt_on_render_thread() && atomic_load(&ctx_owner)==2 && atomic_load(&last)==expected);
    ++diagnostics;
}
int main(void) {
    RtConfig config={0};config.ring_bytes=128;config.max_frames=2;
    config.exec=execute;config.ctx=context;
    atomic_store(&ctx_owner,1);
    CHECK(rt_start(&config));
    for(uint64_t n=1;n<=10000;++n) {
        uint64_t *data=rt_cmd_begin(1,0,sizeof n);CHECK(data);*data=n;rt_cmd_commit();
        if(n==10000) {
            CHECK(rt_batch_begin());CHECK(!rt_cmd_begin(1,0,256));CHECK(!rt_batch_try_commit());
        }
        rt_frame_end();
        if(n==1) while(!atomic_load(&entered)) yield();
    }
    RtStats stats;rt_get_stats(&stats);
    CHECK(rt_frames_ahead()==1); /* optional replay must see the pending packet */
    CHECK(stats.frames_produced==10000 && stats.frames_replaced==9998);
    CHECK(stats.backpressure_waits==0 && stats.acquires==0 && !rt_held());
    uint64_t expected=10000;
    CHECK(rt_request(diagnostic,&expected,sizeof expected));expected=0;
    atomic_store(&unblock,1);
    while(!atomic_load(&diagnostics)) yield();
    CHECK(atomic_load(&count)==2 && atomic_load(&last)==10000);
    CHECK(rt_frames_ahead()==0);
    /* The guest stops publishing, but another diagnostic must still finish. */
    expected=10000;CHECK(rt_request(diagnostic,&expected,sizeof expected));
    while(atomic_load(&diagnostics)!=2) yield();
    CHECK(rt_control(diagnostic,&expected,sizeof expected));
    rt_drain();CHECK(atomic_load(&diagnostics)==3);
    rt_get_stats(&stats);CHECK(stats.frames_produced==10000);
    CHECK(rt_backpressure_events()==0);
    rt_stop();CHECK(atomic_load(&ctx_owner)==1 && !atomic_load(&violations));
    puts("complete-frame render transport: replacement, batches and paused diagnostics passed");
    return 0;
}
