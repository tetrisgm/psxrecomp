/* Complete-frame presentation transport. Native work is never queued here.
 * The producer publishes owned packets; the graphics owner consumes the
 * newest complete packet without applying backpressure to guest execution. */
#include "render_thread.h"
#include "frame_owner.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <sched.h>
#endif

static FrameOwner *owner;
static RtConfig config;
static FramePacketQueue *batch;
static int batching;
static uint64_t sequence;
static _Atomic uint64_t published,completed,records,bytes,produced,consumed,busy_ns,high_water,oversize;
static _Atomic uint64_t rejected;
static size_t frame_bytes;
static unsigned owner_delay_ms;

uint64_t rt_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency,counter;
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&counter);
    return (uint64_t)((double)counter.QuadPart*1e9/(double)frequency.QuadPart);
#else
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
#endif
}
static void execute_packet(void *unused,const FramePacketView *packet) {
    (void)unused;
    const uint64_t begin=rt_now_ns();
#ifndef PSX_NO_DEBUG_TOOLS
    if(owner_delay_ms) {
#ifdef _WIN32
        Sleep(owner_delay_ms);
#else
        struct timespec delay={owner_delay_ms/1000,(long)(owner_delay_ms%1000)*1000000};
        nanosleep(&delay,NULL);
#endif
    }
#endif
    if(config.execute_frame) config.execute_frame(config.user,packet);
    else {
        const unsigned char *data=packet->records;
        for(size_t at=0;at<packet->record_bytes;) {
            const RtCmd *command=(const RtCmd *)(data+at);
            config.exec(config.user,command,command+1);
            at+=command->size;
        }
    }
    atomic_fetch_add(&busy_ns,rt_now_ns()-begin);
    atomic_fetch_add(&consumed,1);
    atomic_store(&completed,packet->sequence);
}
static void context(void *unused,int current) { (void)unused;config.ctx(config.user,current); }
static uint64_t tick(void *unused,uint64_t now) {
    (void)unused;
    uint64_t begin=rt_now_ns(),next=config.tick(config.user,now);
    atomic_fetch_add(&busy_ns,rt_now_ns()-begin);
    return next;
}
static void reset(void *unused,uint64_t epoch) {
    (void)unused;
    if(config.reset_frame) config.reset_frame(config.user,epoch);
}
static void begin_frame(void) {
    ++sequence;frame_bytes=0;
    if(config.begin_frame) (void)config.begin_frame(config.user,owner,sequence);
    else (void)frame_owner_begin(owner,sequence,NULL,0);
}
int rt_start(const RtConfig *cfg) {
    if(owner) return 1;
    if(!cfg || !cfg->ctx || (!cfg->exec && !cfg->execute_frame)) return 0;
    config=*cfg;
    owner_delay_ms=0;
#ifndef PSX_NO_DEBUG_TOOLS
    const char *delay=getenv("PSX_RENDER_OWNER_DELAY_MS");
    if(delay) {
        long value=strtol(delay,NULL,10);
        if(value>0) owner_delay_ms=(unsigned)(value>1000?1000:value);
    }
#endif
    if(!config.ring_bytes) config.ring_bytes=(size_t)32<<20;
    FrameOwnerConfig settings={0};
    settings.initial_limit=config.initial_limit;settings.record_limit=config.ring_bytes;
    settings.execute=execute_packet;settings.context=context;
    settings.clock_ns=rt_now_ns;settings.tick=config.tick?tick:NULL;settings.reset=reset;
    batch=frame_packet_create(0,config.ring_bytes);
    if(!batch) return 0;
    owner=frame_owner_start(&settings);
    if(!owner) { frame_packet_destroy(batch);batch=NULL;return 0; }
    sequence=0;batching=0;
    atomic_store(&published,0);atomic_store(&completed,0);
    atomic_store(&records,0);atomic_store(&bytes,0);atomic_store(&produced,0);
    atomic_store(&consumed,0);atomic_store(&busy_ns,0);atomic_store(&high_water,0);
    atomic_store(&oversize,0);
    atomic_store(&rejected,0);
    begin_frame();return 1;
}
void rt_stop(void) {
    if(!owner) return;
    rt_batch_discard();frame_owner_cancel(owner);
    frame_owner_stop(owner);owner=NULL;
    frame_packet_destroy(batch);batch=NULL;
}
int rt_running(void) { return owner!=NULL; }
int rt_on_render_thread(void) { return frame_owner_is_thread(); }
int rt_held(void) { return 0; }
void *rt_cmd_begin(uint16_t op,uint16_t flags,uint32_t payload) {
    if(!owner || frame_owner_is_thread()) return NULL;
    void *result=batching?frame_packet_append(batch,op,flags,payload):
                         frame_owner_append(owner,op,flags,payload);
    if(result) {
        uint64_t size=((uint64_t)sizeof(RtCmd)+payload+RT_ALIGN-1)&~(uint64_t)(RT_ALIGN-1);
        atomic_fetch_add(&records,1);atomic_fetch_add(&bytes,size);
        if(!batching) {
            frame_bytes+=(size_t)size;
            if(frame_bytes>atomic_load(&high_water)) atomic_store(&high_water,frame_bytes);
        }
    } else atomic_fetch_add(&oversize,1);
    return result;
}
void rt_cmd_commit(void) {}
/* An incomplete guest frame is never published merely to wake diagnostics. */
void rt_flush(void) {}
int rt_batch_begin(void) {
    if(!owner || batching) return 0;
    batching=frame_packet_begin(batch,0,NULL,0);return batching;
}
int rt_batch_active(void) { return batching; }
int rt_batch_try_commit(void) {
    if(!batching) return 0;
    batching=0;
    if(!frame_packet_publish(batch)) return 0;
    FramePacketView view;
    if(!frame_packet_acquire(batch,&view)) return 0;
    int ok=frame_owner_append_records(owner,view.records,view.record_bytes);
    if(ok) frame_bytes+=view.record_bytes;
    frame_packet_release(batch);return ok;
}
void rt_batch_discard(void) {
    if(!batching) return;
    frame_packet_cancel(batch);batching=0;
}
void rt_frame_end(void) {
    if(!owner) return;
    if(batching) abort();
    if(config.seal_frame && !config.seal_frame(config.user,owner)) frame_owner_cancel(owner);
    if(frame_owner_publish(owner)) {
        atomic_store(&published,sequence);atomic_fetch_add(&produced,1);
    } else atomic_fetch_add(&rejected,1);
    begin_frame();
}
void rt_invalidate(void) {
    if(!owner) return;
    rt_batch_discard();frame_owner_invalidate(owner);
    atomic_store(&published,0);begin_frame();
}
int rt_frames_ahead(void) { return owner?frame_owner_pending(owner):0; }
uint64_t rt_backpressure_events(void) { return 0; }
uint64_t rt_render_idle_ns(void) { return owner?frame_owner_idle_ns(owner):0; }
void rt_tick_at(uint64_t deadline) { if(owner) frame_owner_tick_at(owner,deadline); }
/* Runtime context borrowing has been removed. Callers must queue owner work. */
void rt_acquire(const char *reason) {
    if(!owner || frame_owner_is_thread()) return;
    fprintf(stderr,"presentation owner violation: synchronous %s\n",reason?reason:"GL access");
    abort();
}
void rt_release(void) {}
/* Test-only completion barrier. Runtime source must not call this; unlike
 * acquisition it never transfers the context. Tests close their synthetic
 * frame here before inspecting it through an owner-side request. */
static void drain_complete(const void *payload) {
    _Atomic int *done;memcpy(&done,payload,sizeof done);atomic_store(done,1);
}
void rt_drain(void) {
    if(!owner || frame_owner_is_thread()) return;
    if(frame_bytes) rt_frame_end();
    _Atomic int done=0;_Atomic int *pointer=&done;
    if(!rt_request(drain_complete,&pointer,sizeof pointer)) abort();
    while(!atomic_load(&done)) {
#ifdef _WIN32
        Sleep(0);
#else
        sched_yield();
#endif
    }
}
typedef struct Request { void (*callback)(const void *); unsigned char payload[]; } Request;
static void execute_request(void *unused,const void *payload,size_t count,int stale) {
    (void)unused;(void)count;(void)stale;
    const Request *request=payload;
    request->callback(request->payload);
}
static int request_work(void (*callback)(const void *),const void *payload,uint32_t count,int control) {
    if(!owner || !callback || (count && !payload) || count>1024*1024-sizeof(Request)) return 0;
    Request *request=malloc(sizeof(Request)+count);
    if(!request) return 0;
    request->callback=callback;if(count) memcpy(request->payload,payload,count);
    int ok=control?frame_owner_control(owner,sequence,execute_request,NULL,request,sizeof(Request)+count):
                   frame_owner_request(owner,atomic_load(&published),execute_request,NULL,request,sizeof(Request)+count);
    free(request);return ok;
}
int rt_request(void (*callback)(const void *),const void *payload,uint32_t count) {
    return request_work(callback,payload,count,0);
}
int rt_control(void (*callback)(const void *),const void *payload,uint32_t count) {
    return request_work(callback,payload,count,1);
}
int rt_packet_current(const FramePacketView *packet) {
    return owner && frame_owner_current(owner,packet);
}
void rt_get_stats(RtStats *out) {
    if(!out) return;
    memset(out,0,sizeof *out);
    out->running=owner!=NULL;out->max_frames=2;
    out->owner_delay_ms=owner_delay_ms;
    out->records=atomic_load(&records);out->bytes=atomic_load(&bytes);
    out->frames_produced=atomic_load(&produced);out->frames_consumed=atomic_load(&consumed);
    out->frames_replaced=owner?frame_owner_replaced(owner):0;
    out->frames_rejected=atomic_load(&rejected);
    out->render_busy_ns=atomic_load(&busy_ns);out->ring_high_water=atomic_load(&high_water);
    out->render_idle_ns=rt_render_idle_ns();
    out->oversize=atomic_load(&oversize);
}
int rt_acquire_events(RtAcquireEvent *out,int cap) { (void)out;(void)cap;return 0; }
