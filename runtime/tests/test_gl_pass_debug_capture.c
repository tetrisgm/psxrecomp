#undef PSX_NO_DEBUG_TOOLS
#define main render_thread_fixture_main
#include "test_gl_render_thread.c"
#undef main
void host_sampler_retarget_current(const char *thread) { (void)thread; }
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
    check(glGetError()==GL_NO_ERROR,"capture leaves clean GL state");
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    printf("checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
