/* Completed wide images and observable margin depth/masks match full replay. */
#define main readback_fixture_main
#include "test_gl_readback_region.c"
#undef main

static uint64_t wm_hash(GLuint fbo,int x,int y,int w,int h,int attachments) {
    size_t n=(size_t)w*h;uint32_t *pixels=malloc(n*4);
    if(!pixels){check(0,"hash allocation");return 0;}
    uint64_t hash=1469598103934665603ull;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,fbo);
    const GLenum formats[3]={GL_RGBA,GL_DEPTH_COMPONENT,GL_STENCIL_INDEX};
    for(int a=0;a<attachments;++a) {
        glReadPixels(x,y,w,h,formats[a],a==1?GL_FLOAT:GL_UNSIGNED_BYTE,pixels);
        for(size_t i=0;i<n*(a==2?1:4);++i)hash=(hash^((uint8_t *)pixels)[i])*1099511628211ull;
    }
    free(pixels);return hash;
}
static void wm_scene(int y,int variant) {
    glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
    glb_set_draw_area(0,y,319,y+239);
    glb_wide_set_view(variant==1,variant==1?32:0,0,0);
    test_full_composite=variant==2;
    glb_fill_rect(0,y,320,240,0x0421);glb_wide_clear(0,y,240,0x0421);
    flush_flat_batch();flush_tex_batch();flush_cpu_upload();hiw_flush_queue();
    GLuint targets[2]={s_hr_fbo,g_wide_cur};
    for(int k=0;k<2;++k) {
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,targets[k]);
        glEnable(GL_SCISSOR_TEST);glScissor(0,y*s_hr_scale,(k?560:1024)*s_hr_scale,240*s_hr_scale);
        glDepthMask(GL_TRUE);glStencilMask(0xff);glClearDepth(1);glClearStencil(0);
        glClear(GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    }
    s_depth_need_clear=0;s_depth_used=0;s_depth_last_avg=0;
    for(int i=0;i<4;++i) {
        int x0=-110+i*9,x1=430-i*7,y0=y+7+i*11,y1=y+220-i*5;
        glb_set_mask_bits(variant==3 && i==0,variant==3 && i>0);
        glb_set_semi_transparency(i>0,i);
        glb_set_precise_triangle(1,x0*65536,y0*65536,x1*65536,y0*65536,160*65536,y1*65536);
        glb_set_depth_triangle(1,1000,1200,1400);
        glb_set_perspective_triangle(1,1.f/1000,1.f/1200,1.f/1400);
        glb_draw_shaded_textured_triangle(x0,y0,0,0,0x808080,x1,y0,15,0,0x808080,
            160,y1,7,15,0x808080,0,0,0x108,1);
    }
    glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
    glb_set_precise_triangle(1,-100*65536,(y+190)*65536,420*65536,(y+190)*65536,160*65536,(y+230)*65536);
    glb_set_depth_triangle(1,900,1100,1300);glb_set_perspective_triangle(1,1.f/900,1.f/1100,1.f/1300);
    glb_draw_gouraud_triangle(-100,y+190,0x001f,420,y+190,0x03e0,160,y+230,0x7c00);
    flush_flat_batch();flush_tex_batch();
    if(s_hq_n)check(s_hq[s_hq_n-1].margins==(s_wide_margins_only && variant!=1 && variant!=2),
                   "queued clipping retains the source view/composite contract");
    hiw_flush_queue();wide_blit_center(g_wide_cur,0,y,240);
}
int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Wide margins",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 2;
    for(int si=0;si<3;++si) {
        int scale=si==0?1:si==1?4:9;
        glb_init(image);glb_set_scale(scale);if(!gl_renderer_init_context(win))return 2;
        gl_renderer_set_pgxp_depth(1);s_bcls_on=1;s_wide_fast=1;s_wide_queue=1;test_batch_hle=1;
        glb_wide_configure(560,120);glb_wide_set_target(0);
        uint16_t tex[256];for(int i=0;i<256;++i)tex[i]=i%7==0?0:(uint16_t)(0x1234|(i&1?0x8000:0));
        glb_vram_transfer_in(512,0,16,16,tex);flush_cpu_upload();
        for(int band=0;band<2;++band)for(int variant=0;variant<4;++variant) {
            uint64_t hashes[2][4];int y=band*240;
            for(int fast=0;fast<2;++fast) {
                s_wide_margins_only=fast;wm_scene(y,variant);
                hashes[fast][0]=wm_hash(s_hr_fbo,0,y*scale,320*scale,240*scale,3);
                hashes[fast][1]=wm_hash(g_wide_cur,0,y*scale,560*scale,240*scale,1);
                hashes[fast][2]=wm_hash(g_wide_cur,0,y*scale,120*scale,240*scale,3);
                hashes[fast][3]=wm_hash(g_wide_cur,440*scale,y*scale,120*scale,240*scale,3);
            }
            check(memcmp(hashes[0],hashes[1],sizeof hashes[0])==0,"canonical attachments, wide pixels and margin depth/masks match full replay");
        }
        check(glGetError()==GL_NO_ERROR,"margin replay has no GL errors");
        gl_renderer_shutdown();
    }
    SDL_DestroyWindow(win);SDL_Quit();printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
