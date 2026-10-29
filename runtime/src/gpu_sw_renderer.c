/*
 * gpu_sw_renderer.c — PS1 software rasterizer
 *
 * Operates on a 1024x512 uint16_t VRAM array (15-bit color + mask bit).
 * All drawing is clipped to the current draw area.
 *
 * Texture formats: 4-bit CLUT, 8-bit CLUT, 15-bit direct.
 * Supports: semi-transparency (4 modes), mask bit, texture window,
 *           color modulation for textured primitives.
 *
 * Internal-resolution supersampling (SSAA)
 * ----------------------------------------
 * When sw_renderer_set_scale(S>1) is active, the renderer maintains a
 * second buffer `g_hr` that is an S*-scaled mirror of VRAM. Every drawing
 * primitive is rasterized twice: once into native VRAM (byte-identical to
 * the S==1 path — VRAM stays the authoritative copy that the game reads
 * back for framebuffer effects, render-to-texture, transfers, etc.) and
 * once into g_hr at S* the linear resolution. Block operations (fill, copy,
 * CPU->VRAM transfer, single-pixel write) replicate/scale into g_hr so the
 * mirror stays coherent. Textures are ALWAYS sampled from native VRAM, so
 * texels keep their native resolution (point-sampled) while geometry edges
 * and shading are evaluated at the higher resolution. The display reads g_hr
 * (sw_render_display_hires) and the present path downsamples to the window,
 * which is true ordered-grid supersampling / anti-aliasing.
 *
 * When scale==1 (default) g_hr is NULL and the renderer behaves exactly as
 * it did before this feature existed.
 */

#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include "gpu_sw_edges.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

#define WIDE_MAX_SURF 4
/* Independent mutable raster state. The legacy default follows GL context
 * ownership; the native worker binds its own context and never shares it. */
struct SwRendererContext {
    uint16_t *g_vram;
    uint16_t *g_hr;
    int       g_scale;
    int       g_hr_w;
    int       g_hr_h;
    int       g_precise_valid;
    int32_t   g_precise_x16[3], g_precise_y16[3];
    int       g_perspective_valid;
    float     g_perspective_q[3];
    uint32_t  g_perspective_triangles;
    uint16_t *g_wide_surf[WIDE_MAX_SURF];
    int       g_wide_base[WIDE_MAX_SURF];
    int       g_wide_w;
    int       g_wide_off;
    uint16_t *g_wide_cur;
    int       g_wide_cur_base;
    int g_clip_x1, g_clip_y1, g_clip_x2, g_clip_y2;
    int g_offset_x, g_offset_y;
    int g_semi_trans_enabled;
    int g_semi_trans_mode;
    int g_mask_set_bit;
    int g_mask_check_bit;
    uint8_t g_tw_mask_x, g_tw_mask_y;
    uint8_t g_tw_off_x, g_tw_off_y;
    uint8_t g_mod_r, g_mod_g, g_mod_b;
    int g_raw_texture;
    int g_texture_filter;
    int g_texture_filter_req;
    int g_faithful_authority;
    int view_enabled, view_shift, view_pad_left, view_pad_right;
    int g_uv_lim[4];
    int private_dirty;
    uint64_t dirty_rows[8];
    int observe_writes, written_bounds[4];
    struct { uint32_t tag; uint16_t words[4]; } native_texture[256];
    uint16_t native_palette[256];
    uint32_t native_palette_key, native_page;
    unsigned native_skip_field;
};
static SwRendererContext s_default = {
    .g_scale = 1, .g_hr_w = VRAM_WIDTH, .g_hr_h = VRAM_HEIGHT,
    .g_uv_lim = {0, 0, 255, 255}
};
#if defined(_MSC_VER)
static __declspec(thread) SwRendererContext *s_sw = &s_default;
#else
static _Thread_local SwRendererContext *s_sw = &s_default;
#endif

SwRendererContext *sw_context_create(void) {
    SwRendererContext *c = (SwRendererContext *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->g_scale = 1; c->g_hr_w = VRAM_WIDTH; c->g_hr_h = VRAM_HEIGHT;
    c->g_uv_lim[2] = c->g_uv_lim[3] = 255;
    c->private_dirty = 1;
    memset(c->dirty_rows, 0xff, sizeof c->dirty_rows);
    for(int i=0;i<256;++i) c->native_texture[i].tag=UINT32_MAX;
    c->native_palette_key=c->native_page=UINT32_MAX;
    return c;
}
SwRendererContext *sw_context_bind(SwRendererContext *c) {
    SwRendererContext *previous = s_sw;
    s_sw = c ? c : &s_default;
    return previous == &s_default ? NULL : previous;
}
void sw_context_destroy(SwRendererContext *c) {
    if (!c || c == &s_default) return;
    free(c->g_hr);
    for (int i = 0; i < WIDE_MAX_SURF; ++i) free(c->g_wide_surf[i]);
    if (s_sw == c) s_sw = &s_default;
    free(c);
}
void sw_context_take_dirty(uint64_t rows[8]) {
    memcpy(rows, s_sw->dirty_rows, sizeof s_sw->dirty_rows);
    memset(s_sw->dirty_rows, 0, sizeof s_sw->dirty_rows);
}
void sw_context_begin_write_observation(int enabled) {
    s_sw->observe_writes=enabled;
    s_sw->written_bounds[0]=1024;s_sw->written_bounds[1]=512;
    s_sw->written_bounds[2]=s_sw->written_bounds[3]=-1;
}
int sw_context_written_bounds(int bounds[4]) {
    if(bounds) memcpy(bounds,s_sw->written_bounds,sizeof s_sw->written_bounds);
    return s_sw->written_bounds[0]<=s_sw->written_bounds[2];
}
static inline void sw_note_span(int x,int y,int width) {
    if(!s_sw->observe_writes || width<=0) return;
    x&=1023;y&=511;
    int right=width>=1024?1024:x+width-1;
    if(right>=1024) { x=0;right=1023; }
    int* b=s_sw->written_bounds;
    if(x<b[0]) b[0]=x;if(y<b[1]) b[1]=y;
    if(right>b[2]) b[2]=right;if(y>b[3]) b[3]=y;
}
static void sw_dirty_row(uint32_t y) {
    if (!s_sw->private_dirty) { gpu_vram_dirty_mark_row(y); return; }
    y &= 511;
    s_sw->dirty_rows[y >> 6] |= UINT64_C(1) << (y & 63);
}
static void sw_dirty_rect(int x, int y, int w, int h) {
    for(int i=0;s_sw->observe_writes && i<h && i<512;++i) sw_note_span(x,y+i,w);
    if (!s_sw->private_dirty) { gpu_vram_dirty_mark_rect(x, y, w, h); return; }
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h && i < 512; ++i) sw_dirty_row((uint32_t)(y+i));
}
static void sw_dirty_all(void) { sw_dirty_rect(0, 0, 1024, 512); }
#define g_vram (s_sw->g_vram)
#define g_hr (s_sw->g_hr)
#define g_scale (s_sw->g_scale)
#define g_hr_w (s_sw->g_hr_w)
#define g_hr_h (s_sw->g_hr_h)
#define g_precise_valid (s_sw->g_precise_valid)
#define g_precise_x16 (s_sw->g_precise_x16)
#define g_precise_y16 (s_sw->g_precise_y16)
#define g_perspective_valid (s_sw->g_perspective_valid)
#define g_perspective_q (s_sw->g_perspective_q)
#define g_perspective_triangles (s_sw->g_perspective_triangles)
#define g_wide_surf (s_sw->g_wide_surf)
#define g_wide_base (s_sw->g_wide_base)
#define g_wide_w (s_sw->g_wide_w)
#define g_wide_off (s_sw->g_wide_off)
#define g_wide_cur (s_sw->g_wide_cur)
#define g_wide_cur_base (s_sw->g_wide_cur_base)
#define g_clip_x1 (s_sw->g_clip_x1)
#define g_clip_y1 (s_sw->g_clip_y1)
#define g_clip_x2 (s_sw->g_clip_x2)
#define g_clip_y2 (s_sw->g_clip_y2)
#define g_offset_x (s_sw->g_offset_x)
#define g_offset_y (s_sw->g_offset_y)
#define g_semi_trans_enabled (s_sw->g_semi_trans_enabled)
#define g_semi_trans_mode (s_sw->g_semi_trans_mode)
#define g_mask_set_bit (s_sw->g_mask_set_bit)
#define g_mask_check_bit (s_sw->g_mask_check_bit)
#define g_tw_mask_x (s_sw->g_tw_mask_x)
#define g_tw_mask_y (s_sw->g_tw_mask_y)
#define g_tw_off_x (s_sw->g_tw_off_x)
#define g_tw_off_y (s_sw->g_tw_off_y)
#define g_mod_r (s_sw->g_mod_r)
#define g_mod_g (s_sw->g_mod_g)
#define g_mod_b (s_sw->g_mod_b)
#define g_raw_texture (s_sw->g_raw_texture)
#define g_texture_filter (s_sw->g_texture_filter)
#define g_texture_filter_req (s_sw->g_texture_filter_req)
#define g_faithful_authority (s_sw->g_faithful_authority)
#define view_enabled (s_sw->view_enabled)
#define view_shift (s_sw->view_shift)
#define view_pad_left (s_sw->view_pad_left)
#define view_pad_right (s_sw->view_pad_right)
#define g_uv_lim (s_sw->g_uv_lim)

/* Native-wide presentation remains a separate set of surfaces, keyed by
 * buffer origin. Private native contexts never enable those surfaces. */
static void wide_free_all(void);

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int min_i(int a, int b) { return a < b ? a : b; }
static inline int max_i(int a, int b) { return a > b ? a : b; }

static inline uint16_t vram_get(int x, int y) {
    x &= (VRAM_WIDTH - 1);
    y &= (VRAM_HEIGHT - 1);
    return g_vram[y * VRAM_WIDTH + x];
}

/* ------------------------------------------------------------------ */
/* Render target — selects native VRAM or the hi-res mirror.          */
/*                                                                    */
/* `s` is the coordinate scale of this target (1 for native, S for    */
/* hi-res). Clip rectangle is expressed in the target's own pixel     */
/* space. Texture fetches never use the target: texels always come    */
/* from native VRAM via vram_get/texel_fetch.                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t *buf;
    int       w, h;            /* buffer dimensions */
    int       s;               /* coordinate scale */
    int       cx1, cy1, cx2, cy2; /* clip rect (inclusive) in buffer space */
} RTarget;

static inline RTarget rt_native(void) {
    RTarget t;
    t.buf = g_vram; t.w = VRAM_WIDTH; t.h = VRAM_HEIGHT; t.s = 1;
    t.cx1 = g_clip_x1; t.cy1 = g_clip_y1; t.cx2 = g_clip_x2; t.cy2 = g_clip_y2;
    return t;
}

static inline RTarget rt_hires(void) {
    int s = g_scale;
    RTarget t;
    t.buf = g_hr; t.w = g_hr_w; t.h = g_hr_h; t.s = s;
    t.cx1 = g_clip_x1 * s;
    t.cy1 = g_clip_y1 * s;
    t.cx2 = g_clip_x2 * s + (s - 1);
    t.cy2 = g_clip_y2 * s + (s - 1);
    return t;
}

/* Native-wide mirror target: the active wide surface (g_wide_cur). X spans the
 * full wide width (that is where the revealed margins live), but Y is clipped to
 * the canonical DRAW AREA — exactly like rt_native — so a primitive that bleeds
 * past the current framebuffer's vertical band is confined to it. This matters
 * for VERTICALLY double-buffered games (e.g. MMX6: front y0..239 / back
 * y240..479, same base_x): the background tile loop starts a partial row at
 * y = buffer_top - (scrollY & 0xf), which without the band clip would draw up
 * into the OTHER buffer's band in the shared wide surface and corrupt it
 * (top/bottom edge flicker as the buffers flip). Canonical VRAM never showed
 * this because its draw-area clip already confines it. Scale matches SSAA. */

static inline RTarget rt_wide(void) {
    int s = g_scale;
    RTarget t;
    t.buf = g_wide_cur;
    t.w = g_wide_w * s; t.h = VRAM_HEIGHT * s; t.s = s;
    t.cx1 = view_pad_left * s; t.cy1 = g_clip_y1 * s;
    t.cx2 = (g_wide_w - view_pad_right) * s - 1;
    t.cy2 = g_clip_y2 * s + (s - 1);
    return t;
}

/* X-translation (native px) from canonical VRAM space into the active wide
 * surface: local_x = vram_x - base_x + OFFSET. */
static inline int wide_dx(void) { return g_wide_off + view_shift - g_wide_cur_base; }

/* Native-wide 2D-backdrop X-stretch (SW renderer), mirroring the GL
 * wide_set_bd_scale exactly. The far 2D backdrop (sprite-tagged tile grid: sky
 * band + flower field) bypasses the GTE, so in native-wide it stays 4:3-width and
 * leaves a void; this scales its VRAM x about the 4:3 screen centre by
 * g_wide_w/native_w so it fills the widened frame. Gated per prim by
 * psx_ws_prim_in_backdrop() (native-wide + sprite-tagged + background-phase) so
 * Tomba/HUD/3D are untouched. wide_bd_get() resolves the per-prim scale once. */
typedef struct { int on; float scale, center; } WideBd;
static inline WideBd wide_bd_get(void) {
    WideBd b; b.on = 0; b.scale = 1.0f; b.center = 0.0f;
    extern int psx_ws_prim_in_backdrop(void);
    extern int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
    if (g_wide_w > 0 && g_wide_cur && g_ws_bd_stretch_on && psx_ws_prim_in_backdrop()) {
        int native_w = g_wide_w - 2 * g_wide_off;
        if (native_w > 0) {
            b.on = 1;
            b.scale = g_ws_bd_stretch_pct > 0 ? (float)g_ws_bd_stretch_pct / 100.0f
                                              : (float)g_wide_w / (float)native_w;
            b.center = (float)g_wide_cur_base + (float)native_w / 2.0f;
        }
    }
    return b;
}
static inline int wide_bd_x(const WideBd *b, int x) {
    if (!b->on) return x;
    float v = ((float)x - b->center) * b->scale + b->center;
    return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);   /* round half away from zero */
}
/* Fwd decl: the backdrop-stretch path in sw_draw_textured_rect routes through the
 * scaled rasterizer (defined later) so a widened sprite stretches, not tiles. */
static void raster_textured_rect_scaled(const RTarget *t, int x, int y, int w, int h,
                                        int u0, int v0, int u1, int v1,
                                        uint16_t clut_x, uint16_t clut_y, uint16_t texpage);

/* ------------------------------------------------------------------ */
/* Semi-transparency blending                                         */
/* ------------------------------------------------------------------ */

static inline uint16_t blend_pixels(uint16_t back, uint16_t front, int mode) {
    int br = (back >>  0) & 0x1F;
    int bg = (back >>  5) & 0x1F;
    int bb = (back >> 10) & 0x1F;
    int fr = (front >>  0) & 0x1F;
    int fg = (front >>  5) & 0x1F;
    int fb = (front >> 10) & 0x1F;
    int r, g, b;

    switch (mode) {
    case 0: /* B/2+F/2 */
        r = (br + fr) >> 1;
        g = (bg + fg) >> 1;
        b = (bb + fb) >> 1;
        break;
    case 1: /* B+F */
        r = br + fr; if (r > 31) r = 31;
        g = bg + fg; if (g > 31) g = 31;
        b = bb + fb; if (b > 31) b = 31;
        break;
    case 2: /* B-F */
        r = br - fr; if (r < 0) r = 0;
        g = bg - fg; if (g < 0) g = 0;
        b = bb - fb; if (b < 0) b = 0;
        break;
    case 3: /* B+F/4 */
        r = br + (fr >> 2); if (r > 31) r = 31;
        g = bg + (fg >> 2); if (g > 31) g = 31;
        b = bb + (fb >> 2); if (b > 31) b = 31;
        break;
    default:
        r = fr; g = fg; b = fb;
        break;
    }
    return (uint16_t)(r | (g << 5) | (b << 10));
}

/* ------------------------------------------------------------------ */
/* Pixel write — central functions with mask + semi-trans             */
/* ------------------------------------------------------------------ */

/* Write an opaque (untextured) pixel with semi-transparency if enabled */
static inline void put_opaque(const RTarget *t, int x, int y, uint16_t color) {
    if (x < 0 || x >= t->w || y < 0 || y >= t->h) return;
    if (x < t->cx1 || x > t->cx2 || y < t->cy1 || y > t->cy2) return;

    int idx = y * t->w + x;

    /* Mask bit check: don't overwrite if dest has bit 15 set */
    if (g_mask_check_bit && (t->buf[idx] & 0x8000)) return;

    /* Semi-transparency: for untextured primitives, always blend when flag set */
    if (g_semi_trans_enabled) {
        color = blend_pixels(t->buf[idx], color, g_semi_trans_mode);
    }

    /* Mask bit set: force bit 15 on written pixel */
    if (g_mask_set_bit) color |= 0x8000;

    t->buf[idx] = color;
    /* Canonical VRAM only (hi-res / wide mirrors are present-side). */
    if (t->buf == g_vram) {
        sw_note_span(x,y,1);
        sw_dirty_row((uint32_t)y);
    }
}

/* Write a textured pixel — semi-trans only if texel bit 15 is set */
static inline void put_textured(const RTarget *t, int x, int y, uint16_t texel,
                                int mod_r, int mod_g, int mod_b,
                                int raw_texture) {
    if (x < 0 || x >= t->w || y < 0 || y >= t->h) return;
    if (x < t->cx1 || x > t->cx2 || y < t->cy1 || y > t->cy2) return;

    /* Transparent texel (0x0000) is always skipped */
    if (texel == 0x0000) return;

    int idx = y * t->w + x;

    /* Mask bit check */
    if (g_mask_check_bit && (t->buf[idx] & 0x8000)) return;

    /* Color modulation: multiply texel by vertex color unless raw texture */
    uint16_t color;
    if (!raw_texture) {
        int tr = (texel >>  0) & 0x1F;
        int tg = (texel >>  5) & 0x1F;
        int tb = (texel >> 10) & 0x1F;
        /* PS1 formula: (texel * color * 2) / 256, with 5-bit texel scaled to 8-bit.
         * Simplified: (tr * mod_r) >> 4, clamped to 31 */
        int r = (tr * mod_r) >> 4; if (r > 31) r = 31;
        int g = (tg * mod_g) >> 4; if (g > 31) g = 31;
        int b = (tb * mod_b) >> 4; if (b > 31) b = 31;
        color = (uint16_t)(r | (g << 5) | (b << 10));
    } else {
        color = texel & 0x7FFF;
    }

    /* Semi-transparency: for textured primitives, only blend if texel has bit 15 */
    if (g_semi_trans_enabled && (texel & 0x8000)) {
        color = blend_pixels(t->buf[idx], color, g_semi_trans_mode);
    }

    /* The texel's bit 15 reaches the framebuffer, forced on by mask set
     * (canonical VRAM shows it; framebuffer-feedback copies depend on it). */
    color |= (uint16_t)(texel & 0x8000);
    if (g_mask_set_bit) color |= 0x8000;

    t->buf[idx] = color;
    if (t->buf == g_vram) {
        sw_note_span(x,y,1);
        sw_dirty_row((uint32_t)y);
    }
}

/* ------------------------------------------------------------------ */
/* Texture lookup with texture window                                 */
/* ------------------------------------------------------------------ */

static uint16_t texel_fetch(int u, int v, uint16_t texpage,
                            uint16_t clut_x, uint16_t clut_y) {
    /* Apply texture window:
     * texcoord = (texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
     * Mask and Offset are in 8-pixel steps. */
    if (g_tw_mask_x | g_tw_mask_y) {
        u = (u & ~(g_tw_mask_x * 8)) | ((g_tw_off_x & g_tw_mask_x) * 8);
        v = (v & ~(g_tw_mask_y * 8)) | ((g_tw_off_y & g_tw_mask_y) * 8);
    }

    int tpx = (texpage & 0xF) * 64;
    int tpy = ((texpage >> 4) & 1) * 256;
    int depth = (texpage >> 7) & 3;

    switch (depth) {
    case 0: { /* 4-bit CLUT */
        int vram_x = tpx + (u / 4);
        int vram_y = tpy + v;
        uint16_t texel_word = vram_get(vram_x, vram_y);
        int shift = (u & 3) * 4;
        int index = (texel_word >> shift) & 0xF;
        return vram_get(clut_x + index, clut_y);
    }
    case 1: { /* 8-bit CLUT */
        int vram_x = tpx + (u / 2);
        int vram_y = tpy + v;
        uint16_t texel_word = vram_get(vram_x, vram_y);
        int shift = (u & 1) * 8;
        int index = (texel_word >> shift) & 0xFF;
        return vram_get(clut_x + index, clut_y);
    }
    case 2:   /* 15-bit direct */
    case 3: {
        return vram_get(tpx + u, tpy + v);
    }
    default:
        return 0;
    }
}

/* Per-prim uv sampling bounds (inclusive), Beetle-PSX model: bilinear
 * neighbours clamp to the prim's own texture rect so they never blend in
 * texels from a neighbouring sprite/tile or empty VRAM. Set by every
 * textured prim entry point; ignored while a texture window is active
 * (texel_fetch applies the window instead, which wraps by design). */


void sw_native_cache_clear(int palette) {
    for(int i=0;i<256;++i) s_sw->native_texture[i].tag=UINT32_MAX;
    if(palette) s_sw->native_palette_key=UINT32_MAX;
}
void sw_native_set_skip_field(unsigned field) { s_sw->native_skip_field=field; }
static int native_skip_row(int y) {
    return s_sw->native_skip_field && (unsigned)(y&1)==s_sw->native_skip_field-1;
}
void sw_native_texture_save(SwNativeTextureState *state) {
    for(int i=0;i<256;++i) {
        state->tags[i]=s_sw->native_texture[i].tag;
        if(state->tags[i]==UINT32_MAX) memset(state->words[i],0,8);
        else memcpy(state->words[i],s_sw->native_texture[i].words,8);
    }
    memcpy(state->palette,s_sw->native_palette,sizeof state->palette);
    state->palette_key=s_sw->native_palette_key;
    if(state->palette_key==UINT32_MAX) memset(state->palette,0,sizeof state->palette);
    state->page=s_sw->native_page;
}
void sw_native_texture_load(const SwNativeTextureState *state) {
    for(int i=0;i<256;++i) {
        s_sw->native_texture[i].tag=state->tags[i];
        memcpy(s_sw->native_texture[i].words,state->words[i],8);
    }
    memcpy(s_sw->native_palette,state->palette,sizeof state->palette);
    s_sw->native_palette_key=state->palette_key;
    s_sw->native_page=state->page;
}
void sw_native_copy(int sx,int sy,int dx,int dy,int w,int h) {
    sw_native_cache_clear(0);
    for(int row=0;row<h;++row) {
        int target_y=(dy+row)&511;
        for(int block=0;block<w;block+=128) {
            uint16_t pixels[128];
            int count=min_i(128,w-block);
            for(int i=0;i<count;++i) pixels[i]=vram_get(sx+block+i,sy+row);
            for(int i=0;i<count;++i) {
                uint16_t *dst=g_vram+target_y*1024+((dx+block+i)&1023);
                if(!g_mask_check_bit || !(*dst&0x8000))
                    *dst=pixels[i]|(g_mask_set_bit?0x8000:0);
            }
        }
        sw_note_span(dx,target_y,w);
        sw_dirty_row((uint32_t)target_y);
    }
}
static void native_texture_begin(uint16_t page,uint16_t cx,uint16_t cy) {
    unsigned mode=(page>>7)&3;
    uint32_t key=(page&31)|((mode!=0)?32:0);
    if(key!=s_sw->native_page) {
        sw_native_cache_clear(0);
        s_sw->native_page=key;
    }
    if(mode<2) {
        key=(cx/16)|(cy<<6)|(mode<<16);
        if(key!=s_sw->native_palette_key) {
            for(unsigned i=0;i<(mode?256u:16u);++i)
                s_sw->native_palette[i]=vram_get(cx+i,cy);
            s_sw->native_palette_key=key;
        }
    }
}
static uint16_t native_texel(int u,int v,uint16_t page) {
    unsigned mode=(page>>7)&3;
    if(mode>2) mode=2;
    u=(u&255&~(g_tw_mask_x*8))|((g_tw_off_x&g_tw_mask_x)*8);
    v=(v&255&~(g_tw_mask_y*8))|((g_tw_off_y&g_tw_mask_y)*8);
    unsigned x=(((page&15)*64)+(unsigned)(u>>(2-mode)))&1023;
    unsigned y=(((page>>4)&1)*256+(unsigned)v)&511;
    unsigned address=y*1024+x,line=address&~3u;
    unsigned slot=mode?((x/4)&7)|((y&31)<<3):((x/4)&3)|((y&63)<<2);
    if(s_sw->native_texture[slot].tag!=line) {
        memcpy(s_sw->native_texture[slot].words,g_vram+line,8);
        s_sw->native_texture[slot].tag=line;
    }
    uint16_t word=s_sw->native_texture[slot].words[x&3];
    if(mode==2) return word;
    unsigned index=mode?((word>>((u&1)*8))&255):((word>>((u&3)*4))&15);
    return s_sw->native_palette[index];
}

/* Native attribute planes use 12-bit truncated derivatives with a half-unit
 * seed, accumulated modulo 32 bits at 24 fractional bits. This preserves
 * RGB888 until pixel quantization; the presentation raster remains separate.
 * Reference contract: accuracy/axis5_gpu.md D1/D3 and Mednafen's native
 * attribute precision (COORD_FBS=12, COORD_POST_PADDING=12). */
typedef struct NativePlane { uint32_t origin, dx, dy; } NativePlane;
static NativePlane native_plane(const SwNativeVertex v[3], const int value[3],
                                 int core, int64_t area) {
    int64_t ex1=v[1].x-v[0].x, ey1=v[1].y-v[0].y;
    int64_t ex2=v[2].x-v[0].x, ey2=v[2].y-v[0].y;
    int64_t d1=value[1]-value[0], d2=value[2]-value[0];
    NativePlane p;
    p.dx=(uint32_t)(((d1*ey2-d2*ey1)*4096)/area)*4096u;
    p.dy=(uint32_t)(((ex1*d2-ex2*d1)*4096)/area)*4096u;
    p.origin=((uint32_t)value[core]<<24)+0x00800000u
        -p.dx*(uint32_t)v[core].x-p.dy*(uint32_t)v[core].y;
    return p;
}
static unsigned native_plane_at(NativePlane p,int x,int y) {
    return (p.origin+p.dx*(uint32_t)x+p.dy*(uint32_t)y)>>24;
}
static uint16_t native_quantize(int r,int g,int b,int dither) {
    r=clamp_i(r+dither,0,255)>>3;
    g=clamp_i(g+dither,0,255)>>3;
    b=clamp_i(b+dither,0,255)>>3;
    return (uint16_t)(r|(g<<5)|(b<<10));
}
void sw_draw_native_triangle(const SwNativeVertex input[3],uint16_t cx,uint16_t cy,
                             uint16_t page,unsigned flags) {
    SwNativeVertex v[3];
    memcpy(v,input,sizeof v);
    int core=0;
    if (v[1].x<=v[0].x) core=1;
    if (v[2].x<v[core].x || (core==1 && v[2].x==v[core].x)) core=2;
    const int64_t area=(int64_t)(v[1].x-v[0].x)*(v[2].y-v[0].y)
                     -(int64_t)(v[2].x-v[0].x)*(v[1].y-v[0].y);
    if (!area) return;
    int xmin=v[0].x,xmax=xmin,ymin=v[0].y,ymax=ymin;
    for(int i=1;i<3;++i) {
        xmin=min_i(xmin,v[i].x); xmax=max_i(xmax,v[i].x);
        ymin=min_i(ymin,v[i].y); ymax=max_i(ymax,v[i].y);
    }
    if (xmax-xmin>=1024 || ymax-ymin>=512) return;
    NativePlane plane[5];
    for(int channel=0;channel<5;++channel) {
        int value[3];
        for(int i=0;i<3;++i) {
            if(channel<3) value[i]=(int)(((flags&4)?v[i].rgb:v[0].rgb)>>(channel*8))&255;
            else value[i]=(channel==3?v[i].u:v[i].v)&255;
        }
        plane[channel]=native_plane(v,value,core,area);
    }
    // Sorting is solely for coverage; planes retain the original core seed.
    int order[3]={0,1,2};
    for(int i=1;i<3;++i) {
        SwNativeVertex current=v[i]; int original=order[i],j=i;
        while(j>0 && v[j-1].y>current.y) { v[j]=v[j-1];order[j]=order[j-1]; --j; }
        v[j]=current;
        order[j]=original;
    }
    int sorted_core=0;
    while(order[sorted_core]!=core) ++sorted_core;
    static const int8_t matrix[4][4]={
        {-4,0,-3,1},{2,-2,3,-1},{-3,1,-4,0},{3,-1,2,-2}
    };
    const int textured=(flags&1)!=0, raw=(flags&2)!=0;
    if(textured) native_texture_begin(page,cx,cy);
    const int dither=(flags&8) && (textured?!raw:(flags&4)!=0);
    const int top=v[0].y,bottom=v[2].y;
    /* Feedback observes traversal order: start at the core's half, moving
     * away from its Y. Each horizontal span still runs left to right. */
    for(int part=0;part<2;++part) {
      int lower=sorted_core?(part==0):(part==1);
      int lo=max_i(top,lower?v[1].y:v[0].y);
      int hi=min_i(bottom,lower?v[2].y:v[1].y);
      int descending=lower?(sorted_core==2):(sorted_core!=0);
      for(int row=0;row<hi-lo;++row) {
        int y=descending?hi-1-row:lo+row;
        const int py=(y&1023)-(y&1024);
        if(native_skip_row(py)) continue;
        if(py<0 || py>=512 || py<g_clip_y1 || py>g_clip_y2) continue;
        int a,b;
        psx_triangle_edges_at_y(v[0].x,v[0].y,v[1].x,v[1].y,v[2].x,v[2].y,y,&a,&b);
        const int start=min_i(a,b),wrapped=(start&1023)-(start&1024);
        const int translation=start-wrapped;
        int left=max_i(max_i(wrapped,g_clip_x1),0);
        int right=min_i(min_i(wrapped+abs(a-b),g_clip_x2+1),1024);
        int wrote=0;
        for(int px=left;px<right;++px) {
            const int x=px+translation;
            uint16_t *dst=g_vram+py*1024+px;
            int r=(int)native_plane_at(plane[0],x,y);
            int g=(int)native_plane_at(plane[1],x,y);
            int bcol=(int)native_plane_at(plane[2],x,y);
            uint16_t color,stp=0;
            if(textured) {
                uint16_t texel=native_texel((int)native_plane_at(plane[3],x,y),
                                            (int)native_plane_at(plane[4],x,y),page);
                if(!texel) continue;
                stp=texel&0x8000;
                if(raw) color=texel&0x7fff;
                else {
                    r=((texel&31)*r)>>4;
                    g=(((texel>>5)&31)*g)>>4;
                    bcol=(((texel>>10)&31)*bcol)>>4;
                    color=native_quantize(r,g,bcol,dither?matrix[y&3][x&3]:0);
                }
            } else color=native_quantize(r,g,bcol,dither?matrix[y&3][x&3]:0);
            if(g_mask_check_bit && (*dst&0x8000)) continue;
            if(g_semi_trans_enabled && (!textured || stp))
                color=blend_pixels(*dst,color,g_semi_trans_mode);
            *dst=color|stp|(g_mask_set_bit?0x8000:0);
            wrote=1;
        }
        if(wrote) { sw_note_span(left,py,right-left);sw_dirty_row((uint32_t)py); }
      }
    }
}

/* Native line coordinates use a dominant-axis DDA, not Bresenham. Keep
 * colour at 12 fractional bits until quantization, including flat dithering.
 * The endpoint is inclusive; the subpixel bias resolves half-pixel ties. */
void sw_draw_native_line(const SwNativeVertex input[2],unsigned flags) {
    SwNativeVertex a=input[0],b=input[1];
    int dx=abs(b.x-a.x),dy=abs(b.y-a.y),steps=max_i(dx,dy);
    if(dx>=1024 || dy>=512) return;
    if(a.x>b.x) { SwNativeVertex swap=a;a=b;b=swap; }
    int64_t sx=(int64_t)(b.x-a.x)*INT64_C(4294967296);
    int64_t sy=(int64_t)(b.y-a.y)*INT64_C(4294967296);
    if(steps) {
        sx=(sx+(sx>0?steps-1:sx<0?1-steps:0))/steps;
        sy=(sy+(sy>0?steps-1:sy<0?1-steps:0))/steps;
    }
    int64_t x=(int64_t)a.x*INT64_C(4294967296)+INT64_C(2147483648)-1024;
    int64_t y=(int64_t)a.y*INT64_C(4294967296)+INT64_C(2147483648)-(sy<0?1024:0);
    int color[3],delta[3];
    for(int c=0;c<3;++c) {
        int first=(int)((a.rgb>>(8*c))&255);
        int last=(int)((b.rgb>>(8*c))&255);
        color[c]=first*4096+2048;
        delta[c]=(steps && (flags&4))?(last-first)*4096/steps:0;
    }
    static const int8_t matrix[4][4]={{-4,0,-3,1},{2,-2,3,-1},{-3,1,-4,0},{3,-1,2,-2}};
    for(int i=0;i<=steps;++i,x+=sx,y+=sy) {
        int px=(int)(((uint64_t)x>>32)&2047),py=(int)(((uint64_t)y>>32)&2047);
        if(!native_skip_row(py) && px>=g_clip_x1 && px<=g_clip_x2 && px<1024 &&
           py>=g_clip_y1 && py<=g_clip_y2 && py<512) {
            uint16_t *dst=g_vram+py*1024+px;
            if(!g_mask_check_bit || !(*dst&0x8000)) {
                uint16_t value=native_quantize(color[0]/4096,color[1]/4096,color[2]/4096,
                                               (flags&8)?matrix[py&3][px&3]:0);
                if(g_semi_trans_enabled) value=blend_pixels(*dst,value,g_semi_trans_mode);
                *dst=value|(g_mask_set_bit?0x8000:0);
                sw_note_span(px,py,1);sw_dirty_row((uint32_t)py);
            }
        }
        for(int c=0;c<3;++c) color[c]+=delta[c];
    }
}

void sw_draw_native_rect(int x,int y,int w,int h,uint32_t rgb,int u,int v,
                         uint16_t cx,uint16_t cy,uint16_t page,unsigned flags) {
    /* Horizontal sprite reversal starts on the odd texel of its pair. */
    if(flags&16) u|=1;
    if(flags&1) native_texture_begin(page,cx,cy);
    const int left=max_i(max_i(x,g_clip_x1),0),top=max_i(max_i(y,g_clip_y1),0);
    const int right=min_i(min_i(x+w,g_clip_x2+1),1024);
    const int bottom=min_i(min_i(y+h,g_clip_y2+1),512);
    for(int py=top;py<bottom;++py) {
        if(native_skip_row(py)) continue;
        int wrote=0;
        for(int px=left;px<right;++px) {
            uint16_t *dst=g_vram+py*1024+px,stp=0,color;
            int r=rgb&255,g=(rgb>>8)&255,b=(rgb>>16)&255;
            if(flags&1) {
                uint16_t texel=native_texel(u+((flags&16)?x-px:px-x),
                                           v+((flags&32)?y-py:py-y),page);
                if(!texel) continue;
                stp=texel&0x8000;
                if(flags&2) color=texel&0x7fff;
                else color=native_quantize(((texel&31)*r)>>4,
                    (((texel>>5)&31)*g)>>4,(((texel>>10)&31)*b)>>4,0);
            } else color=native_quantize(r,g,b,0);
            if(g_mask_check_bit && (*dst&0x8000)) continue;
            if(g_semi_trans_enabled && (!(flags&1) || stp))
                color=blend_pixels(*dst,color,g_semi_trans_mode);
            *dst=color|stp|(g_mask_set_bit?0x8000:0);
            wrote=1;
        }
        if(wrote) { sw_note_span(left,py,right-left);sw_dirty_row((uint32_t)py); }
    }
}

static uint16_t bl_fetch(int u, int v, uint16_t texpage,
                         uint16_t clut_x, uint16_t clut_y) {
    u &= 0xFF; v &= 0xFF;
    if (!(g_tw_mask_x | g_tw_mask_y)) {
        if (u < g_uv_lim[0]) u = g_uv_lim[0]; else if (u > g_uv_lim[2]) u = g_uv_lim[2];
        if (v < g_uv_lim[1]) v = g_uv_lim[1]; else if (v > g_uv_lim[3]) v = g_uv_lim[3];
    }
    return texel_fetch(u, v, texpage, clut_x, clut_y);
}

/* Bilinear texel sample, in RGB space (after the CLUT lookup — never
 * interpolate palette indices). fu/fv are texel-space coordinates.
 *
 * Beetle-PSX formulation: the NEAREST texel is the base (cutout + STP
 * authority), the neighbours lie toward the sub-texel offset and clamp to
 * g_uv_lim, and each texel's weight is gated by its opacity with the colour
 * renormalised — so prim edges and cutout borders keep their colour instead
 * of dissolving into the transparent (black) neighbour and dropping whole
 * edge columns (the v1 "-0.5 then floor" base sampled one texel OUTSIDE the
 * prim on its top/left edges). Matches the GL TEX shader's bilinear path. */
static uint16_t texel_fetch_bilinear(float fu, float fv, uint16_t texpage,
                                     uint16_t clut_x, uint16_t clut_y) {
    int iu = (int)floorf(fu);
    int iv = (int)floorf(fv);
    int fx = (int)((fu - (float)iu) * 256.0f) - 128;
    int fy = (int)((fv - (float)iv) * 256.0f) - 128;
    int su = fx < 0 ? -1 : 1, sv = fy < 0 ? -1 : 1;
    if (fx < 0) fx = -fx;
    if (fy < 0) fy = -fy;
    if (fx > 128) fx = 128;
    if (fy > 128) fy = 128;

    uint16_t c00 = bl_fetch(iu, iv, texpage, clut_x, clut_y);
    if (c00 == 0) return 0x0000;
    uint16_t c10 = bl_fetch(iu + su, iv, texpage, clut_x, clut_y);
    uint16_t c01 = bl_fetch(iu, iv + sv, texpage, clut_x, clut_y);
    uint16_t c11 = bl_fetch(iu + su, iv + sv, texpage, clut_x, clut_y);

    int w00 = (c00 ? 1 : 0) * (256 - fx) * (256 - fy);
    int w10 = (c10 ? 1 : 0) * fx * (256 - fy);
    int w01 = (c01 ? 1 : 0) * (256 - fx) * fy;
    int w11 = (c11 ? 1 : 0) * fx * fy;
    int opac = w00 + w10 + w01 + w11;

    int r = ((c00 & 0x1F) * w00 + (c10 & 0x1F) * w10
           + (c01 & 0x1F) * w01 + (c11 & 0x1F) * w11) / opac;
    int g = (((c00 >> 5) & 0x1F) * w00 + ((c10 >> 5) & 0x1F) * w10
           + ((c01 >> 5) & 0x1F) * w01 + ((c11 >> 5) & 0x1F) * w11) / opac;
    int b = (((c00 >> 10) & 0x1F) * w00 + ((c10 >> 10) & 0x1F) * w10
           + ((c01 >> 10) & 0x1F) * w01 + ((c11 >> 10) & 0x1F) * w11) / opac;
    int stp = (((c00 >> 15) & 1) * w00 + ((c10 >> 15) & 1) * w10
             + ((c01 >> 15) & 1) * w01 + ((c11 >> 15) & 1) * w11) * 2 >= opac;

    return (uint16_t)(r | (g << 5) | (b << 10) | (stp ? 0x8000 : 0));
}

static inline float bilinear_center_shift_for_target(const RTarget *t) {
    int s = (t && t->s > 0) ? t->s : 1;
    return 0.5f / (float)s - 1.0f / 64.0f;
}

/* uv sampling bounds (see g_uv_lim): the shared PS1 uv model in gpu_uv.h.
 * The software DDA truncates exactly like the PS1, so unlike the GL/VK
 * backends NO mirrored-2D compensation is applied here — these bounds only
 * feed the bilinear-filter neighbour clamp. */
#include "gpu_uv.h"

static void sw_tri_uv_limits(const int *xs, const int *ys,
                             const int *us, const int *vs) {
    if (g_perspective_valid) psx_uv_tri_world_limits(us, vs, g_uv_lim);
    else psx_uv_tri_limits(xs, ys, us, vs, g_uv_lim);
}

static void sw_rect_uv_limits(int u0, int v0, int u1, int v1) {
    psx_uv_rect_limits(u0, v0, u1, v1, g_uv_lim);
}

/* ------------------------------------------------------------------ */
/* Public API: init                                                   */
/* ------------------------------------------------------------------ */

void sw_renderer_init(uint16_t *vram) {
    g_vram = vram;
    g_clip_x1 = 0;
    g_clip_y1 = 0;
    g_clip_x2 = VRAM_WIDTH - 1;
    g_clip_y2 = VRAM_HEIGHT - 1;
    g_offset_x = 0;
    g_offset_y = 0;
    g_semi_trans_enabled = 0;
    g_semi_trans_mode = 0;
    g_mask_set_bit = 0;
    g_mask_check_bit = 0;
    g_tw_mask_x = g_tw_mask_y = 0;
    g_tw_off_x = g_tw_off_y = 0;
    g_mod_r = g_mod_g = g_mod_b = 16; /* neutral = 128/8 = 16 (no modulation) */
    g_raw_texture = 0;
    g_texture_filter = 0;
    g_texture_filter_req = 0;
}

/* Point the rasterizer at another VRAM array without touching any draw
 * state (the OpenGL render thread switches its upload source this way). */
void sw_renderer_rebind_vram(uint16_t *vram) {
    g_vram = vram;
}

/* ------------------------------------------------------------------ */
/* Supersampling control                                              */
/* ------------------------------------------------------------------ */

void sw_renderer_set_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > SW_MAX_INTERNAL_SCALE) scale = SW_MAX_INTERNAL_SCALE;

    if (g_hr) { free(g_hr); g_hr = NULL; }
    /* Wide compositor surfaces are scale-sized; drop them so they re-allocate
     * at the new scale on next use (gpu re-configures native-wide after this). */
    wide_free_all();

    g_scale = scale;
    if (scale > 1) {
        g_hr_w = VRAM_WIDTH * scale;
        g_hr_h = VRAM_HEIGHT * scale;
        g_hr = (uint16_t *)calloc((size_t)g_hr_w * (size_t)g_hr_h, sizeof(uint16_t));
        if (!g_hr) {
            /* Allocation failed — fall back to native rendering. */
            g_scale = 1;
            g_hr_w = VRAM_WIDTH;
            g_hr_h = VRAM_HEIGHT;
        }
    } else {
        g_hr_w = VRAM_WIDTH;
        g_hr_h = VRAM_HEIGHT;
    }
}

int sw_renderer_scale(void) { return g_scale; }

/* Faithful authority (netplay): while this rasterizer writes the guest's
 * authoritative VRAM -- the netplay dual-raster SW@1x mirror or the netplay
 * software GPU -- it draws exactly what hardware draws. The PGXP overrides and
 * texture filtering are presentation enhancements; here they would put host-
 * only state (PGXP shadows, which a rollback load drops on one peer only, and
 * each player's Display filter) into pixels the game can read back with GP0
 * C0 (V8:2 samples its own frame), forking guest RAM between peers. */
void sw_set_faithful_authority(int on) {
    g_faithful_authority = on ? 1 : 0;
    g_texture_filter = g_faithful_authority ? 0 : g_texture_filter_req;
    if (g_faithful_authority) {
        g_precise_valid = 0;
        g_perspective_valid = 0;
    }
}

int sw_faithful_authority(void) { return g_faithful_authority; }

void sw_set_precise_triangle(int enabled,
                             int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2) {
    g_precise_valid = (enabled && !g_faithful_authority) ? 1 : 0;
    g_precise_x16[0] = x0; g_precise_y16[0] = y0;
    g_precise_x16[1] = x1; g_precise_y16[1] = y1;
    g_precise_x16[2] = x2; g_precise_y16[2] = y2;
}

void sw_set_perspective_triangle(int enabled,
                                 float q0, float q1, float q2) {
    const int requested = enabled && q0 > 0.0f && q1 > 0.0f && q2 > 0.0f;
    g_perspective_valid = requested && !g_faithful_authority;
    g_perspective_q[0] = q0;
    g_perspective_q[1] = q1;
    g_perspective_q[2] = q2;
    /* Counts requests: under faithful authority the GPU presentation surface
     * still draws them perspective-correct. */
    if (requested) g_perspective_triangles++;
}

uint32_t sw_perspective_triangle_count(void) {
    return g_perspective_triangles;
}

static inline int precise_scaled(int axis, int vertex, int fallback, int scale) {
    if (!g_precise_valid) return fallback * scale;
    int64_t fixed = axis ? g_precise_y16[vertex] : g_precise_x16[vertex];
    int64_t scaled = fixed * scale;
    if (scaled >= 0) return (int)((scaled + 0x8000) >> 16);
    return -(int)((-scaled + 0x8000) >> 16);
}

static inline int precise_wide_x(int vertex, int fallback, int scale,
                                 int dx, const WideBd *bd) {
    if (g_precise_valid)
        return precise_scaled(0, vertex, fallback, scale) + dx * scale;
    return (wide_bd_x(bd, fallback) + dx) * scale;
}

static inline void precise_consumed(void) {
    g_precise_valid = 0;
    g_perspective_valid = 0;
}

void sw_set_texture_filter(int bilinear) {
    g_texture_filter_req = bilinear ? 1 : 0;
    g_texture_filter = g_faithful_authority ? 0 : g_texture_filter_req;
}
int  sw_texture_filter(void) { return g_texture_filter; }

/* ------------------------------------------------------------------ */
/* Draw state setters                                                 */
/* ------------------------------------------------------------------ */

void sw_set_semi_transparency(int enabled, int mode) {
    g_semi_trans_enabled = enabled;
    g_semi_trans_mode = mode & 3;
}

void sw_set_mask_bits(int set_bit, int check_bit) {
    g_mask_set_bit = set_bit;
    g_mask_check_bit = check_bit;
}

void sw_set_texture_window(uint32_t raw) {
    g_tw_mask_x = (uint8_t)(raw & 0x1F);
    g_tw_mask_y = (uint8_t)((raw >> 5) & 0x1F);
    g_tw_off_x  = (uint8_t)((raw >> 10) & 0x1F);
    g_tw_off_y  = (uint8_t)((raw >> 15) & 0x1F);
}

void sw_set_color_modulation(int r, int g, int b, int raw_texture) {
    /* Convert 8-bit vertex color to modulation factor.
     * result_5bit = (texel * (color >> 3)) >> 4
     * At color=128: factor=16, (31*16)>>4 = 31 — neutral.
     * At color=255: factor=31, saturates. At color=0: black. */
    g_mod_r = (uint8_t)(r >> 3);
    g_mod_g = (uint8_t)(g >> 3);
    g_mod_b = (uint8_t)(b >> 3);
    g_raw_texture = raw_texture;
}

/* ------------------------------------------------------------------ */
/* Draw area / offset                                                 */
/* ------------------------------------------------------------------ */

void sw_set_draw_area(int x1, int y1, int x2, int y2) {
    g_clip_x1 = clamp_i(x1, 0, VRAM_WIDTH - 1);
    g_clip_y1 = clamp_i(y1, 0, VRAM_HEIGHT - 1);
    g_clip_x2 = clamp_i(x2, 0, VRAM_WIDTH - 1);
    g_clip_y2 = clamp_i(y2, 0, VRAM_HEIGHT - 1);
}

void sw_get_draw_area(int *x1, int *y1, int *x2, int *y2) {
    *x1 = g_clip_x1; *y1 = g_clip_y1;
    *x2 = g_clip_x2; *y2 = g_clip_y2;
}

void sw_set_draw_offset(int x, int y) {
    g_offset_x = x;
    g_offset_y = y;
}

/* ------------------------------------------------------------------ */
/* Fill rectangle (GP0(02h) — directly in VRAM, no draw offset)       */
/* ------------------------------------------------------------------ */

static void hr_fill_block(int x, int y, int w, int h, uint16_t color) {
    int s = g_scale;
    int x0 = (x & (VRAM_WIDTH - 1)) * s;
    int y0 = (y & (VRAM_HEIGHT - 1)) * s;
    int W = w * s, H = h * s;
    for (int row = 0; row < H; row++) {
        int py = (y0 + row) % g_hr_h;
        uint16_t *dst = g_hr + (size_t)py * g_hr_w;
        for (int col = 0; col < W; col++) {
            int px = (x0 + col) % g_hr_w;
            dst[px] = color;
        }
    }
}

void sw_fill_rect(int x, int y, int w, int h, uint16_t color) {
    /* Fill rect ignores draw area, mask bits, and semi-transparency.
     * It writes directly to VRAM with coordinates wrapping. */
    int x0 = x & (VRAM_WIDTH - 1);
    int y0 = y & (VRAM_HEIGHT - 1);

    for (int row = 0; row < h; row++) {
        int py = (y0 + row) & (VRAM_HEIGHT - 1);
        if (native_skip_row(py)) continue;
        for (int col = 0; col < w; col++) {
            int px = (x0 + col) & (VRAM_WIDTH - 1);
            g_vram[py * VRAM_WIDTH + px] = color;
        }
    }
    sw_dirty_rect(x0, y0, w, h);

    if (g_hr) hr_fill_block(x, y, w, h, color);
}

/* ------------------------------------------------------------------ */
/* Copy rectangle (VRAM -> VRAM)                                      */
/* ------------------------------------------------------------------ */

void sw_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    uint16_t row_buf[VRAM_WIDTH];

    /* Hi-res scratch: one native row's worth of source super-rows. */
    int s = g_scale;
    uint16_t *hr_rows = NULL;
    if (g_hr) hr_rows = (uint16_t *)malloc((size_t)w * s * s * sizeof(uint16_t));

    for (int row = 0; row < h; row++) {
        int sy = (src_y + row) & (VRAM_HEIGHT - 1);
        int dy = (dst_y + row) & (VRAM_HEIGHT - 1);

        for (int col = 0; col < w; col++) {
            int sx = (src_x + col) & (VRAM_WIDTH - 1);
            row_buf[col] = g_vram[sy * VRAM_WIDTH + sx];
        }

        /* Snapshot the hi-res source super-rows for this native row before
         * any destination write (handles overlapping copies per native row,
         * mirroring the native row_buf approach above). */
        if (hr_rows) {
            for (int sr = 0; sr < s; sr++) {
                int hsy = ((sy * s) + sr) % g_hr_h;
                const uint16_t *src = g_hr + (size_t)hsy * g_hr_w;
                for (int col = 0; col < w; col++) {
                    int hsx_base = ((src_x + col) & (VRAM_WIDTH - 1)) * s;
                    for (int sc = 0; sc < s; sc++) {
                        int hsx = (hsx_base + sc) % g_hr_w;
                        hr_rows[(sr * w + col) * s + sc] = src[hsx];
                    }
                }
            }
        }

        for (int col = 0; col < w; col++) {
            int dx = (dst_x + col) & (VRAM_WIDTH - 1);
            uint16_t pix = row_buf[col];
            /* Copy applies mask bit settings */
            if (g_mask_check_bit && (g_vram[dy * VRAM_WIDTH + dx] & 0x8000))
                continue;
            if (g_mask_set_bit) pix |= 0x8000;
            g_vram[dy * VRAM_WIDTH + dx] = pix;

            if (hr_rows) {
                int hdx_base = dx * s;
                for (int sr = 0; sr < s; sr++) {
                    int hdy = ((dy * s) + sr) % g_hr_h;
                    uint16_t *dst = g_hr + (size_t)hdy * g_hr_w;
                    for (int sc = 0; sc < s; sc++) {
                        int hdx = (hdx_base + sc) % g_hr_w;
                        uint16_t hp = hr_rows[(sr * w + col) * s + sc];
                        if (g_mask_set_bit) hp |= 0x8000;
                        dst[hdx] = hp;
                    }
                }
            }
        }
        sw_note_span(dst_x,dy,w);sw_dirty_row((uint32_t)dy);
    }

    if (hr_rows) free(hr_rows);
}

/* ------------------------------------------------------------------ */
/* Flat-shaded triangle (scanline rasterization)                      */
/* ------------------------------------------------------------------ */

static void raster_flat_triangle(const RTarget *t,
                                 int x0, int y0, int x1, int y1,
                                 int x2, int y2, uint16_t color) {
    /* Sort vertices by Y coordinate */
    if (y0 > y1) { int tt; tt=x0; x0=x1; x1=tt; tt=y0; y0=y1; y1=tt; }
    if (y0 > y2) { int tt; tt=x0; x0=x2; x2=tt; tt=y0; y0=y2; y2=tt; }
    if (y1 > y2) { int tt; tt=x1; x1=x2; x2=tt; tt=y1; y1=y2; y2=tt; }

    int dy_total = y2 - y0;
    if (dy_total == 0) return;

    for (int y = y0; y < y2; y++) {
        if (y < t->cy1 || y > t->cy2) continue;

        int xa, xb;
        psx_triangle_edges_at_y(x0, y0, x1, y1, x2, y2, y, &xa, &xb);

        if (xa > xb) { int tt = xa; xa = xb; xb = tt; }

        int sx = max_i(xa, t->cx1);
        int ex = min_i(xb, t->cx2 + 1);

        for (int x = sx; x < ex; x++) {
            put_opaque(t, x, y, color);
        }
    }
}

void sw_draw_flat_triangle(int x0, int y0, int x1, int y1,
                           int x2, int y2, uint16_t color) {
    RTarget n = rt_native();
    raster_flat_triangle(&n, x0, y0, x1, y1, x2, y2, color);
    if (g_hr) {
        int s = g_scale;
        RTarget hr = rt_hires();
        raster_flat_triangle(&hr,
            precise_scaled(0,0,x0,s), precise_scaled(1,0,y0,s),
            precise_scaled(0,1,x1,s), precise_scaled(1,1,y1,s),
            precise_scaled(0,2,x2,s), precise_scaled(1,2,y2,s), color);
    }
    if (g_wide_cur) {
        int s = g_scale, dx = wide_dx();
        RTarget wt = rt_wide();
        WideBd bd = wide_bd_get();
        raster_flat_triangle(&wt,
            precise_wide_x(0,x0,s,dx,&bd), precise_scaled(1,0,y0,s),
            precise_wide_x(1,x1,s,dx,&bd), precise_scaled(1,1,y1,s),
            precise_wide_x(2,x2,s,dx,&bd), precise_scaled(1,2,y2,s), color);
    }
    precise_consumed();
}

/* ------------------------------------------------------------------ */
/* Gouraud-shaded triangle (scanline rasterization with color interp) */
/* ------------------------------------------------------------------ */

static void raster_gouraud_triangle(const RTarget *t,
                                    int x0, int y0, uint16_t c0,
                                    int x1, int y1, uint16_t c1,
                                    int x2, int y2, uint16_t c2) {
    /* Extract 5-bit color components for each vertex */
    int r0 = (c0 >>  0) & 0x1F, g0 = (c0 >>  5) & 0x1F, b0 = (c0 >> 10) & 0x1F;
    int r1 = (c1 >>  0) & 0x1F, g1 = (c1 >>  5) & 0x1F, b1 = (c1 >> 10) & 0x1F;
    int r2 = (c2 >>  0) & 0x1F, g2 = (c2 >>  5) & 0x1F, b2 = (c2 >> 10) & 0x1F;

    /* Sort vertices by Y coordinate, keeping colors in sync */
    if (y0 > y1) {
        int tt; tt=x0; x0=x1; x1=tt; tt=y0; y0=y1; y1=tt;
        tt=r0; r0=r1; r1=tt; tt=g0; g0=g1; g1=tt; tt=b0; b0=b1; b1=tt;
    }
    if (y0 > y2) {
        int tt; tt=x0; x0=x2; x2=tt; tt=y0; y0=y2; y2=tt;
        tt=r0; r0=r2; r2=tt; tt=g0; g0=g2; g2=tt; tt=b0; b0=b2; b2=tt;
    }
    if (y1 > y2) {
        int tt; tt=x1; x1=x2; x2=tt; tt=y1; y1=y2; y2=tt;
        tt=r1; r1=r2; r2=tt; tt=g1; g1=g2; g2=tt; tt=b1; b1=b2; b2=tt;
    }

    int dy_total = y2 - y0;
    if (dy_total == 0) return;

    for (int y = y0; y < y2; y++) {
        if (y < t->cy1 || y > t->cy2) continue;

        int second_half = (y >= y1);
        int seg_height = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_height == 0) seg_height = 1;

        float alpha = (float)(y - y0) / (float)dy_total;
        float beta;
        if (second_half)
            beta = (float)(y - y1) / (float)seg_height;
        else
            beta = (float)(y - y0) / (float)seg_height;

        /* Interpolate X along edges */
        int xa, xb;
        psx_triangle_edges_at_y(x0, y0, x1, y1, x2, y2, y, &xa, &xb);

        /* Interpolate colors along edges (in 5-bit space) */
        int ra = r0 + (int)((float)(r2 - r0) * alpha);
        int ga = g0 + (int)((float)(g2 - g0) * alpha);
        int ba = b0 + (int)((float)(b2 - b0) * alpha);
        int rb, gb, bb;
        if (second_half) {
            rb = r1 + (int)((float)(r2 - r1) * beta);
            gb = g1 + (int)((float)(g2 - g1) * beta);
            bb = b1 + (int)((float)(b2 - b1) * beta);
        } else {
            rb = r0 + (int)((float)(r1 - r0) * beta);
            gb = g0 + (int)((float)(g1 - g0) * beta);
            bb = b0 + (int)((float)(b1 - b0) * beta);
        }

        /* Ensure xa < xb, swap colors too */
        if (xa > xb) {
            int tt;
            tt = xa; xa = xb; xb = tt;
            tt = ra; ra = rb; rb = tt;
            tt = ga; ga = gb; gb = tt;
            tt = ba; ba = bb; bb = tt;
        }

        int sx = max_i(xa, t->cx1);
        int ex = min_i(xb, t->cx2 + 1);
        int span = xb - xa;

        for (int x = sx; x < ex; x++) {
            /* Interpolate color across the scanline */
            uint16_t color;
            if (span > 0) {
                float tf = (float)(x - xa) / (float)span;
                int r = ra + (int)((float)(rb - ra) * tf);
                int g = ga + (int)((float)(gb - ga) * tf);
                int b = ba + (int)((float)(bb - ba) * tf);
                if (r < 0) r = 0; if (r > 31) r = 31;
                if (g < 0) g = 0; if (g > 31) g = 31;
                if (b < 0) b = 0; if (b > 31) b = 31;
                color = (uint16_t)(r | (g << 5) | (b << 10));
            } else {
                color = (uint16_t)(ra | (ga << 5) | (ba << 10));
            }
            put_opaque(t, x, y, color);
        }
    }
}

void sw_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                              int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2) {
    RTarget n = rt_native();
    raster_gouraud_triangle(&n, x0, y0, c0, x1, y1, c1, x2, y2, c2);
    if (g_hr) {
        int s = g_scale;
        RTarget hr = rt_hires();
        raster_gouraud_triangle(&hr,
            precise_scaled(0,0,x0,s), precise_scaled(1,0,y0,s), c0,
            precise_scaled(0,1,x1,s), precise_scaled(1,1,y1,s), c1,
            precise_scaled(0,2,x2,s), precise_scaled(1,2,y2,s), c2);
    }
    if (g_wide_cur) {
        int s = g_scale, dx = wide_dx();
        RTarget wt = rt_wide();
        WideBd bd = wide_bd_get();
        raster_gouraud_triangle(&wt,
            precise_wide_x(0,x0,s,dx,&bd), precise_scaled(1,0,y0,s), c0,
            precise_wide_x(1,x1,s,dx,&bd), precise_scaled(1,1,y1,s), c1,
            precise_wide_x(2,x2,s,dx,&bd), precise_scaled(1,2,y2,s), c2);
    }
    precise_consumed();
}

/* ------------------------------------------------------------------ */
/* Textured triangle                                                  */
/* ------------------------------------------------------------------ */

static void raster_textured_triangle(const RTarget *t,
                                     int x0, int y0, int u0, int v0,
                                     int x1, int y1, int u1, int v1,
                                     int x2, int y2, int u2, int v2,
                                     uint16_t clut_x, uint16_t clut_y,
                                     uint16_t texpage,
                                     int perspective,
                                     float q0, float q1, float q2) {
    /* Sort by Y, keeping UV in sync */
    if (y0 > y1) {
        int tt;
        tt=x0; x0=x1; x1=tt; tt=y0; y0=y1; y1=tt;
        tt=u0; u0=u1; u1=tt; tt=v0; v0=v1; v1=tt;
        float tq=q0; q0=q1; q1=tq;
    }
    if (y0 > y2) {
        int tt;
        tt=x0; x0=x2; x2=tt; tt=y0; y0=y2; y2=tt;
        tt=u0; u0=u2; u2=tt; tt=v0; v0=v2; v2=tt;
        float tq=q0; q0=q2; q2=tq;
    }
    if (y1 > y2) {
        int tt;
        tt=x1; x1=x2; x2=tt; tt=y1; y1=y2; y2=tt;
        tt=u1; u1=u2; u2=tt; tt=v1; v1=v2; v2=tt;
        float tq=q1; q1=q2; q2=tq;
    }

    int dy_total = y2 - y0;
    if (dy_total == 0) return;

    for (int y = y0; y < y2; y++) {
        if (y < t->cy1 || y > t->cy2) continue;

        int second_half = (y >= y1);
        int seg_height = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_height == 0) seg_height = 1;

        float alpha = (float)(y - y0) / (float)dy_total;
        float beta;
        if (second_half)
            beta = (float)(y - y1) / (float)seg_height;
        else
            beta = (float)(y - y0) / (float)seg_height;

        int xa, xb;
        psx_triangle_edges_at_y(x0, y0, x1, y1, x2, y2, y, &xa, &xb);

        float ua = u0 + (float)(u2 - u0) * alpha;
        float va = v0 + (float)(v2 - v0) * alpha;
        float qa = q0 + (q2 - q0) * alpha;
        float uqa = (float)u0 * q0 + ((float)u2 * q2 - (float)u0 * q0) * alpha;
        float vqa = (float)v0 * q0 + ((float)v2 * q2 - (float)v0 * q0) * alpha;
        float ub, vb;
        float qb, uqb, vqb;
        if (second_half) {
            ub = u1 + (float)(u2 - u1) * beta;
            vb = v1 + (float)(v2 - v1) * beta;
            qb = q1 + (q2 - q1) * beta;
            uqb = (float)u1 * q1 + ((float)u2 * q2 - (float)u1 * q1) * beta;
            vqb = (float)v1 * q1 + ((float)v2 * q2 - (float)v1 * q1) * beta;
        } else {
            ub = u0 + (float)(u1 - u0) * beta;
            vb = v0 + (float)(v1 - v0) * beta;
            qb = q0 + (q1 - q0) * beta;
            uqb = (float)u0 * q0 + ((float)u1 * q1 - (float)u0 * q0) * beta;
            vqb = (float)v0 * q0 + ((float)v1 * q1 - (float)v0 * q0) * beta;
        }

        if (xa > xb) {
            int tt = xa; xa = xb; xb = tt;
            float tf;
            tf = ua; ua = ub; ub = tf;
            tf = va; va = vb; vb = tf;
            tf = qa; qa = qb; qb = tf;
            tf = uqa; uqa = uqb; uqb = tf;
            tf = vqa; vqa = vqb; vqb = tf;
        }

        int span = xb - xa;
        if (span == 0) span = 1;

        int sx = max_i(xa, t->cx1);
        int ex = min_i(xb, t->cx2 + 1);

        for (int x = sx; x < ex; x++) {
            float t_val = (float)(x - xa) / (float)span;
            float fu = ua + (ub - ua) * t_val;
            float fv = va + (vb - va) * t_val;
            if (perspective) {
                float q = qa + (qb - qa) * t_val;
                if (q > 1.0e-12f) {
                    fu = (uqa + (uqb - uqa) * t_val) / q;
                    fv = (vqa + (vqb - vqa) * t_val) / q;
                }
            }

            if (g_texture_filter) {
                float sh = bilinear_center_shift_for_target(t);
                uint16_t texel = texel_fetch_bilinear(fu + sh, fv + sh,
                                                      texpage, clut_x, clut_y);
                put_textured(t, x, y, texel, g_mod_r, g_mod_g, g_mod_b, g_raw_texture);
            } else {
                uint16_t texel = texel_fetch((int)fu & 0xFF, (int)fv & 0xFF,
                                             texpage, clut_x, clut_y);
                put_textured(t, x, y, texel, g_mod_r, g_mod_g, g_mod_b, g_raw_texture);
            }
        }
    }
}

void sw_draw_textured_triangle(int x0, int y0, int u0, int v0,
                               int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage) {
    int64_t area2 = (int64_t)(x1 - x0) * (int64_t)(y2 - y0)
                  - (int64_t)(x2 - x0) * (int64_t)(y1 - y0);
    if (area2 == 0) { precise_consumed(); return; }

    if (g_texture_filter) {
        int xs[3] = {x0,x1,x2}, ys[3] = {y0,y1,y2};
        int us[3] = {u0,u1,u2}, vs[3] = {v0,v1,v2};
        sw_tri_uv_limits(xs, ys, us, vs);
    }

    RTarget n = rt_native();
    raster_textured_triangle(&n, x0, y0, u0, v0, x1, y1, u1, v1,
                             x2, y2, u2, v2, clut_x, clut_y, texpage,
                             g_perspective_valid,
                             g_perspective_q[0], g_perspective_q[1], g_perspective_q[2]);
    if (g_hr) {
        int s = g_scale;
        RTarget hr = rt_hires();
        raster_textured_triangle(&hr,
                                 precise_scaled(0,0,x0,s), precise_scaled(1,0,y0,s), u0, v0,
                                 precise_scaled(0,1,x1,s), precise_scaled(1,1,y1,s), u1, v1,
                                 precise_scaled(0,2,x2,s), precise_scaled(1,2,y2,s), u2, v2,
                                 clut_x, clut_y, texpage, g_perspective_valid,
                                 g_perspective_q[0], g_perspective_q[1], g_perspective_q[2]);
    }
    if (g_wide_cur) {
        int s = g_scale, dx = wide_dx();
        RTarget wt = rt_wide();
        WideBd bd = wide_bd_get();
        raster_textured_triangle(&wt,
                                 precise_wide_x(0,x0,s,dx,&bd), precise_scaled(1,0,y0,s), u0, v0,
                                 precise_wide_x(1,x1,s,dx,&bd), precise_scaled(1,1,y1,s), u1, v1,
                                 precise_wide_x(2,x2,s,dx,&bd), precise_scaled(1,2,y2,s), u2, v2,
                                 clut_x, clut_y, texpage, g_perspective_valid,
                                 g_perspective_q[0], g_perspective_q[1], g_perspective_q[2]);
    }
    precise_consumed();
}

static inline void color24_to_mod(uint32_t color, int *r, int *g, int *b) {
    *r = (int)((color >> 0) & 0xFF) >> 3;
    *g = (int)((color >> 8) & 0xFF) >> 3;
    *b = (int)((color >> 16) & 0xFF) >> 3;
}

static void raster_shaded_textured_triangle(const RTarget *t,
                                            int x0, int y0, int u0, int v0,
                                            int r0, int g0, int b0,
                                            int x1, int y1, int u1, int v1,
                                            int r1, int g1, int b1,
                                            int x2, int y2, int u2, int v2,
                                            int r2, int g2, int b2,
                                            uint16_t clut_x, uint16_t clut_y,
                                            uint16_t texpage, int raw_texture,
                                            int perspective,
                                            float q0, float q1, float q2) {
    /* Sort by Y, keeping UV and color modulation in sync */
    if (y0 > y1) {
        int tt;
        tt=x0; x0=x1; x1=tt; tt=y0; y0=y1; y1=tt;
        tt=u0; u0=u1; u1=tt; tt=v0; v0=v1; v1=tt;
        tt=r0; r0=r1; r1=tt; tt=g0; g0=g1; g1=tt; tt=b0; b0=b1; b1=tt;
        float tq=q0; q0=q1; q1=tq;
    }
    if (y0 > y2) {
        int tt;
        tt=x0; x0=x2; x2=tt; tt=y0; y0=y2; y2=tt;
        tt=u0; u0=u2; u2=tt; tt=v0; v0=v2; v2=tt;
        tt=r0; r0=r2; r2=tt; tt=g0; g0=g2; g2=tt; tt=b0; b0=b2; b2=tt;
        float tq=q0; q0=q2; q2=tq;
    }
    if (y1 > y2) {
        int tt;
        tt=x1; x1=x2; x2=tt; tt=y1; y1=y2; y2=tt;
        tt=u1; u1=u2; u2=tt; tt=v1; v1=v2; v2=tt;
        tt=r1; r1=r2; r2=tt; tt=g1; g1=g2; g2=tt; tt=b1; b1=b2; b2=tt;
        float tq=q1; q1=q2; q2=tq;
    }

    int dy_total = y2 - y0;
    if (dy_total == 0) return;

    for (int y = y0; y < y2; y++) {
        if (y < t->cy1 || y > t->cy2) continue;

        int second_half = (y >= y1);
        int seg_height = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_height == 0) seg_height = 1;

        float alpha = (float)(y - y0) / (float)dy_total;
        float beta;
        if (second_half)
            beta = (float)(y - y1) / (float)seg_height;
        else
            beta = (float)(y - y0) / (float)seg_height;

        int xa, xb;
        psx_triangle_edges_at_y(x0, y0, x1, y1, x2, y2, y, &xa, &xb);

        float ua = u0 + (float)(u2 - u0) * alpha;
        float va = v0 + (float)(v2 - v0) * alpha;
        float qa = q0 + (q2 - q0) * alpha;
        float uqa = (float)u0 * q0 + ((float)u2 * q2 - (float)u0 * q0) * alpha;
        float vqa = (float)v0 * q0 + ((float)v2 * q2 - (float)v0 * q0) * alpha;
        float ra = r0 + (float)(r2 - r0) * alpha;
        float ga = g0 + (float)(g2 - g0) * alpha;
        float ba = b0 + (float)(b2 - b0) * alpha;

        float ub, vb, qb, uqb, vqb, rb, gb, bb;
        if (second_half) {
            ub = u1 + (float)(u2 - u1) * beta;
            vb = v1 + (float)(v2 - v1) * beta;
            qb = q1 + (q2 - q1) * beta;
            uqb = (float)u1 * q1 + ((float)u2 * q2 - (float)u1 * q1) * beta;
            vqb = (float)v1 * q1 + ((float)v2 * q2 - (float)v1 * q1) * beta;
            rb = r1 + (float)(r2 - r1) * beta;
            gb = g1 + (float)(g2 - g1) * beta;
            bb = b1 + (float)(b2 - b1) * beta;
        } else {
            ub = u0 + (float)(u1 - u0) * beta;
            vb = v0 + (float)(v1 - v0) * beta;
            qb = q0 + (q1 - q0) * beta;
            uqb = (float)u0 * q0 + ((float)u1 * q1 - (float)u0 * q0) * beta;
            vqb = (float)v0 * q0 + ((float)v1 * q1 - (float)v0 * q0) * beta;
            rb = r0 + (float)(r1 - r0) * beta;
            gb = g0 + (float)(g1 - g0) * beta;
            bb = b0 + (float)(b1 - b0) * beta;
        }

        if (xa > xb) {
            int tt = xa; xa = xb; xb = tt;
            float tf;
            tf = ua; ua = ub; ub = tf;
            tf = va; va = vb; vb = tf;
            tf = qa; qa = qb; qb = tf;
            tf = uqa; uqa = uqb; uqb = tf;
            tf = vqa; vqa = vqb; vqb = tf;
            tf = ra; ra = rb; rb = tf;
            tf = ga; ga = gb; gb = tf;
            tf = ba; ba = bb; bb = tf;
        }

        int span = xb - xa;
        if (span == 0) span = 1;

        int sx = max_i(xa, t->cx1);
        int ex = min_i(xb, t->cx2 + 1);

        for (int x = sx; x < ex; x++) {
            float t_val = (float)(x - xa) / (float)span;
            float fu = ua + (ub - ua) * t_val;
            float fv = va + (vb - va) * t_val;
            if (perspective) {
                float q = qa + (qb - qa) * t_val;
                if (q > 1.0e-12f) {
                    fu = (uqa + (uqb - uqa) * t_val) / q;
                    fv = (vqa + (vqb - vqa) * t_val) / q;
                }
            }
            int mr = (int)(ra + (rb - ra) * t_val);
            int mg = (int)(ga + (gb - ga) * t_val);
            int mb = (int)(ba + (bb - ba) * t_val);
            if (mr < 0) mr = 0; if (mr > 31) mr = 31;
            if (mg < 0) mg = 0; if (mg > 31) mg = 31;
            if (mb < 0) mb = 0; if (mb > 31) mb = 31;

            if (g_texture_filter) {
                float sh = bilinear_center_shift_for_target(t);
                uint16_t texel = texel_fetch_bilinear(fu + sh, fv + sh,
                                                      texpage, clut_x, clut_y);
                put_textured(t, x, y, texel, mr, mg, mb, raw_texture);
            } else {
                uint16_t texel = texel_fetch((int)fu & 0xFF, (int)fv & 0xFF,
                                             texpage, clut_x, clut_y);
                put_textured(t, x, y, texel, mr, mg, mb, raw_texture);
            }
        }
    }
}

void sw_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0,
                                      uint32_t color0,
                                      int x1, int y1, int u1, int v1,
                                      uint32_t color1,
                                      int x2, int y2, int u2, int v2,
                                      uint32_t color2,
                                      uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage, int raw_texture) {
    int64_t area2 = (int64_t)(x1 - x0) * (int64_t)(y2 - y0)
                  - (int64_t)(x2 - x0) * (int64_t)(y1 - y0);
    if (area2 == 0) { precise_consumed(); return; }

    if (g_texture_filter) {
        int xs[3] = {x0,x1,x2}, ys[3] = {y0,y1,y2};
        int us[3] = {u0,u1,u2}, vs[3] = {v0,v1,v2};
        sw_tri_uv_limits(xs, ys, us, vs);
    }

    int r0, g0, b0, r1, g1, b1, r2, g2, b2;
    color24_to_mod(color0, &r0, &g0, &b0);
    color24_to_mod(color1, &r1, &g1, &b1);
    color24_to_mod(color2, &r2, &g2, &b2);

    RTarget n = rt_native();
    raster_shaded_textured_triangle(&n,
        x0, y0, u0, v0, r0, g0, b0,
        x1, y1, u1, v1, r1, g1, b1,
        x2, y2, u2, v2, r2, g2, b2,
        clut_x, clut_y, texpage, raw_texture, g_perspective_valid,
        g_perspective_q[0], g_perspective_q[1], g_perspective_q[2]);
    if (g_hr) {
        int s = g_scale;
        RTarget hr = rt_hires();
        raster_shaded_textured_triangle(&hr,
            precise_scaled(0,0,x0,s), precise_scaled(1,0,y0,s), u0, v0, r0, g0, b0,
            precise_scaled(0,1,x1,s), precise_scaled(1,1,y1,s), u1, v1, r1, g1, b1,
            precise_scaled(0,2,x2,s), precise_scaled(1,2,y2,s), u2, v2, r2, g2, b2,
            clut_x, clut_y, texpage, raw_texture, g_perspective_valid,
            g_perspective_q[0], g_perspective_q[1], g_perspective_q[2]);
    }
    if (g_wide_cur) {
        int s = g_scale, dx = wide_dx();
        RTarget wt = rt_wide();
        WideBd bd = wide_bd_get();
        raster_shaded_textured_triangle(&wt,
            precise_wide_x(0,x0,s,dx,&bd), precise_scaled(1,0,y0,s), u0, v0, r0, g0, b0,
            precise_wide_x(1,x1,s,dx,&bd), precise_scaled(1,1,y1,s), u1, v1, r1, g1, b1,
            precise_wide_x(2,x2,s,dx,&bd), precise_scaled(1,2,y2,s), u2, v2, r2, g2, b2,
            clut_x, clut_y, texpage, raw_texture, g_perspective_valid,
            g_perspective_q[0], g_perspective_q[1], g_perspective_q[2]);
    }
    precise_consumed();
}

/* ------------------------------------------------------------------ */
/* Flat rectangle                                                     */
/* ------------------------------------------------------------------ */

static void raster_flat_rect(const RTarget *t, int x, int y, int w, int h,
                             uint16_t color) {
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < t->cy1 || py > t->cy2) continue;

        int sx = max_i(x, t->cx1);
        int ex = min_i(x + w - 1, t->cx2);

        for (int px = sx; px <= ex; px++) {
            put_opaque(t, px, py, color);
        }
    }
}

void sw_draw_flat_rect(int x, int y, int w, int h, uint16_t color) {
    RTarget n = rt_native();
    raster_flat_rect(&n, x, y, w, h, color);
    if (g_hr) {
        int s = g_scale;
        RTarget hr = rt_hires();
        raster_flat_rect(&hr, x*s, y*s, w*s, h*s, color);
    }
    if (g_wide_cur) {
        int s = g_scale, dx = wide_dx();
        RTarget wt = rt_wide();
        /* Full-screen 2D overlay (pause gray-filter / load fade): a flat rect
         * spanning the whole 4:3 framebuffer must cover the whole wide surface
         * too, else the revealed 16:9 margins are left undimmed/unfaded. Detect
         * a rect whose native screen-X span (x - base) covers [0, native_w) and
         * extend its wide pass across the full surface; every other rect mirrors
         * 1:1 as before. native_w = the 4:3 framebuffer width (g_wide_w less the
         * per-side reveal on both sides); g_wide_cur_base is its VRAM left edge.
         * Only runs in native-wide (g_wide_cur != NULL), so 4:3 is unaffected. */
        int native_w = g_wide_w - 2 * g_wide_off;
        int lx = x - g_wide_cur_base, rx = x + w - g_wide_cur_base;
        WideBd bd = wide_bd_get();
        if (native_w > 0 && lx <= 0 && rx >= native_w)
            raster_flat_rect(&wt, 0, y*s, g_wide_w*s, h*s, color);
        else if (bd.on) {
            int xl = wide_bd_x(&bd, x), xr = wide_bd_x(&bd, x + w);
            raster_flat_rect(&wt, (xl+dx)*s, y*s, (xr-xl)*s, h*s, color);
        } else
            raster_flat_rect(&wt, (x+dx)*s, y*s, w*s, h*s, color);
    }
}

/* ------------------------------------------------------------------ */
/* Textured rectangle                                                 */
/*                                                                    */
/* Hi-res variant samples one native texel per native-pixel footprint */
/* (target coord / scale), keeping textures at native resolution while */
/* the rectangle's footprint is rendered at the higher resolution.    */
/* ------------------------------------------------------------------ */

static void raster_textured_rect(const RTarget *t, int x, int y, int w, int h,
                                 int u, int v,
                                 uint16_t clut_x, uint16_t clut_y,
                                 uint16_t texpage) {
    int s = t->s;
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < t->cy1 || py > t->cy2) continue;
        int tv = (v + row / s) & 0xFF;
        float fv = (float)v + (float)row / (float)s;

        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < t->cx1 || px > t->cx2) continue;

            uint16_t texel;
            if (g_texture_filter) {
                float fu = (float)u + (float)col / (float)s;
                float sh = bilinear_center_shift_for_target(t);
                texel = texel_fetch_bilinear(fu + sh, fv + sh, texpage, clut_x, clut_y);
            } else {
                int tu = (u + col / s) & 0xFF;
                texel = texel_fetch(tu, tv, texpage, clut_x, clut_y);
            }
            put_textured(t, px, py, texel, g_mod_r, g_mod_g, g_mod_b, g_raw_texture);
        }
    }
}

void sw_draw_textured_rect(int x, int y, int w, int h,
                           int u, int v,
                           uint16_t clut_x, uint16_t clut_y,
                           uint16_t texpage) {
    if (g_texture_filter) sw_rect_uv_limits(u, v, u + w, v + h);
    RTarget n = rt_native();
    raster_textured_rect(&n, x, y, w, h, u, v, clut_x, clut_y, texpage);
    if (g_hr) {
        int s = g_scale;
        RTarget hr = rt_hires();
        raster_textured_rect(&hr, x*s, y*s, w*s, h*s, u, v, clut_x, clut_y, texpage);
    }
    if (g_wide_cur) {
        int s = g_scale, dx = wide_dx();
        RTarget wt = rt_wide();
        WideBd bd = wide_bd_get();
        if (bd.on) {
            /* Stretch about screen centre: widen the destination span and map the
             * native texel footprint across it via the SCALED rasterizer (the
             * 1:1 sampler would TILE a widened rect instead of stretching it). */
            int xl = wide_bd_x(&bd, x), xr = wide_bd_x(&bd, x + w);
            raster_textured_rect_scaled(&wt, (xl+dx)*s, y*s, (xr-xl)*s, h*s,
                                        u, v, u + w, v + h, clut_x, clut_y, texpage);
        } else
            raster_textured_rect(&wt, (x+dx)*s, y*s, w*s, h*s, u, v, clut_x, clut_y, texpage);
    }
}

/* ---- MMX6 native-wide reveal-column tile (host-side, WIDE SURFACE ONLY) --------
 * Rasterize ONE 16x16 textured tile into the active wide surface for a given vertical
 * band, leaving canonical VRAM AND the hi-res mirror byte-identical. This fills the
 * 16:9 reveal margins with background columns the guest BG renderer (kept at its native
 * 21-column window) never draws — so the guest packet buffer / OT / 999-tile cap and the
 * 4:3 image are completely untouched (no guest-RAM mutation, elective, 4:3 identical).
 *
 * `x`,`y` are guest screen coords; `y` already includes the band offset. The clip is the
 * EXPLICIT band [band_y, band_y+240) — not g_clip — because this runs at the per-frame
 * fill/clear, before the guest applies E3/E4/E5 for the new frame. Texture state globals
 * are set transiently; the next real GP0 draw's setup overwrites them. */
void sw_wide_emit_tile(int band_y, int x, int y, int u, int v,
                       uint16_t clut_x, uint16_t clut_y, uint16_t texpage,
                       uint32_t color24, int semi_trans, int semi_mode, int raw_texture) {
    if (!g_wide_cur) return;
    int s = g_scale, dx = wide_dx();
    RTarget t;
    t.buf = g_wide_cur; t.w = g_wide_w * s; t.h = VRAM_HEIGHT * s; t.s = s;
    t.cx1 = 0;            t.cx2 = g_wide_w * s - 1;
    t.cy1 = band_y * s;   t.cy2 = (band_y + 240) * s - 1;
    g_mod_r = (uint8_t)((color24 & 0xff) >> 3);
    g_mod_g = (uint8_t)(((color24 >> 8) & 0xff) >> 3);
    g_mod_b = (uint8_t)(((color24 >> 16) & 0xff) >> 3);
    g_raw_texture        = raw_texture ? 1 : 0;
    g_semi_trans_enabled = semi_trans ? 1 : 0;
    g_semi_trans_mode    = semi_mode & 3;
    raster_textured_rect(&t, (x + dx) * s, y * s, 16 * s, 16 * s,
                         u, v, clut_x, clut_y, texpage);
}

static void raster_textured_rect_scaled(const RTarget *t, int x, int y,
                                        int w, int h,
                                        int u0, int v0, int u1, int v1,
                                        uint16_t clut_x, uint16_t clut_y,
                                        uint16_t texpage) {
    if (w <= 0 || h <= 0) return;

    int du = u1 - u0;
    int dv = v1 - v0;

    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < t->cy1 || py > t->cy2) continue;

        int tv = (int)(v0 + ((int64_t)dv * row) / h) & 0xFF;
        float fv = (float)v0 + (float)dv * (float)row / (float)h;
        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < t->cx1 || px > t->cx2) continue;

            uint16_t texel;
            if (g_texture_filter) {
                float fu = (float)u0 + (float)du * (float)col / (float)w;
                float sh = bilinear_center_shift_for_target(t);
                texel = texel_fetch_bilinear(fu + sh, fv + sh, texpage, clut_x, clut_y);
            } else {
                int tu = (int)(u0 + ((int64_t)du * col) / w) & 0xFF;
                texel = texel_fetch(tu, tv, texpage, clut_x, clut_y);
            }
            put_textured(t, px, py, texel, g_mod_r, g_mod_g, g_mod_b, g_raw_texture);
        }
    }
}

void sw_draw_textured_rect_scaled(int x, int y, int w, int h,
                                  int u0, int v0, int u1, int v1,
                                  uint16_t clut_x, uint16_t clut_y,
                                  uint16_t texpage) {
    if (w <= 0 || h <= 0) return;
    if (g_texture_filter) sw_rect_uv_limits(u0, v0, u1, v1);
    RTarget n = rt_native();
    raster_textured_rect_scaled(&n, x, y, w, h, u0, v0, u1, v1,
                                clut_x, clut_y, texpage);
    if (g_hr) {
        int s = g_scale;
        RTarget hr = rt_hires();
        /* Footprint scales by s; the destination span widens so the texel
         * step stays per-native-pixel. */
        raster_textured_rect_scaled(&hr, x*s, y*s, w*s, h*s, u0, v0, u1, v1,
                                    clut_x, clut_y, texpage);
    }
    if (g_wide_cur) {
        int s = g_scale, dx = wide_dx();
        RTarget wt = rt_wide();
        WideBd bd = wide_bd_get();
        int xl = wide_bd_x(&bd, x), xr = wide_bd_x(&bd, x + w);
        raster_textured_rect_scaled(&wt, (xl+dx)*s, y*s, (xr-xl)*s, h*s, u0, v0, u1, v1,
                                    clut_x, clut_y, texpage);
    }
}

/* ------------------------------------------------------------------ */
/* Line (Bresenham)                                                   */
/*                                                                    */
/* Lines are 1px on PS1. In the hi-res mirror each visited native     */
/* pixel is replicated as an s*s block so line thickness survives     */
/* downsampling (re-rasterizing a 1-hires-pixel line would vanish).   */
/* ------------------------------------------------------------------ */

static inline void hr_put_block_opaque(int nx, int ny, uint16_t color) {
    int s = g_scale;
    RTarget hr = rt_hires();
    int bx = nx * s, by = ny * s;
    for (int dy = 0; dy < s; dy++)
        for (int dx = 0; dx < s; dx++)
            put_opaque(&hr, bx + dx, by + dy, color);
}

/* Mirror a 1px line pixel into the active wide surface (s*s block, x-translated).
 * Self-gates on g_wide_cur; runs at any scale. */
static inline void wide_put_block_opaque(int nx, int ny, uint16_t color) {
    if (!g_wide_cur) return;
    int s = g_scale, tdx = wide_dx();
    RTarget wt = rt_wide();
    int bx = (nx + tdx) * s, by = ny * s;
    for (int dy = 0; dy < s; dy++)
        for (int dx = 0; dx < s; dx++)
            put_opaque(&wt, bx + dx, by + dy, color);
}

void sw_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    RTarget n = rt_native();
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        put_opaque(&n, x0, y0, color);
        if (g_hr) hr_put_block_opaque(x0, y0, color);
        wide_put_block_opaque(x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void sw_draw_shaded_line(int x0, int y0, uint16_t c0,
                         int x1, int y1, uint16_t c1) {
    RTarget n = rt_native();
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    /* Total steps for interpolation */
    int total = max_i(abs(x1 - x0), abs(y1 - y0));
    if (total == 0) total = 1;

    /* Extract 5-bit color channels */
    int r0 = c0 & 0x1F, g0 = (c0 >> 5) & 0x1F, b0 = (c0 >> 10) & 0x1F;
    int r1 = c1 & 0x1F, g1 = (c1 >> 5) & 0x1F, b1 = (c1 >> 10) & 0x1F;

    int step = 0;
    for (;;) {
        int r = r0 + (r1 - r0) * step / total;
        int g = g0 + (g1 - g0) * step / total;
        int b = b0 + (b1 - b0) * step / total;
        uint16_t color = (uint16_t)(r | (g << 5) | (b << 10));
        put_opaque(&n, x0, y0, color);
        if (g_hr) hr_put_block_opaque(x0, y0, color);
        wide_put_block_opaque(x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        step++;
    }
}

/* ------------------------------------------------------------------ */
/* VRAM pixel access                                                  */
/* ------------------------------------------------------------------ */

void sw_vram_write(int x, int y, uint16_t pixel) {
    x &= (VRAM_WIDTH - 1);
    y &= (VRAM_HEIGHT - 1);
    g_vram[y * VRAM_WIDTH + x] = pixel;
    sw_note_span(x,y,1);
    sw_dirty_row((uint32_t)y);

    if (g_hr) {
        int s = g_scale;
        int bx = x * s, by = y * s;
        for (int dy = 0; dy < s; dy++) {
            uint16_t *dst = g_hr + (size_t)(by + dy) * g_hr_w + bx;
            for (int dx = 0; dx < s; dx++) dst[dx] = pixel;
        }
    }
}

uint16_t sw_vram_read(int x, int y) {
    return vram_get(x, y);
}

/* ------------------------------------------------------------------ */
/* Bulk VRAM transfers                                                */
/* ------------------------------------------------------------------ */

/* Rebuild the supersampled mirror from canonical VRAM after a bulk replace.
 * Row-wise replication (memcpy) beats the per-texel nested loop used by the
 * general transfer path — savestate restore hits full 1024×512 often. */
static void hr_rebuild_from_vram(void) {
    if (!g_hr || !g_vram) return;
    const int s = g_scale;
    if (s <= 1) return;
    uint16_t *row = (uint16_t *)malloc((size_t)g_hr_w * sizeof(uint16_t));
    if (!row) {
        /* Fall back to slow per-pixel replication if OOM (should not happen). */
        for (int py = 0; py < VRAM_HEIGHT; py++) {
            for (int px = 0; px < VRAM_WIDTH; px++) {
                uint16_t pixel = g_vram[py * VRAM_WIDTH + px];
                int bx = px * s, by = py * s;
                for (int dy = 0; dy < s; dy++) {
                    uint16_t *dst = g_hr + (size_t)(by + dy) * (size_t)g_hr_w;
                    for (int dx = 0; dx < s; dx++)
                        dst[bx + dx] = pixel;
                }
            }
        }
        return;
    }
    for (int y = 0; y < VRAM_HEIGHT; y++) {
        const uint16_t *src = g_vram + (size_t)y * VRAM_WIDTH;
        for (int x = 0; x < VRAM_WIDTH; x++) {
            uint16_t p = src[x];
            uint16_t *d = row + (size_t)x * (size_t)s;
            for (int dx = 0; dx < s; dx++)
                d[dx] = p;
        }
        for (int dy = 0; dy < s; dy++) {
            memcpy(g_hr + ((size_t)(y * s + dy) * (size_t)g_hr_w),
                   row, (size_t)g_hr_w * sizeof(uint16_t));
        }
    }
    free(row);
}

void sw_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    if (!data || !g_vram || w <= 0 || h <= 0) return;

    /* Full-VRAM replace (savestate / boot_state): memcpy + one HR rebuild.
     * The general path is a wrapped per-pixel loop that, with supersampling,
     * does s² stores per guest texel — multi-second hitches on 2×/4×. */
    if (x == 0 && y == 0 && w == VRAM_WIDTH && h == VRAM_HEIGHT) {
        memcpy(g_vram, data, (size_t)VRAM_WIDTH * (size_t)VRAM_HEIGHT * sizeof(uint16_t));
        sw_dirty_all();
        if (g_hr) hr_rebuild_from_vram();
        return;
    }

    /* Contiguous non-wrapping rect, no HR: memcpy each row. */
    x &= (VRAM_WIDTH - 1);
    y &= (VRAM_HEIGHT - 1);
    if (!g_hr && x + w <= VRAM_WIDTH && y + h <= VRAM_HEIGHT) {
        for (int row = 0; row < h; row++) {
            memcpy(g_vram + (size_t)(y + row) * VRAM_WIDTH + (size_t)x,
                   data + (size_t)row * (size_t)w,
                   (size_t)w * sizeof(uint16_t));
        }
        sw_dirty_rect(x, y, w, h);
        return;
    }

    int idx = 0;
    int s = g_scale;
    for (int row = 0; row < h; row++) {
        int py = (y + row) & (VRAM_HEIGHT - 1);
        for (int col = 0; col < w; col++) {
            int px = (x + col) & (VRAM_WIDTH - 1);
            uint16_t pixel = data[idx++];
            g_vram[py * VRAM_WIDTH + px] = pixel;

            if (g_hr) {
                int bx = px * s, by = py * s;
                for (int dy = 0; dy < s; dy++) {
                    uint16_t *dst = g_hr + (size_t)((by + dy) % g_hr_h) * g_hr_w;
                    for (int dx = 0; dx < s; dx++)
                        dst[(bx + dx) % g_hr_w] = pixel;
                }
            }
        }
        sw_note_span(x,py,w);sw_dirty_row((uint32_t)py);
    }
}

void sw_vram_transfer_out(int x, int y, int w, int h, uint16_t *data) {
    if (!data || !g_vram || w <= 0 || h <= 0) return;

    if (x == 0 && y == 0 && w == VRAM_WIDTH && h == VRAM_HEIGHT) {
        memcpy(data, g_vram, (size_t)VRAM_WIDTH * (size_t)VRAM_HEIGHT * sizeof(uint16_t));
        return;
    }

    x &= (VRAM_WIDTH - 1);
    y &= (VRAM_HEIGHT - 1);
    if (x + w <= VRAM_WIDTH && y + h <= VRAM_HEIGHT) {
        for (int row = 0; row < h; row++) {
            memcpy(data + (size_t)row * (size_t)w,
                   g_vram + (size_t)(y + row) * VRAM_WIDTH + (size_t)x,
                   (size_t)w * sizeof(uint16_t));
        }
        return;
    }

    int idx = 0;
    for (int row = 0; row < h; row++) {
        int py = (y + row) & (VRAM_HEIGHT - 1);
        for (int col = 0; col < w; col++) {
            int px = (x + col) & (VRAM_WIDTH - 1);
            data[idx++] = g_vram[py * VRAM_WIDTH + px];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Display output — convert 15-bit VRAM to 32-bit RGBA                */
/* ------------------------------------------------------------------ */

static inline uint32_t rgb555_to_argb(uint16_t pix) {
    int r5 = (pix >>  0) & 0x1F;
    int g5 = (pix >>  5) & 0x1F;
    int b5 = (pix >> 10) & 0x1F;
    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g5 << 3) | (g5 >> 2));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
    /* ARGB8888 (matches gpu_display_pixel_argb): A<<24 | R<<16 | G<<8 | B. */
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b | 0xFF000000u;
}

int sw_render_display(uint32_t *out_pixels, int out_pitch,
                      int disp_x, int disp_y, int disp_w, int disp_h) {
    int count = 0;

    for (int row = 0; row < disp_h; row++) {
        int vy = (disp_y + row) & (VRAM_HEIGHT - 1);
        uint32_t *dst = (uint32_t *)((uint8_t *)out_pixels + row * out_pitch);

        for (int col = 0; col < disp_w; col++) {
            int vx = (disp_x + col) & (VRAM_WIDTH - 1);
            dst[col] = rgb555_to_argb(g_vram[vy * VRAM_WIDTH + vx]);
            count++;
        }
    }

    return count;
}

int sw_render_display_hires(uint32_t *out_pixels, int out_pitch,
                            int disp_x, int disp_y, int disp_w, int disp_h) {
    if (!g_hr || g_scale <= 1)
        return sw_render_display(out_pixels, out_pitch, disp_x, disp_y,
                                 disp_w, disp_h);

    int s = g_scale;
    int hx = disp_x * s;
    int hy = disp_y * s;
    int out_w = disp_w * s;
    int out_h = disp_h * s;
    int count = 0;

    for (int row = 0; row < out_h; row++) {
        int vy = (hy + row) % g_hr_h;
        const uint16_t *src = g_hr + (size_t)vy * g_hr_w;
        uint32_t *dst = (uint32_t *)((uint8_t *)out_pixels + row * out_pitch);

        for (int col = 0; col < out_w; col++) {
            int vx = (hx + col) % g_hr_w;
            dst[col] = rgb555_to_argb(src[vx]);
            count++;
        }
    }

    return count;
}

/* ------------------------------------------------------------------ */
/* Native-wide compositor surfaces                                    */
/* ------------------------------------------------------------------ */

static void wide_free_all(void) {
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (g_wide_surf[i]) { free(g_wide_surf[i]); g_wide_surf[i] = NULL; }
        g_wide_base[i] = -1;
    }
    g_wide_cur = NULL;
}

static uint16_t *wide_surf_for(int base_x) {
    if (g_wide_w <= 0) return NULL;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (g_wide_surf[i] && g_wide_base[i] == base_x) return g_wide_surf[i];
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (!g_wide_surf[i]) {
            size_t n = (size_t)(g_wide_w * g_scale) * (size_t)(VRAM_HEIGHT * g_scale);
            g_wide_surf[i] = (uint16_t *)calloc(n, sizeof(uint16_t));
            if (!g_wide_surf[i]) return NULL;
            g_wide_base[i] = base_x;
            return g_wide_surf[i];
        }
    }
    return NULL;  /* more distinct buffers than WIDE_MAX_SURF — shouldn't happen */
}

/* Enable native-wide with a wide width + centering offset (native px), or
 * disable (wide_w <= 0). Re-allocates if the width changed. */
void sw_wide_set_view(int enabled, int shift, int pad_left, int pad_right) {
    if (!enabled) shift = pad_left = pad_right = 0;
    if (view_enabled == enabled && view_shift == shift &&
        view_pad_left == pad_left && view_pad_right == pad_right) return;
    view_enabled = enabled;
    view_shift = shift;
    view_pad_left = pad_left;
    view_pad_right = pad_right;
}

void sw_wide_configure(int wide_w, int offset) {
    if (wide_w <= 0) { wide_free_all(); g_wide_w = 0; g_wide_off = 0; return; }
    if (wide_w != g_wide_w) wide_free_all();
    g_wide_w = wide_w;
    g_wide_off = offset;
}

/* Select the wide surface to mirror into for the back buffer at base_x. */
void sw_wide_set_target(int base_x) {
    g_wide_cur = wide_surf_for(base_x);
    g_wide_cur_base = base_x;
}

/* Stop mirroring (offscreen draws that don't target a framebuffer). */
void sw_wide_disable_target(void) { g_wide_cur = NULL; }

/* Mirror a framebuffer clear: fill the full wide width over [y, y+h) of the
 * surface for base_x, so the revealed margins are clean (not stale). */
void sw_wide_clear(int base_x, int y, int h, uint16_t color) {
    uint16_t *surf = wide_surf_for(base_x);
    if (!surf) return;
    int s = g_scale;
    int W = g_wide_w * s;
    int H = VRAM_HEIGHT * s;
    int y0 = y * s, y1 = (y + h) * s;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    for (int row = y0; row < y1; row++) {
        uint16_t *dst = surf + (size_t)row * W;
        for (int col = 0; col < W; col++) dst[col] = color;
    }
}

/* Clear only the synthetic columns outside the centred canonical framebuffer.
 * This provides a defined black void without disturbing guest-owned pixels. */
void sw_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    uint16_t *surf = wide_surf_for(base_x);
    if (!surf) return;
    int s = g_scale;
    int W = g_wide_w * s;
    int H = VRAM_HEIGHT * s;
    int margin = g_wide_off * s;
    int y0 = y * s, y1 = (y + h) * s;
    if (margin <= 0 || margin * 2 >= W) return;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    for (int row = y0; row < y1; row++) {
        uint16_t *dst = surf + (size_t)row * W;
        if (sides & 1) for (int col = 0; col < margin; col++) dst[col] = color;
        if (sides & 2) for (int col = W - margin; col < W; col++) dst[col] = color;
    }
}

/* An existing wide surface for base_x (never allocates: a source that was not
 * drawn yet has nothing to copy). */
static uint16_t *wide_surf_existing(int base_x) {
    if (g_wide_w <= 0) return NULL;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (g_wide_surf[i] && g_wide_base[i] == base_x) return g_wide_surf[i];
    return NULL;
}

/* Band texels count as bit 15 set: the band columns' own bit 15 is not
 * authoritative (margin fills write 0), and a framebuffer trail keeps the
 * centre at bit 15 once it blends, so the bands blend exactly like it. */
void sw_wide_feedback_rect(int src_base, int src_x, int src_y, int dst_x, int dst_y,
                           int w, int h, int r, int g, int b, int raw, int semi) {
    const uint16_t *src = wide_surf_existing(src_base);
    if (!src || !g_wide_cur || src == g_wide_cur || w <= 0 || h <= 0) return;
    RTarget t = rt_wide();
    const int s = t.s;
    const int sx0 = (src_x - src_base + g_wide_off + view_shift) * s;
    const int dx0 = (dst_x + wide_dx()) * s;
    const int saved_enabled = g_semi_trans_enabled, saved_mode = g_semi_trans_mode;
    g_semi_trans_enabled = semi >= 0;
    g_semi_trans_mode = semi >= 0 ? semi : saved_mode;
    for (int row = 0; row < h * s; row++) {
        const int sy = src_y * s + row;
        if (sy < 0 || sy >= t.h) continue;
        for (int col = 0; col < w * s; col++) {
            const int sx = sx0 + col;
            if (sx < 0 || sx >= t.w) continue;
            const uint16_t texel = src[(size_t)sy * t.w + sx];
            if (texel == 0x0000) continue;
            /* Bit 15 is forced: see sw_wide_feedback_rect's comment. put_textured
             * takes the factor sw_set_color_modulation stores. */
            put_textured(&t, dx0 + col, dst_y * s + row, (uint16_t)(texel | 0x8000),
                         r >> 3, g >> 3, b >> 3, raw);
        }
    }
    g_semi_trans_enabled = saved_enabled;
    g_semi_trans_mode = saved_mode;
}

/* Present source: convert the wide surface for the displayed buffer (base_x) to
 * ARGB. Output is (g_wide_w*scale) wide × (disp_h*scale) tall. Returns 0 if no
 * surface exists for base_x (caller falls back to the canonical present). */
int sw_render_wide_display(uint32_t *out_pixels, int out_pitch, int base_x,
                           int disp_y, int disp_h) {
    uint16_t *surf = NULL;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (g_wide_surf[i] && g_wide_base[i] == base_x) { surf = g_wide_surf[i]; break; }
    if (!surf || g_wide_w <= 0) return 0;
    int s = g_scale;
    int W = g_wide_w * s;
    int H = VRAM_HEIGHT * s;
    int out_h = disp_h * s;
    int count = 0;
    for (int row = 0; row < out_h; row++) {
        int vy = disp_y * s + row;
        if (vy < 0) vy = 0;
        if (vy >= H) vy = H - 1;
        const uint16_t *src = surf + (size_t)vy * W;
        uint32_t *dst = (uint32_t *)((uint8_t *)out_pixels + row * out_pitch);
        for (int col = 0; col < W; col++) { dst[col] = rgb555_to_argb(src[col]); count++; }
    }
    return count;
}

int sw_wide_width(void) { return g_wide_w; }

/* Diagnostic: dump the ENTIRE active wide surface for base_x (full g_wide_w*scale
 * × VRAM_HEIGHT*scale — BOTH double-buffer y-bands, all margins) to ARGB. Lets a
 * probe see exactly where the over-draw lands, independent of the per-frame band
 * the present selects. *ow/*oh receive the surface dims. Returns pixels written,
 * or 0 if no surface for base_x / it won't fit in cap_pixels. */
int sw_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh, int base_x) {
    uint16_t *surf = NULL;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (g_wide_surf[i] && g_wide_base[i] == base_x) { surf = g_wide_surf[i]; break; }
    if (!surf || g_wide_w <= 0) return 0;
    int s = g_scale;
    int W = g_wide_w * s, H = VRAM_HEIGHT * s;
    if ((long long)W * H > cap_pixels) return 0;
    for (int i = 0; i < W * H; i++) out[i] = rgb555_to_argb(surf[i]);
    if (ow) *ow = W;
    if (oh) *oh = H;
    return W * H;
}
