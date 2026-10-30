#ifndef PSX_GL_SURFACE_COVERAGE_H
#define PSX_GL_SURFACE_COVERAGE_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Disjoint canonical VRAM coverage, independent of allocation/reveal bounds.
 * Owner zero denotes native-only pixels. Replacing a rectangle preserves
 * every outside pixel's owner, including partial overlap on either axis. */
typedef struct GlCoverageRect { int x,y,w,h; unsigned owner; } GlCoverageRect;
typedef struct GlCoverage {
    GlCoverageRect *rects;
    size_t count;
    int width,height;
} GlCoverage;

static int gl_coverage_init(GlCoverage *map,int width,int height) {
    if(width<=0 || height<=0)return 0;
    GlCoverageRect *r=(GlCoverageRect *)malloc(sizeof *r);
    if(!r)return 0;
    *r=(GlCoverageRect){0,0,width,height,0};
    *map=(GlCoverage){r,1,width,height};
    return 1;
}
static void gl_coverage_destroy(GlCoverage *map) {
    free(map->rects);memset(map,0,sizeof *map);
}
static int gl_coverage_assign(GlCoverage *map,int x,int y,int w,int h,unsigned owner,int native_only) {
    if(w<=0 || h<=0)return 1;
    int64_t right=(int64_t)x+w,bottom=(int64_t)y+h;
    if(x<0)x=0;
    if(y<0)y=0;
    if(right>map->width)right=map->width;
    if(bottom>map->height)bottom=map->height;
    if(right<=x || bottom<=y)return 1;
    const int x1=(int)right,y1=(int)bottom;
    for(size_t i=0;i<map->count;++i) {
        const GlCoverageRect *r=map->rects+i;
        if(r->owner==owner && x>=r->x && y>=r->y &&
           x1<=r->x+r->w && y1<=r->y+r->h)return 1;
    }
    if(map->count>(SIZE_MAX/sizeof(GlCoverageRect)-1)/5)return 0;
    GlCoverageRect *next=(GlCoverageRect *)malloc((map->count*5+1)*sizeof *next);
    if(!next)return 0;
    size_t n=0;
    for(size_t i=0;i<map->count;++i) {
        GlCoverageRect r=map->rects[i];
        int a=x>r.x?x:r.x,b=y>r.y?y:r.y;
        int c=x1<r.x+r.w?x1:r.x+r.w,d=y1<r.y+r.h?y1:r.y+r.h;
        if(a>=c || b>=d || (native_only && r.owner)){next[n++]=r;continue;}
        if(a>r.x)next[n++]=(GlCoverageRect){r.x,r.y,a-r.x,r.h,r.owner};
        if(c<r.x+r.w)next[n++]=(GlCoverageRect){c,r.y,r.x+r.w-c,r.h,r.owner};
        if(b>r.y)next[n++]=(GlCoverageRect){a,r.y,c-a,b-r.y,r.owner};
        if(d<r.y+r.h)next[n++]=(GlCoverageRect){a,d,c-a,r.y+r.h-d,r.owner};
        if(native_only)next[n++]=(GlCoverageRect){a,b,c-a,d-b,owner};
    }
    if(!native_only)next[n++]=(GlCoverageRect){x,y,x1-x,y1-y,owner};
    /* Coalesce shared edges so alternating framebuffer rectangles do not
     * accumulate historical cuts. Ownership, not insertion order, decides. */
    int merged;
    do {
        merged=0;
        for(size_t i=0;i<n && !merged;++i)for(size_t j=i+1;j<n;++j) {
            GlCoverageRect *a=next+i,*b=next+j;
            if(a->owner!=b->owner)continue;
            if(a->y==b->y && a->h==b->h && (a->x+a->w==b->x || b->x+b->w==a->x)) {
                if(b->x<a->x)a->x=b->x;
                a->w+=b->w;
            } else if(a->x==b->x && a->w==b->w && (a->y+a->h==b->y || b->y+b->h==a->y)) {
                if(b->y<a->y)a->y=b->y;
                a->h+=b->h;
            } else continue;
            next[j]=next[--n];merged=1;break;
        }
    } while(merged);
    free(map->rects);map->rects=next;map->count=n;
    return 1;
}
static int gl_coverage_replace(GlCoverage *map,int x,int y,int w,int h,unsigned owner) {
    return gl_coverage_assign(map,x,y,w,h,owner,0);
}
/* Offscreen tiles can straddle a display allocation. Claim only native-only
 * pieces, keeping the display's enhanced pixels as their existing owner. */
static int gl_coverage_claim_native(GlCoverage *map,int x,int y,int w,int h,unsigned owner) {
    return gl_coverage_assign(map,x,y,w,h,owner,1);
}

#endif
