#include "mednafen/psx/gpu.h"
#include "pgxp/pgxp_main.h"
#include <string.h>
#ifdef _WIN32
#define ORACLE_EXPORT __declspec(dllexport)
#else
#define ORACLE_EXPORT __attribute__((visibility("default")))
#endif
extern PS_GPU GPU;
ORACLE_EXPORT int oracle_init(void) {
    PGXP_Init();
    PGXP_SetModes(PGXP_MODE_MEMORY|PGXP_MODE_CPU|PGXP_MODE_GTE|
                  PGXP_VERTEX_CACHE|PGXP_TEXTURE_CORRECTION|PGXP_NCLIP_IMPL);
    return GPU_Init(false,0,239,0);
}
ORACLE_EXPORT void oracle_reset(const uint16_t *vram) {
    GPU_Power();
    memcpy(GPU.vram,vram,1024*512*2);
}
ORACLE_EXPORT void oracle_write(const uint32_t *words,unsigned count) {
    for(unsigned i=0;i<count;++i) {
        GPU.DrawTimeAvail=100000000;
        GPU_WriteDMA(words[i],0);
    }
}
ORACLE_EXPORT void oracle_read(uint16_t *vram) {
    memcpy(vram,GPU.vram,1024*512*2);
}
