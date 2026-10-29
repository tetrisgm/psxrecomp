/* Cropping a render target cannot change PGXP rasterization or attributes. */
#define main legacy_fixture_main
#include "test_gl_render_thread.c"
#undef main
static GLuint direct_scene_fbo,direct_scene_tex,direct_scene_rb;
static int scene_backdrop;
static void scene(int x,int y,int textured) {
    g_prim_id=scene_backdrop?0:1; /* Exercise both ordinary and authored backdrop geometry. */
    glb_wide_set_target(x);
    if(direct_scene_fbo) {
        g_wide_cur=s_hr_fbo=direct_scene_fbo;
        s_hr_tex=direct_scene_tex;s_hr_rb=direct_scene_rb;
        s_hr_origin_x=x-53;s_hr_origin_y=y;s_hr_width=426;s_hr_height=240;
    }
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
    if(!scene_backdrop) {
    gr_fill_rect(x+100,y+100,16,16,0x001f);
    uint16_t patch[8*8];for(unsigned i=0;i<64;++i)patch[i]=(i&1)?0xffff:0x03e0;
    gr_vram_transfer_in(x+10,y+10,8,8,patch);
    gr_copy_rect(x+10,y+10,x+40,y+40,8,8);
    flush_cpu_upload();
    }
    gr_set_precise_triangle(0,0,0,0,0,0,0);
    gr_set_depth_triangle(0,0,0,0);
    gr_set_perspective_triangle(0,0,0,0);
    gr_draw_line(x+3,y+230,x+280,y+230,0x001f);
    gr_draw_shaded_line(x+12,y+225,0x001f,x+290,y+229,0x03e0);
    gr_set_semi_transparency(1,0);
    gr_draw_flat_rect(x,y,320,240,0x4210);
    flush_flat_batch();flush_tex_batch();hiw_flush_queue();
    gr_set_semi_transparency(0,0);
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
    GLuint tex,rb,fbo,direct_tex,direct_rb,direct_fbo;
    check(raster_surface_alloc(320,240,scale,&tex,&rb,&fbo),"cropped target allocation");
    check(raster_surface_alloc(426,240,scale,&direct_tex,&direct_rb,&direct_fbo),"direct wide target allocation");
    glBindTexture(GL_TEXTURE_2D,direct_tex);
    GLint allocated_w=0,allocated_h=0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&allocated_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&allocated_h);
    check(allocated_w==426*scale && allocated_h==240*scale,"direct allocation contains only output pixels");
    GLuint invalid_tex=7,invalid_rb=7,invalid_fbo=7;
    check(!raster_surface_alloc(INT_MAX,240,2,&invalid_tex,&invalid_rb,&invalid_fbo) &&
        !invalid_tex && !invalid_rb && !invalid_fbo,"oversized region refuses without partial allocation");
    GLuint tile_tex[2],tile_fbo[2];
    for(int i=0;i<2;++i) {
        tile_tex[i]=make_tex(GL_RGBA8,160*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE);
        check(make_fbo(&tile_fbo[i],tile_tex[i],0),"independent RT copy tile allocation");
    }
    size_t bytes=(size_t)320*240*scale*scale*4;
    uint8_t *reference=malloc(bytes),*cropped=malloc(bytes);
    size_t wide_bytes=(size_t)426*240*scale*scale*4;
    uint8_t *wide_reference=malloc(wide_bytes),*wide_cropped=malloc(wide_bytes);
    uint8_t *wide_page_zero=malloc(wide_bytes);
    for(int kind=0;kind<4;++kind)for(int page=0;page<3;++page) {
        scene_backdrop=kind>=2;
        int x=page==2?320:0,y=page==1?240:0;
        s_hr_fbo=full_fbo;s_hr_tex=full_tex;s_hr_rb=full_rb;
        s_hr_origin_x=s_hr_origin_y=0;s_hr_width=1024;s_hr_height=512;
        scene(x,y,kind&1);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,g_wide_cur);
        glReadPixels(0,y*scale,426*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,wide_reference);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_hr_fbo);
        glReadPixels(x*scale,y*scale,320*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        s_hr_fbo=fbo;s_hr_tex=tex;s_hr_rb=rb;
        s_hr_origin_x=x;s_hr_origin_y=y;s_hr_width=320;s_hr_height=240;
        scene(x,y,kind&1);
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
        if(!scene_backdrop) {
            s_wide_fast=1;
            wide_blit_center(g_wide_cur,x,y,240);
            s_wide_fast=0;
        }
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,g_wide_cur);
        glReadPixels(0,y*scale,426*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,wide_reference);
        GLuint old_wide=g_wide_cur;

        direct_scene_fbo=direct_fbo;direct_scene_tex=direct_tex;direct_scene_rb=direct_rb;
        scene(x,y,kind&1);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,direct_fbo);
        glReadPixels(0,0,426*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,wide_cropped);
        size_t wide_changed=0;
        for(size_t i=0;i<wide_bytes;i+=4)wide_changed+=memcmp(wide_reference+i,wide_cropped+i,4)!=0;
        printf("direct-wide scale=%d kind=%d page=%d changed=%zu\n",scale,kind,page,wide_changed);

        if(wide_changed && wide_changed<32)for(size_t i=0;i<wide_bytes;i+=4)if(memcmp(wide_reference+i,wide_cropped+i,4))
            printf("diff %zu,%zu: %02x%02x%02x%02x / %02x%02x%02x%02x\n",(i/4)%(426*scale),(i/4)/(426*scale),
                wide_reference[i],wide_reference[i+1],wide_reference[i+2],wide_reference[i+3],
                wide_cropped[i],wide_cropped[i+1],wide_cropped[i+2],wide_cropped[i+3]);
        /* Fixed-function Gouraud interpolation may round an 8-bit colour by
         * one code when the viewport changes width. Texture attributes are
         * explicitly triangle-local and remain byte exact. Never tolerate
         * an alpha/mask difference, a lost edge, or page-dependent output. */
        unsigned max_colour_error=0;int alpha_equal=1;
        for(size_t i=0;i<wide_bytes;i++) {
            unsigned delta=(unsigned)abs((int)wide_reference[i]-(int)wide_cropped[i]);
            if(i%4==3)alpha_equal&=delta==0;
            else if(delta>max_colour_error)max_colour_error=delta;
        }
        check(alpha_equal && max_colour_error<=((kind&1)?0u:1u),
            "direct wide matches composed coverage, mask and colour precision");
        if(page==0)memcpy(wide_page_zero,wide_cropped,wide_bytes);
        else check(!memcmp(wide_page_zero,wide_cropped,wide_bytes),
            "direct wide output is byte-identical across framebuffer pages");
        direct_scene_fbo=0;g_wide_cur=old_wide;
        s_hr_fbo=fbo;s_hr_tex=tex;s_hr_rb=rb;
        s_hr_origin_x=x;s_hr_origin_y=y;s_hr_width=320;s_hr_height=240;
        /* Independent source and destination allocations must retain enhanced
         * subpixel colour, including mask checks, without native reseeding. */
        for(int mask=0;mask<4;++mask) {
            gr_set_mask_bits(mask&1,mask>>1);
            s_hr_fbo=full_fbo;s_hr_tex=full_tex;s_hr_rb=full_rb;
            s_hr_origin_x=s_hr_origin_y=0;s_hr_width=1024;s_hr_height=512;
            gpu_copy_rect(x+10,y+10,x+150,y+150,32,32);
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,full_fbo);
            glReadPixels((x+150)*scale,(y+150)*scale,32*scale,32*scale,
                GL_RGBA,GL_UNSIGNED_BYTE,reference);
            s_hr_fbo=fbo;s_hr_tex=tex;s_hr_rb=rb;
            s_hr_origin_x=x;s_hr_origin_y=y;s_hr_width=320;s_hr_height=240;
            gpu_copy_rect_from(full_fbo,0,0,x+10,y+10,x+150,y+150,32,32);
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
            glReadPixels(150*scale,150*scale,32*scale,32*scale,
                GL_RGBA,GL_UNSIGNED_BYTE,cropped);
            check(!memcmp(reference,cropped,(size_t)32*32*scale*scale*4),
                "cross-target enhanced copy preserves mask semantics");
            s_hr_fbo=full_fbo;s_hr_tex=full_tex;s_hr_rb=full_rb;
            s_hr_origin_x=s_hr_origin_y=0;s_hr_width=1024;s_hr_height=512;
            gpu_copy_rect(x+10,y+10,x+200,y+100,32,32);
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,full_fbo);
            glReadPixels((x+200)*scale,(y+100)*scale,32*scale,32*scale,
                GL_RGBA,GL_UNSIGNED_BYTE,reference);
            gpu_copy_rect_from(fbo,x,y,x+10,y+10,x+200,y+100,32,32);
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,full_fbo);
            glReadPixels((x+200)*scale,(y+100)*scale,32*scale,32*scale,
                GL_RGBA,GL_UNSIGNED_BYTE,cropped);
            check(!memcmp(reference,cropped,(size_t)32*32*scale*scale*4),
                "cross-target copy translates cropped source origin");
        }
        gr_set_mask_bits(0,0);
        /* A source straddling two enhanced allocations is gathered before
         * the destination write; no downsample to native pixels is allowed. */
        for(int i=0;i<2;++i) {
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,full_fbo);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,tile_fbo[i]);
            glDisable(GL_SCISSOR_TEST);
            p_glBlitFramebuffer((x+i*160)*scale,y*scale,(x+(i+1)*160)*scale,(y+240)*scale,
                0,0,160*scale,240*scale,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        }
        const GlCopyRegion tiles[2]={{tile_fbo[0],x,y,160,240,scale},
                                     {tile_fbo[1],x+160,y,160,240,scale}};
        s_hr_fbo=full_fbo;s_hr_tex=full_tex;s_hr_rb=full_rb;
        s_hr_origin_x=s_hr_origin_y=0;s_hr_width=1024;s_hr_height=512;
        gpu_copy_rect(x+144,y+60,x+100,y+180,32,16);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,full_fbo);
        glReadPixels((x+100)*scale,(y+180)*scale,32*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        s_hr_fbo=fbo;s_hr_tex=tex;s_hr_rb=rb;
        s_hr_origin_x=x;s_hr_origin_y=y;s_hr_width=320;s_hr_height=240;
        gpu_copy_rect_regions(tiles,2,x+144,y+60,x+100,y+180,32,16);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(100*scale,180*scale,32*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        check(!memcmp(reference,cropped,(size_t)32*16*scale*scale*4),
            "copy gathers adjacent enhanced RT tiles without losing subpixel colour");
        /* Canonical pixels in a wide allocation start after its reveal
         * margin. The margin must never masquerade as neighbouring VRAM. */
        const GlCopyRegion layered[2]={{full_fbo,0,0,1024,512,scale,0,0},
            {direct_fbo,x,y,320,240,scale,53,0}};
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,full_fbo);
        glReadPixels((x+312)*scale,(y+60)*scale,16*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,direct_fbo);
        glReadPixels((53+312)*scale,60*scale,8*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        for(int row=0;row<16*scale;++row)
            memcpy(reference+(size_t)row*16*scale*4,cropped+(size_t)row*8*scale*4,(size_t)8*scale*4);
        gpu_copy_rect_regions(layered,2,x+312,y+60,x+100,y+150,16,16);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(100*scale,150*scale,16*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        check(!memcmp(reference,cropped,(size_t)16*16*scale*scale*4),
            "enhanced copy excludes wide reveal margins from guest VRAM coverage");
        /* No enhanced allocation exists beyond the canonical wide region.
         * Seed that portion from native, then overlay the retained enhanced
         * pixels. Neither the reveal margin nor an unrelated framebuffer is
         * a valid source for the missing neighbouring region. */
        uint16_t native_before[16*16];
        for(int row=0;row<16;++row)for(int col=0;col<16;++col) {
            const size_t pos=(size_t)(y+60+row)*1024+x+312+col;
            native_before[row*16+col]=s_vram[pos];s_vram[pos]=0x801f;
        }
        for(int row=0;row<16*scale;++row)for(int col=8*scale;col<16*scale;++col) {
            uint8_t *pixel=reference+((size_t)row*16*scale+col)*4;
            pixel[0]=248;pixel[1]=pixel[2]=0;pixel[3]=255;
        }
        gpu_copy_rect_regions_seeded(layered+1,1,1,x+312,y+60,x+100,y+150,16,16);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(100*scale,150*scale,16*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        check(!memcmp(reference,cropped,(size_t)16*16*scale*scale*4),
            "copy seeds missing regions from native and preserves enhanced subpixels");
        /* The gathered source is immutable across destination writes. A
         * surface-pool copy can therefore apply one staged piece to several
         * targets without sampling a destination that was already changed. */
        gpu_copy_rect_apply(s_scratch_tex,x+312,y+60,x+200,y+170,16,16);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(200*scale,170*scale,16*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        check(!memcmp(reference,cropped,(size_t)16*16*scale*scale*4),
            "gathered enhanced source survives separate destination application");
        for(int row=0;row<16;++row)for(int col=0;col<16;++col)
            s_vram[(size_t)(y+60+row)*1024+x+312+col]=native_before[row*16+col];
        native_texture_upload_rect(x+312,y+60,16,16);
        /* A full native copy into a cropped destination only stages the
         * intersection. Both axes have a nonzero origin on the second page. */
        int old_scratch_w=s_scratch_w,old_scratch_h=s_scratch_h;
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,full_fbo);
        glReadPixels(x*scale,y*scale,320*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        gpu_copy_rect_from(full_fbo,0,0,0,0,0,0,1024,512);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(0,0,320*scale,240*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        check(!memcmp(reference,cropped,(size_t)320*240*scale*scale*4),
            "copy clipping translates source and destination together");
        check(s_scratch_w<=(old_scratch_w>320*scale?old_scratch_w:320*scale) &&
              s_scratch_h<=(old_scratch_h>240*scale?old_scratch_h:240*scale),
            "cropped destination never stages full scaled VRAM");
        /* Display extraction consumes only its requested rows, and translates
         * a cropped source without touching another framebuffer band. */
        s_hr_fbo=fbo;s_hr_tex=tex;s_hr_rb=rb;
        s_hr_origin_x=x;s_hr_origin_y=y;s_hr_width=320;s_hr_height=240;
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(0,20*scale,320*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,g_wide_cur);
        glDisable(GL_SCISSOR_TEST);glClearColor(1,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
        s_wide_fast=1;
        wide_blit_center(g_wide_cur,x,y+20,16);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,g_wide_cur);
        glReadPixels(53*scale,(y+20)*scale,320*scale,16*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        check(!memcmp(reference,cropped,(size_t)320*16*scale*scale*4),
            "display band extracts cropped target exactly");
        uint8_t outside[4];
        glReadPixels(54*scale,(y+19)*scale,1,1,GL_RGBA,GL_UNSIGNED_BYTE,outside);
        check(outside[0]==255 && outside[1]==0 && outside[2]==255 && outside[3]==255,
            "display extraction leaves unconsumed rows untouched");
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(0,20*scale,320*scale,16*scale,GL_BGRA,GL_UNSIGNED_BYTE,reference);
        for(size_t i=3;i<(size_t)320*16*scale*scale*4;i+=4)reference[i]=255;
        int captured_w=0,captured_h=0;
        check(gl_read_display_argb(x,y+20,320,16,(uint32_t *)cropped,320*scale*4,
            320*16*scale*scale,&captured_w,&captured_h)==320*16*scale*scale,
            "diagnostic capture accepts cropped display coordinates");
        check(!memcmp(reference,cropped,(size_t)320*16*scale*scale*4),
            "diagnostic capture translates cropped origin exactly");
        check(scratch_ensure(32*scale,32*scale),"cropped transaction scratch");
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(10*scale,10*scale,32*scale,32*scale,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        pass_blit(fbo,s_scratch_fbo,(x+10)*scale,(y+10)*scale,0,0,
            32*scale,32*scale,GL_COLOR_BUFFER_BIT);
        pass_blit(s_scratch_fbo,fbo,0,0,(x+200)*scale,(y+180)*scale,
            32*scale,32*scale,GL_COLOR_BUFFER_BIT);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);
        glReadPixels(200*scale,180*scale,32*scale,32*scale,GL_RGBA,GL_UNSIGNED_BYTE,cropped);
        check(!memcmp(reference,cropped,(size_t)32*32*scale*scale*4),
            "transaction backup/restore uses cropped framebuffer coordinates");
        s_wide_fast=0;
    }
    s_hr_fbo=full_fbo;s_hr_tex=full_tex;s_hr_rb=full_rb;
    s_hr_origin_x=s_hr_origin_y=0;s_hr_width=1024;s_hr_height=512;
    check(glGetError()==GL_NO_ERROR,"cropped GL errors");
    p_glDeleteFramebuffers(1,&fbo);p_glDeleteRenderbuffers(1,&rb);glDeleteTextures(1,&tex);
    p_glDeleteFramebuffers(1,&direct_fbo);p_glDeleteRenderbuffers(1,&direct_rb);glDeleteTextures(1,&direct_tex);
    p_glDeleteFramebuffers(2,tile_fbo);glDeleteTextures(2,tile_tex);
    free(reference);free(cropped);free(wide_reference);free(wide_cropped);free(wide_page_zero);
    gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();
    return failures?1:0;
}
int main(void) {
    const int scales[]={1,4,9,18};
    for(unsigned i=0;i<sizeof scales/sizeof scales[0];++i){int rc=run_scale(scales[i]);if(rc)return rc;}
    return 0;
}
