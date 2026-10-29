/* Render-owner allocations. Canonical coverage never includes wide reveal
 * margins; colour/depth transfers preserve enhanced pixels when ownership
 * changes between a display allocation and offscreen tiles. */
static int surface_pool_init(GlSurfacePool *pool) {
    memset(pool,0,sizeof *pool);
    if(!gl_coverage_init(&pool->coverage,VRAM_W,VRAM_H))return 0;
    pool->surfaces=(GlRasterSurface **)calloc(8,sizeof *pool->surfaces);
    if(!pool->surfaces){gl_coverage_destroy(&pool->coverage);return 0;}
    pool->capacity=8;pool->count=1;
    return 1;
}
static void surface_pool_destroy(GlSurfacePool *pool) {
    for(size_t i=1;i<pool->count;++i) {
        GlRasterSurface *s=pool->surfaces[i];
        if(!s)continue;
        if(s->fbo)p_glDeleteFramebuffers(1,&s->fbo);
        if(s->rb)p_glDeleteRenderbuffers(1,&s->rb);
        if(s->tex)glDeleteTextures(1,&s->tex);
        free(s);
    }
    free(pool->surfaces);gl_coverage_destroy(&pool->coverage);
    memset(pool,0,sizeof *pool);
}
/* Call after flushing batches. Retain any front/back target handles still
 * used by the caller; otherwise allocations without canonical owners can
 * be deleted immediately (GL retains pending command references). */
static void surface_pool_collect(GlSurfacePool *pool,const unsigned *keep,size_t keep_count) {
    for(size_t i=1;i<pool->count;++i) {
        GlRasterSurface *s=pool->surfaces[i];
        if(!s)continue;
        int used=0;
        for(size_t j=0;j<keep_count;++j)used|=keep[j]==i;
        for(size_t j=0;j<pool->coverage.count && !used;++j)used=pool->coverage.rects[j].owner==i;
        if(used)continue;
        p_glDeleteFramebuffers(1,&s->fbo);p_glDeleteRenderbuffers(1,&s->rb);glDeleteTextures(1,&s->tex);
        free(s);pool->surfaces[i]=NULL;
    }
}
static GlRasterSurface *surface_pool_find(GlSurfacePool *pool,GLuint fbo) {
    for(size_t i=1;i<pool->count;++i)
        if(pool->surfaces[i] && pool->surfaces[i]->fbo==fbo)return pool->surfaces[i];
    return NULL;
}
/* R16UI has already received these exact native upload rectangles. Apply
 * them only to owned canonical pieces, never to another target's reveal. */
static void surface_pool_upload(GlSurfacePool *pool,const DirtyRect *uploads,int count) {
    for(size_t i=0;i<pool->coverage.count;++i) {
        const GlCoverageRect *r=pool->coverage.rects+i;
        if(!r->owner)continue;
        const GlRasterSurface *s=pool->surfaces[r->owner];
        for(int j=0;j<count;++j) {
            int x0=uploads[j].x0>r->x?uploads[j].x0:r->x;
            int y0=uploads[j].y0>r->y?uploads[j].y0:r->y;
            int x1=uploads[j].x1<r->x+r->w-1?uploads[j].x1:r->x+r->w-1;
            int y1=uploads[j].y1<r->y+r->h-1?uploads[j].y1:r->y+r->h-1;
            if(x0>x1 || y0>y1)continue;
            DirtyRect rect={0};rect_add(&rect,x0,y0,x1,y1);
            native_surface_upload(s->fbo,s->x,s->y,s->w,s->h,s->allocation_scale,&rect,1);
        }
    }
}
static void surface_pool_fill_segment(GlSurfacePool *pool,int x,int y,int w,int h,uint16_t colour) {
    float red,green,blue;c5_to_float(colour,&red,&green,&blue);
    glClearColor(red,green,blue,0);glClearStencil(0);glStencilMask(255);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glEnable(GL_SCISSOR_TEST);
    for(size_t i=0;i<pool->coverage.count;++i) {
        const GlCoverageRect *r=pool->coverage.rects+i;
        if(!r->owner)continue;
        int x0=x>r->x?x:r->x,y0=y>r->y?y:r->y;
        int x1=x+w<r->x+r->w?x+w:r->x+r->w,y1=y+h<r->y+r->h?y+h:r->y+r->h;
        if(x0>=x1 || y0>=y1)continue;
        const GlRasterSurface *s=pool->surfaces[r->owner];int scale=s->allocation_scale;
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s->fbo);
        glScissor((x0-s->x)*scale,(y0-s->y)*scale,(x1-x0)*scale,(y1-y0)*scale);
        glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);
}
static void surface_pool_seed(GlSurfacePool *pool,unsigned owner,
                              int x,int y,int w,int h,int native_only) {
    GlRasterSurface *target=pool->surfaces[owner];
    for(size_t i=0;i<pool->coverage.count;++i) {
        const GlCoverageRect *r=pool->coverage.rects+i;
        int x0=x>r->x?x:r->x,y0=y>r->y?y:r->y;
        int x1=x+w<r->x+r->w?x+w:r->x+r->w;
        int y1=y+h<r->y+r->h?y+h:r->y+r->h;
        if(x0>=x1 || y0>=y1 || r->owner==owner || (native_only && r->owner))continue;
        if(!r->owner) {
            native_texture_upload_rect(x0,y0,x1-x0,y1-y0);
            DirtyRect rect={0};rect_add(&rect,x0,y0,x1-1,y1-1);
            native_surface_upload(target->fbo,target->x,target->y,target->w,target->h,
                target->allocation_scale,&rect,1);
        } else {
            const GlRasterSurface *source=pool->surfaces[r->owner];
            const int a=source->allocation_scale,b=target->allocation_scale;
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,source->fbo);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,target->fbo);
            glDisable(GL_SCISSOR_TEST);
            p_glBlitFramebuffer((x0-source->x)*a,(y0-source->y)*a,
                (x1-source->x)*a,(y1-source->y)*a,
                (x0-target->x)*b,(y0-target->y)*b,(x1-target->x)*b,(y1-target->y)*b,
                GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,GL_NEAREST);
            /* An unchecked draw can leave the old stencil behind alpha.
             * Transfer depth and colour, then reconstruct this owned piece's
             * mask from alpha rather than publishing stale stencil bits. */
            rebuild_target_stencil_tiled(target->fbo,target->w*b,target->h*b,
                (x0-target->x)*b,(y0-target->y)*b,(x1-x0)*b,(y1-y0)*b);
        }
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);
}
/* Bounds describe the physical allocation in canonical coordinates; valid
 * describes the guest pixels it owns. A tile claims native-only coverage,
 * while a display target takes ownership of its entire valid rectangle. */
static unsigned surface_pool_acquire(GlSurfacePool *pool,
    int x,int y,int w,int h,int valid_x,int valid_y,int valid_w,int valid_h,int scale,int tile) {
    if(valid_w<=0 || valid_h<=0 || valid_x<0 || valid_y<0 ||
       (int64_t)valid_x+valid_w>VRAM_W || (int64_t)valid_y+valid_h>VRAM_H ||
       valid_x<x || valid_y<y ||
       (int64_t)valid_x+valid_w>(int64_t)x+w ||
       (int64_t)valid_y+valid_h>(int64_t)y+h)return 0;
    unsigned owner=0;
    for(size_t i=1;i<pool->count;++i) {
        const GlRasterSurface *s=pool->surfaces[i];
        if(s && s->x==x && s->y==y && s->w==w && s->h==h && s->allocation_scale==scale) {
            owner=(unsigned)i;break;
        }
    }
    if(!owner) {
        size_t slot=1;
        while(slot<pool->count && pool->surfaces[slot])++slot;
        if(slot==pool->capacity) {
            if(pool->capacity>SIZE_MAX/(2*sizeof *pool->surfaces) || slot>UINT_MAX)return 0;
            size_t capacity=pool->capacity*2;
            GlRasterSurface **next=(GlRasterSurface **)realloc(pool->surfaces,capacity*sizeof *next);
            if(!next)return 0;
            pool->surfaces=next;pool->capacity=capacity;
        }
        GlRasterSurface *s=(GlRasterSurface *)calloc(1,sizeof *s);
        if(!s)return 0;
        if(!raster_surface_alloc(w,h,scale,&s->tex,&s->rb,&s->fbo)){free(s);return 0;}
        s->x=x;s->y=y;s->w=w;s->h=h;s->allocation_scale=scale;
        owner=(unsigned)slot;pool->surfaces[slot]=s;
        if(slot==pool->count)++pool->count;
    }
    surface_pool_seed(pool,owner,valid_x,valid_y,valid_w,valid_h,tile);
    int assigned=tile?gl_coverage_claim_native(&pool->coverage,valid_x,valid_y,valid_w,valid_h,owner):
        gl_coverage_replace(&pool->coverage,valid_x,valid_y,valid_w,valid_h,owner);
    if(!assigned)return 0;
    return owner;
}
/* Display bands have independent allocations even when vertical buffering
 * reuses the same X coordinate. View shifts move the physical coordinate
 * range; only its intersection with guest framebuffer pixels owns VRAM. */
static GlRasterSurface *surface_pool_display(GlSurfacePool *pool,
    int base_x,int y,int native_w,int h,int output_w,int offset,int shift,int scale) {
    if(native_w<=0 || output_w<=0 || h<=0 || y<0 || y+h>VRAM_H)return NULL;
    int origin=base_x-offset-shift;
    int valid_x=base_x>origin?base_x:origin;
    int valid_end=base_x+native_w<origin+output_w?base_x+native_w:origin+output_w;
    if(valid_x<0)valid_x=0;if(valid_end>VRAM_W)valid_end=VRAM_W;
    unsigned owner=surface_pool_acquire(pool,origin,y,output_w,h,
        valid_x,y,valid_end-valid_x,h,scale,0);
    return owner?pool->surfaces[owner]:NULL;
}
