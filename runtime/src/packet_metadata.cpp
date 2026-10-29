#include "packet_metadata.h"
#include <vector>
#include <mutex>
#include <cstring>
#include <cstddef>
#include <algorithm>
#include <array>
#include <atomic>
namespace {
struct Provider {uint64_t key;size_t bytes;PacketMetadataCapture capture;PacketMetadataBlend blend;};
struct Entry {Provider provider;std::vector<std::max_align_t> data;bool valid=false;};
std::vector<Provider>& providers() {static std::vector<Provider> list;return list;}
std::mutex& provider_mutex() {static std::mutex m;return m;}
}
struct PacketMetadata {std::vector<Entry> entries;PacketMetadata* next=nullptr;};
namespace {
/* The runtime caps rewind at 3600 frames. Leave room for presentation leases,
 * producer and replaced packets; registration prepares all payload storage. */
constexpr size_t metadata_capacity=4096;
std::array<PacketMetadata,metadata_capacity>& storage() {
    static std::array<PacketMetadata,metadata_capacity> pool;return pool;
}
std::atomic<PacketMetadata*> returned{nullptr};
PacketMetadata* ready=nullptr;
bool initialized=false,started=false;
void prepare_pool() {
    if(initialized)return;
    for(auto& p:storage()) {p.next=ready;ready=&p;}
    initialized=true;
}
}
namespace {
thread_local PacketMetadata old,current,sampled;
thread_local uint64_t epoch=0;
thread_local bool active=false;
thread_local const PacketMetadata *endpoint=nullptr;
}
extern "C" int packet_metadata_register(uint64_t key,size_t bytes,PacketMetadataCapture capture,PacketMetadataBlend blend) {
    if(!key || !bytes || bytes>65536 || !capture || !blend) return 0;
    std::lock_guard<std::mutex> lock(provider_mutex());
    if(started)return 0; /* Registration is a startup-only API. */
    for(const auto& p:providers()) if(p.key==key) return 0;
    const Provider provider{key,bytes,capture,blend};
    const size_t previous=providers().size();
    try {
        for(auto& packet:storage()) {
            packet.entries.push_back({provider,{}});
            packet.entries.back().data.resize((bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
        }
        providers().push_back(provider);prepare_pool();return 1;
    } catch(...) {
        for(auto& packet:storage())packet.entries.resize(previous);
        return 0;
    }
}
extern "C" PacketMetadata *packet_metadata_capture(void) {
    started=true;prepare_pool();
    if(!ready)ready=returned.exchange(nullptr,std::memory_order_acquire);
    if(!ready)return nullptr;
    auto* result=ready;ready=result->next;result->next=nullptr;
    try {for(auto& e:result->entries)e.valid=e.provider.capture(e.data.data())!=0;}
    catch(...) {packet_metadata_free(result);return nullptr;}
    return result;
}
extern "C" void packet_metadata_free(PacketMetadata *p) {
    if(!p)return;
    auto* head=returned.load(std::memory_order_relaxed);
    do {p->next=head;} while(!returned.compare_exchange_weak(head,p,
        std::memory_order_release,std::memory_order_relaxed));
}
extern "C" size_t packet_metadata_bytes(const PacketMetadata *p) {
    if(!p) return 0;size_t n=sizeof(*p)+p->entries.capacity()*sizeof(Entry);
    for(const auto& e:p->entries) n+=e.data.capacity()*sizeof(std::max_align_t);return n;
}
extern "C" void packet_metadata_pair(const PacketMetadata *p,uint64_t next_epoch) {
    try {old=next_epoch==epoch?current:PacketMetadata{};current=p?*p:PacketMetadata{};sampled=current;epoch=next_epoch;}
    catch(...) {packet_metadata_reset();}
}
extern "C" void packet_metadata_sample(double t) {
    endpoint=nullptr;
    t=std::clamp(t,0.0,1.0);
    for(size_t i=0;i<current.entries.size();++i) {
        const auto& n=current.entries[i];auto& out=sampled.entries[i];
        std::memcpy(out.data.data(),n.data.data(),n.provider.bytes);
        if(t==1) continue;
        for(const auto& a:old.entries) if(a.valid && n.valid && a.provider.key==n.provider.key && a.provider.bytes==n.provider.bytes) {
            n.provider.blend(a.data.data(),n.data.data(),t,out.data.data());break;
        }
    }
    active=true;
}
extern "C" void packet_metadata_endpoint(const PacketMetadata *p) {endpoint=p;active=true;}
extern "C" void packet_metadata_end(void) {active=false;endpoint=nullptr;}
extern "C" void packet_metadata_reset(void) {old={};current={};sampled={};epoch=0;active=false;}
extern "C" int packet_metadata_read(uint64_t key,void *out,size_t bytes) {
    if(!active || !out) return 0;
    for(const auto& e:(endpoint?endpoint->entries:sampled.entries)) if(e.valid && e.provider.key==key && e.provider.bytes==bytes) {
        std::memcpy(out,e.data.data(),bytes);return 1;
    }
    return 0;
}
