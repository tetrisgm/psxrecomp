/* Bounded GPU timestamps scoped to consumed packets, never the idle gap
 * between packets. Timestamp queries compose with per-pass elapsed queries.
 * An unavailable result drops telemetry capacity, never blocks the owner. */
#define PACKET_TIMER_SLOTS 8
#define PACKET_TIMER_PRESENTS 8
typedef struct PacketTimerSlot {
    GLuint query[2+2*PACKET_TIMER_PRESENTS];
    unsigned presents;
} PacketTimerSlot;
static PacketTimerSlot s_pkt_time[PACKET_TIMER_SLOTS];
static uint64_t s_pkt_head,s_pkt_tail;
static int s_pkt_enabled=-1,s_pkt_active=-1,s_pkt_present_open;
static _Atomic uint64_t s_pkt_samples,s_pkt_ns,s_pkt_present_ns,s_pkt_max_ns,s_pkt_dropped;
static void packet_timer_resolve(void) {
    while(s_pkt_tail<s_pkt_head) {
        PacketTimerSlot *slot=&s_pkt_time[s_pkt_tail%PACKET_TIMER_SLOTS];
        GLint ready=0;p_glGetQueryObjectiv(slot->query[1],GL_QUERY_RESULT_AVAILABLE,&ready);
        if(!ready)break;
        GLuint64 begin=0,end=0,present=0;
        p_glGetQueryObjectui64v(slot->query[0],GL_QUERY_RESULT,&begin);
        p_glGetQueryObjectui64v(slot->query[1],GL_QUERY_RESULT,&end);
        for(unsigned i=0;i<slot->presents;++i) {
            GLuint64 a=0,b=0;
            p_glGetQueryObjectui64v(slot->query[2+i*2],GL_QUERY_RESULT,&a);
            p_glGetQueryObjectui64v(slot->query[3+i*2],GL_QUERY_RESULT,&b);
            if(b>=a)present+=b-a;
        }
        if(end>=begin) {
            const uint64_t ns=end-begin;
            atomic_fetch_add(&s_pkt_samples,1);atomic_fetch_add(&s_pkt_ns,ns);
            atomic_fetch_add(&s_pkt_present_ns,present);
            if(ns>atomic_load(&s_pkt_max_ns))atomic_store(&s_pkt_max_ns,ns);
        }
        ++s_pkt_tail;
    }
}
static void packet_timer_begin(void) {
    if(s_pkt_enabled<0) {
        const char *value=getenv("PSX_GL_ASYNC_GPU_TIMER");
        s_pkt_enabled=value && *value && *value!='0' && p_glQueryCounter &&
            p_glGenQueries && p_glDeleteQueries && p_glGetQueryObjectiv && p_glGetQueryObjectui64v;
        if(s_pkt_enabled) for(unsigned i=0;i<PACKET_TIMER_SLOTS;++i)
            p_glGenQueries(2+2*PACKET_TIMER_PRESENTS,s_pkt_time[i].query);
    }
    if(!s_pkt_enabled)return;
    packet_timer_resolve();
    if(s_pkt_head-s_pkt_tail==PACKET_TIMER_SLOTS) {
        atomic_fetch_add(&s_pkt_dropped,1);return;
    }
    s_pkt_active=(int)(s_pkt_head%PACKET_TIMER_SLOTS);
    s_pkt_time[s_pkt_active].presents=0;s_pkt_present_open=0;
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
    ++s_pkt_head;s_pkt_active=-1;
}
static void packet_timer_reset(void) {
    if(s_pkt_enabled>0)for(unsigned i=0;i<PACKET_TIMER_SLOTS;++i)
        p_glDeleteQueries(2+2*PACKET_TIMER_PRESENTS,s_pkt_time[i].query);
    memset(s_pkt_time,0,sizeof s_pkt_time);
    s_pkt_head=s_pkt_tail=0;s_pkt_enabled=-1;s_pkt_active=-1;s_pkt_present_open=0;
    atomic_store(&s_pkt_samples,0);atomic_store(&s_pkt_ns,0);
    atomic_store(&s_pkt_present_ns,0);atomic_store(&s_pkt_max_ns,0);atomic_store(&s_pkt_dropped,0);
}
void gl_renderer_packet_gpu_stats(uint64_t out[5]) {
    if(!out)return;
    out[0]=atomic_load(&s_pkt_samples);out[1]=atomic_load(&s_pkt_ns);
    out[2]=atomic_load(&s_pkt_present_ns);out[3]=atomic_load(&s_pkt_max_ns);
    out[4]=atomic_load(&s_pkt_dropped);
}
