#ifndef PSX_NATIVE_RASTER_H
#define PSX_NATIVE_RASTER_H
#include <stddef.h>
#include <stdint.h>
#include "gpu_sw_renderer.h"
#ifdef __cplusplus
extern "C" {
#endif

/* One guest producer, one CPU-only consumer. No graphics API or render-queue
 * dependency is permitted here. All command arguments and upload bytes are
 * owned by the queue after submit returns. Native work is never dropped. */
typedef struct NativeRaster NativeRaster;
typedef enum NativeRasterOp {
    NR_AREA, NR_OFFSET, NR_SEMI, NR_MASK, NR_WINDOW, NR_MODULATION,
    NR_FILL, NR_COPY, NR_FLAT_TRIANGLE, NR_GOURAUD_TRIANGLE,
    NR_TEXTURED_TRIANGLE, NR_SHADED_TEXTURED_TRIANGLE,
    NR_FLAT_RECT, NR_TEXTURED_RECT, NR_TEXTURED_RECT_SCALED,
    NR_LINE, NR_SHADED_LINE, NR_WRITE, NR_UPLOAD,
    /* GP0(A0) payload fragment: x,y,w,h,pixel_offset,set_mask,check_mask.
     * Upload contains the halfwords actually received so far (odd final
     * halfword excluded). Unlike NR_UPLOAD, this applies guest mask rules. */
    NR_CPU_UPLOAD,
    /* Three (x,y,RGB888,u,v) vertices, CLUT x/y, texpage, native flags. */
    NR_NATIVE_TRIANGLE,
    /* RGB888 endpoints: x,y,rgb,x,y,rgb,flags (Gouraud=4,dither=8). */
    NR_NATIVE_LINE,
    /* x,y,w,h,rgb,u,v,clut_x,clut_y,page,flags (texture=1,raw=2,
     * horizontal texture flip=16,vertical texture flip=32). */
    NR_NATIVE_RECT,
    NR_CACHE_CLEAR
} NativeRasterOp;
typedef struct NativeRasterCommand {
    NativeRasterOp op;
    /* Arguments in the order of the corresponding sw_* function. */
    int32_t args[19];
} NativeRasterCommand;

/* State is captured with the original packet, before any presentation-only
 * transforms. Offsets are signed guest coordinates, page is GP0(E1)'s low
 * 14 bits (including sprite flips); textured polygons supply their own page. */
typedef struct NativeRasterState {
    int32_t left,top,right,bottom,offset_x,offset_y;
    uint32_t page,window,set_mask,check_mask;
    /* 0 draws every line; 1 skips even native rows, 2 skips odd rows. */
    uint32_t skip_field;
} NativeRasterState;
/* Complete fixed-length draw/fill/copy packets only. Environment and partial
 * transfers use the existing state/fragment operations. */
int native_raster_gp0(NativeRaster *raster,const NativeRasterState *state,
                      const uint32_t *words,size_t count);

NativeRaster *native_raster_create(const uint16_t *initial_vram);
void native_raster_destroy(NativeRaster *raster);
/* Returns 0 for invalid opcode/upload dimensions or allocation failure. */
int native_raster_submit(NativeRaster *raster,
                         const NativeRasterCommand *command,
                         const uint16_t *upload, size_t words);
/* Reads fence only preceding native work. Snapshot also consumes this
 * instance's dirty-row mask, never the renderer's global dirty tracker. */
void native_raster_read(NativeRaster *raster, int x, int y, int w, int h,
                        uint16_t *destination);
void native_raster_snapshot(NativeRaster *raster, uint16_t *destination,
                            uint64_t dirty_rows[8]);
/* Barrier followed by replacement; draw state is retained. */
void native_raster_restore(NativeRaster *raster, const uint16_t *source);
/* Machine checkpoints preserve stale-but-valid texture/palette entries as
 * well as VRAM. A VRAM-only restore above invalidates caches deliberately. */
void native_raster_checkpoint(NativeRaster *raster,uint16_t *vram,
                              uint64_t dirty_rows[8],SwNativeTextureState *texture);
void native_raster_restore_checkpoint(NativeRaster *raster,const uint16_t *vram,
                                      const SwNativeTextureState *texture);
/* Caller-owned destinations remain alive until release; release waits for
 * CPU completion even when the raster itself has already been destroyed. */
typedef struct NativeRasterCheckpoint NativeRasterCheckpoint;
NativeRasterCheckpoint *native_raster_checkpoint_async(NativeRaster *raster,
    uint16_t *vram,uint64_t dirty_rows[8],SwNativeTextureState *texture);
int native_raster_checkpoint_ready(NativeRasterCheckpoint *checkpoint);
void native_raster_checkpoint_release(NativeRasterCheckpoint *checkpoint);
uint64_t native_raster_completed(NativeRaster *raster);
#ifdef __cplusplus
}
#endif
#endif
