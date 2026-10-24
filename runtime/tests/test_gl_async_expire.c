/* Expiring a queued extra frame during replay skips its remaining draw work
 * and restores the live colour/stencil transaction exactly. */
#define main render_thread_fixture_main
#include "test_gl_render_thread.c"
#undef main

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win = SDL_CreateWindow("Async expiry hidden test", 0, 0, 320, 240,
                                      SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) return 77;
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(1);
    if (!gl_renderer_init_context(win)) return 77;
    gr_set_draw_area(0, 0, 319, 239);
    gr_set_draw_offset(0, 0);
    gr_fill_rect(0, 0, 320, 240, 0x0421);
    flush_flat_batch(); flush_tex_batch(); flush_cpu_upload();

    s_interp_enabled=1; s_interp_source=1; s_interp_source_hz=30.0;
    s_interp_target_hz=60.0; s_interp_valid=1;
    s_interp_source_path=GL_PRES_VRAM; s_interp_w=320; s_interp_h=240;
    int prior_write=s_pgen_write;
    check(transaction_begin(0,0,320,240,1,1,0,0), "transaction begin");
    if (!s_pass_active) return 2;
    s_async_previous_write=prior_write;
    s_async_exec=1; s_async_replay_started=pt_clock_ns();
    s_async_executing.output.epoch=gl_renderer_present_epoch();
    s_async_executing.output.expires=pt_clock_ns()+1000000000ull;
    s_pass_leaks_at_begin=s_pass_leaks;
    int32_t tri[7]={15,15,120,15,15,120,0x7fff};
    RtCmd draw={sizeof(RtCmd)+sizeof tri,RTH_FLAT_TRI,0,sizeof tri,1};
    gl_rth_exec_impl(NULL,&draw,tri);
    s_async_executing.output.expires=pt_clock_ns()-1;
    s_async_expiry_poll=15;
    gl_rth_exec_impl(NULL,&draw,tri);  /* fast path skips this expired draw */
    const char *env=getenv("PSX_GL_ASYNC_EXPIRE_FAST");
    int fast=!(env && env[0]=='0');
    check((s_pass_dead!=0)==fast,"expired replay classification");
    int32_t end[2]={0,0};
    RtCmd finish={sizeof(RtCmd)+sizeof end,RTH_PASS_END,0,sizeof end,2};
    gl_rth_exec_impl(NULL,&finish,end);
    check(!s_pass_active && !s_async_exec && !s_pass_dead,"transaction ended");
    check(s_vram==vram && s_pgen_write==prior_write,"live state rebound");
    check((atomic_load(&s_async_count_render_expired)>0)==fast,"expiry counter");

    unsigned char *color=(unsigned char *)malloc(320u*240u*4u);
    unsigned char *stencil=(unsigned char *)malloc(320u*240u);
    if (!color || !stencil) return 2;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_hr_fbo);
    glReadPixels(0,0,320,240,GL_RGBA,GL_UNSIGNED_BYTE,color);
    glReadPixels(0,0,320,240,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,stencil);
    uint64_t ch=fnv(color,320u*240u*4u,0xcbf29ce484222325ull);
    uint64_t sh=fnv(stencil,320u*240u,0xcbf29ce484222325ull);
    GLenum err=glGetError();
    printf("stencil=%016llx color=%016llx expired=%llu checks=%d failures=%d error=%x\n",
           (unsigned long long)sh,(unsigned long long)ch,
           (unsigned long long)atomic_load(&s_async_count_render_expired),
           checks,failures,(unsigned)err);
    free(color); free(stencil);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures || err!=GL_NO_ERROR ? 2 : 0;
}
