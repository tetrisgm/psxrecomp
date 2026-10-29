/* Exercise GPU buffer reuse and both supported GL storage contracts. */
#define main legacy_fixture_main
#include "test_gl_render_thread.c"
#undef main
static int stream_case(int portable) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Vertex stream hidden test",0,0,320,240,
        SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 77;
    gr_set_backend(GR_BACKEND_OPENGL);gr_init(vram);gr_set_scale(3);
    if(!gl_renderer_init_context(win))return 77;
    gl_renderer_set_pgxp_depth(1);gl_renderer_set_pgxp_color_perspective(1);
    gl_renderer_set_seam_snap(1);
    vertex_stream_load();
    if(portable)s_stream_persistent=s_stream_bindings=0;
    s_stream_test_segment_bytes=4096;
    gr_set_draw_area(0,0,319,239);gr_set_draw_offset(0,0);
    gr_set_color_modulation(128,128,128,1);
    uint16_t white[64*64];for(unsigned i=0;i<64*64;++i)white[i]=0x7fff;
    gr_vram_transfer_in(640,256,64,64,white);
    uint32_t *pixels=malloc(320*240*9*4);int width,height;
    for(int kind=0;kind<2;++kind) {
        gr_fill_rect(0,0,320,240,0);
        for(unsigned i=0;i<1024;++i) {
            int x=(i%32)*8,y=(i/32)*6;
            gr_set_precise_triangle(1,x*65536,y*65536,(x+6)*65536,y*65536,x*65536,(y+5)*65536);
            gr_set_depth_triangle(1,1024,1024,1024);
            gr_set_perspective_triangle(1,1,1,1);
            uint16_t color=(i&1)?0x001f:0x03e0;
            if(kind)gr_draw_shaded_textured_triangle(x,y,0,0,(i&1)?0x000080:0x008000,
                x+6,y,5,0,(i&1)?0x000080:0x008000,x,y+5,0,5,
                (i&1)?0x000080:0x008000,0,0,0x11a,0);
            else gr_draw_flat_triangle(x,y,x+6,y,x,y+5,color);
            flush_flat_batch();flush_tex_batch();
        }
        check(gl_renderer_read_display_hires(0,0,320,240,pixels,320*240*9,&width,&height)==320*240*9,"stream capture");
        for(unsigned i=0;i<1024;++i) {
            unsigned pixel=pixels[((i/32)*6+1)*3*width+((i%32)*8+1)*3];
            int red=(pixel>>16)&255,green=(pixel>>8)&255,blue=pixel&255;
            check((i&1)?(red>=240 && green==0 && blue==0):(green>=240 && red==0 && blue==0),"ring retains each submitted vertex range");
        }
        check(s_vertex_stream[kind].rotations>=VERTEX_STREAM_SEGMENTS,"stream wraps fenced segments");
        check((s_vertex_stream[kind].mapped!=NULL)==!!s_stream_persistent,"requested storage contract used");
    }
    check(glGetError()==GL_NO_ERROR,"stream GL errors");
    printf("portable=%d persistent=%d geo_rotations=%llu tex_rotations=%llu checks=%d failures=%d\n",
        portable,s_stream_persistent,(unsigned long long)s_vertex_stream[0].rotations,
        (unsigned long long)s_vertex_stream[1].rotations,checks,failures);
    free(pixels);gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();
    return failures?1:0;
}
int main(void) {
    int result=stream_case(0);if(result)return result;
    return stream_case(1);
}
