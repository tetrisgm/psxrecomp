/*
 * iso_reader_c.cpp — C wrapper for the C++ ISOReader class
 *
 * Provides iso_open() / iso_read_sector() / iso_close() for use by cdrom.c
 */

#include "host_span_ring.h"
#include "iso_reader.h"
#include "mod_runtime.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/* Disc read-ahead (PSX_CD_PREFETCH=0 turns it off). Raw sector reads (data,
 * XA and CD-DA streaming all read raw from a BIN/CHD) used to seek and read
 * the image file -- and for a CHD decompress a hunk -- on the emulation
 * thread, one sector at a time. A worker with its own ISOReader on the same
 * image now reads the next sectors ahead into a small direct-mapped cache;
 * the emulation thread copies a hit and reads a miss itself, exactly as
 * before. The bytes are the image's either way: mod extents and patches are
 * applied by the callers below after the base read, unchanged. Guest timing
 * never depended on host I/O, so nothing the guest sees changes. */
namespace {
constexpr uint32_t kPrefetchSlots = 64;     /* cache size, sectors */
constexpr uint32_t kPrefetchAhead = 24;     /* read this far past a request */

struct Prefetch {
    std::string path;
    std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    bool stop = false, started = false, failed = false;
    uint32_t next = 0, end = 0;  /* the worker's window [next, end) */
    struct Slot { uint32_t lba; bool valid, ok; uint8_t data[2352]; };
    Slot slots[kPrefetchSlots];
    uint64_t hits = 0, misses = 0;

    explicit Prefetch(const char* p) : path(p) {
        for (auto& s : slots) { s.valid = false; s.ok = false; s.lba = 0; }
    }
    ~Prefetch() {
        { std::lock_guard<std::mutex> lock(m); stop = true; }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
    void run() {
        PS1::ISOReader reader;
        if (!reader.Open(path)) {
            std::lock_guard<std::mutex> lock(m);
            failed = true;
            return;
        }
        uint8_t buf[2352];
        std::unique_lock<std::mutex> lock(m);
        for (;;) {
            cv.wait(lock, [&] { return stop || next < end; });
            if (stop) return;
            const uint32_t lba = next++;
            if (slots[lba % kPrefetchSlots].valid && slots[lba % kPrefetchSlots].lba == lba)
                continue;
            lock.unlock();
            const bool ok = reader.ReadRawSector(lba, buf);
            lock.lock();
            /* Any sector read is the image's: store it even if the window
             * moved meanwhile (at worst it evicts one the window wants,
             * which the emulation thread then reads itself). */
            Slot& d = slots[lba % kPrefetchSlots];
            d.lba = lba; d.ok = ok; d.valid = true;
            if (ok) std::memcpy(d.data, buf, sizeof buf);
        }
    }
    /* 1 hit (buffer filled, *ok = that read's result), 0 miss. Either way
     * moves the window to the kPrefetchAhead sectors after lba. */
    int lookup(uint32_t lba, uint8_t* buffer, bool* ok) {
        std::lock_guard<std::mutex> lock(m);
        if (failed) return 0;
        if (!started) {
            started = true;
            worker = std::thread([this] { run(); });
        }
        Slot& s = slots[lba % kPrefetchSlots];
        const int hit = s.valid && s.lba == lba;
        if (hit) {
            *ok = s.ok;
            if (s.ok) std::memcpy(buffer, s.data, sizeof s.data);
            hits++;
        } else {
            misses++;
        }
        /* Sequential reads keep the worker going; a seek (the worker behind
         * this read, or far past it) restarts it right after it. */
        if (next <= lba || next > lba + 1 + kPrefetchAhead) next = lba + 1;
        end = lba + 1 + kPrefetchAhead;
        cv.notify_one();
        return hit;
    }
};

std::mutex g_prefetch_mutex;
std::map<void*, std::unique_ptr<Prefetch>> g_prefetch;

bool prefetch_enabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = std::getenv("PSX_CD_PREFETCH");
        on = (e && *e) ? (*e != '0') : 1;
    }
    return on != 0;
}

Prefetch* prefetch_for(void* handle) {
    std::lock_guard<std::mutex> lock(g_prefetch_mutex);
    auto it = g_prefetch.find(handle);
    return it == g_prefetch.end() ? nullptr : it->second.get();
}

bool read_raw_base(void* handle, uint32_t lba, uint8_t* buffer) {
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (Prefetch* p = prefetch_for(handle)) {
        bool ok = false;
        if (p->lookup(lba, buffer, &ok)) return ok;
    }
    return reader->ReadRawSector(lba, buffer);
}
}  // namespace

extern "C" {

void* iso_open(const char* path) {
    auto* reader = new PS1::ISOReader();
    if (!reader->Open(path)) {
        delete reader;
        return nullptr;
    }
    if (path && prefetch_enabled()) {
        std::lock_guard<std::mutex> lock(g_prefetch_mutex);
        g_prefetch[reader] = std::make_unique<Prefetch>(path);
    }
    return reader;
}

/* Read-ahead counters for the debug server (0 when off). */
void iso_prefetch_stats(void* handle, uint64_t* hits, uint64_t* misses) {
    *hits = *misses = 0;
    if (Prefetch* p = handle ? prefetch_for(handle) : nullptr) {
        std::lock_guard<std::mutex> lock(p->m);
        *hits = p->hits;
        *misses = p->misses;
    }
}

int iso_read_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle || !buffer || size < 2048) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    const uint64_t span_t0 = host_span_begin();
    const bool read_ok = mod_runtime_read_disc_extent(lba, 0, buffer, size) || reader->ReadSector(lba, buffer);
    host_span_end(HSPAN_DISC_READ, span_t0, lba);   /* a stalled disk shows here, not as an unexplained frame */
    if (!read_ok) return 0;
    mod_runtime_patch_disc_sector(lba, 0, buffer, 2048);
    return 1;
}

int iso_read_raw_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle || !buffer || size < 2352) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    const uint64_t span_t0 = host_span_begin();
    const bool read_ok = mod_runtime_read_disc_extent(lba, 1, buffer, size) || read_raw_base(reader, lba, buffer);
    host_span_end(HSPAN_DISC_READ, span_t0, lba);
    if (!read_ok) return 0;
    mod_runtime_patch_disc_sector(lba, 1, buffer, 2352);
    return 1;
}

int iso_read_subq(void* handle, uint32_t lba, uint8_t* buffer, int size,
                  int* valid) {
    if (!handle || !buffer || size < 12 || !valid) return 0;
    const auto* reader=static_cast<PS1::ISOReader*>(handle);
    const uint32_t start=mod_runtime_disc_extent_start();
    if (start && lba>=start && lba<mod_runtime_disc_sector_count(start)) {
        const auto bcd=[](uint32_t v) {return uint8_t((v/10)*16+v%10);};
        std::memset(buffer,0,12);buffer[0]=0x41;
        buffer[1]=bcd(reader->TrackCount()+1);buffer[2]=1;
        const uint32_t rel=lba-start,abs=lba+150;
        buffer[3]=bcd(rel/4500);buffer[4]=bcd(rel/75%60);buffer[5]=bcd(rel%75);
        buffer[6]=bcd(abs/4500);buffer[7]=bcd(abs/75%60);buffer[8]=bcd(abs%75);
        uint16_t crc=0;
        for(unsigned i=0;i<10;++i) {
            crc^=uint16_t(buffer[i])<<8;
            for(unsigned j=0;j<8;++j)crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0));
        }
        crc=uint16_t(~crc);buffer[10]=uint8_t(crc);buffer[11]=uint8_t(crc>>8);
        *valid=1;return 1;
    }
    bool crc_valid = false;
    if (!static_cast<PS1::ISOReader*>(handle)->ReadSubChannelQ(
            lba, buffer, &crc_valid)) return 0;
    *valid = crc_valid ? 1 : 0;
    return 1;
}

int iso_has_subq_replacements(void* handle) {
    return handle && static_cast<PS1::ISOReader*>(handle)->HasSubChannelReplacements();
}

uint32_t iso_sector_count(void* handle) {
    if (!handle) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return mod_runtime_disc_sector_count(reader->GetSectorCount());
}

/* CD-track TOC accessors (multi-track / CD-DA support). track is 1-based. */
int iso_track_count(void* handle) {
    if (!handle) return 1;
    return static_cast<PS1::ISOReader*>(handle)->TrackCount() + (mod_runtime_disc_extent_start()?1:0);
}

uint32_t iso_track_start_lba(void* handle, int track) {
    if (!handle) return 0;
    auto* reader=static_cast<PS1::ISOReader*>(handle);
    if (track==reader->TrackCount()+1 && mod_runtime_disc_extent_start()) return mod_runtime_disc_extent_start();
    return reader->TrackStartLBA(track);
}

uint32_t iso_track_pregap_lba(void* handle, int track) {
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (reader && track==reader->TrackCount()+1 && mod_runtime_disc_extent_start()) return mod_runtime_disc_extent_start();
    return reader ? reader->TrackPregapLBA(track) : 0;
}

int iso_track_is_audio(void* handle, int track) {
    if (!handle) return 0;
    return static_cast<PS1::ISOReader*>(handle)->TrackIsAudio(track) ? 1 : 0;
}

void iso_close(void* handle) {
    if (!handle) return;
    std::unique_ptr<Prefetch> prefetch;
    {
        std::lock_guard<std::mutex> lock(g_prefetch_mutex);
        auto it = g_prefetch.find(handle);
        if (it != g_prefetch.end()) { prefetch = std::move(it->second); g_prefetch.erase(it); }
    }
    prefetch.reset();   /* joins the worker before the reader goes */
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    reader->Close();
    delete reader;
}

/* Audio-only overrides never affect filesystem reads, XA or disc validation. */
int iso_cdda_track_count(void* handle) {
    const int count=mod_runtime_cdda_track_count();
    return count?count:iso_track_count(handle);
}
uint32_t iso_cdda_track_start_lba(void* handle,int track) {
    return mod_runtime_cdda_track_count()?mod_runtime_cdda_track_start(track):iso_track_start_lba(handle,track);
}
uint32_t iso_cdda_track_pregap_lba(void* handle,int track) {
    return mod_runtime_cdda_track_count()?mod_runtime_cdda_track_start(track):iso_track_pregap_lba(handle,track);
}
int iso_cdda_track_is_audio(void* handle,int track) {
    const int count=mod_runtime_cdda_track_count();
    return count?(track>=2 && track<=count):iso_track_is_audio(handle,track);
}
uint32_t iso_cdda_sector_count(void* handle) {
    return mod_runtime_cdda_track_count()?mod_runtime_cdda_track_start(0):iso_sector_count(handle);
}
int iso_read_cdda_sector(void* handle,uint32_t lba,uint8_t* buffer,int size) {
    if(!handle || !buffer || size<2352) return 0;
    if(!mod_runtime_cdda_track_count()) return iso_read_raw_sector(handle,lba,buffer,size);
    uint32_t source=0;
    const int kind=mod_runtime_read_cdda_sector(lba,buffer,size,&source);
    return kind==1 || (kind==2 && read_raw_base(handle,source,buffer));
}

} /* extern "C" */
