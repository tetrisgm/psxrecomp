#define RECOMP_AUDIO_DRC_IMPL
#include "audio_transport.h"
#undef NDEBUG
#include <cassert>
#include <thread>
#include <vector>
#include <cstdio>

int main() {
    rab_config config;rab_config_defaults(&config);
    config.source_rate=44100;config.host_rate=48000;
    PsxAudioTransport transport;
    rab_bridge reference;
    assert(!transport.init(config,false));assert(!rab_init(&reference,&config));
    int16_t input[2048*2],actual[1024*2],expected[1024*2];
    /* Exact waveform/controller parity through repeated wrap, startup,
     * depletion, overflow, resume and non-integral resampling. */
    for(int tick=0;tick<2000;++tick) {
        const int n=tick%23==0?2048:tick%29==0?0:735;
        for(int i=0;i<n*2;++i) input[i]=int16_t((tick*1470+i)*97);
        transport.push(input,n);rab_push(&reference,input,n);
        if(tick%127==0) {transport.resume_after_sink();rab_resume_after_sink(&reference);}
        transport.pull(actual,1024);rab_pull(&reference,expected,1024);
        assert(!std::memcmp(actual,expected,sizeof actual));
        rab_stats a,b;transport.stats(&a);rab_get_stats(&reference,&b);
        assert(a.pushed_frames==b.pushed_frames && a.pulled_frames==b.pulled_frames);
        assert(a.underrun_events==b.underrun_events && a.overflow_drops==b.overflow_drops);
    }
    transport.free();rab_free(&reference);
    /* A load discards already-resampled and pending old-timeline PCM, but
     * retains samples published after the reset request. No callback-side
     * allocation or producer-side device synchronization is involved. */
    assert(!transport.init(config,false));
    for(auto &v:input) v=24000;
    for(int i=0;i<6;++i) transport.push(input,2048);
    transport.pull(actual,1024);
    transport.push(input,2048);
    transport.reset_timeline();
    for(auto &v:input) v=-12000;
    for(int i=0;i<6;++i) transport.push(input,2048);
    transport.pull(actual,1024);
    for(auto v:actual) assert(v<=0 && v>=-12001);
    assert(actual[2000]<-11990);
    transport.reset_timeline();transport.reset_timeline();
    transport.pull(actual,1024);
    for(auto v:actual) assert(v==0);
    transport.free();
    assert(!transport.init(config,false));
    std::atomic<bool> done{false};
    std::thread producer([&]{
        int16_t constant[256*2];for(auto &v:constant)v=8192;
        for(int block=0;block<4000;++block) {
            while(transport.fill_ms()>220) std::this_thread::yield();
            if(block%37==0) transport.reset_timeline();
            transport.push(constant,256);
        }
        done.store(true,std::memory_order_release);
    });
    while(!done.load(std::memory_order_acquire)) {
        transport.pull(actual,256);
        for(int i=0;i<512;++i) assert(actual[i]>=0 && actual[i]<=8193);
    }
    producer.join();
    rab_stats stats;transport.stats(&stats);
    assert(stats.pushed_frames==4000u*256u && stats.overflow_drops==0);
    transport.free();
    puts("audio SPSC transport: exact resampler parity and concurrent wrap passed");
}
