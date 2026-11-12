#include "rewind_history.h"
#include "psx_sdl.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) && !defined(PSX_REWIND_TEST_PORTABLE_CODEC)
#include <compression.h>
#else
#include <zlib.h>
#endif

enum { RH_SLOTS=3 };
enum { RH_FREE, RH_RESERVED, RH_QUEUED, RH_ENCODING };
typedef struct RhBlob {
    atomic_uint references;
    RewindHistoryMeta meta;
    size_t raw_bytes, encoded_bytes;
    unsigned char data[];
} RhBlob;
typedef struct RhJob {
    unsigned char *raw;
    size_t bytes;
    uint64_t epoch;
    RewindHistoryMeta meta;
    int state;
} RhJob;
struct RewindHistory {
    SDL_mutex *mutex;
    SDL_cond *changed;
    SDL_Thread *thread;
    RhJob jobs[RH_SLOTS];
    int queue[RH_SLOTS], head, queued, stop, frozen;
    uint64_t epoch;
    size_t budget, raw_capacity;
    RhBlob **entries;
    uint32_t count, max_entries;
    RewindHistoryStats stats;
};
static void release(RhBlob *b) {
    if(b && atomic_fetch_sub_explicit(&b->references,1,memory_order_acq_rel)==1)free(b);
}
static void drop_first(RewindHistory *h) {
    RhBlob *b=h->entries[0];
    h->stats.bytes-=b->encoded_bytes;
    memmove(h->entries,h->entries+1,(--h->count)*sizeof(*h->entries));
    release(b);
}
static int worker(void *context) {
    RewindHistory *h=context;
    size_t bound=h->raw_capacity+h->raw_capacity/128+4096;
    unsigned char *encoded_buffer=malloc(bound);
#if defined(__APPLE__) && !defined(PSX_REWIND_TEST_PORTABLE_CODEC)
    void *scratch=malloc(compression_encode_scratch_buffer_size(COMPRESSION_LZ4));
#endif
    for(;;) {
        SDL_LockMutex(h->mutex);
        while(!h->stop && !h->queued)SDL_CondWait(h->changed,h->mutex);
        if(h->stop){SDL_UnlockMutex(h->mutex);break;}
        int slot=h->queue[h->head];h->head=(h->head+1)%RH_SLOTS;--h->queued;
        RhJob *j=&h->jobs[slot];j->state=RH_ENCODING;
        SDL_UnlockMutex(h->mutex);
        RhBlob *b=NULL;
        size_t encoded=0;
        if(encoded_buffer) {
#if defined(__APPLE__) && !defined(PSX_REWIND_TEST_PORTABLE_CODEC)
            encoded=compression_encode_buffer(encoded_buffer,bound,j->raw,j->bytes,scratch,COMPRESSION_LZ4);
#else
            uLongf n=(uLongf)bound;
            if(compress2(encoded_buffer,&n,j->raw,(uLong)j->bytes,Z_BEST_SPEED)==Z_OK)encoded=(size_t)n;
#endif
        }
        if(encoded) b=malloc(sizeof(*b)+encoded);
        if(b) {
            memcpy(b->data,encoded_buffer,encoded);
            b->meta=j->meta;b->raw_bytes=j->bytes;b->encoded_bytes=encoded;
            atomic_init(&b->references,1);
        } else {free(b);b=NULL;}
        SDL_LockMutex(h->mutex);
        if(b && !h->stop && !h->frozen && j->epoch==h->epoch && encoded<=h->budget &&
           (!h->count || b->meta.key>h->entries[h->count-1]->meta.key)) {
            while(h->count && (h->count==h->max_entries || h->stats.bytes>h->budget-encoded))drop_first(h);
            h->entries[h->count++]=b;h->stats.bytes+=encoded;++h->stats.completed;
        } else {release(b);++h->stats.dropped;}
        j->state=RH_FREE;SDL_CondBroadcast(h->changed);
        SDL_UnlockMutex(h->mutex);
    }
#if defined(__APPLE__) && !defined(PSX_REWIND_TEST_PORTABLE_CODEC)
    free(scratch);
#endif
    free(encoded_buffer);
    return 0;
}
RewindHistory *rewind_history_create(size_t budget,uint32_t entries,size_t raw_capacity) {
    if(!budget || !entries || entries>4096 || !raw_capacity || raw_capacity>128u*1024*1024)return NULL;
    RewindHistory *h=calloc(1,sizeof(*h));if(!h)return NULL;
    h->budget=budget;h->max_entries=entries;h->raw_capacity=raw_capacity;
    h->mutex=SDL_CreateMutex();h->changed=SDL_CreateCond();h->entries=calloc(entries,sizeof(*h->entries));
    if(!h->mutex || !h->changed || !h->entries){rewind_history_destroy(h);return NULL;}
    for(int i=0;i<RH_SLOTS;i++) {
        h->jobs[i].raw=malloc(raw_capacity);h->jobs[i].state=RH_FREE;
        if(!h->jobs[i].raw){rewind_history_destroy(h);return NULL;}
        /* Fault the bounded pool before recording starts. */
        memset(h->jobs[i].raw,0,raw_capacity);
    }
    h->thread=SDL_CreateThread(worker,"rewind-compress",h);
    if(!h->thread){rewind_history_destroy(h);return NULL;}
    return h;
}
void rewind_history_destroy(RewindHistory *h) {
    if(!h)return;
    if(h->thread) {
        SDL_LockMutex(h->mutex);h->stop=1;SDL_CondBroadcast(h->changed);SDL_UnlockMutex(h->mutex);
        SDL_WaitThread(h->thread,NULL);
    }
    for(uint32_t i=0;i<h->count;i++)release(h->entries[i]);
    for(int i=0;i<RH_SLOTS;i++)free(h->jobs[i].raw);
    free(h->entries);if(h->changed)SDL_DestroyCond(h->changed);if(h->mutex)SDL_DestroyMutex(h->mutex);free(h);
}
void *rewind_history_reserve(RewindHistory *h,size_t bytes,int *slot) {
    if(slot)*slot=-1;
    if(!h || !slot || !bytes || bytes>h->raw_capacity)return NULL;
    SDL_LockMutex(h->mutex);
    if(!h->frozen && !h->stop)for(int i=0;i<RH_SLOTS;i++)if(h->jobs[i].state==RH_FREE) {
        RhJob *j=&h->jobs[i];j->state=RH_RESERVED;j->bytes=bytes;j->epoch=h->epoch;*slot=i;
        SDL_UnlockMutex(h->mutex);return j->raw;
    }
    ++h->stats.dropped;SDL_UnlockMutex(h->mutex);return NULL;
}
void rewind_history_submit(RewindHistory *h,int slot,const RewindHistoryMeta *meta) {
    if(!h || !meta || slot<0 || slot>=RH_SLOTS)return;
    SDL_LockMutex(h->mutex);RhJob *j=&h->jobs[slot];
    if(j->state==RH_RESERVED) {
        if(h->frozen || h->stop || j->epoch!=h->epoch){j->state=RH_FREE;++h->stats.dropped;}
        else {j->meta=*meta;j->state=RH_QUEUED;h->queue[(h->head+h->queued)%RH_SLOTS]=slot;++h->queued;++h->stats.submitted;SDL_CondSignal(h->changed);}
    }
    SDL_UnlockMutex(h->mutex);
}
void rewind_history_cancel(RewindHistory *h,int slot) {
    if(!h || slot<0 || slot>=RH_SLOTS)return;
    SDL_LockMutex(h->mutex);if(h->jobs[slot].state==RH_RESERVED)h->jobs[slot].state=RH_FREE;SDL_UnlockMutex(h->mutex);
}
void rewind_history_freeze(RewindHistory *h,int frozen) {
    if(!h)return;
    SDL_LockMutex(h->mutex);if(frozen && !h->frozen)++h->epoch;h->frozen=!!frozen;SDL_UnlockMutex(h->mutex);
}
void rewind_history_clear(RewindHistory *h) {
    if(!h)return;
    SDL_LockMutex(h->mutex);++h->epoch;while(h->count)drop_first(h);SDL_UnlockMutex(h->mutex);
}
void rewind_history_drop_after(RewindHistory *h,uint64_t key) {
    if(!h)return;
    SDL_LockMutex(h->mutex);++h->epoch;
    while(h->count && h->entries[h->count-1]->meta.key>key){RhBlob *b=h->entries[--h->count];h->stats.bytes-=b->encoded_bytes;release(b);}
    SDL_UnlockMutex(h->mutex);
}
int rewind_history_decode_before(RewindHistory *h,uint64_t key,void *out,size_t cap,size_t *bytes,RewindHistoryMeta *meta) {
    if(bytes)*bytes=0;if(!h || !out || !bytes || !meta)return 0;
    SDL_LockMutex(h->mutex);RhBlob *b=NULL;
    for(uint32_t i=h->count;i>0;i--)if(h->entries[i-1]->meta.key<=key){b=h->entries[i-1];break;}
    if(b)atomic_fetch_add_explicit(&b->references,1,memory_order_relaxed);SDL_UnlockMutex(h->mutex);
    if(!b)return 0;
    int ok=0;
    if(cap>=b->raw_bytes) {
#if defined(__APPLE__) && !defined(PSX_REWIND_TEST_PORTABLE_CODEC)
        ok=compression_decode_buffer(out,cap,b->data,b->encoded_bytes,NULL,COMPRESSION_LZ4)==b->raw_bytes;
#else
        uLongf n=(uLongf)cap;ok=uncompress(out,&n,b->data,(uLong)b->encoded_bytes)==Z_OK && n==b->raw_bytes;
#endif
        if(ok){*bytes=b->raw_bytes;*meta=b->meta;}
    }
    release(b);return ok;
}
void rewind_history_stats(RewindHistory *h,RewindHistoryStats *stats) {
    memset(stats,0,sizeof(*stats));if(!h)return;
    SDL_LockMutex(h->mutex);*stats=h->stats;stats->entries=h->count;
    for(int i=0;i<RH_SLOTS;i++)stats->pending+=h->jobs[i].state!=RH_FREE;
    SDL_UnlockMutex(h->mutex);
}

int rewind_history_contains(RewindHistory *h,uint64_t key) {
    if(!h)return 0;
    SDL_LockMutex(h->mutex);int found=0;
    for(uint32_t i=h->count;i>0;i--)if(h->entries[i-1]->meta.key<=key){found=h->entries[i-1]->meta.key==key;break;}
    SDL_UnlockMutex(h->mutex);return found;
}
