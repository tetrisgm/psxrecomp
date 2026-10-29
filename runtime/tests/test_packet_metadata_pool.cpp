#undef NDEBUG
#include "packet_metadata.h"
#include <cassert>
#include <cstdlib>
#include <new>
#include <array>
#include <thread>
static thread_local bool counting=false;
static thread_local size_t allocations=0;
void* operator new(size_t size) {if(counting)++allocations;if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
static int value=0;
static int capture(void* out) {*static_cast<int*>(out)=value;return value!=42;}
static void blend(const void*,const void* b,double,void* out) {*static_cast<int*>(out)=*static_cast<const int*>(b);}
struct StartupRegistration {StartupRegistration() {
    assert(packet_metadata_register(1,sizeof(int),capture,blend));
}} startup_registration;
int main() {
    std::array<PacketMetadata*,4096> held{};
    counting=true;
    for(auto& p:held) {p=packet_metadata_capture();assert(p);}
    assert(!packet_metadata_capture());
    counting=false;
    std::thread cleanup([&] {for(auto* p:held)packet_metadata_free(p);});cleanup.join();
    counting=true;
    for(int i=0;i<10000;++i) {
        value=i;auto* p=packet_metadata_capture();assert(p);packet_metadata_endpoint(p);
        int got=-1;assert(packet_metadata_read(1,&got,sizeof(got))==(i!=42));
        if(i!=42)assert(got==i);
        packet_metadata_end();packet_metadata_free(p);
    }
    counting=false;assert(allocations==0);
}
