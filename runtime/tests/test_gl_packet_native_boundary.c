/* Packet presentation must not execute native raster work a second time. */
#define main inherited_fixture_main
#define PSX_GL_PRODUCTION_SURFACES_TEST 1
#include "test_gl_render_thread.c"
#undef main
int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Packet authority boundary",0,0,320,240,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 77;
    gr_set_backend(GR_BACKEND_OPENGL);gr_init(vram);gr_set_scale(1);
    if(!gl_renderer_init_context(win))return 77;
    gl_renderer_set_pgxp_depth(1);gl_renderer_set_pgxp_color_perspective(1);gl_renderer_set_seam_snap(1);
    gr_set_draw_area(0,0,319,239);gr_set_draw_offset(0,0);
    gr_fill_rect(0,0,320,240,0);flush_flat_batch();flush_tex_batch();
    static uint16_t before[1024*512];memcpy(before,s_vram,sizeof before);
    s_rth_packet_replaying=1;
    for(int hd=0;hd<2;++hd)for(int dual=0;dual<2;++dual) {
        s_hd_native_authority=hd;s_cpu_auth_dual=dual;
        check(!cpu_raster_required(),"packet has immutable native inputs under every authority mode");
        glb_draw_flat_rect(20,20,40,40,0x001f);
        check(!memcmp(before,s_vram,sizeof before),"presentation cannot mutate native sampling mirror by rerasterizing");
    }
    flush_flat_batch();flush_tex_batch();s_rth_packet_replaying=0;
    s_hd_native_authority=1;s_cpu_auth_dual=0;
    check(cpu_raster_required(),"direct HD rendering keeps its native obligation");
    s_hd_native_authority=0;
    static uint32_t pixels[320*240];int w,h;
    check(gl_renderer_read_display_hires(0,0,320,240,pixels,320*240,&w,&h)==320*240,"GPU image still rendered");
    check((pixels[30*320+30]&0x00ffffff)!=0,"packet draw produced colour");
    unsigned owner=surface_pool_acquire(&s_region_pool,-16,0,352,240,0,0,320,240,1,0);
    check(owner!=0,"allocation for reset regression");
    GLuint old_fbo=s_region_pool.surfaces[owner]->fbo;
    /* Poison every attachment, including the reveal margin. A reset test
     * starting from freshly cleared storage cannot detect a missing clear. */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,old_fbo);
    glDisable(GL_SCISSOR_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthMask(GL_TRUE);glStencilMask(255);
    glClearColor(1,0,1,1);glClearDepth(0.25);glClearStencil(123);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);
    gl_renderer_restage_vram_after_savestate();
    check(s_region_pool.surfaces[owner]->fbo==old_fbo,"invalidation retains allocation identity");
    check(s_region_pool.coverage.count==1 && !s_region_pool.coverage.rects[0].owner,"invalidation removes all enhanced ownership");
    surface_pool_retire_unused();
    check(s_region_pool.surfaces[owner]!=NULL,"alternate target survives packet retirement");
    unsigned reused=surface_pool_acquire(&s_region_pool,-16,0,352,240,0,0,320,240,1,0);
    check(reused==owner && s_region_pool.surfaces[reused]->fbo==old_fbo,"same bounds reacquire exact allocation");
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,old_fbo);
    uint32_t clear_colour=~0u;float clear_depth=0;unsigned char clear_stencil=255;
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,&clear_colour);
    glReadPixels(0,0,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&clear_depth);
    glReadPixels(0,0,1,1,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,&clear_stencil);
    check(clear_colour==0 && clear_depth==1 && clear_stencil==0,"reveal colour depth stencil match fresh allocation");
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);
    printf("checks=%d failures=%d\n",checks,failures);
    gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
