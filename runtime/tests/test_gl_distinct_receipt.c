/* Signalled asynchronous pixel receipts distinguish repeated images. */
#define main legacy_fixture_main
#include "test_gl_render_thread.c"
#undef main
int main(void) {
#ifdef _WIN32
    _putenv_s("PSX_PRESENT_TRACE","distinct-receipt.csv");
#else
    setenv("PSX_PRESENT_TRACE","distinct-receipt.csv",1);
#endif
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Distinct receipt hidden test",0,0,320,240,
        SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 77;
    gr_set_backend(GR_BACKEND_OPENGL);gr_init(vram);gr_set_scale(1);
    if(!gl_renderer_init_context(win))return 77;
    GLuint texture=make_tex(GL_RGBA8,320,240,GL_RGBA,GL_UNSIGNED_BYTE),fbo=0;
    check(make_fbo(&fbo,texture,0),"receipt source allocation");
    s_ptp_fbo[0]=fbo;s_pts_w[0]=320;s_pts_h[0]=240;
    for(int frame=0;frame<40;++frame) {
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,fbo);
        glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
        glClearColor((frame/2)&1?1.f:0.f,.25f,.5f,1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        uint64_t token=0;
        check(pt_trace_sample(0,&token),"receipt queued");
        pt_trace_record(0,token);
        glFinish(); /* Fixture only: make completion deterministic. */
        pt_trace_resolve();
        check(s_pt_trace_read==s_pt_trace_write,"completed receipt retired");
    }
    pt_trace_shutdown();
    FILE *f=fopen("distinct-receipt.csv","rb");
    check(f!=NULL,"receipt file available");
    unsigned rows=0,changes=0;
    char line[512];
    if(f) {
        fgets(line,sizeof line,f);
        while(fgets(line,sizeof line,f)) {
            char *last=strrchr(line,',');
            check(last!=NULL && atoi(last+1)>=0,"no missing receipt");
            if(last){++rows;changes+=atoi(last+1)>0;}
        }
        fclose(f);
    }
    check(rows==40 && changes==20,"forty swaps contain twenty changed images");
    s_ptp_fbo[0]=0;p_glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);
    check(glGetError()==GL_NO_ERROR,"asynchronous receipt GL state valid");
    gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();
    printf("distinct receipts rows=%u changes=%u failures=%d\n",rows,changes,failures);
    return failures?1:0;
}
