#include "presentation_wake.h"
#undef NDEBUG
#include <cassert>
#include <thread>
#include <cstdio>
int main() {
    using Clock=std::chrono::steady_clock;
    PresentationWake wake;
    wake.notify();wake.notify();
    auto start=Clock::now();wake.wait(500000000);
    assert(Clock::now()-start<std::chrono::milliseconds(100));
    std::thread producer([&] {std::this_thread::sleep_for(std::chrono::milliseconds(2));wake.notify();});
    start=Clock::now();wake.wait(500000000);producer.join();
    assert(Clock::now()-start<std::chrono::milliseconds(100));
    // The cancelled deadline cannot leak into a subsequent timed wait.
    start=Clock::now();wake.wait(10000000);
    assert(Clock::now()-start>=std::chrono::milliseconds(10));
    start=Clock::now();
    for(int i=0;i<100;++i)wake.wait(1000000);
    double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count()/100;
    printf("1 ms retained-event deadline mean: %.3f ms\n",ms);
#ifdef _WIN32
    // The old system-tick timeout consistently takes about 15.6 ms here.
    assert(ms<8.0);
#endif
}
