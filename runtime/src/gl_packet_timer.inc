/* Bounded GPU timestamps scoped to consumed packets, never the idle gap
 * between packets. Timestamp queries compose with per-pass elapsed queries.
 * An unavailable result drops telemetry capacity, never blocks the owner. */
#define PACKET_TIMER_SLOTS 8
#define PACKET_TIMER_PRESENTS 8
#define PACKET_TIMER_WAITS 8
#define PACKET_TIMER_RESTAGE (2+2*PACKET_TIMER_PRESENTS+2*PACKET_TIMER_WAITS)
#define PACKET_TIMER_QUERIES (PACKET_TIMER_RESTAGE+2)
typedef struct PacketTimerSlot {
    GLuint query[PACKET_TIMER_QUERIES];
    unsigned presents,waits;
    int restage;
    void *fence;
} PacketTimerSlot;
static PacketTimerSlot s_pkt_time[PACKET_TIMER_SLOTS];
static StreamWait s_pkt_fence_wait;
static uint64_t s_pkt_head,s_pkt_tail;
static int s_pkt_enabled=-1,s_pkt_active=-1,s_pkt_present_open;
static int s_pkt_wait_open;
static _Atomic uint64_t s_pkt_samples,s_pkt_ns,s_pkt_present_ns,s_pkt_max_ns,s_pkt_dropped;
static _Atomic uint64_t s_pkt_wait_ns,s_pkt_wait_dropped;
static _Atomic uint64_t s_pkt_fence_retired;
static _Atomic uint64_t s_pkt_restage_ns,s_pkt_restage_samples;
static void packet_timer_resolve(void) {
    while(s_pkt_tail<s_pkt_head) {
        PacketTimerSlot *slot=&s_pkt_time[s_pkt_tail%PACKET_TIMER_SLOTS];
        GLint ready=0;p_glGetQueryObjectiv(slot->query[1],GL_QUERY_RESULT_AVAILABLE,&ready);
        if(!ready) {
            GLenum completed=slot->fence?s_pkt_fence_wait(slot->fence,0,0):0;
            if(completed!=0x911A && completed!=0x911C)break;
            /* Some drivers leave QUERY_RESULT_AVAILABLE false across
             * shared-context presentation. A signalled fence placed after
             * the end timestamp proves that both results have completed. */
            atomic_fetch_add(&s_pkt_fence_retired,1);
        }
        GLuint64 begin=0,end=0,present=0,wait=0;
        p_glGetQueryObjectui64v(slot->query[0],GL_QUERY_RESULT,&begin);
        p_glGetQueryObjectui64v(slot->query[1],GL_QUERY_RESULT,&end);
        for(unsigned i=0;i<slot->presents;++i) {
            GLuint64 a=0,b=0;
            p_glGetQueryObjectui64v(slot->query[2+i*2],GL_QUERY_RESULT,&a);
            p_glGetQueryObjectui64v(slot->query[3+i*2],GL_QUERY_RESULT,&b);
            if(b>=a)present+=b-a;
        }
        for(unsigned i=0;i<slot->waits;++i) {
            GLuint64 a=0,b=0;
            const unsigned q=2+2*PACKET_TIMER_PRESENTS+2*i;
            p_glGetQueryObjectui64v(slot->query[q],GL_QUERY_RESULT,&a);
            p_glGetQueryObjectui64v(slot->query[q+1],GL_QUERY_RESULT,&b);
            if(b>=a)wait+=b-a;
        }
        if(end>=begin) {
            const uint64_t ns=end-begin;
            atomic_fetch_add(&s_pkt_samples,1);atomic_fetch_add(&s_pkt_ns,ns);
            atomic_fetch_add(&s_pkt_present_ns,present);
            atomic_fetch_add(&s_pkt_wait_ns,wait);
            if(ns>atomic_load(&s_pkt_max_ns))atomic_store(&s_pkt_max_ns,ns);
        }
        if(slot->restage) {
            GLuint64 a=0,b=0;
            p_glGetQueryObjectui64v(slot->query[PACKET_TIMER_RESTAGE],GL_QUERY_RESULT,&a);
            p_glGetQueryObjectui64v(slot->query[PACKET_TIMER_RESTAGE+1],GL_QUERY_RESULT,&b);
            if(b>=a) { atomic_fetch_add(&s_pkt_restage_ns,b-a);atomic_fetch_add(&s_pkt_restage_samples,1); }
        }
        if(slot->fence){p_glDeleteSync(slot->fence);slot->fence=NULL;}
        ++s_pkt_tail;
    }
}
static void packet_timer_begin(void) {
    if(s_pkt_enabled<0) {
        const char *value=getenv("PSX_GL_ASYNC_GPU_TIMER");
        s_pkt_fence_wait=(StreamWait)SDL_GL_GetProcAddress("glClientWaitSync");
        s_pkt_enabled=value && *value && *value!='0' && s_pkt_fence_wait && p_glFenceSync && p_glDeleteSync && p_glQueryCounter &&
            p_glGenQueries && p_glDeleteQueries && p_glGetQueryObjectiv && p_glGetQueryObjectui64v;
        if(s_pkt_enabled) for(unsigned i=0;i<PACKET_TIMER_SLOTS;++i)
            p_glGenQueries(PACKET_TIMER_QUERIES,s_pkt_time[i].query);
    }
    if(!s_pkt_enabled)return;
    packet_timer_resolve();
    if(s_pkt_head-s_pkt_tail==PACKET_TIMER_SLOTS) {
        atomic_fetch_add(&s_pkt_dropped,1);return;
    }
    s_pkt_active=(int)(s_pkt_head%PACKET_TIMER_SLOTS);
    s_pkt_time[s_pkt_active].presents=s_pkt_time[s_pkt_active].waits=0;
    s_pkt_time[s_pkt_active].restage=0;
    s_pkt_present_open=s_pkt_wait_open=0;
    p_glQueryCounter(s_pkt_time[s_pkt_active].query[0],GL_TIMESTAMP);
}
static void packet_timer_present_begin(void) {
    if(s_pkt_active<0)return;
    PacketTimerSlot *slot=&s_pkt_time[s_pkt_active];
    if(slot->presents==PACKET_TIMER_PRESENTS)return;
    p_glQueryCounter(slot->query[2+2*slot->presents],GL_TIMESTAMP);
    s_pkt_present_open=1;
}
static void packet_timer_present_end(void) {
    if(s_pkt_active<0 || !s_pkt_present_open)return;
    PacketTimerSlot *slot=&s_pkt_time[s_pkt_active];
    p_glQueryCounter(slot->query[3+2*slot->presents],GL_TIMESTAMP);
    ++slot->presents;s_pkt_present_open=0;
}
static void packet_timer_end(void) {
    if(s_pkt_active<0)return;
    packet_timer_present_end();
    p_glQueryCounter(s_pkt_time[s_pkt_active].query[1],GL_TIMESTAMP);
    s_pkt_time[s_pkt_active].fence=p_glFenceSync(0x9117 /* GPU_COMMANDS_COMPLETE */,0);
    ++s_pkt_head;s_pkt_active=-1;
}
static void packet_timer_restage_begin(void) {
    if(s_pkt_active>=0) p_glQueryCounter(s_pkt_time[s_pkt_active].query[PACKET_TIMER_RESTAGE],GL_TIMESTAMP);
}
static void packet_timer_restage_end(void) {
    if(s_pkt_active<0)return;
    p_glQueryCounter(s_pkt_time[s_pkt_active].query[PACKET_TIMER_RESTAGE+1],GL_TIMESTAMP);
    s_pkt_time[s_pkt_active].restage=1;
}
/* Measure a server-side wait separately; keep the original inclusive packet
 * span unchanged. A present-slot reuse fence can idle the GPU without doing
 * rendering work, and must not be mistaken for shader/fill-rate cost. */
static void packet_timer_wait_begin(void) {
    if(s_pkt_active<0)return;
    PacketTimerSlot *slot=&s_pkt_time[s_pkt_active];
    if(slot->waits==PACKET_TIMER_WAITS){++s_pkt_wait_dropped;return;}
    p_glQueryCounter(slot->query[2+2*PACKET_TIMER_PRESENTS+2*slot->waits],GL_TIMESTAMP);
    s_pkt_wait_open=1;
}
static void packet_timer_wait_end(void) {
    if(s_pkt_active<0 || !s_pkt_wait_open)return;
    PacketTimerSlot *slot=&s_pkt_time[s_pkt_active];
    p_glQueryCounter(slot->query[3+2*PACKET_TIMER_PRESENTS+2*slot->waits],GL_TIMESTAMP);
    ++slot->waits;s_pkt_wait_open=0;
}
static void packet_timer_reset(void) {
    if(s_pkt_enabled>0)for(unsigned i=0;i<PACKET_TIMER_SLOTS;++i) {
        if(s_pkt_time[i].fence)p_glDeleteSync(s_pkt_time[i].fence);
        p_glDeleteQueries(PACKET_TIMER_QUERIES,s_pkt_time[i].query);
    }
    memset(s_pkt_time,0,sizeof s_pkt_time);
    s_pkt_head=s_pkt_tail=0;s_pkt_enabled=-1;s_pkt_active=-1;s_pkt_present_open=0;
    atomic_store(&s_pkt_samples,0);atomic_store(&s_pkt_ns,0);
    atomic_store(&s_pkt_present_ns,0);atomic_store(&s_pkt_max_ns,0);atomic_store(&s_pkt_dropped,0);
    atomic_store(&s_pkt_fence_retired,0);
    atomic_store(&s_pkt_restage_ns,0);atomic_store(&s_pkt_restage_samples,0);
    atomic_store(&s_pkt_wait_ns,0);atomic_store(&s_pkt_wait_dropped,0);s_pkt_wait_open=0;
}
void gl_renderer_packet_gpu_stats(uint64_t out[5]) {
    if(!out)return;
    out[0]=atomic_load(&s_pkt_samples);out[1]=atomic_load(&s_pkt_ns);
    out[2]=atomic_load(&s_pkt_present_ns);out[3]=atomic_load(&s_pkt_max_ns);
    out[4]=atomic_load(&s_pkt_dropped);
}
void gl_renderer_packet_gpu_details(uint64_t out[4]) {
    out[0]=atomic_load(&s_pkt_wait_ns);out[1]=atomic_load(&s_pkt_restage_ns);
    out[2]=atomic_load(&s_pkt_restage_samples);out[3]=atomic_load(&s_pkt_fence_retired);
}
