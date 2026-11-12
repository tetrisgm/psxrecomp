#include "rewind_history.h"
#include "psx_sdl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void drained(RewindHistory *h) {
    RewindHistoryStats s;
    for(int i=0;i<5000;i++){rewind_history_stats(h,&s);if(!s.pending)return;SDL_Delay(1);}
    assert(!"compression worker timed out");
}
int main(void) {
    unsigned char expected[16384],out[16384];
    RewindHistory *h=rewind_history_create(32768,4,sizeof out);assert(h);
    for(unsigned tick=1;tick<=12;tick++) {
        for(unsigned i=0;i<sizeof expected;i++)expected[i]=(unsigned char)((i*71+tick)^((i>>3)*17));
        int slot;void *raw=rewind_history_reserve(h,sizeof expected,&slot);assert(raw);
        memcpy(raw,expected,sizeof expected);
        RewindHistoryMeta m={tick,tick*100,64,64};rewind_history_submit(h,slot,&m);drained(h);
        size_t bytes=0;RewindHistoryMeta got;
        assert(rewind_history_decode_before(h,tick,out,sizeof out,&bytes,&got));
        assert(bytes==sizeof expected && got.key==tick && got.width==64 && !memcmp(out,expected,bytes));
        assert(!rewind_history_decode_before(h,tick,out,10,&bytes,&got));
    }
    RewindHistoryStats s;rewind_history_stats(h,&s);assert(s.completed==12 && s.entries<=4 && s.bytes<=32768);
    rewind_history_drop_after(h,10);size_t bytes;RewindHistoryMeta got;
    assert(rewind_history_decode_before(h,UINT64_MAX,out,sizeof out,&bytes,&got) && got.key==10);
    int slot;assert(rewind_history_reserve(h,sizeof out,&slot));
    rewind_history_freeze(h,1);RewindHistoryMeta m={13,1300,64,64};rewind_history_submit(h,slot,&m);drained(h);
    assert(!rewind_history_reserve(h,sizeof out,&slot));
    rewind_history_freeze(h,0);void *raw=rewind_history_reserve(h,sizeof out,&slot);assert(raw);memset(raw,17,sizeof out);
    rewind_history_clear(h);rewind_history_submit(h,slot,&m);drained(h);
    assert(!rewind_history_decode_before(h,UINT64_MAX,out,sizeof out,&bytes,&got));
    raw=rewind_history_reserve(h,sizeof out,&slot);assert(raw);rewind_history_cancel(h,slot);
    rewind_history_destroy(h);
    puts("lossless rewind history: exact pixels, bounded eviction, branching and pending invalidation passed");
}
