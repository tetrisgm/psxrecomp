/* Native snapshot history: one current image and reverse changed-page edges.
 * Streaming comparison covers CPU, DMA, HLE and direct-pointer mod writes.
 * No full-state staging copy, compression, GL readback or thumbnail capture. */
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
    uint8_t *image,*scratch,*dirty;
    size_t size,image_capacity,bytes,scratch_capacity;
    size_t old_size,offset,used;
    RwDelta *edges;
    uint32_t capacity,count,first,tick;
} RwHistory;
static void rw_clear(RwHistory *r) {
    for(uint32_t i=0;i<r->capacity;i++) free(r->edges[i].data);
    free(r->edges);free(r->image);free(r->scratch);free(r->dirty);
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
static int rw_begin(RwHistory *r,uint32_t capacity) {
    if(!capacity) return 0;
    if(r->capacity!=capacity) {
        RwDelta *edges=(RwDelta*)calloc(capacity,sizeof(*edges));
        if(!edges) return 0;
        rw_clear(r);r->edges=edges;r->capacity=capacity;
    }
    size_t need=r->size+((r->size+RW_PAGE-1)/RW_PAGE)*4;
    if(need>r->scratch_capacity) {
        uint8_t *scratch=(uint8_t*)realloc(r->scratch,need);
        if(!scratch) return 0;
        r->scratch=scratch;r->scratch_capacity=need;
    }
    if(r->dirty) memset(r->dirty,0,(r->image_capacity+RW_PAGE-1)/RW_PAGE);
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
    if(end>r->image_capacity) {
        size_t capacity=(end+RW_PAGE-1)&~(size_t)(RW_PAGE-1);
        uint8_t *image=(uint8_t*)realloc(r->image,capacity);
        if(!image) return 0;
        r->image=image;
        uint8_t *dirty=(uint8_t*)realloc(r->dirty,capacity/RW_PAGE);
        if(!dirty) return 0;
        memset(dirty+r->image_capacity/RW_PAGE,0,(capacity-r->image_capacity)/RW_PAGE);
        r->dirty=dirty;r->image_capacity=capacity;
    }
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
        uint8_t *delta=(uint8_t*)malloc(r->used?r->used:1);
        if(!delta) { rw_abort(r);return 0; }
        if(r->used) memcpy(delta,r->scratch,r->used);
        uint32_t slot=(r->first+r->count)%r->capacity;
        if(r->count==r->capacity) {
            r->bytes-=r->edges[slot].bytes;free(r->edges[slot].data);
            r->first=(r->first+1)%r->capacity;
        } else r->count++;
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
    free(d->data);memset(d,0,sizeof(*d));r->count--;return 1;
}
#endif
