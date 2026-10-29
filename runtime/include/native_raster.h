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

/* CPU-native provenance observation, ordered with the original writes. All
 * pointers are borrowed only for the callback. before is 1 before the write,
 * 0 after; restore marks a full machine restore. For GP0, state/packet are
 * provided; otherwise command/upload describe the native operation. These
 * callbacks run on the native worker, must not call GL or re-enter its queue,
 * and cannot modify VRAM. Observer user storage must outlive registration. */
typedef struct NativeRasterEvent {
    int before, restore;
    int has_writes, written_bounds[4]; /* post-write conservative native spans */
    const NativeRasterCommand *command;
    const NativeRasterState *state;
    const uint32_t *packet;
    size_t packet_words;
    const uint16_t *upload;
    size_t upload_words;
    const uint16_t *vram;
} NativeRasterEvent;
typedef void (*NativeRasterObserver)(void *user,const NativeRasterEvent *event);
typedef void (*NativeRasterVisit)(void *user,const uint16_t *vram);
/* CPU-only barriers. Registration and visits occur after all preceding native
 * work, without touching presentation. Visits configure/capture provenance
 * on its owner, and may not retain the borrowed VRAM pointer after shutdown. */
void native_raster_set_observer(NativeRaster *raster,NativeRasterObserver observer,void *user);
void native_raster_visit(NativeRaster *raster,NativeRasterVisit visit,void *user);
/* Complete fixed-length draw/fill/copy packets only. Environment and partial
 * transfers use the existing state/fragment operations. */
int native_raster_gp0(NativeRaster *raster,const NativeRasterState *state,
                      const uint32_t *words,size_t count);
/* Same submission, returning a monotonic source identity (zero on failure).
 * All enhanced primitives derived from this GP0 share its texture input. */
uint64_t native_raster_gp0_identified(NativeRaster *raster,const NativeRasterState *state,
                                      const uint32_t *words,size_t count);

/* Immutable native texture inputs captured BEFORE each textured GP0 executes.
 * Only sampled pages/CLUTs are retained, in shared 64x64 native tiles. Writes
 * invalidate future versions without changing any packet's held inputs.
 * This is presentation data; native GPU cache/guest semantics are unchanged.
 * Enable/seal are CPU-only ordered barriers. Seal starts the next collection.
 * An over-budget/allocation-failed collection is invalid and the whole
 * presentation packet must be dropped; native guest work is never dropped. */
typedef struct NativeRasterInputs NativeRasterInputs;
typedef struct NativeRasterInputMetadata {
    void *data;
    size_t bytes;
    void (*release)(void*);
} NativeRasterInputMetadata;
/* Optional immutable provenance captured on the native worker with each
 * sampled input. A successful callback transfers one retained reference.
 * Failure invalidates presentation only. Registration is a CPU barrier. */
typedef int (*NativeRasterMetadataCapture)(void *user,NativeRasterInputMetadata *out);
void native_raster_inputs_metadata_provider(NativeRaster *raster,
                                            NativeRasterMetadataCapture capture,void *user);
typedef struct NativeRasterTile {
    uint32_t x,y;
    uint64_t version;
    const uint16_t *pixels; /* 64x64, borrowed until inputs destruction */
} NativeRasterTile;
void native_raster_inputs_enable(NativeRaster *raster,int enabled);
NativeRasterInputs *native_raster_inputs_seal(NativeRaster *raster);
void native_raster_inputs_destroy(NativeRasterInputs *inputs);
int native_raster_inputs_valid(const NativeRasterInputs *inputs);
size_t native_raster_inputs_bytes(const NativeRasterInputs *inputs);
/* Returns tile count, zero for a non-textured source, -1 for invalid inputs
 * or insufficient output capacity. It never silently truncates a page. */
int native_raster_inputs_get(const NativeRasterInputs *inputs,uint64_t source,
                             NativeRasterTile *tiles,size_t capacity);
const void *native_raster_inputs_metadata(const NativeRasterInputs *inputs,uint64_t source);

NativeRaster *native_raster_create(const uint16_t *initial_vram);
void native_raster_destroy(NativeRaster *raster);
/* Returns 0 for invalid opcode/upload dimensions or allocation failure. */
int native_raster_submit(NativeRaster *raster,
                         const NativeRasterCommand *command,
                         const uint16_t *upload, size_t words);
/* Reads fence only preceding native work. With non-NULL dirty_rows, a snapshot
 * consumes this instance's dirty-row mask, never the global tracker. NULL
 * leaves dirty tracking intact (immutable presentation seeds are observers,
 * not save/rewind checkpoints). */
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
uint64_t native_raster_completed(NativeRaster *raster);
#ifdef __cplusplus
}
#endif
#endif
