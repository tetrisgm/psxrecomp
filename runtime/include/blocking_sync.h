#ifndef PSX_BLOCKING_SYNC_H
#define PSX_BLOCKING_SYNC_H
/* Statically initialized blocking primitives for C runtime publication and
 * completion paths. Predicate changes and notifications share the mutex. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK PsxBlockingMutex;
typedef CONDITION_VARIABLE PsxBlockingCond;
#define PSX_BLOCKING_MUTEX_INIT SRWLOCK_INIT
#define PSX_BLOCKING_COND_INIT CONDITION_VARIABLE_INIT
static inline void psx_blocking_lock(PsxBlockingMutex *m) { AcquireSRWLockExclusive(m); }
static inline void psx_blocking_unlock(PsxBlockingMutex *m) { ReleaseSRWLockExclusive(m); }
static inline void psx_blocking_wait(PsxBlockingCond *c, PsxBlockingMutex *m) {
    SleepConditionVariableSRW(c,m,INFINITE,0);
}
static inline void psx_blocking_notify(PsxBlockingCond *c) { WakeAllConditionVariable(c); }
#else
#include <pthread.h>
typedef pthread_mutex_t PsxBlockingMutex;
typedef pthread_cond_t PsxBlockingCond;
#define PSX_BLOCKING_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define PSX_BLOCKING_COND_INIT PTHREAD_COND_INITIALIZER
static inline void psx_blocking_lock(PsxBlockingMutex *m) { pthread_mutex_lock(m); }
static inline void psx_blocking_unlock(PsxBlockingMutex *m) { pthread_mutex_unlock(m); }
static inline void psx_blocking_wait(PsxBlockingCond *c, PsxBlockingMutex *m) { pthread_cond_wait(c,m); }
static inline void psx_blocking_notify(PsxBlockingCond *c) { pthread_cond_broadcast(c); }
#endif
#endif
