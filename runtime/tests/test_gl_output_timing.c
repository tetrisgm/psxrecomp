/* Real hidden-GL verification of timed slot handoff and reset cancellation.
 * Reuse the source-owned render-thread fixture's backend and host services. */
static void timed_pixel_probe(int slot);
#define GL_PRESENT_THREAD_TEST_HOOK timed_pixel_probe
#define main render_thread_fixture_main
#include "test_gl_render_thread.c"
#undef main

static unsigned timed_red_pixels;
static void timed_pixel_probe(int slot) {
    (void)slot;
    unsigned char pixels[320*240*4];
    glReadPixels(0,0,320,240,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    for(unsigned i=0;i<320*240;i++)
        if(pixels[i*4]>200 && pixels[i*4+1]<20) ++timed_red_pixels;
}

void host_sampler_retarget_current(const char *thread) { (void)thread; }

int main(void) {
#ifdef _WIN32
    _putenv_s("PSX_PRESENT_TRACE","present-trace.csv");
#else
    setenv("PSX_PRESENT_TRACE","present-trace.csv",1);
#endif
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
                       now + 1000000000u,7,40,32768};
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
    for(int changed=0;changed<2;changed++) {
        timing.not_before_ns=pt_clock_ns(); timing.expires_ns=timing.not_before_ns+1000000000u;
        timing.index=41+changed;
        check(gl_renderer_set_present_timing(&timing),"trace frame timing");
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);
        glClearColor(changed?0.8f:0.2f,0.4f,0.6f,1.0f); glClear(GL_COLOR_BUFFER_BIT);
        gl_present_swap(); pt_drain();
    }
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
    check(st.timed_dropped == 1 && st.presents == 3, "history reset retires queued image");
    check(!gl_renderer_set_present_timing(&timing), "stale epoch cannot arm a new swap");
    check(pt_clock_ns() - now < 1000000000u, "reset did not wait for future presentation");
    check(glGetError() == GL_NO_ERROR, "no GL errors after cancelled fence reuse");
    /* Actual queued generation reservation/admission at a noninteger rate. */
    s_interp_valid=1; s_interp_w=320; s_interp_h=240; s_interp_source_path=GL_PRES_VRAM;
    s_interp_source=1; s_interp_source_hz=60; s_interp_target_hz=100;
    gr_fill_rect(384,0,16,16,0x1F); /* red texture at snapshot time */
    rt_release();
    uint64_t lease=atomic_load(&s_async_resource_epoch);
    gl_renderer_note_present_depth24(0);
    check(!rt_held() && atomic_load(&s_async_resource_epoch)==lease,
          "unchanged display depth preserves asynchronous lease and thread ownership");
    /* A notification without residency changes must preserve the lease. */
    rt_acquire("HD policy test");
    test_hd_active=1; test_hd_overlap=0;
    hd_note_exec(GR_HD_NOTE_INVALIDATE,0,0,16,16,0,0);
    check(atomic_load(&s_async_resource_epoch)==lease,"unrelated HD note preserves resource epoch");
    test_hd_overlap=1;
    hd_note_exec(GR_HD_NOTE_INVALIDATE,0,0,16,16,0,0);
    check(atomic_load(&s_async_resource_epoch)==lease+1,"changed HD residency invalidates resource epoch");
    const uint64_t tracking=test_hd_tracking;
    s_async_exec=1; s_async_invalid=0;
    hd_note_exec(GR_HD_NOTE_INVALIDATE,0,0,16,16,0,0);
    check(s_async_invalid && test_hd_tracking==tracking,"private overlapping note refuses without live metadata mutation");
    test_hd_overlap=0; s_async_invalid=0;
    hd_note_exec(GR_HD_NOTE_INVALIDATE,0,0,16,16,0,0);
    check(!s_async_invalid && test_hd_tracking==tracking,"private unrelated note preserves live metadata");
    hd_note_exec(GR_HD_NOTE_BEGIN_COPY,0,0,16,16,0,0);
    check(!s_async_invalid && s_async_hd_copy && test_hd_tracking==tracking,"private untracked copy begins without live mutation");
    hd_note_exec(GR_HD_NOTE_END_COPY,0,0,0,0,0,0);
    check(!s_async_invalid && !s_async_hd_copy && test_hd_tracking==tracking,"private untracked copy pair completes without live mutation");
    hd_note_exec(GR_HD_NOTE_END_COPY,0,0,0,0,0,0);
    check(s_async_invalid,"unpaired private copy end refuses");
    s_async_invalid=0; test_hd_overlap=2;
    hd_note_exec(GR_HD_NOTE_BEGIN_COPY,0,0,16,16,256,0);
    check(s_async_invalid && !s_async_hd_copy && test_hd_tracking==tracking,"tracked copy source refuses without live mutation");
    s_async_invalid=0;
    hd_note_exec(GR_HD_NOTE_BEGIN_COPY,256,0,16,16,0,0);
    check(s_async_invalid && !s_async_hd_copy && test_hd_tracking==tracking,"tracked copy destination refuses without live mutation");
    test_hd_overlap=0;
    s_async_exec=0; s_async_invalid=0; test_hd_active=0;
    rt_release();
    GLRenderAsyncToken tokens[8];
    now=pt_clock_ns();
    uint32_t count=gl_renderer_pass_async_reserve(2,now+100000000u,now+133333333u,
                                                0,0,320,240,tokens,8);
    check(count>=3 && count<=4,"100Hz asynchronous outputs over 30Hz window");
    GLRenderAsyncToken next_tokens[8];
    uint32_t next_count=gl_renderer_pass_async_reserve(2,now+145000000u,now+178333333u,
                                                     0,0,320,240,next_tokens,8);
    check(next_count>0 && next_tokens[0].output.alpha_q16>0,
          "second window begins between output-grid timestamps");
    rt_drain();
    GLRenderAsyncStats reserved_stats; gl_renderer_pass_async_stats(&reserved_stats);
    check(reserved_stats.presented_to_queue==1,
          "only the grid-aligned reservation presents its phase-zero capture");
    if (count>1) {
        gr_fill_rect(384,0,16,16,0x3E0); /* live texture changes before worker returns */
        rtb_hd_texture_note(GR_HD_NOTE_INVALIDATE,384,0,16,16,0,0);
        check(gl_renderer_pass_async_begin(&tokens[1]),
              "older reserved generation remains admissible with bounded lookahead");
        gr_set_depth_triangle(1,10,11,12);
        GLRenderAsyncToken wrong=tokens[1]; wrong.output.index++;
        check(!gl_renderer_pass_async_end(&wrong,1) && !s_rth_group_n,
              "aborted async batch cannot leak deferred primitive state");
        check(gl_renderer_pass_async_begin(&tokens[1]),"retry after discarded batch");
        gr_set_draw_area(0,0,319,239);
        gr_set_color_modulation(128,128,128,1);
        gr_draw_textured_rect(0,0,16,16,0,0,0,0,0x106);
        check(gl_renderer_pass_async_end(&tokens[1],1),"publish whole async transaction");
        check(!gl_renderer_pass_async_begin(&tokens[1]),"duplicate completion rejected");
        GLRenderAsyncToken stale=tokens[2]; stale.output.epoch++;
        check(!gl_renderer_pass_async_begin(&stale),"old epoch rejected before recording");
        check(gl_renderer_pass_async_begin(&tokens[2]),"second completed output accepted");
        gr_set_color_modulation(128,128,128,1);
        gr_draw_textured_rect(0,0,16,16,0,0,0,0,0x106);
        check(gl_renderer_pass_async_end(&tokens[2],1),"second output transaction committed");
        gl_renderer_render_thread_sync("check async capture");
        GLRenderAsyncStats base_stats; gl_renderer_pass_async_stats(&base_stats);
        check(base_stats.presented_to_queue<=3 && !base_stats.resource,
              "no off-grid phase0 or inactive HD-note cancellation");
        int found=0;
        for(int i=0;i<PASS_GENERATIONS;++i)
            if(s_pgen[i].async_id==tokens[1].output.generation && s_pgen[i].n==2) found++;
        check(found==1,"completed output appended to original generation");
        pass_note_new_frame(999,499,GL_PRES_VRAM,320,240);
        found=0;
        for(int i=0;i<PASS_GENERATIONS;++i)
            if(s_pgen[i].async_id==tokens[1].output.generation && s_pgen[i].valid) found++;
        check(found==1,"unmatched legacy flip cannot retire an asynchronous generation");
        unsigned char *pixels=malloc(320u*240u*4u);
        int red=0;
        for(int i=0;i<PASS_GENERATIONS;++i) if(s_pgen[i].async_id==tokens[1].output.generation) {
            glBindTexture(GL_TEXTURE_2D,s_pgen_tex[i][1]);
            glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
            for(unsigned k=0;k<320u*240u;++k)
                if(pixels[4*k]>200 && pixels[4*k+1]<20) red++;
        }
        free(pixels);
        check(red>=200,"worker sampled immutable red texture after live texture became green");
        pack_flush();
        uint16_t *raw=malloc(VRAM_W*VRAM_H*sizeof(uint16_t));
        glBindTexture(GL_TEXTURE_2D,s_raw_tex);
        glGetTexImage(GL_TEXTURE_2D,0,PSXGL_RED_INTEGER,GL_UNSIGNED_SHORT,raw);
        check((raw[384]&0x7FFF)==0x3E0,"live green texture restored after worker transaction");
        free(raw);
        check(glGetError()==GL_NO_ERROR,"async GPU transaction clean");
        pt_drain(); pt_get_stats(&st);
        check(st.submits>=4 && st.presents+st.timed_dropped==st.submits,
              "phase0 and worker submissions presented or explicitly expired");
        check(pt_clock_ns()>=tokens[1].output.deadline,"async output honored its own timestamp");
    }
    GLRenderAsyncStats async_stats; gl_renderer_pass_async_stats(&async_stats);
    check(async_stats.duplicate==1 && async_stats.stale==1 && async_stats.submitted==2,
          "async diagnostics distinguish duplicate, epoch and successful admission");
    for(int i=0;i<PASS_GENERATIONS;++i) if(s_pgen[i].async_id==tokens[1].output.generation) {
        s_pgen_cur=i; s_pgen[i].promoted=1; s_pgen[i].t_len=1.0;
        check(!pass_gen_present_at(0.5,1),"timed generation cannot leak into legacy flip presentation");
    }
    // No fresh reservation during this gap: ownership must not revert to a
    // newer live frame before delayed worker output resumes.
    while(pt_clock_ns()<next_tokens[next_count-1].output.expires+1000000u) SDL_Delay(1);
    check(async_display_active(),"expired output window still holds timed-session ownership");
    // Presentation must establish its own raster state after a guest draw.
    GLuint solid=make_tex(GL_RGBA8,320,240,GL_RGBA,GL_UNSIGNED_BYTE);
    unsigned char solid_pixels[320*240*4];
    for(unsigned i=0;i<320*240;++i) {
        solid_pixels[4*i]=255; solid_pixels[4*i+1]=solid_pixels[4*i+2]=0; solid_pixels[4*i+3]=255;
    }
    glBindTexture(GL_TEXTURE_2D,solid);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,320,240,GL_RGBA,GL_UNSIGNED_BYTE,solid_pixels);
    unsigned red_before=timed_red_pixels;
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_NEVER);
    glEnable(GL_STENCIL_TEST); glStencilFunc(GL_NEVER,0,0xff);
    glEnable(GL_BLEND); glBlendFunc(GL_ZERO,GL_ONE);
    glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
    uint64_t immediate=pt_clock_ns();
    PtTiming color_time={gl_renderer_present_epoch(),immediate,immediate+1000000000u};
    check(gl_renderer_set_present_timing(&color_time) && interp_present_pair(solid,solid,0,0),
          "compose timed image after hostile guest raster state");
    pt_drain();
    check(timed_red_pixels-red_before>70000,"presenter actually receives the composed red image");
    glDeleteTextures(1,&solid);
    uint64_t old_epoch=gl_renderer_present_epoch();
    gl_renderer_pass_async_invalidate();
    check(gl_renderer_present_epoch()!=old_epoch && !async_display_active() && !gl_renderer_pass_async_begin(&tokens[1]),
          "discontinuity retires outstanding tokens without GL acquisition");
    gl_renderer_shutdown();
    FILE *trace=fopen("present-trace.csv","rb");
    check(trace!=NULL,"presenter wrote trace");
    if(trace) {
        char line[400]; unsigned long long hash[3]={0}; int changes[3]={0};
        (void)fgets(line,sizeof line,trace);
        for(int i=0;i<3;i++) {
            unsigned long long time,epoch,gen,index,deadline,expires; unsigned alpha;
            int fields=fgets(line,sizeof line,trace)?sscanf(line,"%llu,%llu,%llu,%llu,%u,%llu,%llu,%llX,%d",
                &time,&epoch,&gen,&index,&alpha,&deadline,&expires,&hash[i],&changes[i]):0;
            check(fields==9 && gen==7 && index==(unsigned)(40+i) && alpha==32768,
                  "trace retains output provenance at actual callback");
        }
        check(hash[0]==hash[1] && hash[1]!=hash[2] && changes[0]==1 && changes[1]==0 && changes[2]==1,
              "actual presenter sampling distinguishes changing and repeated images");
        fclose(trace);
    }
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
