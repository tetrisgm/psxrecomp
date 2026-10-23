#ifndef PSX_DRAW_WORKER_POOL_H
#define PSX_DRAW_WORKER_POOL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Bounded draw execution, independent of the authoritative guest clock.
 * One persistent executor per slot; callbacks may use its private context.
 * Producer APIs are serialized on main. They never wait for a draw to finish.
 * The owner supplies the same monotonic nanosecond clock as its presenter. */
typedef struct DrawWorkerPool DrawWorkerPool;
typedef uint64_t (*DrawWorkerClock)(void *);
typedef int (*DrawWorkerExecute)(void *, unsigned slot, void *payload);
typedef void (*DrawWorkerDispose)(void *payload);
typedef struct DrawWorkerJob {
    uint64_t epoch, generation, index, deadline, expires;
    void *payload; /* immutable inputs; callback owns its private result fields */
    DrawWorkerDispose dispose;
} DrawWorkerJob;
enum { DRAW_WORKER_COMPLETE = 0, DRAW_WORKER_EXPIRED = -1,
       DRAW_WORKER_CANCELLED = -2 };
typedef struct DrawWorkerResult {
    uint64_t ticket;
    unsigned slot;
    int status; /* callback result, or EXPIRED/CANCELLED */
    DrawWorkerJob job;
    int executed; /* callback ran, even if its output later became stale */
    int execution_status; /* unchanged callback result when executed */
} DrawWorkerResult;

DrawWorkerPool *draw_worker_create(unsigned workers, uint64_t epoch,
    DrawWorkerClock clock, DrawWorkerExecute execute, void *user);
/* Success transfers job payload ownership to pool. Failure leaves it with
 * caller. Full pools and invalid/expired jobs return immediately. */
int draw_worker_submit(DrawWorkerPool *, const DrawWorkerJob *);
/* Claim one ready result, earliest deadline first. Zero means none ready. */
int draw_worker_poll(DrawWorkerPool *, DrawWorkerResult *);
/* Recheck after any time spent consuming other results; stale output must
 * never enter a later presentation generation. */
int draw_worker_current(DrawWorkerPool *, const DrawWorkerResult *, uint64_t generation);
/* Release a claimed result exactly once. Its payload then becomes invalid. */
int draw_worker_release(DrawWorkerPool *, const DrawWorkerResult *);
/* Invalidates queued/running/results; running callbacks finish privately.
 * A result must be released even when it became stale. */
void draw_worker_set_epoch(DrawWorkerPool *, uint64_t epoch);
/* Teardown only: join active callbacks, then dispose all owned payloads. */
void draw_worker_destroy(DrawWorkerPool *);
#ifdef __cplusplus
}
#endif
#endif
