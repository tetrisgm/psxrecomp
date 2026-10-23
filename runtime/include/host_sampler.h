#ifndef PSX_HOST_SAMPLER_H
#define PSX_HOST_SAMPLER_H
/* Always-on host CPU sampler for the emulation thread (debug-tools builds).
 * See host_sampler.c; TCP host_profile; tools/host_profile.py. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 Mi samples: ~17 minutes at 1 kHz, enough to keep a whole load-transition route (16 MB,
 * debug-tools builds only) so a hitch found afterwards still has its host call sites. */
#define HOST_SAMPLER_CAP 1048576u

typedef struct HostSample {
    uint64_t rva;      /* instruction pointer minus the executable's base */
    uint32_t frame;    /* guest frame (s_frame_count) */
    uint16_t in_pass;  /* 1 while a render pass ran */
    uint16_t phase;    /* the emulation thread's g_exec_phase at the sample
                          (0 other, 1 interp, 2 native, 3 static, 4 GP0) */
    uint64_t caller;   /* PSX_HOST_SAMPLER_CALLERS (Windows): for a pc outside
                          the executable, its innermost exe caller (rva), else 0 */
} HostSample;

/* Start sampling the calling thread (the emulation thread). Idempotent. */
void host_sampler_start(void);
/* Windows: when PSX_HOST_SAMPLER_THREAD equals `name`, sample the calling
 * thread from now on ("render": the render thread). No-op elsewhere. */
void host_sampler_retarget_current(const char *name);
int host_sampler_supported(void);
/* Total samples recorded; the ring holds the newest HOST_SAMPLER_CAP. */
uint64_t host_sampler_seq(void);
uint64_t host_sampler_image_base(void);
/* Sample `seq`, if it is still in the ring. */
int host_sampler_get(uint64_t seq, HostSample *out);

#ifdef __cplusplus
}
#endif
#endif
