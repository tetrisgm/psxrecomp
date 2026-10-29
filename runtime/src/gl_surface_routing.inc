/* Enumerate disjoint destinations once per uploaded batch. The pool owns
 * canonical VRAM rectangles; wide rendering selects its physical target once
 * and never submits a second canonical pass. */
static int s_draw_regions_saved_scale;
static void surface_wide_select(void) {
    if(!s_region_pool.coverage.rects || !s_pool_wide_enabled || g_wide_w<=0)return;
    int y=s_area_y1<0?0:s_area_y1;
    int end=s_area_y2+1>VRAM_H?VRAM_H:s_area_y2+1;
    int origin=g_wide_cur_base-g_wide_off-view_shift;
    GlRasterSurface *current=surface_pool_find(&s_region_pool,g_wide_cur);
    if(current && current->x==origin && current->y==y && current->h==end-y &&
        current->w==g_wide_w && current->allocation_scale==s_out_scale) {
        s_draw_surface=current;s_hr_scale=s_out_scale;return;
    }
    flush_flat_batch();flush_tex_batch();
    current=surface_pool_display(&s_region_pool,g_wide_cur_base,y,g_wide_w-2*g_wide_off,
        end-y,g_wide_w,g_wide_off,view_shift,s_out_scale);
    if(!current)abort();
    g_wide_cur=current->fbo;s_draw_surface=current;s_hr_scale=s_out_scale;
}
static void surface_draw_region_reserve(size_t count) {
    if(count<=s_draw_region_capacity)return;
    size_t capacity=count<16?16:count;
    GlDrawRegion *next=(GlDrawRegion *)realloc(s_draw_regions,capacity*sizeof *next);
    if(!next)abort();
    s_draw_regions=next;s_draw_region_capacity=capacity;
}
static int surface_draw_regions(const float *vertices,int count,int stride) {
    if(!s_region_pool.coverage.rects)return 0;
    s_draw_region_count=0;s_draw_regions_saved_scale=s_hr_scale;
    if(g_wide_cur)for(size_t i=1;i<s_region_pool.count;++i) {
        GlRasterSurface *s=s_region_pool.surfaces[i];
        if(s && s->fbo==g_wide_cur) {
            surface_draw_region_reserve(1);
            s_draw_regions[s_draw_region_count++]=(GlDrawRegion){s,s->x,s->y,s->w,s->h,1};
            return 1;
        }
    }
    float min_x=INFINITY,min_y=INFINITY,max_x=-INFINITY,max_y=-INFINITY;
    for(int i=0;i<count;++i) {
        const float *v=vertices+(size_t)i*stride;
        int local=pgxp_vertex_local(v,stride);
        float x=v[0]+(local?s_area_x1:0),y=v[1]+(local?s_area_y1:0);
        if(x<min_x)min_x=x;if(x>max_x)max_x=x;
        if(y<min_y)min_y=y;if(y>max_y)max_y=y;
    }
    if(!isfinite(min_x) || !isfinite(min_y) || !isfinite(max_x) || !isfinite(max_y))return 1;
    min_x=fmaxf(min_x,(float)(s_area_x1>0?s_area_x1:0));
    min_y=fmaxf(min_y,(float)(s_area_y1>0?s_area_y1:0));
    max_x=fminf(max_x+1,(float)(s_area_x2+1<VRAM_W?s_area_x2+1:VRAM_W));
    max_y=fminf(max_y+1,(float)(s_area_y2+1<VRAM_H?s_area_y2+1:VRAM_H));
    if(min_x>=max_x || min_y>=max_y)return 1;
    int x0=(int)floorf(min_x),y0=(int)floorf(min_y),x1=(int)ceilf(max_x),y1=(int)ceilf(max_y);
    for(int y=(y0/64)*64;y<y1;y+=64)for(int x=(x0/64)*64;x<x1;x+=64) {
        int needed=0;
        int a=x>x0?x:x0,b=y>y0?y:y0,c=x+64<x1?x+64:x1,d=y+64<y1?y+64:y1;
        for(size_t i=0;i<s_region_pool.coverage.count;++i) {
            const GlCoverageRect *r=s_region_pool.coverage.rects+i;
            if(!r->owner && r->x<c && r->y<d && r->x+r->w>a && r->y+r->h>b) {
                needed=1;break;
            }
        }
        if(needed && !surface_pool_acquire(&s_region_pool,x,y,64,64,x,y,64,64,s_out_scale,1))abort();
    }
    surface_draw_region_reserve(s_region_pool.coverage.count);
    for(size_t i=0;i<s_region_pool.coverage.count;++i) {
        const GlCoverageRect *r=s_region_pool.coverage.rects+i;
        if(!r->owner)continue;
        int a=x0>r->x?x0:r->x,b=y0>r->y?y0:r->y;
        int c=x1<r->x+r->w?x1:r->x+r->w,d=y1<r->y+r->h?y1:r->y+r->h;
        if(a<c && b<d)s_draw_regions[s_draw_region_count++]=(GlDrawRegion){
            s_region_pool.surfaces[r->owner],a,b,c-a,d-b,0};
    }
    return 1;
}
static void surface_draw_region_begin(size_t index) {
    const GlDrawRegion *r=s_draw_regions+index;
    s_draw_surface=r->surface;s_hr_scale=r->surface->allocation_scale;
    s_draw_region_clip=!r->direct;
    s_draw_region_bounds=(GlCoverageRect){r->x,r->y,r->w,r->h,0};
}
static void surface_draw_regions_end(GlRasterSurface *previous) {
    s_draw_region_clip=0;s_draw_surface=previous;s_hr_scale=s_draw_regions_saved_scale;
}
/* Clear existing allocations only. A future tile starts with depth one;
 * clearing a draw domain must not allocate every tile in that domain. */
static void surface_depth_clear(void) {
    GlRasterSurface *previous=s_draw_surface;
    const int scale=s_hr_scale;
    for(size_t i=0;i<s_region_pool.coverage.count;++i) {
        const GlCoverageRect *r=s_region_pool.coverage.rects+i;
        if(!r->owner)continue;
        GlRasterSurface *s=s_region_pool.surfaces[r->owner];
        if(s->fbo==g_wide_cur)continue;
        if(r->x>s_area_x2 || r->y>s_area_y2 || r->x+r->w<=s_area_x1 || r->y+r->h<=s_area_y1)continue;
        s_draw_surface=s;s_hr_scale=s->allocation_scale;
        s_draw_region_clip=1;s_draw_region_bounds=*r;
        hr_begin(1);glClear(GL_DEPTH_BUFFER_BIT);hr_end();
    }
    if(g_wide_cur)for(size_t i=1;i<s_region_pool.count;++i) {
        GlRasterSurface *s=s_region_pool.surfaces[i];
        if(!s || s->fbo!=g_wide_cur)continue;
        s_draw_surface=s;s_hr_scale=s->allocation_scale;s_draw_region_clip=0;
        hr_begin(1);glClear(GL_DEPTH_BUFFER_BIT);hr_end();
        break;
    }
    s_draw_surface=previous;s_hr_scale=scale;s_draw_region_clip=0;
}
static void surface_stencil_rebuild(void) {
    if(!s_stencil_stale.set)return;
    for(size_t i=1;i<s_region_pool.count;++i) {
        const GlRasterSurface *s=s_region_pool.surfaces[i];
        if(!s)continue;
        int x0=s_stencil_stale.x0,x1=s_stencil_stale.x1+1;
        int y0=s_stencil_stale.y0,y1=s_stencil_stale.y1+1;
        /* Wide reveal has no canonical VRAM coordinates. Its rows share the
         * same primitive stream, so include the reveal columns of those rows. */
        if(g_wide_w>0 && s->w==g_wide_w){x0=s->x;x1=s->x+s->w;}
        if(x0<s->x)x0=s->x;if(y0<s->y)y0=s->y;
        if(x1>s->x+s->w)x1=s->x+s->w;if(y1>s->y+s->h)y1=s->y+s->h;
        if(x0>=x1 || y0>=y1)continue;
        int S=s->allocation_scale;
        rebuild_target_stencil_tiled(s->fbo,s->w*S,s->h*S,
            (x0-s->x)*S,(y0-s->y)*S,(x1-x0)*S,(y1-y0)*S);
    }
}
