/* PS1 VRAM copies read one 128-pixel block before writing that block, then
 * continue forwards. Preserve that dependency order across overlap and wrap;
 * native authority and enhanced ownership are independent representations. */
static unsigned surface_owner_at(int x,int y) {
    for(size_t i=0;i<s_region_pool.coverage.count;++i) {
        const GlCoverageRect *r=s_region_pool.coverage.rects+i;
        if(x>=r->x && x<r->x+r->w && y>=r->y && y<r->y+r->h)return r->owner;
    }
    return 0;
}
static void surface_pool_copy(int sx,int sy,int dx,int dy,int w,int h) {
    if(w<=0 || h<=0)return;
    if(w>VRAM_W)w=VRAM_W;if(h>VRAM_H)h=VRAM_H;
    sx&=1023;sy&=511;dx&=1023;dy&=511;
    GlRasterSurface *previous=s_draw_surface;int previous_scale=s_hr_scale;
    const int S=s_out_scale;
    for(int row=0;row<h;++row)for(int col=0;col<w;col+=128) {
        int n=w-col<128?w-col:128;
        int src_x=(sx+col)&1023,src_y=(sy+row)&511;
        int dst_x=(dx+col)&1023,dst_y=(dy+row)&511;
        uint16_t native[128];unsigned enhanced[128];
        for(int i=0;i<n;++i) {
            int x=(src_x+i)&1023;
            native[i]=s_vram[src_y*VRAM_W+x];
            enhanced[i]=surface_owner_at(x,src_y);
        }
        /* Allocate only destinations inheriting enhanced content. Native
         * texture/CLUT copies alone must not create scaled texture pages. */
        for(int i=0;i<n;++i)if(enhanced[i]) {
            int x=(dst_x+i)&1023;
            if(!surface_owner_at(x,dst_y) &&
               !surface_pool_acquire(&s_region_pool,x&~63,dst_y&~63,64,64,
                    x&~63,dst_y&~63,64,64,S,1))abort();
        }
        int has_destination=0;
        for(int i=0;i<n && !has_destination;++i)has_destination=surface_owner_at((dst_x+i)&1023,dst_y)!=0;
        if(has_destination) {
            if(!scratch_ensure(n*S,S))abort();
            int first=VRAM_W-src_x;if(first>n)first=n;
            native_texture_upload_rect(src_x,src_y,first,1);
            if(first<n)native_texture_upload_rect(0,src_y,n-first,1);
            /* Logical scratch extent can cross native X=1024. Unlike dirty
             * tracking, it must not clamp the wrapped tail away. */
            DirtyRect source={src_x,src_y,src_x+n-1,src_y,1};
            native_surface_upload(s_scratch_fbo,src_x,src_y,n,1,S,&source,1);
            glDisable(GL_SCISSOR_TEST);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,s_scratch_fbo);
            for(int i=0;i<n;) {
                unsigned owner=enhanced[i];int end=i+1;
                while(end<n && enhanced[end]==owner && ((src_x+end)&1023)!=0)++end;
                if(owner) {
                    const GlRasterSurface *s=s_region_pool.surfaces[owner];
                    int x=((src_x+i)&1023)-s->x,y=src_y-s->y;
                    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s->fbo);
                    p_glBlitFramebuffer(x*S,y*S,(x+end-i)*S,(y+1)*S,
                        i*S,0,end*S,S,GL_COLOR_BUFFER_BIT,GL_NEAREST);
                }
                i=end;
            }
            /* All source pixels are now immutable. Apply each disjoint
             * destination run using its offset within the staged block. */
            for(int i=0;i<n;) {
                int x=(dst_x+i)&1023;unsigned owner=surface_owner_at(x,dst_y);
                int end=i+1;
                while(end<n && ((dst_x+end)&1023)!=0 &&
                    surface_owner_at((dst_x+end)&1023,dst_y)==owner)++end;
                if(owner) {
                    s_draw_surface=s_region_pool.surfaces[owner];s_hr_scale=S;
                    gpu_copy_rect_apply_at(s_scratch_tex,(src_x+i)&1023,src_y,
                        x,dst_y,end-i,1,x-i,dst_y);
                }
                i=end;
            }
        }
        for(int i=0;i<n;++i) {
            uint16_t *dest=s_vram+dst_y*VRAM_W+((dst_x+i)&1023);
            if(!s_mask_check || !(*dest&0x8000))*dest=native[i]|(s_mask_set?0x8000:0);
        }
    }
    s_draw_surface=previous;s_hr_scale=previous_scale;
}
