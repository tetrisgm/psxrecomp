/* Native snapshot history: one current image and reverse changed-page edges.
 * Streaming comparison covers CPU, DMA, HLE and direct-pointer mod writes.
 * Fixed storage only: no hot-path allocation, GL readback or thumbnails. */
#ifndef PSX_REWIND_DELTA_H
#define PSX_REWIND_DELTA_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#define RW_PAGE 4096u
static int rw_equal(const uint8_t *a,const uint8_t *b,size_t n) {
#if defined(__SSE2__)
    while(n>=64) {
        __m128i e0=_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)a),_mm_loadu_si128((const __m128i*)b));
        __m128i e1=_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(a+16)),_mm_loadu_si128((const __m128i*)(b+16)));
        __m128i e2=_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(a+32)),_mm_loadu_si128((const __m128i*)(b+32)));
        __m128i e3=_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(a+48)),_mm_loadu_si128((const __m128i*)(b+48)));
        if(_mm_movemask_epi8(_mm_and_si128(_mm_and_si128(e0,e1),_mm_and_si128(e2,e3)))!=65535) return 0;
        a+=64;b+=64;n-=64;
    }
    while(n>=16) {
        __m128i x=_mm_loadu_si128((const __m128i*)a), y=_mm_loadu_si128((const __m128i*)b);
        if(_mm_movemask_epi8(_mm_cmpeq_epi8(x,y))!=65535) return 0;
        a+=16;b+=16;n-=16;
    }
#endif
    return !memcmp(a,b,n);
}
typedef struct RwDelta { uint8_t *data; size_t bytes,size; uint32_t tick; } RwDelta;
typedef struct RwHistory {
    uint8_t *image,*scratch,*dirty,*pool;
    size_t pool_capacity,pool_head;
    size_t size,image_capacity,bytes,scratch_capacity;
    size_t old_size,offset,used;
    RwDelta *edges;
    uint32_t capacity,count,first,tick;
} RwHistory;
static void rw_clear(RwHistory *r) {
    free(r->edges);free(r->image);free(r->scratch);free(r->dirty);free(r->pool);
    memset(r,0,sizeof(*r));
}
static void rw_apply(RwHistory *r,const uint8_t *data,size_t bytes,size_t size) {
    size_t pos=0;
    while(pos<bytes) {
        uint32_t offset; memcpy(&offset,data+pos,4);pos+=4;
        size_t n=size-offset<RW_PAGE?size-offset:RW_PAGE;
        memcpy(r->image+offset,data+pos,n);pos+=n;
    }
    r->size=size;
}
static void rw_abort(RwHistory *r) { rw_apply(r,r->scratch,r->used,r->old_size); }
/* Allocate and fault in storage before emulation starts. The byte budget is
 * independent of retention depth; full-memory churn evicts more old edges. */
static int rw_prepare(RwHistory *r,uint32_t capacity,size_t image_bytes,size_t pool_bytes) {
    rw_clear(r);
    if(!image_bytes || image_bytes>UINT32_MAX-RW_PAGE)return 0;
    image_bytes=(image_bytes+RW_PAGE-1)&~(size_t)(RW_PAGE-1);
    size_t journal_bytes=image_bytes+image_bytes/RW_PAGE*4;
    if(!capacity || pool_bytes<journal_bytes)return 0;
    r->edges=(RwDelta*)calloc(capacity,sizeof(*r->edges));
    r->image=(uint8_t*)calloc(1,image_bytes);
    r->scratch=(uint8_t*)calloc(1,journal_bytes);
    r->dirty=(uint8_t*)calloc(1,image_bytes/RW_PAGE);
    r->pool=(uint8_t*)malloc(pool_bytes);
    if(!r->edges||!r->image||!r->scratch||!r->dirty||!r->pool){rw_clear(r);return 0;}
    /* Volatile stores prevent lazy zero-page allocation on first capture. */
    for(size_t i=0;i<pool_bytes;i+=RW_PAGE)((volatile uint8_t*)r->pool)[i]=0;
    for(size_t i=0;i<image_bytes;i+=RW_PAGE)((volatile uint8_t*)r->image)[i]=0;
    for(size_t i=0;i<journal_bytes;i+=RW_PAGE)((volatile uint8_t*)r->scratch)[i]=0;
    r->capacity=capacity;r->image_capacity=image_bytes;
    r->scratch_capacity=journal_bytes;r->pool_capacity=pool_bytes;
    return 1;
}
static void rw_reset(RwHistory *r) {
    r->size=r->bytes=r->old_size=r->offset=r->used=r->pool_head=0;
    r->count=r->first=r->tick=0;
}
static int rw_begin(RwHistory *r,uint32_t capacity) {
    if(!capacity || r->capacity!=capacity)return 0;
    memset(r->dirty,0,r->image_capacity/RW_PAGE);
    r->old_size=r->size;r->offset=r->used=0;return 1;
}
static void rw_save_page(RwHistory *r,size_t page) {
    size_t start=page*RW_PAGE;
    if(start>=r->old_size || r->dirty[page]) return;
    r->dirty[page]=1;
    size_t n=r->old_size-start<RW_PAGE?r->old_size-start:RW_PAGE;
    uint32_t offset=(uint32_t)start;
    memcpy(r->scratch+r->used,&offset,4);r->used+=4;
    memcpy(r->scratch+r->used,r->image+start,n);r->used+=n;
}
/* Sequential serializer sink. On failure the caller must rw_abort(). */
static int rw_write(void *context,const void *data,size_t bytes) {
    RwHistory *r=(RwHistory*)context;
    const uint8_t *src=(const uint8_t*)data;
    if(bytes>UINT32_MAX-r->offset) return 0;
    size_t end=r->offset+bytes;
    if(end>r->image_capacity)return 0;
    if(!src) { r->offset=end;return 1; }
    while(bytes) {
        size_t p=r->offset, page=p/RW_PAGE, n=RW_PAGE-p%RW_PAGE;
        if(n>bytes)n=bytes;
        if(p+n>r->old_size || !rw_equal(r->image+p,src,n)) {
            rw_save_page(r,page);
            memcpy(r->image+p,src,n);
        }
        r->offset+=n;src+=n;bytes-=n;
    }
    return 1;
}
static int rw_patch(void *context,size_t offset,const void *data,size_t bytes) {
    RwHistory *r=(RwHistory*)context;
    size_t end=r->offset;r->offset=offset;
    int ok=rw_write(context,data,bytes);r->offset=end;return ok;
}
static int rw_finish(RwHistory *r,uint32_t tick) {
    /* A shorter trailing device FIFO must be recoverable too. */
    for(size_t p=r->offset/RW_PAGE;p*RW_PAGE<r->old_size;p++) rw_save_page(r,p);
    if(r->old_size) {
        size_t start=r->pool_head;
        if(start+r->used>r->pool_capacity)start=0;
        /* Retire metadata only. No allocator or page release on this path.
         * At most capacity edges are visited even after full-state churn. */
        uint32_t retire=r->count==r->capacity?1:0;
        for(uint32_t i=0;i<r->count;i++) {
            RwDelta *old=&r->edges[(r->first+i)%r->capacity];
            size_t lo=(size_t)(old->data-r->pool),hi=lo+old->bytes;
            if(r->used && old->bytes && start<hi && start+r->used>lo)retire=i+1;
        }
        while(retire--) {
            r->bytes-=r->edges[r->first].bytes;
            r->first=(r->first+1)%r->capacity;r->count--;
        }
        uint8_t *delta=r->pool+start;
        if(r->used)memcpy(delta,r->scratch,r->used);
        r->pool_head=start+r->used;
        uint32_t slot=(r->first+r->count)%r->capacity;
        r->count++;
        r->edges[slot]=(RwDelta){delta,r->used,r->old_size,r->tick};
        r->bytes+=r->used;
    }
    r->tick=tick;r->size=r->offset;return 1;
}
static int rw_push(RwHistory *r,const uint8_t *data,size_t size,uint32_t tick,uint32_t capacity) {
    if(!size||!rw_begin(r,capacity))return 0;
    if(!rw_write(r,data,size)){rw_abort(r);return 0;}
    return rw_finish(r,tick);
}
static int rw_back(RwHistory *r) {
    if(!r->count)return 0;
    RwDelta *d=&r->edges[(r->first+r->count-1)%r->capacity];
    rw_apply(r,d->data,d->bytes,d->size);
    r->tick=d->tick;r->bytes-=d->bytes;
    r->pool_head=(size_t)(d->data-r->pool);
    memset(d,0,sizeof(*d));r->count--;return 1;
}
#endif
