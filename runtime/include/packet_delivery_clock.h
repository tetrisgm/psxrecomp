#ifndef PSX_PACKET_DELIVERY_CLOCK_H
#define PSX_PACKET_DELIVERY_CLOCK_H
#include <cstdint>

/* Host-only jitter buffer clock. Guest timestamps are elapsed VBlank time,
 * never packet arrival time. A late packet does not change the mapping unless
 * the bounded recovery horizon is exceeded. No feedback reaches the guest. */
class PacketDeliveryClock {
    uint64_t guest_origin=0,host_origin=0,last_guest=0,epoch=0;
    bool initialized=false;
public:
    static constexpr uint64_t max_delay_ns=100000000;
    uint64_t resyncs=0;
    void reset() {initialized=false;}
    uint64_t map(uint64_t guest,uint64_t timeline,uint64_t arrival,uint64_t delay) {
        delay=delay>max_delay_ns?max_delay_ns:delay;
        bool reset_needed=!initialized || timeline!=epoch || guest<=last_guest;
        uint64_t due=0;
        if(!reset_needed) {
            const uint64_t delta=guest-guest_origin;
            reset_needed=delta>UINT64_MAX-host_origin;
            if(!reset_needed) {
                due=host_origin+delta;
                reset_needed=(arrival>due && arrival-due>max_delay_ns) ||
                             (due>arrival && due-arrival>max_delay_ns+delay);
            }
        }
        if(reset_needed) {
            if(initialized) ++resyncs;
            guest_origin=guest;host_origin=delay>UINT64_MAX-arrival?UINT64_MAX:arrival+delay;due=host_origin;
            initialized=true;epoch=timeline;
        }
        last_guest=guest;
        return due;
    }
};
#endif
