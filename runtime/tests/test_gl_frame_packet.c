/* Real GL ownership and exact per-draw inputs across discarded frames.
 * Reuse the renderer fixture's platform and title-hook stubs, not its ring
 * transport. This test transfers the context only at start and shutdown. */
#define main legacy_ring_fixture_main
#include "test_gl_render_thread.c"
#undef main
#include "frame_owner.h"
#include "native_raster.h"

typedef struct PacketSeed { RthFrameState state; uint16_t pixels[1024*512]; } PacketSeed;
static _Atomic int packet_entered,packet_unblock,packet_acquires,packet_releases;
static uint16_t packet_private[1024*512];
static FrameOwner *packet_owner;
static void packet_context(void *unused,int current) {
    (void)unused;
    if(current) {
        atomic_fetch_add(&packet_acquires,1);
        if(SDL_GL_MakeCurrent(s_win,s_ctx)!=0) abort();
    } else {
        atomic_fetch_add(&packet_releases,1);
        glFlush();
        if(SDL_GL_MakeCurrent(s_win,NULL)!=0) abort();
    }
}
static void packet_dispose(void *inputs) { native_raster_inputs_destroy(inputs); }
static void packet_execute(void *unused,const FramePacketView *packet) {
    (void)unused;
    if(!frame_owner_is_thread()) abort();
    if(packet->sequence==1) {
        atomic_store(&packet_entered,1);
        while(!atomic_load(&packet_unblock)) SDL_Delay(1);
    }
    packet_timer_begin();
    if(packet->initial_bytes!=sizeof(PacketSeed)) abort();
    s_rth_packet_replaying=1;
    const PacketSeed *seed=packet->initial;
    memcpy(packet_private,seed->pixels,sizeof packet_private);
    glb_init(packet_private);
    gl_renderer_restage_vram_after_savestate();
    /* Poison owner state as if a discarded frame's predecessor left it.
     * Every persistent setting must come from the retained packet. */
    glb_set_draw_area(900,400,901,401);
    glb_set_draw_offset(200,200);
    glb_set_color_modulation(0,0,0,0);
    glb_set_mask_bits(1,1);
    glb_set_semi_transparency(1,2);
    rth_frame_state_apply(&seed->state);
    check(s_off_x==seed->state.offset[0] && s_off_y==0,"packet draw-offset environment");
    const uint8_t *record=packet->records;
    for(size_t at=0;at<packet->record_bytes;) {
        const RtCmd *command=(const RtCmd *)(record+at);
        if(command->op==RTH_NATIVE_SOURCE) {
            uint64_t source;memcpy(&source,command+1,sizeof source);
            NativeRasterTile tiles[128];
            int count=native_raster_inputs_get(packet->attachment,source,tiles,128);
            if(count<=0) abort();
            for(int t=0;t<count;++t)
                glb_vram_transfer_in(tiles[t].x,tiles[t].y,64,64,tiles[t].pixels);
        }
        gl_rth_exec(NULL,command,command+1);
        at+=command->size;
    }
    flush_flat_batch();flush_tex_batch();flush_cpu_upload();pack_flush();
    packet_timer_end();
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_raw_fbo);
    for(unsigned draw=0;draw<2;++draw) {
        uint16_t pixels[16];
        /* Inspect the GPU on its owner. The backend's native read accessor
         * deliberately never reads GL during replay; it returns the seed. */
        glReadPixels(16+draw*16,16,4,4,PSXGL_RED_INTEGER,GL_UNSIGNED_SHORT,pixels);
        uint16_t expected=(uint16_t)(1+(packet->sequence+draw)%30);
        if(pixels[0]!=expected) fprintf(stderr,"packet=%llu draw=%u got=%04x expected=%04x bank_failed=%d depth24=%d\n",
            (unsigned long long)packet->sequence,draw,pixels[0],expected,s_selected_bank_failed,ctx_depth24());
        for(unsigned i=0;i<16;++i) check(pixels[i]==expected,"packet pre-draw texture version");
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,0);
    check(glGetError()==GL_NO_ERROR,"packet owner GL error");
    s_rth_packet_replaying=0;
}
int main(void) {
#ifdef _WIN32
    _putenv_s("PSX_GL_ASYNC_GPU_TIMER","1");
#else
    setenv("PSX_GL_ASYNC_GPU_TIMER","1",1);
#endif
    if(SDL_Init(SDL_INIT_VIDEO)!=0) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,8);
    SDL_Window *win=SDL_CreateWindow("Frame packet hidden test",0,0,320,240,
                                     SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win) return 77;
    SDL_GLContext probe=SDL_GL_CreateContext(win);
    if(!probe) { SDL_DestroyWindow(win);SDL_Quit();return 77; }
    SDL_GL_MakeCurrent(win,NULL);SDL_GL_DeleteContext(probe);
    gr_set_backend(GR_BACKEND_OPENGL);gr_init(vram);gr_set_scale(3);
    gl_renderer_set_pgxp_depth(1);
    gl_renderer_set_pgxp_color_perspective(1);
    gl_renderer_set_pgxp_seam(2);
    gl_renderer_set_swap_interval(0);
    if(!gl_renderer_init_context(win)) return 2;
    NativeRaster *native=native_raster_create(vram);
    if(!native) abort();
    native_raster_inputs_enable(native,1);
    FrameOwnerConfig config={0};
    config.initial_limit=2*1024*1024;config.record_limit=4096;
    config.execute=packet_execute;config.context=packet_context;
    packet_owner=frame_owner_start(&config);
    if(!packet_owner) abort();
    NativeRasterState state={0,0,1023,511,0,0,0x108,0,0,0,0};
    uint16_t upload[64*64];
    for(uint64_t sequence=1;sequence<=64;++sequence) {
        void *seed=NULL;
        if(!frame_owner_reserve_initial(packet_owner,sequence,sizeof(PacketSeed),&seed)) abort();
        PacketSeed *initial=seed;
        memset(&initial->state,0,sizeof initial->state);
        initial->state.area[2]=1023;initial->state.area[3]=511;
        initial->state.mod[0]=initial->state.mod[1]=initial->state.mod[2]=128;
        initial->state.mod[3]=1;
        initial->state.offset[0]=(int)(sequence%3);
        native_raster_snapshot(native,initial->pixels,NULL);
        for(unsigned draw=0;draw<2;++draw) {
            uint16_t color=(uint16_t)(1+(sequence+draw)%30);
            for(unsigned i=0;i<64*64;++i) upload[i]=color;
            NativeRasterCommand transfer={NR_UPLOAD,{512,0,64,64}};
            if(!native_raster_submit(native,&transfer,upload,64*64)) abort();
            uint32_t gp0[]={0x65000000,((uint32_t)16<<16)|(16+draw*16),0,0x00040004};
            uint64_t source=native_raster_gp0_identified(native,&state,gp0,4);
            uint64_t *out=frame_owner_append(packet_owner,RTH_NATIVE_SOURCE,0,sizeof *out);
            if(!source || !out) abort();
            *out=source;
            int32_t arguments[]={16+draw*16,16,4,4,0,0,0,0,0x108};
            void *primitive=frame_owner_append(packet_owner,RTH_TEX_RECT,RTHF_PRIM,sizeof arguments);
            if(!primitive) abort();
            memcpy(primitive,arguments,sizeof arguments);
        }
        /* End-of-tick VRAM deliberately differs from both draws' inputs. */
        for(unsigned i=0;i<64*64;++i) upload[i]=0x7fff;
        NativeRasterCommand transfer={NR_UPLOAD,{512,0,64,64}};
        if(!native_raster_submit(native,&transfer,upload,64*64)) abort();
        NativeRasterInputs *inputs=native_raster_inputs_seal(native);
        if(!native_raster_inputs_valid(inputs) ||
           !frame_owner_attach(packet_owner,inputs,native_raster_inputs_bytes(inputs),packet_dispose) ||
           !frame_owner_publish(packet_owner)) abort();
        if(sequence==1) while(!atomic_load(&packet_entered)) SDL_Delay(1);
    }
    check(frame_owner_replaced(packet_owner)==62,"62 complete pending packets replaced");
    check(atomic_load(&packet_acquires)==1 && atomic_load(&packet_releases)==1,
          "guest publication never borrows GL context");
    /* Inputs are independently owned even after the native worker dies. */
    native_raster_destroy(native);
    atomic_store(&packet_unblock,1);
    while(frame_owner_completed(packet_owner)<2) SDL_Delay(1);
    frame_owner_stop(packet_owner);
    check(atomic_load(&packet_acquires)==2 && atomic_load(&packet_releases)==2,
          "context transfers only at lifecycle boundaries");
    glFinish(); /* Test-only completion before inspecting asynchronous telemetry. */
    packet_timer_resolve();
    uint64_t timing[5];gl_renderer_packet_gpu_stats(timing);
    check(timing[0]==2,"only consumed packets timed");
    check(timing[1]>0 && timing[3]>0 && timing[3]<=timing[1],"packet timestamp totals");
    check(timing[2]<=timing[1] && timing[4]==0,"bounded nonblocking packet telemetry");
    printf("checks=%d failures=%d\n",checks,failures);
    gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();
    return failures?1:0;
}
