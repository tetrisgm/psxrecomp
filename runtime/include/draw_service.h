#ifndef PSX_DRAW_SERVICE_H
#define PSX_DRAW_SERVICE_H
#ifdef __cplusplus
extern "C" {
#endif
/* Main-thread-only admission of completed private draws. The callback must
 * not run guest code, change guest state, wait for workers, or longjmp.
 * Register/unregister on the owning emulation thread. Polling elsewhere is
 * harmless: a renderer's pacing loop must never parse GPU work for main. */
typedef void (*DrawServicePoll)(void *user);
void draw_service_register(DrawServicePoll callback, void *user);
void draw_service_poll(void);
int draw_service_active(void);
extern int g_draw_service_enabled; /* main interrupt hot-path gate only */
#ifdef __cplusplus
}
#endif
#endif
