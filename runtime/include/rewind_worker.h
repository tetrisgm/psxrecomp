#ifndef PSX_REWIND_WORKER_H
#define PSX_REWIND_WORKER_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RewindWorker RewindWorker;
/* One preallocated job, single producer. Callback never accesses live guest
 * state. Ready is an acquire publication of all callback writes. */
RewindWorker *rewind_worker_create(void (*run)(void *),void *context);
int rewind_worker_submit(RewindWorker *worker);
int rewind_worker_ready(RewindWorker *worker);
void rewind_worker_wait(RewindWorker *worker);
void rewind_worker_destroy(RewindWorker *worker);
#ifdef __cplusplus
}
#endif
#endif
