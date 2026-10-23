/* Real hidden-GL verification of timed slot handoff and reset cancellation.
 * Reuse the source-owned render-thread fixture's backend and host services. */
#define main render_thread_fixture_main
#include "test_gl_render_thread.c"
#undef main

void psx_video_telemetry_compose(int renderer, int scale, int sw, int sh,
                                int dw, int dh, int vw, int vh) {
    (void)renderer; (void)scale; (void)sw; (void)sh;
    (void)dw; (void)dh; (void)vw; (void)vh;
}
void psx_video_telemetry_present(int result) { (void)result; }
int psx_host_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
    *pixels = NULL; *w = *h = 0; return 0;
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
    gl_renderer_set_present_thread(1, 3);
    gl_renderer_set_interpolation(1, 144, 100, 60, 2);
    check(gl_renderer_render_thread_start(2), "100 Hz interpolation permits render thread");
    check(gl_renderer_present_thread_active(), "independent present thread active");
    if (!gl_renderer_present_thread_active()) {
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }
    gl_renderer_render_thread_sync("timed output fixture");
    uint64_t now = pt_clock_ns();
    PtTiming timing = {gl_renderer_present_epoch(), now + 30000000u,
                       now + 1000000000u};
    check(gl_renderer_set_present_timing(&timing), "valid timing arms composed output");
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    gl_present_swap();
    check(!s_pt_next_timing.epoch, "timing consumed exactly once");
    pt_drain();
    PtStats st;
    pt_get_stats(&st);
    check(st.presents == 1 && !st.waits, "timed swap presented without producer wait");
    check(pt_clock_ns() >= timing.not_before_ns, "presentation honored deadline");
    now = pt_clock_ns();
    timing.not_before_ns = now + 5000000000ull;
    timing.expires_ns = now + 6000000000ull;
    check(gl_renderer_set_present_timing(&timing), "second future output armed");
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    gl_present_swap();
    interp_reset_history_unlocked();
    pt_drain();
    pt_get_stats(&st);
    check(st.timed_dropped == 1 && st.presents == 1, "history reset retires queued image");
    check(!gl_renderer_set_present_timing(&timing), "stale epoch cannot arm a new swap");
    check(pt_clock_ns() - now < 1000000000u, "reset did not wait for future presentation");
    check(glGetError() == GL_NO_ERROR, "no GL errors after cancelled fence reuse");
    gl_renderer_shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
