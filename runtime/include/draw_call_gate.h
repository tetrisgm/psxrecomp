#ifndef PSX_DRAW_CALL_GATE_H
#define PSX_DRAW_CALL_GATE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* A single worker rendezvous with its creating (owner) thread. The worker
 * keeps its native stack alive. Payload ownership remains with the caller;
 * the owner callback may inspect/update it only while servicing the request.
 * CPU registers, guest memory ownership and cycle charging belong to the
 * title adapter, not this transport. No automatic integration is enabled. */
typedef struct DrawCallGate DrawCallGate;
typedef int (*DrawCallServe)(void *payload, void *user);
enum {
    DRAW_CALL_REPLIED = 1, DRAW_CALL_CLOSED = 0,
    DRAW_CALL_TIMEOUT = -1, DRAW_CALL_BUSY = -2,
    DRAW_CALL_WRONG_THREAD = -3, DRAW_CALL_BAD_ARGUMENT = -4,
    DRAW_CALL_CALLBACK_FAILED = -5
};
DrawCallGate *draw_call_gate_create(void);
/* Worker only. timeout_ms bounds waiting for the owner to claim the request.
 * Once claimed, wait for callback completion even past that deadline: the
 * payload cannot be destroyed while the owner still accesses it. Zero means
 * immediate timeout unless the request is already claimed. No busy polling. */
int draw_call_gate_request(DrawCallGate *, void *payload, uint32_t timeout_ms,
                          int *reply);
/* Owner only, nonblocking if no pending request. Executes one callback on the
 * owner thread. Returns 1 serviced, 0 none, or a negative refusal. Recursive
 * service/close/destroy from the callback is refused. */
int draw_call_gate_service(DrawCallGate *, DrawCallServe, void *user);
/* Owner only. Close refuses new requests and wakes an unclaimed requester.
 * Destroy also waits for that requester to leave before freeing the gate.
 * Callers must prevent any new access after destroy begins. */
int draw_call_gate_close(DrawCallGate *);
int draw_call_gate_destroy(DrawCallGate *);
#ifdef __cplusplus
}
#endif
#endif
