/* Compare eager mask maintenance with alpha-derived stencil reconstruction.
 * Keep PGXP enabled; compare full-resolution colour, stencil and depth. */
#define DEPTH_FIXTURE_MAIN depth_fixture_main
#include "test_gl_depth_clear.c"

int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win=SDL_CreateWindow("Mask parity",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return 2;
    for(int scale=1;scale<=9;scale+=4) {
        glb_init(vram);glb_set_scale(scale);gl_renderer_set_swap_interval(0);
        if(!gl_renderer_init_context(win))return 2;
        gl_renderer_set_pgxp_depth(1);glb_wide_configure(560,120);glb_wide_set_target(0);
        uint16_t tex[64*64];
        for(int i=0;i<64*64;i++) tex[i]=(i%3==0)?0:(uint16_t)(0x1234|((i&1)?0x8000:0));
        glb_vram_transfer_in(512,0,64,64,tex);
        for(int semi=-1;semi<4;semi++) for(int set=0;set<2;set++) {
            uint64_t hashes[2][2];
            for(int eager=0;eager<2;eager++) {
                flush_flat_batch();flush_tex_batch();flush_cpu_upload();hiw_flush_queue();
                seed_surface(s_hr_fbo);seed_surface(g_wide_cur);
                s_stencil_valid=1;rect_clear(&s_stencil_stale);
                for(int i=0;i<WIDE_MAX_SURF;i++)wst_clear(i);
                glb_set_draw_area(0,0,319,239);glb_set_draw_offset(0,0);
                glb_set_mask_bits(0,0);glb_fill_rect(0,0,320,240,0x1084);
                flush_flat_batch();flush_tex_batch();hiw_flush_queue();
                s_stencil_eager=eager;
                glb_set_mask_bits(set,0);glb_set_semi_transparency(semi>=0,semi<0?0:semi);
                glb_set_color_modulation(128,128,128,0);
                for(int i=0;i<3;i++)
                    glb_draw_shaded_textured_triangle(20+i*10,20,0,0,0x808080,
                        280,30+i*20,63,0,0x808080,100,220,20,63,0x808080,0,0,0x108,0);
                flush_flat_batch();flush_tex_batch();hiw_flush_queue();
                glb_set_mask_bits(0,1);
                glb_set_semi_transparency(0,0);
                glb_draw_flat_rect(0,0,320,240,0x7c00);
                flush_flat_batch();flush_tex_batch();hiw_flush_queue();
                hashes[eager][0]=surface_hash(s_hr_fbo,VRAM_W*s_hr_scale,VRAM_H*s_hr_scale);
                hashes[eager][1]=surface_hash(g_wide_cur,g_wide_w*scale,VRAM_H*scale);
            }
            printf("scale=%d semi=%d set=%d match=%d\n",scale,semi,set,hashes[0][0]==hashes[1][0]);
            check(hashes[0][0]==hashes[1][0],"eager mask equals reconstructed canonical attachments");
            check(hashes[0][1]==hashes[1][1],"eager mask equals reconstructed wide attachments");
        }
        gl_renderer_shutdown();
    }
    SDL_DestroyWindow(win);SDL_Quit();
    printf("checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
