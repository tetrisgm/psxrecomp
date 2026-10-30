#include "blocking_sync.h"
#include "present_image_ring.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

typedef struct {
    uint32_t frame;
    uint32_t sequence;
    uint16_t w;
    uint8_t  valid;
    uint16_t *px;   /* PRESENT_IMAGE_RING_MAX_W * PRESENT_IMAGE_RING_H, RGB565 */
} PresentImage;

static PresentImage s_ring[PRESENT_IMAGE_RING_CAP];
static uint16_t *s_pixels = NULL;
static uint32_t s_head = 0;
static uint32_t s_sequence = 0;
static _Atomic int s_ready = 0; /* 0 uninitialized, 1 ready, -1 disabled */
static _Atomic int s_frozen = 0;
static PsxBlockingMutex s_lock = PSX_BLOCKING_MUTEX_INIT;
static void ring_lock(void) {
    psx_blocking_lock(&s_lock);
}
static void ring_unlock(void) { psx_blocking_unlock(&s_lock); }
/* Conversion happens outside the publication lock. Only a completed native
 * thumbnail and its metadata are copied while readers are excluded. */
static _Thread_local uint16_t s_staging[PRESENT_IMAGE_RING_MAX_W*PRESENT_IMAGE_RING_H];
static int ring_ready(void) {
    int ready=atomic_load_explicit(&s_ready,memory_order_acquire);
    if(ready) return ready>0;
    ring_lock();
    if(!s_ready) {
        const char *e=getenv("PSX_PRESENT_IMAGE_RING");
        if(e && *e=='0') s_ready=-1;
        else {
            const size_t per=(size_t)PRESENT_IMAGE_RING_MAX_W*PRESENT_IMAGE_RING_H;
            s_pixels=calloc(PRESENT_IMAGE_RING_CAP*per,sizeof(uint16_t));
            if(s_pixels) {
                for(int i=0;i<PRESENT_IMAGE_RING_CAP;++i) s_ring[i].px=s_pixels+i*per;
                atomic_store_explicit(&s_ready,1,memory_order_release);
            } else s_ready=-1;
        }
    }
    ready=s_ready;
    ring_unlock();
    return ready>0;
}

int present_image_ring_thumb_w(int ww, int wh) {
    if (ww <= 0 || wh <= 0) return 0;
    long w = ((long)ww * PRESENT_IMAGE_RING_H + wh / 2) / wh;
    if (w < 1) w = 1;
    if (w > PRESENT_IMAGE_RING_MAX_W) w = PRESENT_IMAGE_RING_MAX_W;
    return (int)w;
}

int present_image_ring_accepting(void) {
    return !s_frozen && ring_ready();
}

static void publish_thumbnail(uint32_t frame,int w) {
    ring_lock();
    if(!s_frozen) {
        PresentImage *e=&s_ring[s_head];
        memcpy(e->px,s_staging,(size_t)w*PRESENT_IMAGE_RING_H*sizeof(uint16_t));
        e->frame=frame; e->sequence=++s_sequence; e->w=(uint16_t)w; e->valid=1;
        s_head=(s_head+1)%PRESENT_IMAGE_RING_CAP;
    }
    ring_unlock();
}

static uint16_t pack565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void present_image_ring_push(uint32_t frame, const uint8_t *rgba, int w,
                             int pitch, int bottom_up) {
    if (!rgba || w <= 0 || w > PRESENT_IMAGE_RING_MAX_W) return;
    if (!present_image_ring_accepting()) return;
    for (int y = 0; y < PRESENT_IMAGE_RING_H; y++) {
        const int sy = bottom_up ? (PRESENT_IMAGE_RING_H - 1 - y) : y;
        const uint8_t *src = rgba + (size_t)sy * pitch;
        uint16_t *dst = s_staging + (size_t)y * w;
        for (int x = 0; x < w; x++)
            dst[x] = pack565(src[4 * x], src[4 * x + 1], src[4 * x + 2]);
    }
    publish_thumbnail(frame,w);
}

void present_image_ring_push_argb(uint32_t frame, const uint32_t *argb,
                                  int sw, int sh, int src_pitch_px) {
    if (!argb || sw <= 0 || sh <= 0) return;
    if (!present_image_ring_accepting()) return;
    const int w = present_image_ring_thumb_w(sw, sh);
    for (int y = 0; y < PRESENT_IMAGE_RING_H; y++) {
        const int y0 = (int)((long)y * sh / PRESENT_IMAGE_RING_H);
        int y1 = (int)((long)(y + 1) * sh / PRESENT_IMAGE_RING_H);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            const int x0 = (int)((long)x * sw / w);
            int x1 = (int)((long)(x + 1) * sw / w);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++) {
                    const uint32_t p = argb[(size_t)yy * src_pitch_px + xx];
                    r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF; n++;
                }
            if (!n) n = 1;
            s_staging[(size_t)y * w + x] = pack565(r / n, g / n, b / n);
        }
    }
    publish_thumbnail(frame,w);
}

void present_image_ring_set_frozen(int frozen) { ring_lock(); s_frozen = frozen ? 1 : 0; ring_unlock(); }
int  present_image_ring_frozen(void) { return s_frozen; }

void present_image_ring_span(uint32_t *oldest, uint32_t *newest, int *count) {
    ring_lock();
    uint32_t lo = UINT32_MAX, hi = 0;
    int n = 0;
    for (int i = 0; i < PRESENT_IMAGE_RING_CAP; i++) {
        if (!s_ring[i].valid) continue;
        n++;
        if (s_ring[i].frame < lo) lo = s_ring[i].frame;
        if (s_ring[i].frame > hi) hi = s_ring[i].frame;
    }
    if (oldest) *oldest = n ? lo : 0;
    if (newest) *newest = hi;
    if (count) *count = n;
    ring_unlock();
}

void present_image_ring_sequence_span(uint32_t *oldest, uint32_t *newest) {
    ring_lock();
    uint32_t lo = UINT32_MAX, hi = 0;
    for (int i = 0; i < PRESENT_IMAGE_RING_CAP; i++) {
        if (!s_ring[i].valid) continue;
        if (s_ring[i].sequence < lo) lo = s_ring[i].sequence;
        if (s_ring[i].sequence > hi) hi = s_ring[i].sequence;
    }
    if (oldest) *oldest = hi ? lo : 0;
    if (newest) *newest = hi;
    ring_unlock();
}

static int get_rgb(uint32_t key, int by_sequence, uint8_t **rgb, int *w, int *h,
                   uint32_t *frame) {
    if(!rgb || atomic_load_explicit(&s_ready,memory_order_acquire)!=1) return 0;
    uint16_t *copy=malloc(sizeof s_staging);
    if(!copy) return 0;
    unsigned width=0; uint32_t captured_frame=0;
    ring_lock();
    for(int i=0;i<PRESENT_IMAGE_RING_CAP;++i) {
        const PresentImage *e=&s_ring[i];
        if(!e->valid || (by_sequence?e->sequence:e->frame)!=key) continue;
        width=e->w; captured_frame=e->frame;
        memcpy(copy,e->px,(size_t)width*PRESENT_IMAGE_RING_H*sizeof(uint16_t));
        break;
    }
    ring_unlock();
    if(!width) { free(copy); return 0; }
    const size_t n=(size_t)width*PRESENT_IMAGE_RING_H;
    uint8_t *out=malloc(n*3);
    if(!out) { free(copy); return 0; }
    for(size_t k=0;k<n;++k) {
        uint16_t p=copy[k];
        out[k*3]=(uint8_t)(((p>>11)&31)<<3);
        out[k*3+1]=(uint8_t)(((p>>5)&63)<<2);
        out[k*3+2]=(uint8_t)((p&31)<<3);
    }
    free(copy); *rgb=out;
    if(w) *w=(int)width;
    if(h) *h=PRESENT_IMAGE_RING_H;
    if(frame) *frame=captured_frame;
    return 1;
}

int present_image_ring_get_rgb(uint32_t frame, uint8_t **rgb, int *w, int *h) {
    return get_rgb(frame, 0, rgb, w, h, NULL);
}

int present_image_ring_get_sequence_rgb(uint32_t sequence, uint8_t **rgb,
                                       int *w, int *h, uint32_t *frame) {
    return get_rgb(sequence, 1, rgb, w, h, frame);
}
