#ifndef PSX_AUDIO_TRANSPORT_H
#define PSX_AUDIO_TRANSPORT_H

#include "recomp_audio_drc.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

/* One guest producer and one device consumer. The resampler, its history and
 * controller belong exclusively to the device callback. Init/free require a
 * stopped callback, just as device open/close do. No callback allocation. */
class PsxAudioTransport {
    static_assert(std::atomic<uint64_t>::is_always_lock_free &&
                  std::atomic<double>::is_always_lock_free,"audio transport requires lock-free counters");
    static constexpr uint64_t capacity=16384;
    alignas(64) std::atomic<uint64_t> written{0};
    alignas(64) std::atomic<uint64_t> consumed{0};
    int16_t pcm[capacity*2]{};
    rab_bridge bridge{};
    std::atomic<bool> resume_requested{false};
    std::atomic<uint64_t> discard_before{UINT64_MAX};
    std::atomic<uint64_t> pushed{0},pulled{0},underruns{0},overflow{0},input_drops{0};
    std::atomic<double> out_position{0},correction{0};
public:
    rab_config cfg{}; /* immutable between init and free */
    int init(const rab_config &config) {
        cfg=config;
        written=0;consumed=0;resume_requested=false;discard_before=UINT64_MAX;
        pushed=0;pulled=0;underruns=0;overflow=0;input_drops=0;
        correction=0;
        const int result=rab_init(&bridge,&cfg);
        out_position=bridge.out_pos;
        return result;
    }
    void free() { rab_free(&bridge); }
    void push(const int16_t *samples,int frames) {
        if(frames<=0) return;
        const uint64_t w=written.load(std::memory_order_relaxed);
        const uint64_t r=consumed.load(std::memory_order_acquire);
        const uint64_t count=std::min<uint64_t>(frames,capacity-(w-r));
        for(uint64_t at=0;at<count;) {
            const uint64_t slot=(w+at)%capacity;
            const uint64_t n=std::min(count-at,capacity-slot);
            std::memcpy(pcm+slot*cfg.channels,samples+at*cfg.channels,
                        n*cfg.channels*sizeof(int16_t));
            at+=n;
        }
        written.store(w+count,std::memory_order_release);
        pushed.fetch_add(frames,std::memory_order_relaxed);
        input_drops.fetch_add(uint64_t(frames)-count,std::memory_order_relaxed);
    }
    void pull(int16_t *out,int frames) {
        uint64_t r=consumed.load(std::memory_order_relaxed);
        uint64_t w=written.load(std::memory_order_acquire);
        /* Read the reset after the producer cursor: a newly published batch
         * must observe its preceding timeline change before it is consumed.
         * A later reset can include more input than that first cursor read. */
        const uint64_t cut=discard_before.exchange(UINT64_MAX,std::memory_order_acquire);
        if(cut!=UINT64_MAX) {
            w=written.load(std::memory_order_acquire);
            r=cut;
            consumed.store(r,std::memory_order_release);
            std::memset(bridge.ring,0,size_t(bridge.cap)*cfg.channels*sizeof(float));
            bridge.in_count=cut;
            bridge.out_pos=double(cut)+bridge.half;
            bridge.primed=0;bridge.gain=0;
            bridge.last_out[0]=bridge.last_out[1]=0;
            rab_resume_after_sink(&bridge);
        }
        while(r<w) {
            const uint64_t slot=r%capacity;
            const uint64_t n=std::min(w-r,capacity-slot);
            rab_push(&bridge,pcm+slot*cfg.channels,int(n));
            r+=n;
            consumed.store(r,std::memory_order_release);
        }
        if(resume_requested.exchange(false,std::memory_order_acquire))
            rab_resume_after_sink(&bridge);
        rab_pull(&bridge,out,frames);
        rab_stats s;rab_get_stats(&bridge,&s);
        pulled.store(s.pulled_frames,std::memory_order_relaxed);
        underruns.store(s.underrun_events,std::memory_order_relaxed);
        overflow.store(s.overflow_drops,std::memory_order_relaxed);
        out_position.store(bridge.out_pos,std::memory_order_release);
        correction.store(s.last_correction,std::memory_order_relaxed);
    }
    void resume_after_sink() { resume_requested.store(true,std::memory_order_release); }
    /* Producer-only: discard the old timeline at the next device callback.
     * Input published after this call is retained. No device lock or wait. */
    void reset_timeline() {
        discard_before.store(written.load(std::memory_order_relaxed),std::memory_order_release);
    }
    double fill_ms() const {
        const uint64_t w=written.load(std::memory_order_acquire);
        /* One monotonic domain covers both queues. Subtracting transport
         * occupancy from a separately published resampler fill would briefly
         * lose the transferred batch and under-report buffering. */
        return std::max(0.0,double(w)-out_position.load(std::memory_order_acquire))*1000.0/cfg.source_rate;
    }
    void stats(rab_stats *out) const {
        out->pushed_frames=pushed.load(std::memory_order_relaxed);
        out->pulled_frames=pulled.load(std::memory_order_relaxed);
        out->underrun_events=underruns.load(std::memory_order_relaxed);
        out->overflow_drops=overflow.load(std::memory_order_relaxed)+input_drops.load(std::memory_order_relaxed);
        out->last_fill_ms=fill_ms();
        out->last_correction=correction.load(std::memory_order_relaxed);
    }
};
#endif
