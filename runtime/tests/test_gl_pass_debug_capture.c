#undef PSX_NO_DEBUG_TOOLS
#define main render_thread_fixture_main
#include "test_gl_render_thread.c"
#undef main
void host_sampler_retarget_current(const char *thread) { (void)thread; }
static void test_depth_clear(GLuint fbo, double value) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,fbo);
    glDisable(GL_SCISSOR_TEST); glDepthMask(GL_TRUE); glClearDepth(value);
    glClear(GL_DEPTH_BUFFER_BIT);
}
static float test_depth_read(GLuint fbo) {
    float value=0;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,fbo);
    glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0);
    glReadPixels(4,4,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&value);
    return value;
}
static int test_snap_triangle(int lower, float gap[3]) {
    int32_t ix[3]={0,40,lower?40:0}, iy[3]={lower?20:10,20,lower?30:20};
    int32_t fx[3]={0,40*65536,lower?40*65536:0};
    int32_t fy[3]={lower?1323827:10*65536,lower?1323827:1284506,lower?30*65536:1284506};
    float z[3]={1000,1000,1000}; uint32_t keys[3]={0};
    return pgxp_snap_triangle(s_snap,ix,iy,fx,fy,z,keys,gap);
}
int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Output timing hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) return 77;
    SDL_GLContext probe = SDL_GL_CreateContext(win);
    if (!probe) return 77;
    SDL_GL_MakeCurrent(win, NULL);
    SDL_GL_DeleteContext(probe);
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(1);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) return 2;
    gr_set_scale(2);
    gr_wide_configure(426,53);
    gr_wide_clear(0,0,240,0x3E0);
    gr_fill_rect(0,0,320,240,0x1F);
    flush_flat_batch(); flush_tex_batch(); flush_cpu_upload(); hiw_flush_queue();
    check(!gl_renderer_pass_debug_capture(NULL,0,NULL,NULL,NULL),"capture requires active pass");
    s_pass_active=1;
    PassGen *g=&s_pgen[s_pgen_write];
    memset(g,0,sizeof *g); g->valid=1; g->x=0; g->y=0; g->w=320; g->h=240;
    for(int wide=0;wide<=1;wide++) {
        g->source_path=wide?GL_PRES_WIDE:GL_PRES_VRAM;
        g->tex_w=(wide?426:320)*s_hr_scale; g->tex_h=240*s_hr_scale;
        uint32_t w=0,h=0; int got_wide=-1;
        size_t n=gl_renderer_pass_debug_capture(NULL,0,&w,&h,&got_wide);
        check(n==(size_t)g->tex_w*g->tex_h*4u && w==(uint32_t)g->tex_w &&
              h==(uint32_t)g->tex_h && got_wide==wide,"query includes full internal source dimensions");
        uint8_t *actual=malloc(n),*expected=malloc(n);
        check(!gl_renderer_pass_debug_capture(actual,n-1,NULL,NULL,NULL),"short capture refused");
        glPixelStorei(GL_PACK_ALIGNMENT,8); glPixelStorei(GL_PACK_ROW_LENGTH,7);
        check(gl_renderer_pass_debug_capture(actual,n,NULL,NULL,NULL)==n,"full source captured");
        GLint pack=0,row=0; glGetIntegerv(GL_PACK_ALIGNMENT,&pack); glGetIntegerv(GL_PACK_ROW_LENGTH,&row);
        check(pack==8 && row==7,"pack state restored");
        GLuint texture; glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,g->tex_w,g->tex_h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        pass_capture_into(texture,g);
        glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0);
        glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,expected);
        check(!memcmp(actual,expected,n),"debug pixels equal production pass capture including wide centre");
        free(actual);free(expected);glDeleteTextures(1,&texture);
    }
    s_pass_active=0;
    s_interp_enabled=1; s_interp_suspended=0; s_interp_source=1;
    s_interp_source_hz=30; s_interp_valid=1; s_interp_source_path=GL_PRES_WIDE;
    s_interp_w=426*s_hr_scale; s_interp_h=240*s_hr_scale;
    test_depth_clear(s_hr_fbo,0.25); test_depth_clear(pass_wide_fbo_for(0),0.375);
    s_depth_need_clear=0; s_depth_used=1; s_depth_need_reason=DCLR_THRESHOLD; s_depth_last_avg=123;
    s_snap=calloc(1,sizeof *s_snap); pgxp_snap_reset(s_snap);
    float ignored[3], first_gap[3], repeated_gap[3];
    test_snap_triangle(0,ignored);
    check(transaction_begin(0,0,320,240,1,2,0,0),"depth checkpoint begins");
    int first_snap=test_snap_triangle(1,first_gap);
    test_depth_clear(s_hr_fbo,0.75); test_depth_clear(pass_wide_fbo_for(0),0.875);
    s_depth_need_clear=1; s_depth_used=0; s_depth_need_reason=DCLR_FILL; s_depth_last_avg=987;
    transaction_restore();
    check(fabsf(test_depth_read(s_hr_fbo)-0.25f)<0.00001f,"canonical depth restored");
    check(fabsf(test_depth_read(pass_wide_fbo_for(0))-0.375f)<0.00001f,"wide depth restored");
    check(!s_depth_need_clear && s_depth_used && s_depth_need_reason==DCLR_THRESHOLD &&
          s_depth_last_avg==123,"depth clear history restored");
    check(first_snap>0 && test_snap_triangle(1,repeated_gap)==first_snap &&
          !memcmp(first_gap,repeated_gap,sizeof first_gap),"repeated full draw has identical seam gaps");
    /* Restore baseline before the next transaction, then force an area change
     * inside it to exercise the GPU register-resync reset ordering. */
    pgxp_snap_reset(s_snap); test_snap_triangle(0,ignored);
    check(transaction_begin(0,0,320,240,0,2,1,0),"depth backup reused");
    check(async_vram_capture(s_pgen_write),"immutable async depth captured");
    test_depth_clear(s_hr_fbo,0.75); s_depth_last_avg=987;
    check(async_vram_enter(s_pgen_write),"immutable async depth entered");
    check(fabsf(test_depth_read(s_hr_fbo)-0.25f)<0.00001f && s_depth_last_avg==123,
          "async job begins with captured depth and history");
    check(test_snap_triangle(1,repeated_gap)==first_snap &&
          !memcmp(first_gap,repeated_gap,sizeof first_gap),"async seam gaps match original baseline");
    glb_set_draw_area(7,8,99,100);
    async_vram_leave(); transaction_restore();
    glb_set_draw_area(s_pb_area[0],s_pb_area[1],s_pb_area[2],s_pb_area[3]);
    check(test_snap_triangle(1,repeated_gap)==first_snap &&
          !memcmp(first_gap,repeated_gap,sizeof first_gap),"register resync preserves restored seam history");
    check(glGetError()==GL_NO_ERROR,"capture leaves clean GL state");
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    printf("checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
