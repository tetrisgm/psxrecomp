#ifndef PSX_NATIVE_RASTER_H
#define PSX_NATIVE_RASTER_H
#include <stddef.h>
#include <stdint.h>
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
    NR_NATIVE_TRIANGLE
} NativeRasterOp;
typedef struct NativeRasterCommand {
    NativeRasterOp op;
    /* Arguments in the order of the corresponding sw_* function. */
    int32_t args[19];
} NativeRasterCommand;

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
uint64_t native_raster_completed(NativeRaster *raster);
#ifdef __cplusplus
}
#endif
#endif
