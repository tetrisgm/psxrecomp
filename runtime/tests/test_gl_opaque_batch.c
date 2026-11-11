/* Opaque HLE groups must retain LLE pixels, PGXP depth and bloom coverage. */
#define main readback_fixture_main
#include "test_gl_readback_region.c"
#undef main
static uint64_t opaque_hash(void) {
    const int width=64*s_hr_scale,height=64*s_hr_scale;
    size_t n=(size_t)width*height;uint32_t *p=malloc(n*4);
    if(!p){check(0,"hash allocation");return 0;}
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_hr_fbo);
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,p);
    uint64_t h=1469598103934665603ull;
    for(size_t i=0;i<n*4;++i)h=(h^((uint8_t*)p)[i])*1099511628211ull;
    glReadPixels(0,0,width,height,GL_DEPTH_COMPONENT,GL_FLOAT,p);
    for(size_t i=0;i<n*4;++i)h=(h^((uint8_t*)p)[i])*1099511628211ull;
    glReadPixels(0,0,width,height,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,p);
    /* Bit zero may be deferred when mask checking is off. Bloom bit one
     * is observable immediately by postprocessing and must match exactly. */
    for(size_t i=0;i<n;++i)h=(h^(((uint8_t*)p)[i]&2))*1099511628211ull;
    free(p);return h;
}
static void opaque_scene(int tracked,int mixed,int blendmode) {
    glb_set_draw_area(0,0,63,63);glb_set_mask_bits(0,0);
    glb_set_semi_transparency(0,0);glb_fill_rect(0,0,64,64,0x421);
    flush_cpu_upload();flush_flat_batch();flush_tex_batch();
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_hr_fbo);
    glEnable(GL_SCISSOR_TEST);glScissor(0,0,64*s_hr_scale,64*s_hr_scale);
    glDepthMask(GL_TRUE);glStencilMask(0xff);glClearStencil(2);glClearDepth(1);
    glClear(GL_STENCIL_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    s_depth_need_clear=0;s_depth_used=0;
    for(int i=0;i<3;++i) {
        glb_set_semi_transparency(mixed && i==1,blendmode);
        glb_set_precise_triangle(tracked,(4+i*7)*65536,4*65536,
            58*65536,(5+i*5)*65536,8*65536,58*65536);
        glb_set_depth_triangle(tracked,1000,1100,1200);
        glb_set_perspective_triangle(tracked,1.f/1000,1.f/1100,1.f/1200);
        glb_draw_shaded_textured_triangle(4+i*7,4,0,0,0x808080,
            58,5+i*5,15,0,0x808080,8,58,0,15,0x808080,0,0,0x108,1);
    }
    flush_tex_batch();flush_flat_batch();hiw_flush_queue();
}
int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Opaque grouping",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 2;
    for(int si=0;si<3;++si) {
        int scale=si==0?1:si==1?4:9;
        glb_init(image);glb_set_scale(scale);if(!gl_renderer_init_context(win))return 2;
        uint16_t tex[256];for(int i=0;i<256;++i)tex[i]=i%7==0?0:(uint16_t)(0x1234|(i&1?0x8000:0));
        glb_vram_transfer_in(512,0,16,16,tex);flush_cpu_upload();
        gl_renderer_set_pgxp_depth(1);s_bcls_on=1;
        for(int tracked=0;tracked<2;++tracked) {
            test_batch_hle=0;opaque_scene(tracked,0,0);uint64_t ref=opaque_hash();
            test_batch_hle=1;uint64_t fused=s_bcls_fused_draws;
            opaque_scene(tracked,0,0);
            check(opaque_hash()==ref,"opaque HLE preserves LLE color/depth/bloom");
            check(s_bcls_fused_draws>fused,"opaque HLE uses fused color/bloom");
        }
        /* Mixed 3D draws still use ordered dual-source blending. */
        test_batch_hle=1;s_opaque_batch_fast=0;opaque_scene(1,1,0);uint64_t ref=opaque_hash();
        s_opaque_batch_fast=1;opaque_scene(1,1,0);
        check(opaque_hash()==ref,"mixed 3D HLE is unchanged by opaque grouping");
        for(int tracked=0;tracked<2;++tracked) for(int mode=0;mode<4;++mode) {
            s_opaque_batch_fast=0;opaque_scene(tracked,1,mode);ref=opaque_hash();
            /* Subtractive blending splits the opaque runs. Their 2D bloom
             * coverage must clear the background, matching LLE; the former
             * HLE key-4 path incorrectly left that background marked. */
            if(!tracked && mode==2) {
                test_batch_hle=0;opaque_scene(tracked,1,mode);ref=opaque_hash();
                test_batch_hle=1;
            }
            s_opaque_batch_fast=1;opaque_scene(tracked,1,mode);
            char label[128];snprintf(label,sizeof label,"mixed coverage: precise=%d mode=%d",tracked,mode);
            check(opaque_hash()==ref,label);
        }
        check(glGetError()==GL_NO_ERROR,"opaque fixture has no GL errors");
        gl_renderer_shutdown();
    }
    SDL_DestroyWindow(win);SDL_Quit();printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
