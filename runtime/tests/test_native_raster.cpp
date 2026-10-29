#include "native_raster.h"
#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include "frame_packet.h"
#include <array>
#include <cassert>
#include <initializer_list>
#include <thread>
#include <vector>
#include <memory>
#include <cstring>
#include "native_raster_oracle_fixtures.h"

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
int main() {
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
        native_raster_checkpoint(r,seed->pixels.data(),nullptr,&seed->texture);
        assert(frame_packet_begin(frames,tick,seed.get(),sizeof(Seed)));
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
    native_raster_destroy(other);
    native_raster_destroy(r);
}

