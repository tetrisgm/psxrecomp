#include "packet_metadata.h"
#include <vector>
#include <mutex>
#include <cstring>
#include <cstddef>
#include <algorithm>
namespace {
struct Provider {uint64_t key;size_t bytes;PacketMetadataCapture capture;PacketMetadataBlend blend;};
struct Entry {Provider provider;std::vector<std::max_align_t> data;};
std::vector<Provider>& providers() {static std::vector<Provider> list;return list;}
std::mutex& provider_mutex() {static std::mutex m;return m;}
}
struct PacketMetadata {std::vector<Entry> entries;};
namespace {
thread_local PacketMetadata old,current,sampled;
thread_local uint64_t epoch=0;
thread_local bool active=false;
thread_local const PacketMetadata *endpoint=nullptr;
}
extern "C" int packet_metadata_register(uint64_t key,size_t bytes,PacketMetadataCapture capture,PacketMetadataBlend blend) {
    if(!key || !bytes || bytes>65536 || !capture || !blend) return 0;
    std::lock_guard<std::mutex> lock(provider_mutex());
    for(const auto& p:providers()) if(p.key==key) return 0;
    try {providers().push_back({key,bytes,capture,blend});return 1;} catch(...) {return 0;}
}
extern "C" PacketMetadata *packet_metadata_capture(void) {
    PacketMetadata *result=nullptr;
    try {
        result=new PacketMetadata;
        std::lock_guard<std::mutex> lock(provider_mutex());
        for(const auto& p:providers()) {
            Entry entry{p,{}};entry.data.resize((p.bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
            if(p.capture(entry.data.data())) result->entries.push_back(std::move(entry));
        }
        return result;
    } catch(...) {delete result;return nullptr;}
}
extern "C" void packet_metadata_free(PacketMetadata *p) {delete p;}
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
        for(const auto& a:old.entries) if(a.provider.key==n.provider.key && a.provider.bytes==n.provider.bytes) {
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
    for(const auto& e:(endpoint?endpoint->entries:sampled.entries)) if(e.provider.key==key && e.provider.bytes==bytes) {
        std::memcpy(out,e.data.data(),bytes);return 1;
    }
    return 0;
}
