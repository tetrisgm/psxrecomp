#include "native_raster.h"
#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include "frame_packet.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>
#include <memory>
#include <new>
#include <cstdlib>
#include <cstring>
#include "native_raster_oracle_fixtures.h"

static thread_local bool count_allocations=false;
static thread_local size_t allocations=0;
void* operator new(size_t size) {
    if(count_allocations) ++allocations;
    if(void* p=std::malloc(size?size:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }

/* Native contexts do not use title-specific wide presentation. */
extern "C" {
int g_ws_bd_stretch_on = 0, g_ws_bd_stretch_pct = 0;
int psx_ws_prim_in_backdrop(void) { return 0; }
}
static void send(NativeRaster *r, NativeRasterOp op, std::initializer_list<int32_t> args) {
    NativeRasterCommand c{};
    c.op = op;
    unsigned i = 0;
    for (auto a : args) c.args[i++] = a;
    assert(native_raster_submit(r,&c,nullptr,0));
}
static uint16_t pixel(NativeRaster *r,int x,int y) {
    uint16_t p = 0;
    native_raster_read(r,x,y,1,1,&p);
    return p;
}
static void immutable_texture_inputs() {
    auto* raster=native_raster_create(nullptr);assert(raster);
    native_raster_inputs_enable(raster,1);
    NativeRasterState state{0,0,1023,511,0,0,16|256,0,0,0,0};
    send(raster,NR_WRITE,{0,256,31});
    const uint32_t first[]={0x65000080,700u|(100u<<16),0,1u|(1u<<16)};
    const uint64_t a=native_raster_gp0_identified(raster,&state,first,4);assert(a);
    send(raster,NR_WRITE,{0,256,992});
    const uint32_t second[]={0x65000080,701u|(100u<<16),0,1u|(1u<<16)};
    const uint64_t b=native_raster_gp0_identified(raster,&state,second,4);assert(b>a);
    auto* inputs=native_raster_inputs_seal(raster);assert(native_raster_inputs_valid(inputs));
    NativeRasterTile before[128],after[128];
    assert(native_raster_inputs_get(inputs,a,before,128)==16);
    assert(native_raster_inputs_get(inputs,b,after,128)==16);
    assert(native_raster_inputs_get(inputs,a,before,15)==-1);
    assert(native_raster_inputs_get(inputs,b+1,before,128)==0);
    assert(before[0].x==0 && before[0].y==256 && before[0].pixels[0]==31);
    assert(after[0].pixels[0]==992 && before[0].version!=after[0].version);
    for(unsigned n=1;n<16;++n) {
        assert(before[n].version==after[n].version);
        assert(before[n].pixels==after[n].pixels);
    }
    assert(native_raster_inputs_bytes(inputs)<150*1024); // 17 tiles, not two VRAM images
    assert(pixel(raster,700,100)==31 && pixel(raster,701,100)==992);
    // A subsequent collection shares unchanged native tile versions.
    const uint64_t c=native_raster_gp0_identified(raster,&state,second,4);assert(c>b);
    auto* next=native_raster_inputs_seal(raster);assert(native_raster_inputs_valid(next));
    NativeRasterTile retained[128];
    assert(native_raster_inputs_get(next,c,retained,128)==16);
    for(unsigned n=0;n<16;++n) assert(retained[n].pixels==after[n].pixels);
    // Eight-bit texpage and CLUT wrap horizontally at native VRAM's edge.
    state.page=15|16|128;
    const uint32_t wrap[]={0x65000080,702u|(100u<<16),uint32_t(63|(255<<6))<<16,1u|(1u<<16)};
    const uint64_t d=native_raster_gp0_identified(raster,&state,wrap,4);assert(d>c);
    auto* wrapped=native_raster_inputs_seal(raster);assert(native_raster_inputs_valid(wrapped));
    NativeRasterTile tiles[128];
    const int count=native_raster_inputs_get(wrapped,d,tiles,128);
    assert(count==13); // eight page tiles and five wrapped CLUT tiles
    bool found[128]{};
    for(int n=0;n<count;++n) found[(tiles[n].y/64)*16+tiles[n].x/64]=true;
    for(unsigned y=4;y<8;++y) assert(found[y*16] && found[y*16+15]);
    for(unsigned x: {0u,1u,2u,3u,15u}) assert(found[3*16+x]);
    // Restore invalidates future versions, not already sealed packets.
    std::vector<uint16_t> zero(1024*512);
    native_raster_restore(raster,zero.data());state.page=16|256;
    const uint64_t e=native_raster_gp0_identified(raster,&state,second,4);
    auto* restored=native_raster_inputs_seal(raster);assert(native_raster_inputs_valid(restored));
    assert(native_raster_inputs_get(restored,e,tiles,128)==16);
    assert(tiles[0].pixels[0]==0 && tiles[0].version!=after[0].version);
    // Malicious/high-churn presentation cannot grow without bound or stop
    // native work. A failed collection is rejected as a whole and recovers.
    for(unsigned n=0;n<8500;++n) {
        send(raster,NR_WRITE,{0,256,int(n+1)});
        assert(native_raster_gp0_identified(raster,&state,second,4));
    }
    auto* overflow=native_raster_inputs_seal(raster);
    assert(overflow && !native_raster_inputs_valid(overflow));
    assert(native_raster_inputs_get(overflow,e,tiles,128)==-1);
    assert(pixel(raster,701,100)==8500);
    const uint64_t recovered=native_raster_gp0_identified(raster,&state,second,4);
    auto* recovery=native_raster_inputs_seal(raster);
    assert(native_raster_inputs_valid(recovery));
    assert(native_raster_inputs_get(recovery,recovered,tiles,128)==16);
    assert(tiles[0].pixels[0]==8500);
    native_raster_destroy(raster);
    assert(before[0].pixels[0]==31 && after[0].pixels[0]==992 && retained[0].pixels[0]==992);
    native_raster_inputs_destroy(inputs);native_raster_inputs_destroy(next);
    native_raster_inputs_destroy(wrapped);native_raster_inputs_destroy(restored);
    native_raster_inputs_destroy(overflow);native_raster_inputs_destroy(recovery);
}
static void dropped_texture_input_packets() {
    auto* raster=native_raster_create(nullptr);assert(raster);
    native_raster_inputs_enable(raster,1);
    auto* packets=frame_packet_create(64*1024*1024,1024);assert(packets);
    NativeRasterState state{0,0,1023,511,0,0,16|256,0,0,0,0};
    const uint32_t draw[]={0x65000080,700u|(100u<<16),0,0x00010001};
    FramePacketView held{};
    for(uint64_t n=1;n<=64;++n) {
        send(raster,NR_WRITE,{0,256,int(n)});
        const auto source=native_raster_gp0_identified(raster,&state,draw,4);assert(source);
        auto* inputs=native_raster_inputs_seal(raster);assert(native_raster_inputs_valid(inputs));
        assert(frame_packet_begin(packets,n,nullptr,0));
        auto* record=static_cast<uint64_t*>(frame_packet_append(packets,1,0,sizeof source));
        assert(record);*record=source;
        assert(frame_packet_attach(packets,inputs,native_raster_inputs_bytes(inputs),
            [](void* p) { native_raster_inputs_destroy(static_cast<NativeRasterInputs*>(p)); }));
        assert(frame_packet_publish(packets));
        if(n==1) assert(frame_packet_acquire(packets,&held));
    }
    assert(frame_packet_replaced(packets)==62);
    native_raster_destroy(raster); // no borrowed pointer into live native VRAM
    const auto sample=[](const FramePacketView& packet) {
        NativeRasterTile tiles[128];
        const auto* command=static_cast<const RtCmd*>(packet.records);
        const uint64_t source=*reinterpret_cast<const uint64_t*>(command+1);
        assert(native_raster_inputs_get(static_cast<const NativeRasterInputs*>(packet.attachment),
                                       source,tiles,128)==16);
        return tiles[0].pixels[0];
    };
    assert(sample(held)==1);
    frame_packet_release(packets);
    assert(frame_packet_acquire(packets,&held) && held.sequence==64 && sample(held)==64);
    frame_packet_release(packets);frame_packet_destroy(packets);
}
static void provenance_observation() {
    struct Sample { int before,restore; uint16_t first,second,drawn; uint32_t opcode; };
    struct Observer {
        std::thread::id producer=std::this_thread::get_id(),worker;
        std::vector<Sample> samples;
        uint16_t visited=0;
    } owner;
    std::vector<uint16_t> initial(1024*512);initial[0]=0x8001;
    auto* raster=native_raster_create(initial.data());assert(raster);
    native_raster_set_observer(raster,[](void* user,const NativeRasterEvent* event) {
        auto& owner=*static_cast<Observer*>(user);
        assert(std::this_thread::get_id()!=owner.producer);
        if(owner.samples.empty()) owner.worker=std::this_thread::get_id();
        assert(owner.worker==std::this_thread::get_id());
        owner.samples.push_back({event->before,event->restore,event->vram[0],event->vram[1],
            event->vram[4],event->packet ? event->packet[0] : 0});
        if(event->command) {
            assert(event->command->op==NR_CPU_UPLOAD && event->upload_words==2);
            assert(event->upload[0]==2 && event->upload[1]==3);
        }
        if(event->before) assert(!event->has_writes);
        else {
            assert(event->has_writes);
            const int* bounds=event->written_bounds;
            if(event->restore) assert(bounds[0]==0 && bounds[1]==0 && bounds[2]==1023 && bounds[3]==511);
            else if(event->command) assert(bounds[0]==1 && bounds[1]==0 && bounds[2]==1 && bounds[3]==0);
            else assert(bounds[0]==4 && bounds[1]==0 && bounds[2]==5 && bounds[3]==0);
        }
    },&owner);
    NativeRasterCommand upload{};upload.op=NR_CPU_UPLOAD;
    upload.args[2]=2;upload.args[3]=1;upload.args[5]=upload.args[6]=1;
    uint16_t words[2]={2,3};assert(native_raster_submit(raster,&upload,words,2));
    words[0]=words[1]=0; // observer sees owned input, then post-mask output
    NativeRasterState state{};state.right=1023;state.bottom=511;
    const uint32_t primitive[3]={0x600000ff,4,0x00010002};
    assert(native_raster_gp0(raster,&state,primitive,3));
    native_raster_visit(raster,[](void* user,const uint16_t* vram) {
        auto& owner=*static_cast<Observer*>(user);
        assert(owner.worker==std::this_thread::get_id());owner.visited=vram[1];
    },&owner);
    assert(owner.visited==0x8003 && owner.samples.size()==4);
    assert(owner.samples[0].before && owner.samples[0].first==0x8001 && owner.samples[0].second==0);
    assert(!owner.samples[1].before && owner.samples[1].first==0x8001 && owner.samples[1].second==0x8003);
    assert(owner.samples[2].opcode==primitive[0] && owner.samples[2].drawn==0);
    assert(owner.samples[3].drawn==31);
    initial[0]=123;native_raster_restore(raster,initial.data());
    assert(owner.samples.size()==6 && owner.samples[4].restore && owner.samples[4].first==0x8001);
    assert(owner.samples[5].restore && owner.samples[5].first==123);
    native_raster_set_observer(raster,nullptr,nullptr);
    send(raster,NR_FILL,{0,0,16,1,0});
    assert(pixel(raster,0,0)==0 && owner.samples.size()==6);
    native_raster_destroy(raster);
}
static void allocation_free_submission() {
    auto* r=native_raster_create(nullptr);assert(r);
    std::array<uint16_t,1024> upload{};
    NativeRasterCommand c{};c.op=NR_UPLOAD;c.args[2]=1024;c.args[3]=1;
    NativeRasterState state{};state.right=1023;state.bottom=511;
    const uint32_t packet[]={0x600000ff,0,0x00010001};
    allocations=0;count_allocations=true;
    for(unsigned frame=0;frame<100;++frame) {
        for(unsigned n=0;n<1000;++n)assert(native_raster_gp0(r,&state,packet,3));
        upload.fill(uint16_t(frame));
        assert(native_raster_submit(r,&c,upload.data(),upload.size()));
        uint16_t result=0;native_raster_read(r,0,0,1,1,&result);assert(result==frame);
    }
    count_allocations=false;assert(allocations==0);
    native_raster_destroy(r);
}
static void bounded_queue_wrap() {
    struct State {
        std::atomic<bool> entered{false},release{false};
        std::atomic<unsigned> submitted{0};
        unsigned seen=0;
    } state;
    auto* raster=native_raster_create(nullptr);assert(raster);
    native_raster_set_observer(raster,[](void* user,const NativeRasterEvent* event) {
        if(!event->before) return;
        auto& state=*static_cast<State*>(user);
        assert(event->command && event->command->op==NR_WRITE);
        assert(unsigned(event->command->args[2])==++state.seen);
        if(state.seen==1) {
            state.entered.store(true);
            while(!state.release.load()) std::this_thread::yield();
        }
    },&state);
    std::thread producer([&] {
        for(unsigned n=1;n<=50000;++n) {
            send(raster,NR_WRITE,{0,0,int(n)});
            state.submitted.store(n);
        }
    });
    while(!state.entered.load() || state.submitted.load()<16384) std::this_thread::yield();
    /* All 16384 queue slots, including the executing command: submission is bounded,
     * but release must wake the producer and preserve order across wraps. */
    assert(state.submitted.load()==16384);
    state.release.store(true);
    producer.join();
    assert(pixel(raster,0,0)== (50000 & 65535) && state.seen==50000);
    native_raster_destroy(raster);
}
int main() {
    allocation_free_submission();
    bounded_queue_wrap();
    immutable_texture_inputs();
    dropped_texture_input_packets();
    provenance_observation();
    auto *r = native_raster_create(nullptr);
    auto *other = native_raster_create(nullptr);
    assert(r && other);
    send(r,NR_FLAT_RECT,{0,0,8,8,0x001f});
    assert(pixel(r,3,3)==0x001f);
    assert(pixel(other,3,3)==0);

    // Destination-mask preservation and forced mask bit are native semantics.
    send(r,NR_MASK,{1,0});
    send(r,NR_FLAT_RECT,{0,0,1,1,0x03e0});
    send(r,NR_MASK,{0,1});
    send(r,NR_FLAT_RECT,{0,0,1,1,0x7c00});
    assert(pixel(r,0,0)==0x83e0);
    send(r,NR_MASK,{0,0});

    // Upload storage is copied, and both axes wrap at native VRAM edges.
    NativeRasterCommand upload{};
    upload.op=NR_UPLOAD;
    upload.args[0]=1023; upload.args[1]=511; upload.args[2]=2; upload.args[3]=2;
    std::array<uint16_t,4> data{1,2,3,4};
    assert(native_raster_submit(r,&upload,data.data(),data.size()));
    data.fill(0xffff);
    assert(pixel(r,1023,511)==1 && pixel(r,0,511)==2);
    assert(pixel(r,1023,0)==3 && pixel(r,0,0)==4);

    // Texture/CLUT decode is exercised against hand-known native pixels.
    send(r,NR_WRITE,{64,0,0x1111});
    send(r,NR_WRITE,{1,256,0x4210});
    send(r,NR_MODULATION,{16,16,16,1});
    send(r,NR_TEXTURED_RECT,{20,20,4,1,0,0,0,256,1});
    assert(pixel(r,20,20)==0x4210 && pixel(r,23,20)==0x4210);

    std::vector<uint16_t> saved(1024*512), restored(saved.size());
    uint64_t dirty[8]{};
    native_raster_snapshot(r,saved.data(),dirty);
    assert((dirty[0]&1) && (dirty[7]&(UINT64_C(1)<<63)));
    native_raster_snapshot(r,restored.data(),dirty);
    for (auto bits:dirty) assert(bits==0);
    assert(saved==restored);
    send(r,NR_FILL,{0,0,1024,512,0});
    native_raster_restore(r,saved.data());
    native_raster_snapshot(r,restored.data(),dirty);
    assert(saved==restored);
    for (auto bits:dirty) assert(bits==UINT64_MAX);

    // A0 is observable midway through DMA. It must not wait for the final
    // rectangle, and a snapshot/load between fragments retains exact pixels.
    send(r,NR_WRITE,{1023,511,0x8001});
    send(r,NR_WRITE,{0,511,0x0002});
    send(r,NR_WRITE,{1,511,0x0003});
    NativeRasterCommand fragment{};
    fragment.op=NR_CPU_UPLOAD;
    fragment.args[0]=1023; fragment.args[1]=511;
    fragment.args[2]=3; fragment.args[3]=1;
    fragment.args[5]=1; fragment.args[6]=1;
    std::array<uint16_t,2> first{10,11};
    assert(native_raster_submit(r,&fragment,first.data(),2));
    native_raster_snapshot(r,saved.data(),dirty);
    assert(saved[511*1024+1023]==0x8001); // masked destination unchanged
    assert(saved[511*1024]==0x800b);      // received second halfword
    assert(saved[511*1024+1]==3);         // third halfword not received yet
    fragment.args[4]=2;
    uint16_t last=12;
    assert(native_raster_submit(r,&fragment,&last,1));
    assert(pixel(r,1,511)==0x800c);
    native_raster_restore(r,saved.data());
    assert(pixel(r,1,511)==3);
    assert(native_raster_submit(r,&fragment,&last,1));
    assert(pixel(r,1,511)==0x800c);
    assert(!native_raster_submit(r,&fragment,first.data(),2));

    // Keep RGB888 through interpolation. At the midpoint, 31/2 rounds to
    // 16 before RGB555 conversion, not to the old 5-bit interpolated 1.
    send(r,NR_NATIVE_TRIANGLE,{10,10,0,0,0,42,10,31,0,0,10,42,0,0,0,0,0,0,4});
    assert(pixel(r,26,11)==2);
    // Every native 4x4 dither position, with a constant Gouraud colour.
    send(r,NR_NATIVE_TRIANGLE,{100,100,0x7f7f7f,0,0,132,100,0x7f7f7f,0,0,
                              100,132,0x7f7f7f,0,0,0,0,0,12});
    const int dither[4][4]={{-4,0,-3,1},{2,-2,3,-1},{-3,1,-4,0},{3,-1,2,-2}};
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
        const int c=(127+dither[y][x])/8;
        assert(pixel(r,104+x,104+y)==(c|(c<<5)|(c<<10)));
    }

    // Sprite modulation preserves all eight source bits: 31*127/128 = 30,
    // whereas quantizing 127 to five bits before modulation gives 29.
    send(r,NR_MASK,{0,0});
    send(r,NR_WRITE,{700,400,31});
    send(r,NR_NATIVE_RECT,{300,300,1,1,127,60,144,0,0,0x11a,1});
    assert(pixel(r,300,300)==30);
    // Horizontal flip starts at the odd member of the supplied texel pair.
    send(r,NR_WRITE,{699,400,4});
    send(r,NR_WRITE,{701,400,7});
    send(r,NR_NATIVE_RECT,{302,300,2,1,0,60,144,0,0,0x11a,19});
    assert(pixel(r,302,300)==7 && pixel(r,303,300)==31);
    // The line endpoint is included and RGB888 rounds at the pixel, not
    // before interpolation. Reversing the endpoints preserves coverage.
    send(r,NR_NATIVE_LINE,{400,300,0,432,300,31,4});
    assert(pixel(r,416,300)==2 && pixel(r,432,300)==3);
    send(r,NR_NATIVE_LINE,{432,301,31,400,301,0,4});
    assert(pixel(r,416,301)==2 && pixel(r,432,301)==3);
    send(r,NR_NATIVE_LINE,{440,300,0x7f7f7f,443,300,0x7f7f7f,8});
    for(int x=0;x<4;++x) {
        const int c=(127+dither[0][x])/8;
        assert(pixel(r,440+x,300)==(c|(c<<5)|(c<<10)));
    }

    // Raw GP0 packets retain RGB888 and the captured guest offset/area.
    NativeRasterState state{0,0,1023,511,10,20,0,0,0,0};
    uint32_t triangle[]={0x30000000,0x001e01f4,31,0x001e0214,0,0x003e01f4};
    assert(native_raster_gp0(r,&state,triangle,6));
    triangle[2]=255; // queue owns the original packet
    state.offset_x=100;
    assert(pixel(r,526,51)==2);
    uint32_t rect[]={0x600000ff,0x006401f4,0x00020003};
    assert(native_raster_gp0(r,&state,rect,3));
    assert(pixel(r,600,120)==31 && pixel(r,602,121)==31);
    assert(!native_raster_gp0(r,&state,rect,2));
    uint32_t environment=0xe1000000;
    assert(!native_raster_gp0(r,&state,&environment,1));

    // Independent contexts race on different threads while the default
    // renderer remains usable. No graphics/context owner participates.
    std::vector<uint16_t> legacy(1024*512);
    sw_renderer_init(legacy.data());
    gpu_vram_dirty_set_tracking(1);
    gpu_vram_dirty_clear();
    std::thread producer([&] {
        for (int i=0;i<60000;++i) send(other,NR_WRITE,{i&1023,(i>>10)&511,i&0x7fff});
    });
    for (int i=0;i<60000;++i) send(r,NR_WRITE,{i&1023,(i>>10)&511,(i+1)&0x7fff});
    sw_set_draw_area(100,100,110,110);
    sw_draw_flat_rect(100,100,2,2,0x7c00);
    producer.join();
    assert(pixel(r,607,58)==(60000&0x7fff));
    assert(pixel(other,607,58)==(59999&0x7fff));
    assert(legacy[100*1024+100]==0x7c00);
    assert(gpu_vram_dirty_row_count()==2);
    assert(native_raster_completed(r)>60000);
    upload.args[2]=1025;
    assert(!native_raster_submit(r,&upload,data.data(),data.size()));
    // Copy reads a 128-halfword chunk before writing it. A rightward
    // overlap can therefore feed the next chunk, unlike a whole-row copy.
    send(r,NR_MASK,{0,0});
    std::array<uint16_t,256> ramp{};
    for(unsigned i=0;i<ramp.size();++i) ramp[i]=uint16_t(i+1);
    NativeRasterCommand ramp_upload{};
    ramp_upload.op=NR_UPLOAD;
    ramp_upload.args[0]=0;ramp_upload.args[1]=450;
    ramp_upload.args[2]=256;ramp_upload.args[3]=1;
    assert(native_raster_submit(r,&ramp_upload,ramp.data(),ramp.size()));
    send(r,NR_COPY,{0,450,1,450,255,1});
    assert(pixel(r,128,450)==128 && pixel(r,129,450)==128);

    // The palette cache may intentionally differ from VRAM. A machine
    // checkpoint must preserve both; clearing it on load changes pixels.
    native_raster_restore(r,saved.data());
    send(r,NR_AREA,{0,0,1023,511});
    send(r,NR_SEMI,{0,0});
    send(r,NR_WRITE,{0,400,1});
    send(r,NR_WRITE,{17,480,31});
    send(r,NR_NATIVE_RECT,{500,400,1,1,0,0,144,16,480,16,3});
    assert(pixel(r,500,400)==31);
    send(r,NR_WRITE,{17,480,992});
    SwNativeTextureState texture{};
    native_raster_checkpoint(r,saved.data(),dirty,&texture);
    native_raster_restore(r,saved.data());
    send(r,NR_NATIVE_RECT,{500,400,1,1,0,0,144,16,480,16,3});
    assert(pixel(r,500,400)==992);
    native_raster_restore_checkpoint(r,saved.data(),&texture);
    send(r,NR_NATIVE_RECT,{500,400,1,1,0,0,144,16,480,16,3});
    assert(pixel(r,500,400)==31);

    // Presentation seeds observe exact bytes/cache without consuming the
    // guest's dirty tracker. A skipped presentation cannot hide rewind data.
    native_raster_snapshot(r,restored.data(),dirty);
    send(r,NR_WRITE,{17,123,0x4321});
    SwNativeTextureState seed_texture{};
    native_raster_checkpoint(r,restored.data(),nullptr,&seed_texture);
    assert(restored[123*1024+17]==0x4321);
    native_raster_snapshot(r,restored.data(),dirty);
    assert(dirty[123/64]&(UINT64_C(1)<<(123%64)));

    // Interlaced drawing skips the display field for fills too; copies and
    // CPU uploads still touch both fields. Fill ignores the draw rectangle.
    state.skip_field=1;
    uint32_t fill[]={0x020000ff,0x01f001f0,0x00020010};
    uint16_t even_before=pixel(r,496,496);
    assert(native_raster_gp0(r,&state,fill,3));
    assert(pixel(r,496,496)==even_before && pixel(r,496,497)==31);

    // Full-VRAM oracle digests include masked pixels, texture feedback,
    // shading, clipping, sprite flips and palette/texture fetches.
    for(size_t i=0;i<saved.size();++i)
        saved[i]=uint16_t((i*173+(i>>10)*31)^0x5317);
    for(const auto &fixture:native_oracle_fixtures) {
        native_raster_restore(r,saved.data());
        assert(native_raster_gp0(r,&fixture.state,fixture.words,fixture.count));
        native_raster_read(r,0,0,1024,512,restored.data());
        uint64_t digest=UINT64_C(14695981039346656037);
        for(auto value:restored) {
            digest=(digest^(value&255))*UINT64_C(1099511628211);
            digest=(digest^(value>>8))*UINT64_C(1099511628211);
        }
        assert(digest==fixture.digest);
    }

    // A held presentation packet cannot delay native work. After replacing
    // intermediate frames, the newest frame's seed and commands reproduce
    // the exact native image, including mask and overlapping-copy semantics.
    struct Seed {
        std::array<uint16_t,1024*512> pixels;
        SwNativeTextureState texture;
    };
    struct Draw { NativeRasterState state; std::array<uint32_t,4> words; uint32_t count; };
    auto seed=std::make_unique<Seed>();
    FramePacketQueue *frames=frame_packet_create(sizeof(Seed),4096);
    assert(frames);
    FramePacketView held{};
    for(unsigned tick=1;tick<=64;++tick) {
        void* storage=nullptr;
        assert(frame_packet_reserve_initial(frames,tick,sizeof(Seed),&storage));
        auto* writable=new(storage) Seed;
        native_raster_checkpoint(r,writable->pixels.data(),nullptr,&writable->texture);
        NativeRasterState draw_state={0,0,1023,511,0,0,0,0,tick&1u,(tick>>1)&1u,0};
        const Draw draws[]={
            {draw_state,{0x600000ffu,80u|(120u<<16),32u|(8u<<16),0},3},
            {draw_state,{0x80000000u,80u|(120u<<16),81u|(120u<<16),32u|(8u<<16)},4},
            {draw_state,{0x6000ff00u,90u|(122u<<16),4u|(2u<<16),0},3}
        };
        for(const auto &d:draws) {
            void *payload=frame_packet_append(frames,1,0,sizeof(Draw));
            assert(payload);std::memcpy(payload,&d,sizeof d);
            assert(native_raster_gp0(r,&d.state,d.words.data(),d.count));
        }
        assert(frame_packet_publish(frames));
        if(tick==1) assert(frame_packet_acquire(frames,&held));
    }
    assert(held.sequence==1 && frame_packet_replaced(frames)==62);
    frame_packet_release(frames);
    FramePacketView latest{};assert(frame_packet_acquire(frames,&latest));
    assert(latest.sequence==64 && latest.initial_bytes==sizeof(Seed));
    std::memcpy(seed.get(),latest.initial,sizeof(Seed));
    native_raster_restore_checkpoint(other,seed->pixels.data(),&seed->texture);
    size_t at=0;
    while(at<latest.record_bytes) {
        const auto *cmd=reinterpret_cast<const RtCmd *>(static_cast<const uint8_t *>(latest.records)+at);
        assert(cmd->op==1 && cmd->payload==sizeof(Draw));
        Draw d;std::memcpy(&d,cmd+1,sizeof d);
        assert(native_raster_gp0(other,&d.state,d.words.data(),d.count));
        at+=cmd->size;
    }
    native_raster_read(r,0,0,1024,512,saved.data());
    native_raster_read(other,0,0,1024,512,restored.data());
    assert(saved==restored);
    frame_packet_release(frames);frame_packet_destroy(frames);

    // Reuse one completion token across changing pixels and an idle consumer.
    NativeRasterCheckpoint *reusable=native_raster_checkpoint_create();
    assert(reusable);
    for(unsigned i=0;i<256;i++) {
        send(r,NR_WRITE,{11,12,(int32_t)i});
        assert(native_raster_checkpoint_submit(reusable,r,restored.data(),dirty,&texture));
        while(!native_raster_checkpoint_ready(reusable))std::this_thread::yield();
        assert(restored[12*1024+11]==i);
    }
    // Wrap the preallocated draw ring, then interleave two checkpoint clients
    // and later draws. Mailbox and ordinary-queue snapshots share exact order.
    for(unsigned i=0;i<40000;i++)send(r,NR_WRITE,{11,12,(int32_t)(i&32767)});
    NativeRasterCheckpoint *second=native_raster_checkpoint_create();assert(second);
    std::vector<uint16_t> second_vram(1024*512);
    uint64_t second_dirty[8]{};
    for(unsigned i=0;i<256;i++) {
        send(r,NR_WRITE,{11,12,(int32_t)i});
        assert(native_raster_checkpoint_submit(reusable,r,restored.data(),dirty,&texture));
        send(r,NR_WRITE,{11,12,(int32_t)(1000+i)});
        assert(native_raster_checkpoint_submit(second,r,second_vram.data(),second_dirty,nullptr));
        send(r,NR_WRITE,{11,12,(int32_t)(2000+i)});
        while(!native_raster_checkpoint_ready(reusable) || !native_raster_checkpoint_ready(second))
            std::this_thread::yield();
        assert(restored[12*1024+11]==i && second_vram[12*1024+11]==1000+i);
        assert(pixel(r,11,12)==2000+i);
    }
    native_raster_checkpoint_release(second);
    NativeRasterCommand checkpoint_fragment{};checkpoint_fragment.op=NR_CPU_UPLOAD;
    checkpoint_fragment.args[0]=30;checkpoint_fragment.args[1]=40;checkpoint_fragment.args[2]=5;checkpoint_fragment.args[3]=1;checkpoint_fragment.args[4]=1;
    uint16_t fragment_pixels[2]={0x1234,0x5678};
    checkpoint_fragment.args[2]=0;
    assert(!native_raster_checkpoint_submit_upload(reusable,r,restored.data(),dirty,&texture,
                                                 &checkpoint_fragment,fragment_pixels,2));
    assert(native_raster_checkpoint_ready(reusable));
    checkpoint_fragment.args[2]=5;
    assert(!native_raster_checkpoint_submit_upload(reusable,r,restored.data(),dirty,&texture,
                                                 &checkpoint_fragment,fragment_pixels,SIZE_MAX));
    assert(native_raster_checkpoint_submit_upload(reusable,r,restored.data(),dirty,&texture,
                                                &checkpoint_fragment,fragment_pixels,2));
    send(r,NR_WRITE,{31,40,0x9999});
    while(!native_raster_checkpoint_ready(reusable))std::this_thread::yield();
    assert(restored[40*1024+31]==0x1234 && restored[40*1024+32]==0x5678);
    assert(pixel(r,31,40)==0x9999);
    native_raster_checkpoint_release(reusable);
    // Async checkpoint owns the exact queue position, despite later writes;
    // release is safe after worker destruction and preserves texture caches.
    native_raster_checkpoint(r,saved.data(),dirty,&texture);
    SwNativeTextureState async_texture{};
    NativeRasterCheckpoint *pending=native_raster_checkpoint_async(r,restored.data(),dirty,&async_texture);
    assert(pending);
    send(r,NR_WRITE,{11,12,0x1234});
    native_raster_destroy(r);r=nullptr;
    assert(native_raster_checkpoint_ready(pending));
    assert(saved==restored);
    assert(!memcmp(&texture,&async_texture,sizeof(texture)));
    native_raster_checkpoint_release(pending);

    native_raster_destroy(other);
    native_raster_destroy(r);
}

