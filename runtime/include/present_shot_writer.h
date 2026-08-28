#ifndef PSXRECOMP_PRESENT_SHOT_WRITER_H
#define PSXRECOMP_PRESENT_SHOT_WRITER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Takes ownership of an RGB malloc() buffer and completes the existing
 * present_shot sequence from a worker thread. */
void present_shot_write_async(const char *path, uint8_t *rgb,
                              uint32_t width, uint32_t height);
void present_shot_writer_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
