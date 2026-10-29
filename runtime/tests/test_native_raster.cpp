#include "native_raster.h"
#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include <array>
#include <cassert>
#include <initializer_list>
#include <thread>
#include <vector>

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
    native_raster_destroy(other);
    native_raster_destroy(r);
}
