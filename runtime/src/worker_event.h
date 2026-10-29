#ifndef PSX_WORKER_EVENT_H
#define PSX_WORKER_EVENT_H
/* A retained one-bit wake: notify before wait cannot be lost. On Windows,
 * publication bypasses the C++ condition variable's internal mutex and its
 * coarse timed-wait fallback. Resources are allocated only at worker setup. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <system_error>
class WorkerEvent {
    HANDLE event;
public:
    WorkerEvent():event(CreateEventW(nullptr,FALSE,FALSE,nullptr)) {
        if(!event)throw std::system_error(GetLastError(),std::system_category(),"CreateEventW");
    }
    ~WorkerEvent(){CloseHandle(event);}
    void notify(){SetEvent(event);}
    void wait(){WaitForSingleObject(event,INFINITE);}
    WorkerEvent(const WorkerEvent&)=delete;
    WorkerEvent& operator=(const WorkerEvent&)=delete;
};
#else
#include <condition_variable>
#include <mutex>
class WorkerEvent {
    std::mutex mutex;
    std::condition_variable changed;
    bool pending=false;
public:
    void notify(){
        {std::lock_guard<std::mutex> lock(mutex);pending=true;}
        changed.notify_one();
    }
    void wait(){
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock,[this]{return pending;});pending=false;
    }
};
#endif
#endif
