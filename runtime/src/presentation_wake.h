#ifndef PSX_PRESENTATION_WAKE_H
#define PSX_PRESENTATION_WAKE_H
#include <atomic>
#include <chrono>
#include <system_error>
#include <new>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <semaphore.h>
#include <cerrno>
#include <ctime>
#endif
/* Retained, coalesced notification. The producer never acquires a lock owned
 * by the render consumer, even when the latter is descheduled. */
class PresentationWake {
#ifdef _WIN32
    HANDLE event;
#else
#ifdef __APPLE__
    dispatch_semaphore_t event;
#else
    sem_t event;
#endif
    std::atomic<bool> pending{false};
#endif
public:
    PresentationWake() {
#ifdef _WIN32
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event) throw std::system_error(GetLastError(),std::system_category());
#elif defined(__APPLE__)
        event=dispatch_semaphore_create(0);
        if(!event) throw std::bad_alloc();
#else
        if(sem_init(&event,0,0)) throw std::system_error(errno,std::generic_category());
#endif
    }
    ~PresentationWake() {
#ifdef _WIN32
        CloseHandle(event);
#elif defined(__APPLE__)
        dispatch_release(event);
#else
        sem_destroy(&event);
#endif
    }
    void notify() {
#ifdef _WIN32
        SetEvent(event);
#elif defined(__APPLE__)
        if(!pending.exchange(true,std::memory_order_acq_rel)) dispatch_semaphore_signal(event);
#else
        if(!pending.exchange(true,std::memory_order_acq_rel)) sem_post(&event);
#endif
    }
    void wait(uint64_t ns=0) {
#ifdef _WIN32
        WaitForSingleObject(event,ns?DWORD((ns+999999)/1000000):INFINITE);
#elif defined(__APPLE__)
        if(!dispatch_semaphore_wait(event,ns?dispatch_time(DISPATCH_TIME_NOW,ns):DISPATCH_TIME_FOREVER))
            pending.store(false,std::memory_order_release);
#else
        int result;
        if(ns) {
            timespec until;clock_gettime(CLOCK_REALTIME,&until);
            const uint64_t nanos=uint64_t(until.tv_nsec)+ns;
            until.tv_sec+=nanos/1000000000;until.tv_nsec=nanos%1000000000;
            do {result=sem_timedwait(&event,&until);} while(result && errno==EINTR);
        } else do {result=sem_wait(&event);} while(result && errno==EINTR);
        if(!result) pending.store(false,std::memory_order_release);
#endif
    }
    PresentationWake(const PresentationWake&)=delete;
    PresentationWake& operator=(const PresentationWake&)=delete;
};
#endif
