#define RECOMP_AUDIO_DRC_IMPL
#include "audio_transport.h"
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
int main() {
    for(double skew : {-0.015,0.0,0.015}) {
        rab_config cfg;rab_config_defaults(&cfg);cfg.source_rate=44100;cfg.host_rate=44100;
        PsxAudioTransport transport;assert(!transport.init(cfg,false));
        int16_t in[4096*2]={},out[1024*2];
        double now=1.0,produced=0;uint64_t sent=0;
        double lo=1e9,hi=0,sum=0;unsigned samples=0;
        for(int i=0;i<5200;++i) {
            // Device period includes systematic clock skew plus bounded
            // alternating callback jitter. Guest output remains 44.1 kHz.
            double dt=1024.0/(44100*(1+skew))+(i%2?0.001:-0.001);
            now+=dt;produced+=dt*44100;
            int count=int(produced)-int(sent);assert(count<4096);
            transport.push(in,count);sent+=count;
            transport.pull(out,1024,uint64_t(now*1e9));
            if(i>1000) {double f=transport.fill_ms();lo=std::min(lo,f);hi=std::max(hi,f);sum+=f;++samples;}
        }
        rab_stats st;transport.stats(&st);
        printf("skew %.3f: fill %.2f..%.2f mean %.2f drops %llu starved %llu correction %.5f\n",skew,lo,hi,sum/samples,(unsigned long long)st.overflow_drops,(unsigned long long)st.underrun_events,st.last_correction);
        assert(!st.overflow_drops && !st.underrun_events);
        // Post-callback occupancy is one half callback below the midpoint.
        assert(lo>34 && hi<45);
        transport.free();
    }
}
