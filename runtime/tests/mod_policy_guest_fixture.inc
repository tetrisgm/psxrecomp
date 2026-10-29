/* Isolated package-policy tests link real override registration, but never
 * execute a guest override or service devices. */
#include "cpu_state.h"
#include <cstdlib>
extern "C" {
uint64_t psx_cycle_count,psx_next_service_cycle;
int psx_in_device_service,g_event_step_conservative,g_ls_replay_active;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
void psx_devices_service_to_now(void) { std::abort(); }
void psx_advance_cycles_slow(uint32_t) { std::abort(); }
void psx_dispatch_call(CPUState*,uint32_t,uint32_t) { std::abort(); }
uint32_t psx_read_word(uint32_t);
uint32_t psx_peek_word_untraced(uint32_t a) { return psx_read_word(a); }
}
