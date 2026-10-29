/* Cropping a render target cannot change PGXP rasterization or attributes. */
#define main legacy_fixture_main
#include "test_gl_render_thread.c"
#undef main
static void scene(int x,int y,int textured) {
    glb_wide_set_target(x);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,g_wide_cur);
    glDisable(GL_SCISSOR_TEST);glStencilMask(255);glDepthMask(GL_TRUE);
    glClearColor(0,0,0,0);glClearDepth(1);glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    gr_set_draw_area(x,y,x+319,y+239);gr_set_draw_offset(0,0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_hr_fbo);
    glDisable(GL_SCISSOR_TEST);glStencilMask(255);glDepthMask(GL_TRUE);
    glClearColor(0,0,0,0);glClearDepth(1);glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    DEPTH_REQUEST_CLEAR(DCLR_AREA);
    uint32_t seed=1234567;
    for(unsigned i=0;i<2000;++i) {
        seed=seed*1664525u+1013904223u;int px=x+seed%290;
        seed=seed*1664525u+1013904223u;int py=y+seed%210;
        gr_set_precise_triangle(1,px*65536+2345,py*65536+12345,
            (px+19)*65536+12345,(py+7)*65536+2345,
            (px+3)*65536+54321,(py+19)*65536+43210);
        gr_set_depth_triangle(1,1047,2531,4516);
        gr_set_perspective_triangle(1,1024.f/1047,1024.f/2531,1024.f/4516);
        if(textured)gr_draw_shaded_textured_triangle(px,py,1,1,0x000080,
            px+19,py+7,250,5,0x008000,px+3,py+19,10,60,0x800000,0,0,0x11a,0);
        else gr_draw_gouraud_triangle(px,py,0x001f,px+19,py+7,0x03e0,px+3,py+19,0x7c00);
    }
    flush_flat_batch();flush_tex_batch();hiw_flush_queue();
    gr_fill_rect(x+100,y+100,16,16,0x001f);
    uint16_t patch[8*8];for(unsigned i=0;i<64;++i)patch[i]=(i&1)?0xffff:0x03e0;
    gr_vram_transfer_in(x+10,y+10,8,8,patch);
    gr_copy_rect(x+10,y+10,x+40,y+40,8,8);
    flush_cpu_upload();
}
static int run_scale(int scale) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Cropped target hidden test",0,0,320,240,
        SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 77;
    gr_set_backend(GR_BACKEND_OPENGL);gr_init(vram);gr_set_scale(scale);
    if(!gl_renderer_init_context(win))return 77;
    gl_renderer_set_pgxp_depth(1);gl_renderer_set_pgxp_color_perspective(1);
    gl_renderer_set_seam_snap(1);s_pgxp_seam=1;
    gr_set_color_modulation(128,128,128,1);
    glb_wide_configure(426,53);s_wide_fast=0;
    uint16_t texels[256*64];for(unsigned i=0;i<256*64;++i)texels[i]=0x8000|((i*37)&32767);
    gr_vram_transfer_in(640,256,256,64,texels);flush_cpu_upload();
    GLuint full_fbo=s_hr_fbo,full_tex=s_hr_tex,full_rb=s_hr_rb;
    GLuint tex=make_tex(GL_RGBA8,320*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE),rb,fbo;
    p_glGenRenderbuffers(1,&rb);p_glBindRenderbuffer(PSXGL_RENDERBUFFER,rb);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER,PSXGL_DEPTH24_STENCIL8,320*scale,240*scale);
    check(make_fbo(&fbo,tex,rb),"cropped target allocation");
    size_t bytes=(size_t)320*240*scale*scale*4;
    uint8_t *reference=malloc(bytes),*cropped=malloc(bytes);
    size_t wide_bytes=(size_t)426*240*scale*scale*4;
    uint8_t *wide_reference=malloc(wide_bytes),*wide_cropped=malloc(wide_bytes);
    for(int kind=0;kind<2;++kind)for(int page=0;page<3;++page) {
        int x=page==2?320:0,y=page==1?240:0;
        s_hr_fbo=full_fbo;s_hr_tex=full_tex;s_hr_rb=full_rb;
        s_hr_origin_x=s_hr_origin_y=0;s_hr_width=1024;s_hr_height=512;
        scene(x,y,kind);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,g_wide_cur);
        glReadPixels(0,y*scale,426*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,wide_reference);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_hr_fbo);
        glReadPixels(x*scale,y*scale,320*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        s_hr_fbo=fbo;s_hr_tex=tex;s_hr_rb=rb;
        s_hr_origin_x=x;s_hr_origin_y=y;s_hr_width=320;s_hr_height=240;
        scene(x,y,kind);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,g_wide_cur);
        glReadPixels(0,y*scale,426*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,wide_cropped);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_hr_fbo);
        glReadPixels(0,0,320*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        size_t changed=0;for(size_t i=0;i<bytes;i+=4)changed+=memcmp(reference+i,cropped+i,4)!=0;
        printf("scale=%d kind=%d page=%d changed=%zu\n",scale,kind,page,changed);
        check(changed==0,"cropped raster equals full-surface rectangle");
        check(!memcmp(wide_reference,wide_cropped,wide_bytes),"wide projection independent of previous target height");
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,full_fbo);
        glReadPixels(x*scale,y*scale,320*scale,240*scale,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,reference);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(0,0,320*scale,240*scale,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,cropped);
        check(!memcmp(reference,cropped,bytes),"cropped depth equals full-surface rectangle");
    }
    s_hr_fbo=full_fbo;s_hr_tex=full_tex;s_hr_rb=full_rb;
    s_hr_origin_x=s_hr_origin_y=0;s_hr_width=1024;s_hr_height=512;
    check(glGetError()==GL_NO_ERROR,"cropped GL errors");
    p_glDeleteFramebuffers(1,&fbo);p_glDeleteRenderbuffers(1,&rb);glDeleteTextures(1,&tex);
    free(reference);free(cropped);free(wide_reference);free(wide_cropped);
    gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();
    return failures?1:0;
}
int main(void) {
    const int scales[]={1,4,9};
    for(unsigned i=0;i<3;++i){int rc=run_scale(scales[i]);if(rc)return rc;}
    return 0;
}
