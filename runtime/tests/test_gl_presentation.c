/* Private presentation targets must leave the native render surfaces intact. */
#define DEPTH_FIXTURE_MAIN depth_reference_main
#include "test_gl_depth_clear.c"
#undef DEPTH_FIXTURE_MAIN
uint16_t gr_vram_read(int x,int y) { return glb_vram_read(x,y); }
void gr_vram_write(int x,int y,uint16_t value) { glb_vram_write(x,y,value); }

int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Independent presentation",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 2;
    for(int si=0;si<3;si++) {
        int scale=si==0?1:si==1?4:9;
        glb_init(vram);glb_set_scale(scale);
        if(!gl_renderer_init_context(win))return 2;
        gl_renderer_set_pgxp_depth(1);
        glb_wide_configure(560,120);glb_wide_set_target(0);
        flush_cpu_upload();flush_flat_batch();flush_tex_batch();hiw_flush_queue();
        seed_surface(s_hr_fbo);seed_surface(g_wide_cur);
        glb_vram_write(960,496,0x7fff);glb_vram_write(961,496,0x7fff);flush_cpu_upload();
        uint64_t native=surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale);
        uint64_t wide=surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale);
        check(psx_presentation_gpu_test_versions(),"immutable texture versions and ordered private draws");
        check(s_pgpu_scale==scale,"private target uses requested internal resolution");
        check(surface_hash(s_hr_fbo,VRAM_W*scale,VRAM_H*scale)==native,"private rendering preserves native color/depth/stencil");
        check(surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale)==wide,"private rendering preserves native-wide color/depth/stencil");
        check(glGetError()==GL_NO_ERROR,"private rendering GL state is valid");
        seed_surface(s_hr_fbo);
        float ambient=-1;
        check(!gl_renderer_sample_ambient(127,127,&ambient),"ambient queues without a synchronous result");
        glFinish();
        check(gl_renderer_sample_ambient(127,127,&ambient),"ambient consumes completed GPU samples");
        check(fabsf(ambient-(0.3f*64+0.55f*128+0.15f*191)/255)<0.00001f,
              "GPU ambient matches the original weighted scene color");
        check(glGetError()==GL_NO_ERROR,"ambient sampling leaves valid GL state");
        /* Uneven row colors, odd dimensions and out-of-sample edge pixels:
         * compare with the former CPU readback calculation. */
        const int rows[3]={127*35/100,127*55/100,127*75/100};
        glEnable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
        for(int k=0;k<3;++k) {
            glScissor(0,rows[k],127,1);glClearColor(k==0,k==1,k==2,1);glClear(GL_COLOR_BUFFER_BIT);
            glScissor(0,rows[k],20+k*13,1);glClearColor(.2,.7,.4,1);glClear(GL_COLOR_BUFFER_BIT);
        }
        uint8_t row[127*4];double expected=0;int samples=0;
        for(int k=0;k<3;++k) {
            glReadPixels(0,rows[k],127,1,GL_RGBA,GL_UNSIGNED_BYTE,row);
            for(int x=127/8;x<127*7/8;x+=8) {
                expected+=.3*row[x*4]+.55*row[x*4+1]+.15*row[x*4+2];++samples;
            }
        }
        gl_renderer_sample_ambient(127,127,&ambient);glFinish();
        PgpuState before={0},after={0};pgpu_state_save(&before);
        check(gl_renderer_sample_ambient(127,127,&ambient),"patterned ambient sample completes");
        pgpu_state_save(&after);
        check(fabs(ambient-expected/(samples*255.0))<.00001,"GPU ambient uses the exact original row and column positions");
        check(memcmp(&before,&after,sizeof before)==0,"ambient sampling preserves host overlay GL state");
        glPixelStorei(GL_PACK_SKIP_ROWS,5);glPixelStorei(GL_PACK_SKIP_PIXELS,7);
        uint8_t pixel[4];
        check(psx_presentation_gpu_read_rgba(pixel,sizeof pixel,20,20,1,1),"bounded export isolates inherited pack skips");
        GLint skip_rows,skip_pixels;glGetIntegerv(GL_PACK_SKIP_ROWS,&skip_rows);glGetIntegerv(GL_PACK_SKIP_PIXELS,&skip_pixels);
        check(skip_rows==5 && skip_pixels==7,"bounded export restores pack skips");
        glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
        setenv("PSX_RENDER_PASS_FRESH_TARGET","1",1);
        transaction_depth_restore(scale);
        unsetenv("PSX_RENDER_PASS_FRESH_TARGET");
        transaction_options=GL_PASS_COMPLETE_IMAGE;
        transaction_depth_restore(scale);
        transaction_options=0;
        pending_capture(scale);
        gl_renderer_shutdown();
    }
    printf("checks=%d failures=%d\n",checks,failures);
    SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
