/* A peer's HUD must not become background for the next canonical frame. */
#define DEPTH_FIXTURE_MAIN depth_fixture_main
#include "test_gl_depth_clear.c"

/* Screenshot history is outside this presentation fixture. */
int present_image_ring_accepting(void) { return 0; }
int present_image_ring_thumb_w(int w,int h) { (void)w;(void)h;return 0; }
void present_image_ring_push(uint32_t f,const uint8_t *p,int w,int pitch,int flip) {
    (void)f;(void)p;(void)w;(void)pitch;(void)flip;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO)!=0) return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Local image isolation",0,0,128,128,
                                     SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if (!win) return 2;
    for (int scale=1;scale<=4;scale+=3) {
        glb_init(vram); glb_set_scale(scale);
        if (!gl_renderer_init_context(win)) return 2;
        gl_renderer_set_pgxp_depth(1);
        gl_renderer_set_cpu_auth_dual(1);
        flush_flat_batch(); flush_tex_batch(); flush_cpu_upload();
        s_wide_fast=1;
        glb_wide_configure(426,53); glb_wide_set_target(0);
        seed_surface(s_hr_fbo);
        uint64_t before=surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale);
        check(gl_renderer_local_view_begin_complete(0,0,320,240),"begin own image");
        glb_set_draw_area(0,0,319,239); glb_set_draw_offset(0,0);
        glb_set_mask_bits(0,0); glb_set_semi_transparency(0,0);
        glb_draw_flat_rect(40,40,20,20,0x1f);
        check(gl_renderer_local_view_end(1),"publish own image");
        check(surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale)==before,
              "canonical colour depth and stencil restored after publication");
        uint8_t pixel[4];
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_local_fbo);
        glReadPixels(10*scale,10*scale,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        check(pixel[0]==0 && pixel[1]==0 && pixel[2]==0,
              "unpainted own-view pixels do not retain canonical HUD");
        glReadPixels(45*scale,45*scale,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        check(pixel[0]!=0 || pixel[1]!=0 || pixel[2]!=0,"own HUD captured");
        check(s_local_has_wide,"wide own image captured");
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_local_wide_fbo);
        glReadPixels((53+45)*scale,45*scale,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        check(pixel[0]!=0 || pixel[1]!=0 || pixel[2]!=0,
              "wide centre contains own view rather than an empty margin mirror");
        glReadPixels((53+10)*scale,10*scale,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        check(pixel[0]==0 && pixel[1]==0 && pixel[2]==0,
              "wide centre does not retain shared HUD");
        check(local_image_matches(0,0,320,240),"presentation selects own image");
        present_dirty_rect(0,240,319,479,1);
        check(local_image_matches(0,0,320,240),"other buffer preserves image");
        present_dirty_rect(0,0,10,10,1);
        check(local_image_matches(0,0,320,240),"partial shared draw retains completed image");
        check(local_image_matches(0,240,320,240),"double-buffer flip retains completed image");
        gl_renderer_local_view_clear();
        check(!local_image_matches(0,0,320,240),"explicit scene transition releases image");
        test_local_view_only=1;
        check(present_wide_fbo_impl(0,0,240,0),"shared local-only view presents");
        check(s_last_present_path==GL_PRES_VRAM && s_last_dw==320,
              "shared view uses canonical width rather than unrendered wide margins");
        test_local_view_only=0;
        int x,y,w,h;
        test_attract_background=1;
        letterbox_rect_aspect(1280,720,4,3,&x,&y,&w,&h);
        check(x==0 && y==-120 && w==1280 && h==960,
              "attract fills widescreen without stretching 4:3");
        test_attract_background=0;
        letterbox_rect_aspect(1280,720,4,3,&x,&y,&w,&h);
        check(x==160 && y==0 && w==960 && h==720,
              "gameplay retains original aspect policy");
        if (scale>1) {
            uint16_t *saved=malloc(sizeof vram);
            memcpy(saved,vram,sizeof vram);
            check(gl_renderer_set_internal_scale_live(scale-1)==scale-1,
                  "dual raster can reduce presentation scale");
            check(gl_renderer_set_internal_scale_live(scale)==scale,
                  "dual raster restores selected scale");
            check(!memcmp(saved,vram,sizeof vram),
                  "scale transitions preserve authoritative CPU VRAM");
            free(saved);
        }
        gl_renderer_shutdown();
    }
    SDL_DestroyWindow(win);SDL_Quit();
    printf("checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
