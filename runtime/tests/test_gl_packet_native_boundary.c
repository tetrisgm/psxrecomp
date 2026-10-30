/* Packet presentation must not execute native raster work a second time. */
#define main inherited_fixture_main
#define PSX_GL_PRODUCTION_SURFACES_TEST 1
#include "test_gl_render_thread.c"
#undef main
int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Packet authority boundary",0,0,320,240,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 77;
    gr_set_backend(GR_BACKEND_OPENGL);gr_init(vram);gr_set_scale(1);
    if(!gl_renderer_init_context(win))return 77;
    gl_renderer_set_pgxp_depth(1);gl_renderer_set_pgxp_color_perspective(1);gl_renderer_set_seam_snap(1);
    gr_set_draw_area(0,0,319,239);gr_set_draw_offset(0,0);
    gr_fill_rect(0,0,320,240,0);flush_flat_batch();flush_tex_batch();
    static uint16_t before[1024*512];memcpy(before,s_vram,sizeof before);
    s_rth_packet_replaying=1;
    for(int hd=0;hd<2;++hd)for(int dual=0;dual<2;++dual) {
        s_hd_native_authority=hd;s_cpu_auth_dual=dual;
        check(!cpu_raster_required(),"packet has immutable native inputs under every authority mode");
        glb_draw_flat_rect(20,20,40,40,0x001f);
        check(!memcmp(before,s_vram,sizeof before),"presentation cannot mutate native sampling mirror by rerasterizing");
    }
    flush_flat_batch();flush_tex_batch();s_rth_packet_replaying=0;
    s_hd_native_authority=1;s_cpu_auth_dual=0;
    check(cpu_raster_required(),"direct HD rendering keeps its native obligation");
    s_hd_native_authority=0;
    static uint32_t pixels[320*240];int w,h;
    check(gl_renderer_read_display_hires(0,0,320,240,pixels,320*240,&w,&h)==320*240,"GPU image still rendered");
    check((pixels[30*320+30]&0x00ffffff)!=0,"packet draw produced colour");
    printf("checks=%d failures=%d\n",checks,failures);
    gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
