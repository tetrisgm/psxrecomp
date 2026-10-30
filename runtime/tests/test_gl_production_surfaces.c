/* Exercise the production allocation/lifecycle path, with all PGXP corrections. */
#define PSX_GL_PRODUCTION_SURFACES_TEST 1
#define main legacy_fixture_main
#include "test_gl_render_thread.c"
#undef main
static int run_scale(int scale) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Production surfaces hidden test",0,0,320,240,
        SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 77;
    memset(vram,0,sizeof vram);
    gr_set_backend(GR_BACKEND_OPENGL);gr_init(vram);gr_set_scale(scale);
    if(!gl_renderer_init_context(win))return 77;
    gl_renderer_set_pgxp_depth(1);gl_renderer_set_pgxp_color_perspective(1);
    gl_renderer_set_seam_snap(1);s_pgxp_seam=1;
    check(s_region_pool.coverage.rects && s_region_pool.count==1,
        "production starts native-only without enhanced allocations");
    check(!s_primary_surface.tex && !s_primary_surface.fbo && !s_primary_surface.rb,
        "production never allocates full scaled VRAM colour/depth");
    glb_wide_configure(426,53);
    GLuint pages[2];
    for(int page=0;page<2;++page) {
        int y=240*page;
        gr_set_draw_area(0,y,319,y+239);gr_set_draw_offset(0,0);
        glb_wide_set_target(0);
        gr_set_precise_triangle(1,48*65536,(y+48)*65536,
            112*65536,(y+48)*65536,48*65536,(y+112)*65536);
        gr_set_depth_triangle(1,2048,2048,2048);
        gr_draw_gouraud_triangle(48,y+48,0x001f,112,y+48,0x001f,48,y+112,0x001f);
        flush_flat_batch();pages[page]=g_wide_cur;
        check(direct_wide_target() && s_hr_width==426 && s_hr_height==240,
            "wide draws directly into one cropped display band");
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,pages[page]);
        uint8_t pixel[4];glReadPixels(113*scale,60*scale,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        check(pixel[0]==248 && !pixel[1] && !pixel[2],"wide geometry reaches selected band");
    }
    gr_set_draw_area(0,240,319,239);
    gr_draw_flat_triangle(10,10,20,10,10,20,0x7fff);flush_flat_batch();
    check(!g_wide_cur,"empty drawing area emits no surface draw");
    gr_set_draw_area(0,240,319,479);
    glb_wide_clear(0,500,100,0x7c00);
    glb_wide_clear_margins(0,-20,32,0x7c00,3);
    check(glGetError()==GL_NO_ERROR,"wide clears clip at both VRAM boundaries");
    check(pages[0]!=pages[1],"two independent vertical buffers");
    check(present_wide_fbo_impl(0,0,240,0)==1,"present front while back selected");
    uint32_t *pixels=malloc((size_t)426*240*scale*scale*4);
    check(glb_render_wide_display(pixels,426*scale*4,0,0,240)==426*240*scale*scale,
        "wide diagnostics read the cropped front band");
    check((pixels[60*scale*426*scale+113*scale]&0xffffff)==0xf80000,
        "diagnostic pixels match wide render");
    check(gl_read_display_argb(0,0,320,240,pixels,320*scale*4,426*240*scale*scale,NULL,NULL)>0,
        "canonical diagnostics gather across owned regions");
    present_vram_impl(0,0,320,240,0,0);
    glb_wide_disable_target();
    gr_set_draw_area(640,256,703,319);
    gr_set_precise_triangle(1,648*65536,264*65536,696*65536,264*65536,648*65536,312*65536);
    gr_set_depth_triangle(1,2048,2048,2048);
    gr_draw_gouraud_triangle(648,264,0x03e0,696,264,0x03e0,648,312,0x03e0);
    flush_flat_batch();
    check(surface_owner_at(660,280)!=0,"offscreen draw owns an on-demand tile");
    size_t old_count=s_region_pool.count;
    s_alloc_scale=scale;check(dyn_apply(scale>1?1:scale),"dynamic scale preserves surface ownership");
    check(s_region_pool.count==old_count,"scale change does not allocate native-only VRAM");
    gl_renderer_restage_vram_after_savestate();
    check(s_region_pool.count==1 && !g_wide_cur && s_draw_surface==&s_primary_surface,
        "restore discards disposable enhanced owners");
    check(!s_primary_surface.fbo,"restore never recreates full-VRAM enhanced target");
    check(glGetError()==GL_NO_ERROR,"production lifecycle GL state");
    free(pixels);gl_renderer_shutdown();
    check(!s_region_pool.coverage.rects && !s_draw_regions,"shutdown releases CPU ownership maps");
    SDL_DestroyWindow(win);SDL_Quit();
    return failures?1:0;
}
int main(void) {
    int scales[]={1,4,9,18};
    for(unsigned i=0;i<4;++i){int rc=run_scale(scales[i]);if(rc)return rc;}
    printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
