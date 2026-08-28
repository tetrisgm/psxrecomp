/* Lightweight production-speed present_shot TCP listener.
 *
 * Unlike debug_server.h this interface never enables generated-block tracing,
 * frame history, watchpoints, or sampler threads.  The listener is compiled
 * into every runtime for a stable source graph, but main.cpp calls it only in
 * an explicit PSX_PRESENT_SHOT_LISTENER build.
 */
#ifndef PSXRECOMP_PRESENT_SHOT_LISTENER_H
#define PSXRECOMP_PRESENT_SHOT_LISTENER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void present_shot_listener_init(int port);
void present_shot_listener_poll(void);
void present_shot_listener_shutdown(void);

/* Pure protocol seam used by the focused test.  The renderer functions it
 * calls are supplied by main.cpp in production and by deterministic stubs in
 * the test.  Returns the response length, including its trailing newline. */
int present_shot_listener_process_request(const char *request,
                                          char *response,
                                          size_t response_size);

#ifdef __cplusplus
}
#endif

#endif
