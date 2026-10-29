/* Complete-frame presentation transport. One producer records owned commands
 * and immutable native inputs; a permanent graphics owner consumes the newest
 * complete packet. Pending packets may be replaced, never partial commands.
 * No render-queue backpressure reaches the guest. Durable controls/diagnostics
 * are independent of dropped packets. Context transfer occurs only at
 * startup/shutdown. Producer methods are not concurrently callable. */
#ifndef PSX_RENDER_THREAD_H
#define PSX_RENDER_THREAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record header. size covers header + payload, rounded up to RT_ALIGN. */
typedef struct RtCmd {
    uint32_t size;
    uint16_t op;      /* backend opcode; 0 is reserved (padding/frame) */
    uint16_t flags;
    uint32_t payload; /* payload bytes actually used */
    uint32_t seq;     /* low 32 bits of the record sequence number */
} RtCmd;

#define RT_ALIGN 16u

/* exec: replay one record on the render thread. ctx: make the graphics
 * context current on the calling thread (current != 0) or release it
 * (current == 0; the backend flushes first). */
typedef void (*RtExecFn)(void *user, const RtCmd *cmd, const void *payload);
typedef void (*RtCtxFn)(void *user, int current);
/* tick (optional): timed work on the render thread while it holds the
 * context (frame generation). Called with the current time before a record
 * is executed and while no complete packet is pending, once the deadline it last
 * returned has passed; returns the next deadline (rt_now_ns clock), 0 for
 * none. An idle render thread sleeps no later than that deadline. */
typedef uint64_t (*RtTickFn)(void *user, uint64_t now_ns);
struct FrameOwner;
struct FramePacketView;

typedef struct RtConfig {
    size_t   ring_bytes;   /* maximum record bytes per complete packet */
    int      max_frames;   /* legacy configuration; mailbox has one pending slot */
    RtExecFn exec;
    RtCtxFn  ctx;
    void    *user;
    RtTickFn tick;         /* optional (NULL) */
    /* Complete-frame transport. begin owns the initial seed before native
     * work for this frame; seal attaches immutable native inputs. Neither
     * callback may access GL. execute_frame runs only on the GL owner. */
    size_t initial_limit;
    int (*begin_frame)(void *user,struct FrameOwner *owner,uint64_t sequence);
    int (*seal_frame)(void *user,struct FrameOwner *owner);
    void (*execute_frame)(void *user,const struct FramePacketView *packet);
    void (*reset_frame)(void *user,uint64_t epoch);
} RtConfig;

/* Copied diagnostic request outside the droppable draw stream. It observes
 * the latest published frame (or its newer replacement), and works paused. */
int rt_request(void (*callback)(const void *),const void *payload,uint32_t bytes);
int rt_control(void (*callback)(const void *),const void *payload,uint32_t bytes);
int rt_packet_current(const struct FramePacketView *packet);
void rt_invalidate(void);

/* Start: the context must be current on the calling thread; on success it is
 * released here and made current on the new render thread. Returns 1 on
 * success, 0 (nothing started, context still current) on failure. */
int  rt_start(const RtConfig *cfg);
/* Stop: discard incomplete producer work, join and make the context current on
 * the calling thread again. Safe when not running. */
void rt_stop(void);
int  rt_running(void);
int  rt_on_render_thread(void);

/* Append to the producer-owned packet; returns aligned payload or NULL.
 * Allocation/limit failure invalidates the complete packet, never permits
 * executing GL on the producer. Optional batches reject atomically. */
void *rt_cmd_begin(uint16_t op, uint16_t flags, uint32_t payload_bytes);
void  rt_cmd_commit(void);
/* Compatibility no-op: incomplete frames are never published. */
void  rt_flush(void);
/* Optional producer transaction. Records stay private until try_commit;
 * overflow drops the entire optional batch without invalidating the frame.
 * No frame markers/context acquisition inside a batch. A NULL cmd_begin while
 * batching is a dropped record, never permission to execute it directly. */
int rt_batch_begin(void);
int rt_batch_active(void);
int rt_batch_try_commit(void);
void rt_batch_discard(void);

/* Seal/publish a complete frame and capture the next native seed. No GPU wait. */
void rt_frame_end(void);

/* Legacy FIFO query, always zero for the newest-wins mailbox. */
int  rt_frames_ahead(void);
/* Consumer running total of idle time awaiting packets or tick deadlines. */
uint64_t rt_render_idle_ns(void);
/* Monotonic nanoseconds (the clock tick deadlines use). */
uint64_t rt_now_ns(void);
/* Any thread: running count of times the emulation thread blocked on the
 * in-flight bound or a full ring (it waited on the render thread). */
uint64_t rt_backpressure_events(void);
/* Render thread, from tick or exec: ask for tick at `deadline_ns` (earliest
 * wins over the one tick last returned). */
void rt_tick_at(uint64_t deadline_ns);

/* Legacy API migration guard: acquire aborts when called on the producer.
 * Context borrowing is forbidden; release is a no-op and held is always 0. */
void rt_acquire(const char *reason);
void rt_release(void);
int rt_held(void);
/* Test-only complete-packet/control acknowledgement; never a runtime pacing
 * primitive and never transfers the graphics context. */
void rt_drain(void);

/* Statistics (emulation thread). */
typedef struct RtStats {
    uint64_t records, bytes, frames_produced, frames_consumed;
    uint64_t frames_replaced,frames_rejected;
    uint64_t acquires, releases, oversize;
    uint64_t backpressure_waits, ring_full_waits;
    uint64_t backpressure_ns, ring_full_ns, acquire_ns;
    uint64_t render_busy_ns, render_idle_ns;
    uint64_t ring_high_water;
    int      max_frames, running, held;
    unsigned owner_delay_ms; /* debug-only stalled-consumer experiment */
} RtStats;
void rt_get_stats(RtStats *out);

/* Ring of the most recent sync points (newest first). Returns the count. */
typedef struct RtAcquireEvent {
    uint64_t frame;      /* frames_produced at the time */
    uint64_t wait_ns;    /* drain + hand-off time */
    const char *reason;
} RtAcquireEvent;
int rt_acquire_events(RtAcquireEvent *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* PSX_RENDER_THREAD_H */
