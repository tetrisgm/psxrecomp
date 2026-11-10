/* Compare hardware depth clears with the former six-vertex clear, using
 * identical queue replay, scissor, surfaces and GL state. */
#include "psx_sdl.h"
#ifdef PSX_SDL3
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_opengl.h>
#endif
static int reference_draw;
static void depth_fixture_clear(GLbitfield mask);
#define glClear depth_fixture_clear
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
#undef glClear
void gpu_hd_textures_track_upload(int x, int y, int w, int h, const uint16_t* words) {
    (void)x; (void)y; (void)w; (void)h; (void)words;
}
void gpu_hd_textures_begin_upload(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void gpu_hd_textures_begin_copy(int sx, int sy, int dx, int dy, int w, int h) {
    (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h;
}
void gpu_hd_textures_end_copy(void) {}
void gpu_hd_textures_invalidate(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void present_shot_done(int ok) { (void)ok; }
int host_osd_needs_present(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
void psx_host_sleep_micros(unsigned us) { SDL_Delay((us + 999u) / 1000u); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }


static void depth_fixture_clear(GLbitfield mask) {
    if (reference_draw && mask == GL_DEPTH_BUFFER_BIT)
        glDrawArrays(GL_TRIANGLES, 0, 6);
    else glClear(mask);
}
static uint64_t surface_hash(GLuint fbo, int w, int h) {
    size_t count=(size_t)w*h;
    uint32_t *pixels=malloc(count*4);
    check(pixels!=NULL,"readback allocation");
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,fbo);
    glReadPixels(0,0,w,h,GL_DEPTH_COMPONENT,GL_FLOAT,pixels);
    uint64_t hash=fnv(pixels,count*4,0xcbf29ce484222325ull);
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    hash=fnv(pixels,count*4,hash);
    glReadPixels(0,0,w,h,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,pixels);
    hash=fnv(pixels,count,hash);
    free(pixels);return hash;
}
static void seed_surface(GLuint fbo) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,fbo);
    glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glStencilMask(0xff);
    glClearDepth(0.25);glClearColor(0.25,0.5,0.75,1);glClearStencil(3);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
}
static void transaction_depth_restore(int scale) {
    if (s_hiw) return;
    flush_flat_batch(); flush_tex_batch(); flush_cpu_upload(); hiw_flush_queue();
    seed_surface(s_hr_fbo); seed_surface(g_wide_cur);
    gl_renderer_set_interpolation(1,60.0,60.0,60.0,2);
    gl_renderer_set_interpolation_source(1);
    s_interp_valid=2; s_interp_w=320*scale; s_interp_h=240*scale;
    s_interp_source_path=GL_PRES_VRAM;
    s_depth_need_clear=0; s_depth_used=1; s_depth_last_avg=1234.0f;
    s_depth_need_reason=DCLR_THRESHOLD;
    uint64_t native=surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale);
    uint64_t wide=surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale);
    for(int keep=0;keep<2;++keep) {
        check(gl_renderer_pass_begin(0,0,320,240,1,2,0),"temporal transaction opens");
        GLuint targets[2]={s_hr_fbo,g_wide_cur};
        for(int k=0;k<2;++k) {
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER,targets[k]);
            glEnable(GL_SCISSOR_TEST);glScissor(0,0,(k?g_wide_w:320)*scale,240*scale);
            glDepthMask(GL_TRUE);glStencilMask(0xff);glColorMask(1,1,1,1);
            glClearDepth(0.75);glClearStencil(0);glClearColor(1,0,0,1);
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
        }
        s_depth_need_clear=1; s_depth_used=0;s_depth_last_avg=9876.0f;
        s_depth_need_reason=DCLR_FILL;
        gl_renderer_pass_end(32768,keep);
        check(surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale)==native,
              "temporal pass restores canonical color, depth and stencil exactly");
        check(surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale)==wide,
              "temporal pass restores wide color, depth and stencil exactly");
        check(!s_depth_need_clear && s_depth_used && s_depth_last_avg==1234.0f &&
              s_depth_need_reason==DCLR_THRESHOLD,"temporal pass restores PGXP depth grouping");
    }
    gl_renderer_set_interpolation(0,60.0,60.0,60.0,2);
}
/* Pending generations must include late native writes (e.g. HUD) and
 * reuse the exact native-wide center already refreshed by presentation. */
static void pending_capture(int scale) {
    if(s_hiw)return;
    for(int wide=0;wide<2;wide++) {
        flush_flat_batch();flush_tex_batch();flush_cpu_upload();hiw_flush_queue();
        seed_surface(s_hr_fbo);seed_surface(g_wide_cur);
        gl_renderer_set_interpolation(1,60,60,60,2);
        gl_renderer_set_interpolation_source(1);
        int width=wide?g_wide_w:320;
        s_interp_valid=2;s_interp_w=width*scale;s_interp_h=240*scale;
        s_interp_source_path=wide?GL_PRES_WIDE:GL_PRES_VRAM;
        check(gl_renderer_pass_begin(0,0,320,240,1,2,0),"pending generation opens");
        gl_renderer_pass_end(32768,1);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,s_hr_fbo);
        glEnable(GL_SCISSOR_TEST);glScissor(20*scale,30*scale,40*scale,50*scale);
        glColorMask(1,1,1,1);glClearColor(0.75,0.125,0.25,1);glClear(GL_COLOR_BUFFER_BIT);
        if(wide)wide_blit_center(g_wide_cur,0,0,240);
        size_t bytes=(size_t)width*scale*240*scale*4;
        uint8_t *want=malloc(bytes),*got=malloc(bytes);
        if(!want||!got){check(0,"capture allocation");free(want);free(got);return;}
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,wide?g_wide_cur:s_hr_fbo);
        glReadPixels(0,0,width*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,want);
        pass_note_new_frame(0,0,s_interp_source_path,width*scale,240*scale);
        pass_apply_promotion();
        check(s_pgen[s_pgen_cur].promoted,"pending generation promoted");
        glBindTexture(GL_TEXTURE_2D,s_pgen_tex[s_pgen_cur][0]);
        glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,got);
        check(!memcmp(want,got,bytes),"phase zero equals final native image byte for byte");
        glBindTexture(GL_TEXTURE_2D,0);free(want);free(got);
        gl_renderer_set_interpolation(0,60,60,60,2);
    }
}
int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Depth clear parity",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 2;
    for(int si=0;si<3;++si) {
        int scale=si==0?1:si==1?4:9;
        glb_init(vram);glb_set_scale(scale);gl_renderer_set_swap_interval(0);
        if(!gl_renderer_init_context(win))return 2;
        check(s_out_scale==scale,"requested scale is active");
        gl_renderer_set_pgxp_depth(1);glb_wide_configure(560,120);glb_wide_set_target(0);
        check(g_wide_cur!=0,"wide surface allocated");
        if(s_hiw) check(hiw_ensure(0,320)!=NULL,"allocate high-resolution tile");
        for(int area=0;area<3;++area) {
            int y=area==1?256:0;
            glb_set_draw_area(area==2?20:0,y,area==2?80:319,y+(area==2?63:239));
            glb_set_mask_bits(1,1);
            uint64_t hashes[2][3];
            for(int ref=0;ref<2;++ref) {
                hiw_flush_queue();
                seed_surface(s_hr_fbo);seed_surface(g_wide_cur);
                for(int t=0;t<s_hiw_n;++t)seed_surface(s_hiw_t[t].fbo);
                reference_draw=ref;
                depth_clear_now();hiw_flush_queue();reference_draw=0;
                hashes[ref][0]=surface_hash(s_hr_fbo,VRAM_W*s_hr_scale,VRAM_H*s_hr_scale);
                hashes[ref][1]=surface_hash(g_wide_cur,g_wide_w*s_out_scale,VRAM_H*s_out_scale);
                hashes[ref][2]=0;
                for(int t=0;t<s_hiw_n;++t)hashes[ref][2]^=surface_hash(s_hiw_t[t].fbo,
                    (s_hiw_t[t].x1-s_hiw_t[t].x0)*s_out_scale,VRAM_H*s_out_scale);
            }
            check(hashes[0][0]==hashes[1][0],"canonical depth/color/stencil equal former draw");
            check(hashes[0][1]==hashes[1][1],"wide depth/color/stencil equal former draw");
            check(hashes[0][2]==hashes[1][2],"tile depth/color/stencil equal former draw");
        }
        transaction_depth_restore(scale);
        pending_capture(scale);
        check(glGetError()==GL_NO_ERROR,"GL error");
        gl_renderer_shutdown();
    }
    printf("checks=%d failures=%d\n",checks,failures);
    SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
