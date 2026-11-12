#ifndef PSX_REWIND_HISTORY_H
#define PSX_REWIND_HISTORY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Bounded, lossless, in-memory history. One producer reserves a writable slot,
 * then submits it; one worker compresses. Readers retain immutable blobs while
 * decoding outside the lock. Keys are monotonically increasing timeline IDs. */
typedef struct RewindHistory RewindHistory;
typedef struct RewindHistoryMeta {
    uint64_t key, clock;
    uint32_t width, height;
} RewindHistoryMeta;
typedef struct RewindHistoryStats {
    uint64_t submitted, completed, dropped, bytes;
    uint32_t entries, pending;
} RewindHistoryStats;
RewindHistory *rewind_history_create(size_t byte_budget, uint32_t entries,
                                    size_t raw_capacity);
void rewind_history_destroy(RewindHistory *history);
void *rewind_history_reserve(RewindHistory *history, size_t bytes, int *slot);
void rewind_history_submit(RewindHistory *history, int slot, const RewindHistoryMeta *meta);
void rewind_history_cancel(RewindHistory *history, int slot);
/* Freeze invalidates pending captures; thaw admits captures again. */
void rewind_history_freeze(RewindHistory *history, int frozen);
void rewind_history_clear(RewindHistory *history);
void rewind_history_drop_after(RewindHistory *history, uint64_t key);
int rewind_history_decode_before(RewindHistory *history, uint64_t key,
    void *output, size_t capacity, size_t *bytes, RewindHistoryMeta *meta);
int rewind_history_contains(RewindHistory *history, uint64_t key);
void rewind_history_stats(RewindHistory *history, RewindHistoryStats *stats);
#ifdef __cplusplus
}
#endif
#endif
