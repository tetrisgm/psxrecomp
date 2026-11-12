/* Private presentation targets must leave the native render surfaces intact. */
#include <assert.h>
#define DEPTH_FIXTURE_MAIN depth_reference_main
#include "test_gl_depth_clear.c"
#undef DEPTH_FIXTURE_MAIN
uint16_t gr_vram_read(int x,int y) { return glb_vram_read(x,y); }
void gr_vram_write(int x,int y,uint16_t value) { glb_vram_write(x,y,value); }

int present_image_ring_accepting(void) { return 0; }
int present_image_ring_thumb_w(int w,int h) { (void)w;(void)h;return 0; }
void present_image_ring_push(uint32_t f,const uint8_t *p,int w,int pitch,int flip) {
    (void)f;(void)p;(void)w;(void)pitch;(void)flip;
}
void present_image_ring_push_argb(uint32_t f,const uint32_t *p,int w,int h,int pitch) {
    (void)f;(void)p;(void)w;(void)h;(void)pitch;
}
/* This fixture has no external replacement pack. */
static atomic_int snapshot_done;
static void snapshot_completed(void *context) {
    atomic_fetch_add_explicit((atomic_int *)context,1,memory_order_release);
}

static void draw_command(uint16_t op,const int32_t *v,size_t bytes) {
    RtCmd c={0};c.op=op;c.payload=(uint32_t)bytes;gl_rth_exec(NULL,&c,v);
}
static void command_pgxp_triangle(void) {
    const int32_t precise[]={1,30*65536+16384,40*65536,180*65536,50*65536+32768,100*65536,190*65536};
    int32_t z[4]={1},q[4]={1};const float depth[]={2048,2300,2800},persp[]={1,0.9f,0.7f};
    memcpy(z+1,depth,sizeof depth);memcpy(q+1,persp,sizeof persp);
    const int32_t tri[]={30,40,0,0,180,50,63,0,100,190,31,63,0,0,0x11a};
    draw_command(RTH_PRECISE,precise,sizeof precise);draw_command(RTH_DEPTH,z,sizeof z);
    draw_command(RTH_PERSP,q,sizeof q);draw_command(RTH_TEX_TRI,tri,sizeof tri);
}
static uint64_t color_texture_hash(GLuint tex,int w,int h) {
    uint8_t *p=malloc((size_t)w*h*4);assert(p);
    glBindTexture(GL_TEXTURE_2D,tex);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,p);
    uint64_t hash=fnv(p,(size_t)w*h*4,0xcbf29ce484222325ull);free(p);return hash;
}
static uint64_t native_texture_hash(void) {
    uint16_t *p=malloc((size_t)VRAM_W*VRAM_H*2);assert(p);
    glBindTexture(GL_TEXTURE_2D,s_raw_tex);
    glGetTexImage(GL_TEXTURE_2D,0,PSXGL_RED_INTEGER,GL_UNSIGNED_SHORT,p);
    uint64_t hash=fnv(p,(size_t)VRAM_W*VRAM_H*2,0xcbf29ce484222325ull);free(p);return hash;
}
static void command_rewind(int scale) {
    setenv("PSX_REWIND_VERIFY_DRAW","1",1);
    gl_renderer_set_interpolation(1,60,60,60,2);
    gl_renderer_set_interpolation_source(1);
    s_interp_valid=2;s_interp_w=320*scale;s_interp_h=240*scale;s_interp_source_path=GL_PRES_VRAM;
    gl_renderer_set_hd_texture_mode(1);
    gl_renderer_rewind_state_enable(2);
    glb_set_draw_area(0,0,319,239);glb_set_draw_offset(0,0);
    uint16_t texture[64*64];for(unsigned n=0;n<64*64;n++)texture[n]=(n&1)?0x7fff:0x7c00;
    glb_vram_transfer_in(640,256,64,64,texture);flush_cpu_upload();
    const int32_t textured[]={133,100,64,64,0,0,0,0,0x100|0x10|10};
    const int32_t begin[]={0,0,320,240,1,2,GL_PASS_COMPLETE_IMAGE,12345,0};
    const int32_t background[]={0,0,320,240,0x001f}, foreground[]={63,41,117,97,0x03e0};
    const int32_t end[]={32768,1};
    draw_command(RTH_PASS_BEGIN,begin,sizeof begin);
    check(s_pass_active && RD.active,"complete command producer opens");
    const int32_t wide_config[]={560,120};
    draw_command(RTH_WIDE_CONFIGURE,wide_config,sizeof wide_config);
    draw_command(RTH_FLAT_RECT,background,sizeof background);
    draw_command(RTH_FLAT_RECT,foreground,sizeof foreground);
    draw_command(RTH_TEX_RECT,textured,sizeof textured);
    command_pgxp_triangle();
    draw_command(RTH_PASS_END,end,sizeof end);
    /* A later phase keeps the original generation cycle, even though the
     * recording command itself runs at a later guest cycle. */
    const int32_t phase_begin[]={0,0,320,240,0,2,GL_PASS_COMPLETE_IMAGE,12346,0};
    const int32_t phase_end[]={49152,1};
    draw_command(RTH_PASS_BEGIN,phase_begin,sizeof phase_begin);
    draw_command(RTH_FLAT_RECT,background,sizeof background);
    draw_command(RTH_FLAT_RECT,foreground,sizeof foreground);
    draw_command(RTH_TEX_RECT,textured,sizeof textured);
    command_pgxp_triangle();
    draw_command(RTH_PASS_END,phase_end,sizeof phase_end);
    PassGen *g=&s_pgen[s_pgen_write];uint64_t key=g->draw_key[g->n-1];
    check(key!=0,"complete ordered commands become rewind history");
    if(key) {
        uint64_t expected=color_texture_hash(s_pgen_tex[s_pgen_write][g->n-1],g->tex_w,g->tex_h);
        for(int n=0;n<1000 && !rewind_history_contains(RD.history,key);n++)SDL_Delay(1);
        memset(texture,0,sizeof texture);glb_vram_transfer_in(640,256,64,64,texture);flush_cpu_upload();
        glb_set_draw_area(5,6,300,220);
        uint64_t raw=native_texture_hash();
        uint64_t cpu=pass_cpu_vram_hash();
        uint64_t native=surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale);
        uint64_t wide=surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale);
        RV.complete_latest=100;RV.complete_ticks[100]=100;RV.complete_cycles[100]=12345;
        RV.complete_phases[100]=49152;RV.draw_keys[100]=key;
        gl_renderer_rewind_video_freeze(1);uint64_t shown=0,tick=0;
        int ok=gl_renderer_rewind_draw_present(100,90,&shown,&tick);
        check(ok && shown==100 && tick==100,"command rewind presents exact historical producer");
        if(ok)check(color_texture_hash(RD.texture,RD.texture_w,RD.texture_h)==expected,
                    "command redraw equals original pixels at requested scale");
        check(RD.pixel_checks==1 && RD.pixel_mismatches==0,"historical full-image diagnostic compares exact replay pixels");
        check(native_texture_hash()==raw,"command rewind preserves the live GPU native texture mirror");
        int x1,y1,x2,y2;sw_get_draw_area(&x1,&y1,&x2,&y2);
        check(x1==5 && y1==6 && x2==300 && y2==220,"command rewind restores live software draw state");
        check(pass_cpu_vram_hash()==cpu,"command rewind preserves live native texture memory");
        check(surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale)==native,
              "command rewind preserves live color depth and stencil");
        check(surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale)==wide,
              "command rewind preserves wide color depth and stencil");
        check(glGetError()==GL_NO_ERROR,"command replay has valid GL state");
        check(gl_renderer_rewind_draw_presented(100),"release recognizes the exact already-presented command image");
        gl_renderer_rewind_draw_resume(100);s_frame_count=101;s_rewind_complete_source=0;
        check(rewind_video_hold() && RV.frozen,"release holds the command image through native restage");
        check(!rewind_state_reconstructing(),"release allows normal forward presentation deadlines");
        s_rewind_complete_source=1;s_rewind_complete_cycle=99999;
        check(!rewind_video_hold() && !RV.frozen && !RV.resume_complete,
              "first complete forward image opens the release gate without an old wall deadline");
        s_rewind_complete_source=0;
    }
    gl_renderer_rewind_video_freeze(0);gl_renderer_rewind_state_enable(0);
    gl_renderer_set_hd_texture_mode(0);
    unsetenv("PSX_REWIND_VERIFY_DRAW");
}

int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Independent presentation",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 2;
    for(int si=0;si<3;si++) {
        int scale=si==0?1:si==1?4:9;
        glb_init(vram);glb_set_scale(scale);
        if(!gl_renderer_init_context(win))return 2;
        gl_renderer_set_pgxp_depth(1);
        glb_wide_configure(560,120);glb_wide_set_target(0);
        flush_cpu_upload();flush_flat_batch();flush_tex_batch();hiw_flush_queue();
        seed_surface(s_hr_fbo);seed_surface(g_wide_cur);
        glb_vram_write(960,496,0x7fff);glb_vram_write(961,496,0x7fff);flush_cpu_upload();
        uint64_t native=surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale);
        uint64_t wide=surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale);
        check(psx_presentation_gpu_test_versions(),"immutable texture versions and ordered private draws");
        check(s_pgpu_scale==scale,"private target uses requested internal resolution");
        check(surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale)==native,"private rendering preserves native color/depth/stencil");
        check(surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale)==wide,"private rendering preserves native-wide color/depth/stencil");
        check(glGetError()==GL_NO_ERROR,"private rendering GL state is valid");
        seed_surface(s_hr_fbo);
        float ambient=-1;
        check(!gl_renderer_sample_ambient(127,127,&ambient),"ambient queues without a synchronous result");
        glFinish();
        check(gl_renderer_sample_ambient(127,127,&ambient),"ambient consumes completed GPU samples");
        check(fabsf(ambient-(0.3f*64+0.55f*128+0.15f*191)/255)<0.00001f,
              "GPU ambient matches the original weighted scene color");
        check(glGetError()==GL_NO_ERROR,"ambient sampling leaves valid GL state");
        /* Uneven row colors, odd dimensions and out-of-sample edge pixels:
         * compare with the former CPU readback calculation. */
        const int rows[3]={127*35/100,127*55/100,127*75/100};
        glEnable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
        for(int k=0;k<3;++k) {
            glScissor(0,rows[k],127,1);glClearColor(k==0,k==1,k==2,1);glClear(GL_COLOR_BUFFER_BIT);
            glScissor(0,rows[k],20+k*13,1);glClearColor(.2,.7,.4,1);glClear(GL_COLOR_BUFFER_BIT);
        }
        uint8_t row[127*4];double expected=0;int samples=0;
        for(int k=0;k<3;++k) {
            glReadPixels(0,rows[k],127,1,GL_RGBA,GL_UNSIGNED_BYTE,row);
            for(int x=127/8;x<127*7/8;x+=8) {
                expected+=.3*row[x*4]+.55*row[x*4+1]+.15*row[x*4+2];++samples;
            }
        }
        gl_renderer_sample_ambient(127,127,&ambient);glFinish();
        PgpuState before={0},after={0};pgpu_state_save(&before);
        check(gl_renderer_sample_ambient(127,127,&ambient),"patterned ambient sample completes");
        pgpu_state_save(&after);
        check(fabs(ambient-expected/(samples*255.0))<.00001,"GPU ambient uses the exact original row and column positions");
        check(memcmp(&before,&after,sizeof before)==0,"ambient sampling preserves host overlay GL state");
        glPixelStorei(GL_PACK_SKIP_ROWS,5);glPixelStorei(GL_PACK_SKIP_PIXELS,7);
        uint8_t pixel[4];
        check(psx_presentation_gpu_read_rgba(pixel,sizeof pixel,20,20,1,1),"bounded export isolates inherited pack skips");
        GLint skip_rows,skip_pixels;glGetIntegerv(GL_PACK_SKIP_ROWS,&skip_rows);glGetIntegerv(GL_PACK_SKIP_PIXELS,&skip_pixels);
        check(skip_rows==5 && skip_pixels==7,"bounded export restores pack skips");
        glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
        setenv("PSX_RENDER_PASS_FRESH_TARGET","1",1);
        transaction_depth_restore(scale);
        unsetenv("PSX_RENDER_PASS_FRESH_TARGET");
        transaction_options=GL_PASS_COMPLETE_IMAGE;
        transaction_depth_restore(scale);
        transaction_options=0;
        pending_capture(scale);
        gl_renderer_set_interpolation_source(1);
        gl_renderer_set_interpolation(1,75.0,60.0,59.94,2);
        check(pass_vblank_locked_rate() && pass_queued_presenter(),
              "nominal 60 on a 75-Hz display retains the queued NTSC presenter");
        check(s_interp_target_hz == 59.94, "hold target follows exact NTSC VBlank rate");
        gl_renderer_set_interpolation(1,75.0,75.0,59.94,2);
        check(!pass_vblank_locked_rate(), "genuine 75-Hz conversion stays distinct");
        gl_renderer_set_interpolation(1,75.0,60.0,59.94,0);
        check(!pass_vblank_locked_rate(), "linear interpolation retains explicit 60-Hz target");
        gl_renderer_set_interpolation(0,75.0,60.0,59.94,2);
        {
            static uint16_t snap_a[1024*512], snap_b[1024*512];
            atomic_store(&snapshot_done,0);
            gl_renderer_set_hd_texture_mode(1);
            check(gl_renderer_render_thread_start(2),"snapshot fixture starts render worker");
            rtb_fill_rect(64,64,16,1,0x001f);
            rtb_snapshot_submit(snap_a,snapshot_completed,&snapshot_done);
            rtb_fill_rect(64,64,16,1,0x03e0);
            rtb_snapshot_submit(snap_b,snapshot_completed,&snapshot_done);
            rtb_fill_rect(64,64,16,1,0x7c00);
            rtb_snapshot_barrier();
            check(atomic_load_explicit(&snapshot_done,memory_order_acquire)==2,
                  "snapshot callbacks finish before the barrier returns");
            check(snap_a[64*1024+64]==0x001f && snap_b[64*1024+64]==0x03e0,
                  "queued snapshots retain their own command-boundary VRAM");
            check(!rt_held(),"snapshot submit and barrier do not migrate the GL context");
            gl_renderer_render_thread_stop();
            gl_renderer_set_hd_texture_mode(0);
        }
        {
            unsigned char expected[128*128*4], decoded[128*128*4];
            gl_renderer_rewind_video_enable(1);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);glViewport(0,0,128,128);
            glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
            glClearColor(.25,.5,.75,1);glClear(GL_COLOR_BUFFER_BIT);
            p_glBindBuffer(RV_PIXEL_PACK_BUFFER,0);
            glPixelStorei(GL_PACK_ALIGNMENT,4);glPixelStorei(GL_PACK_ROW_LENGTH,0);
            glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,expected);
            for(int i=0;i<4;i++){rewind_video_capture();glFinish();SDL_Delay(2);}
            RewindHistoryStats history;
            for(int i=0;i<100;i++){rewind_history_stats(RV.history,&history);if(history.completed>=2)break;SDL_Delay(2);}
            gl_renderer_rewind_video_freeze(1);
            size_t bytes=0;RewindHistoryMeta meta;
            check(rewind_history_decode_before(RV.history,1,decoded,sizeof decoded,&bytes,&meta),
                  "full-resolution PBO capture becomes an immutable rewind frame");
            check(bytes==sizeof expected && meta.width==128 && meta.height==128 &&
                  !memcmp(expected,decoded,sizeof expected),
                  "lossless rewind pixels equal the complete drawable without downsampling");
            gl_renderer_rewind_video_enable(0);
            /* A restored VRAM center, even at the requested guest tick, must
             * never satisfy state-rewind's full-image presentation barrier. */
            gl_renderer_rewind_state_enable(1);
            s_frame_count=100;s_rewind_complete_source=1;
            s_rewind_complete_cycle=12345;s_rewind_complete_phase=32768;
            rewind_state_note_present();
            uint64_t shown=0,tick=0;
            assert(gl_renderer_rewind_state_select(101,90,&shown,&tick) && tick==100);
            gl_renderer_rewind_video_freeze(1);
            FrameInterpolationSchedule schedule_before=s_interp_schedule;
            uint64_t swaps_before=s_interp_swaps;
            interp_present_source_interval();
            check(!memcmp(&schedule_before,&s_interp_schedule,sizeof schedule_before) &&
                  s_interp_swaps==swaps_before,
                  "hidden state reconstruction does not schedule forward presentation");
            gl_renderer_rewind_video_resume(0,100);
            s_rewind_complete_source=0;assert(rewind_video_hold());
            assert(!gl_renderer_rewind_state_presented(100));
            s_rewind_complete_source=1;s_rewind_complete_cycle=12346;
            assert(rewind_video_hold());
            s_rewind_complete_cycle=12345;assert(!rewind_video_hold());
            assert(RV.frozen && gl_renderer_rewind_state_presented(100));
            assert(rewind_video_hold()); /* no unrelated following swap */
            gl_renderer_rewind_state_branch(99);
            assert(!gl_renderer_rewind_state_select(101,90,&shown,&tick));
            gl_renderer_rewind_video_freeze(0);gl_renderer_rewind_state_enable(0);
            s_rewind_complete_source=0;

        }
        command_rewind(scale);
        gl_renderer_shutdown();
    }
    printf("checks=%d failures=%d\n",checks,failures);
    SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
