#define RECOMP_AUDIO_DRC_IMPL
#include "audio_transport.h"
#undef NDEBUG
#include <cassert>
#include <cstdio>
int main() {
    rab_config cfg;rab_config_defaults(&cfg);cfg.source_rate=cfg.host_rate=44100;
    PsxAudioTransport audio;assert(!audio.init(cfg));
    int16_t pcm[10000*2]={},out[441*2];uint64_t ns=1000000000;int pending=0;
    for(int i=0;i<3000;++i) {
        ns+=10000000;pending+=441;
        // Periodic 40 ms producer scheduling gaps, then exact catch-up PCM.
        if(i<200 || i%200>=4) {audio.push(pcm,pending);pending=0;}
        audio.pull(out,441,ns);
    }
    rab_stats st;audio.stats(&st);
    printf("jitter: target %.2f fill %.2f starved %llu drops %llu\n",audio.target_ms(),audio.fill_ms(),(unsigned long long)st.underrun_events,(unsigned long long)st.overflow_drops);
    assert(!st.underrun_events && !st.overflow_drops);
    assert(audio.target_ms()>=60 && audio.target_ms()<=120);
    // An outage beyond the maximum reserve MUST remain visible in counters.
    const double prior=audio.target_ms();
    for(int i=0;i<25;++i){ns+=10000000;audio.pull(out,441,ns);}
    audio.stats(&st);assert(st.underrun_events);assert(audio.target_ms()>prior || prior==120);
    const auto gaps=st.underrun_events;const double grown=audio.target_ms();
    for(int i=0;i<9000;++i){ns+=10000000;audio.push(pcm,441);audio.pull(out,441,ns);}
    audio.stats(&st);
    printf("recovered: target %.2f fill %.2f starved %llu\n",audio.target_ms(),audio.fill_ms(),(unsigned long long)st.underrun_events);
    assert(st.underrun_events==gaps && !st.overflow_drops);
    assert(audio.target_ms()<grown && audio.target_ms()>=60);
    audio.free();
}
