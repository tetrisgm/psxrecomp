/* PGXP draw translation must not change the display-local raster. */
#define main render_thread_fixture_main
#include "test_gl_render_thread.c"
#undef main

static int run_scale(int scale) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win = SDL_CreateWindow("PGXP translation hidden test", 0, 0, 641, 479,
                                      SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) return 77;
    gr_set_backend(GR_BACKEND_OPENGL); gr_init(vram); gr_set_scale(scale);
    if (!gl_renderer_init_context(win)) return 77;
    check(s_raster_ok,"GL shaders compiled");
    GLint gsverts=0;p_glGetProgramiv(s_tex_prog,0x8916,&gsverts);printf("geometry vertices=%d\n",gsverts);check(gsverts==3,"local interpolation stage linked");
    s_pgxp_seam=1;
    gl_renderer_set_pgxp_depth(1); gl_renderer_set_pgxp_color_perspective(1);
    gl_renderer_set_seam_snap(1);
    uint16_t texels[256*64];
    for(int t=0;t<256*64;t++) texels[t]=(uint16_t)(0x8000|((t*37)&32767));
    for(int row=0;row<64;row++)memcpy(vram+(256+row)*1024+640,texels+row*256,256*2);
    gr_vram_transfer_in(640,256,256,64,texels);
    gr_set_color_modulation(128,128,128,1);
    const int size=320*240*scale*scale;
    uint32_t *a=malloc(size*4),*b=malloc(size*4);
    for(int kind=0;kind<4;kind++) {
      for(int page=0;page<3;page++) {
        int y=page==1?240:0, xo=page==2?320:0;
        gr_set_draw_area(xo,y,xo+319,y+239); gr_set_draw_offset(0,0);
        gr_fill_rect(xo,y,320,240,0);
        uint32_t seed=0x12345678;
        for(int prim=0;prim<2000;prim++) {
          seed=seed*1664525u+1013904223u; int x0=xo+seed%300;
          seed=seed*1664525u+1013904223u; int y0=seed%220;
          int x1=x0+19,y1=y0+7,x2=x0+3,y2=y0+19;
          gr_set_precise_triangle(1,x0*65536+2345,(y+y0)*65536+12345,
            x1*65536+12345,(y+y1)*65536+2345,x2*65536+54321,(y+y2)*65536+43210);
          gr_set_depth_triangle(1,1047,2531,4516);
          gr_set_perspective_triangle(1,1024.f/1047,1024.f/2531,1024.f/4516);
          if(kind==0) gr_draw_flat_triangle(x0,y+y0,x1,y+y1,x2,y+y2,0x7fff);
          else if(kind==1) gr_draw_gouraud_triangle(x0,y+y0,0x001f,x1,y+y1,0x03e0,x2,y+y2,0x7c00);
          else if(kind==2) gr_draw_textured_triangle(x0,y+y0,1,1,x1,y+y1,250,5,x2,y+y2,10,60,0,0,0x11a);
          else gr_draw_shaded_textured_triangle(x0,y+y0,1,1,0x000080,x1,y+y1,250,5,0x008000,x2,y+y2,10,60,0x800000,0,0,0x11a,0);
        }
        int w,h;
        check(gl_renderer_read_display_hires(xo,y,320,240,page?b:a,size,&w,&h)==size,"capture");
        if(page){int changed=0;for(int k=0;k<size;k++)changed+=a[k]!=b[k];printf("page=%d changed=%d\n",page,changed);check(!changed,"translated page matches reference");}
      }
      int diff=0;for(int i=0;i<size;i++) diff+=a[i]!=b[i];
      printf("scale=%d kind=%d changed=%d\n",scale,kind,diff);
      check(!diff,"display page translation invariance");
    }
    /* The same rendered pages must resolve identically at a fractional output
     * ratio too. This catches absolute normalized-UV sampling roundoff. */
    uint8_t *pa=malloc(641*479*3),*pb=malloc(641*479*3);
    for(int page=0;page<2;page++) {
        glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);
        present_target_quad(s_hr_tex,1024,512,0,page*240,320,240,1,0,0,641,479,1,0,scale);
        glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,641,479,GL_RGB,GL_UNSIGNED_BYTE,page?pb:pa);
    }
    check(!memcmp(pa,pb,641*479*3),"fractional presentation origin invariance");
    free(pa);free(pb);
    /* Perspective colour has an independent analytical reference. A white
     * texture upload exercises the upload boundary without losing PGXP state. */
    for(int t=0;t<256*64;t++) texels[t]=0xffff;
    for(int row=0;row<64;row++)memcpy(vram+(256+row)*1024+640,texels+row*256,256*2);
    gr_vram_transfer_in(640,256,256,64,texels);
    flush_cpu_upload();
    uint16_t raw_check=0;p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_raw_fbo);
    glReadPixels(640,256,1,1,PSXGL_RED_INTEGER,GL_UNSIGNED_SHORT,&raw_check);
    printf("uploaded white=%04x cpu=%04x\n",raw_check,vram[256*1024+640]);
    check(raw_check==0xffff,"texture upload reaches raw VRAM");
    gr_set_draw_area(0,0,319,239);
    for(int textured=0;textured<2;textured++) {
        gr_fill_rect(0,0,320,240,0);
        DEPTH_REQUEST_CLEAR(DCLR_AREA);
        gr_set_precise_triangle(1,20*65536,20*65536,100*65536,20*65536,20*65536,100*65536);
        gr_set_depth_triangle(1,1024,2048,4096);
        gr_set_perspective_triangle(1,1,.5f,.25f);
        if(textured) gr_draw_shaded_textured_triangle(20,20,1,1,0x000080,100,20,250,5,0x008000,
            20,100,10,60,0x800000,0,0,0x11a,0);
        else gr_draw_gouraud_triangle(20,20,0x001f,100,20,0x03e0,20,100,0x7c00);
        int w,h;check(gl_renderer_read_display_hires(0,0,320,240,a,size,&w,&h)==size,"colour capture");
        int px=40*scale,py=40*scale;uint32_t rgb=a[py*w+px];
        float x=(px+.5f)/scale-s_shift_hr,y=(py+.5f)/scale-s_shift_hr;
        float b1=(x-20)/80,b2=(y-20)/80,b0=1-b1-b2,q=b0+b1*.5f+b2*.25f;
        float gain=textured?(s_accurate_blend?248.0f:255.0f)*256.0f/255.0f:248.0f;
        int red=(int)lrintf(gain*b0/q),green=(int)lrintf(gain*b1*.5f/q),blue=(int)lrintf(gain*b2*.25f/q);
        printf("scale=%d textured=%d colour=%06x expected=%d,%d,%d\n",scale,textured,rgb&0xffffff,red,green,blue);
        check(abs((int)((rgb>>16)&255)-red)<=3 && abs((int)((rgb>>8)&255)-green)<=3 &&
              abs((int)(rgb&255)-blue)<=3,"analytical perspective colour");
    }
    /* A U gradient distinguishes perspective texture lookup from affine
     * lookup; a white texture alone cannot test reciprocal UV weights. */
    for(int row=0;row<64;row++)for(int u=0;u<256;u++)
        texels[row*256+u]=(uint16_t)(0x8000|(u/8));
    for(int row=0;row<64;row++)memcpy(vram+(256+row)*1024+640,texels+row*256,256*2);
    gr_vram_transfer_in(640,256,256,64,texels);
    gr_fill_rect(0,0,320,240,0);DEPTH_REQUEST_CLEAR(DCLR_AREA);
    gr_set_precise_triangle(1,20*65536,20*65536,100*65536,20*65536,20*65536,100*65536);
    gr_set_depth_triangle(1,1024,2048,4096);gr_set_perspective_triangle(1,1,.5f,.25f);
    gr_draw_textured_triangle(20,20,1,1,100,20,250,5,20,100,10,60,0,0,0x11a);
    {
        int w,h;check(gl_renderer_read_display_hires(0,0,320,240,a,size,&w,&h)==size,"UV capture");
        uint32_t rgb=a[40*scale*w+40*scale];
        int expected=s_accurate_blend?40:41;
        check(abs((int)((rgb>>16)&255)-expected)<=1 && !(rgb&0xffff),"analytical perspective UV");
        printf("scale=%d perspective_U_red=%u expected=%d\n",scale,(rgb>>16)&255,expected);
    }
    /* Reject invalid host inputs before they can poison reciprocal interpolation. */
    gr_set_perspective_triangle(1,NAN,1,1);check(!s_pq_valid,"NaN w rejected");
    gr_set_perspective_triangle(1,1,INFINITY,1);check(!s_pq_valid,"infinite w rejected");
    gr_set_depth_triangle(1,1,1,0);check(!s_pz_valid,"zero depth rejected");
    gr_set_perspective_triangle(1,1,.5f,.25f);check(s_pq_valid,"valid perspective restored");
    gr_set_depth_triangle(1,1024,2048,4096);check(s_pz_valid,"valid depth restored");
    /* Controlled epsilon probe: the second surface is just behind the first,
     * then well behind it. Only the first trial should count close_depth failures. */
    gr_set_draw_area(0,0,319,239);flush_flat_batch();flush_tex_batch();
    for(int close_depth=0;close_depth<2;close_depth++) {
        float v[18]={10,10,1,0,0,60000, 80,10,0,1,0,60000, 10,80,0,0,1,60000};
        hr_begin(1);p_glUseProgram(s_geo_prog);p_glBindVertexArray(s_geo_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER,s_geo_vbo);
        p_glUniform1f(s_geo_uZbias,0);p_glUniform1f(s_geo_uDepthProbe,0);
        glDisable(GL_STENCIL_TEST);glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);
        glDepthFunc(GL_ALWAYS);glClearDepth(1);glClear(GL_DEPTH_BUFFER_BIT);
        vertex_stream_upload(0,v,sizeof v);
        glDrawArrays(GL_TRIANGLES,0,3);
        for(int i=0;i<3;i++) v[i*6+5]+=close_depth?2.0f:200.0f;
        vertex_stream_upload(0,v,sizeof v);
        s_ra_near=s_ra_failed=0;s_ra_depth_probe=1;
        depth_restore_ex(1,0,3);s_ra_depth_probe=0;
        check(s_ra_failed>0,"depth probe finds hidden fragments");
        check(close_depth?s_ra_near>0:s_ra_near==0,"depth probe epsilon classification");
        printf("scale=%d close_depth=%d failed=%llu epsilon=%llu\n",scale,close_depth,
          (unsigned long long)s_ra_failed,(unsigned long long)s_ra_near);
        hr_end();p_glBindVertexArray(0);
    }
    /* Depth clears must retain neighbouring draw domains, colour and mask.
     * Read depth only on this fixture's GL owner. */
    flush_flat_batch();flush_tex_batch();hiw_flush_queue();
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_hr_fbo);
    glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);
    glClearDepth(.25);glClear(GL_DEPTH_BUFFER_BIT);
    gr_set_draw_area(20,30,39,49);
    depth_clear_now();
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_hr_fbo);
    float depths[3]={0};
    glReadPixels(19*scale,35*scale,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depths[0]);
    glReadPixels(25*scale,35*scale,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depths[1]);
    glReadPixels(40*scale,35*scale,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depths[2]);
    check(fabsf(depths[0]-.25f)<.00001f && depths[1]==1.f &&
          fabsf(depths[2]-.25f)<.00001f,"scissored depth clear preserves neighbouring domains");
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);
    check(glGetError()==GL_NO_ERROR,"GL audit has no errors");
    free(a);free(b);gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();
    return failures?1:0;
}

int main(void) {
    const char *requested=getenv("PGXP_AUDIT_SCALE");
    if(requested && atoi(requested)>0)return run_scale(atoi(requested));
    const int scales[]={1,4,9};
    for(unsigned i=0;i<sizeof(scales)/sizeof(scales[0]);i++) {
        int rc=run_scale(scales[i]);if(rc)return rc;
    }
    return 0;
}
