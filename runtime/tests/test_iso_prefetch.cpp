#include "host_span_ring.h"
#include "iso_reader.h"
#include "mod_runtime.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

extern "C" {
void* iso_open(const char*);
void iso_close(void*);
int iso_read_raw_sector(void*, uint32_t, uint8_t*, int);
void iso_prefetch_stats(void*, uint64_t*, uint64_t*);
uint64_t host_span_begin() { return 0; }
void host_span_end(HostSpanKind, uint64_t, uint32_t) {}
// Model the media layer around the base read, independently of its cache.
int mod_runtime_read_disc_extent(uint32_t lba, int raw, uint8_t* out, uint32_t size) {
    if (lba != 200 || !raw || size < 2352) return 0;
    std::memset(out, 0x5a, 2352);
    return 1;
}
void mod_runtime_patch_disc_sector(uint32_t, int, uint8_t* out, uint32_t) {
    out[100] ^= 0xff;
}
uint32_t mod_runtime_disc_extent_start() { return 0; }
uint32_t mod_runtime_disc_sector_count(uint32_t count) { return count; }
int mod_runtime_cdda_track_count() { return 0; }
uint32_t mod_runtime_cdda_track_start(int) { return 0; }
int mod_runtime_read_cdda_sector(uint32_t, uint8_t*, uint32_t, uint32_t*) { return 0; }
}

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
        ("psx-prefetch-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    const auto bin = dir / "test.bin", cue = dir / "test.cue";
    {
        std::ofstream file(bin, std::ios::binary);
        std::array<uint8_t, 2352> sector{};
        for (unsigned n = 0; n < 128; ++n) {
            for (unsigned i = 0; i < sector.size(); ++i)
                sector[i] = uint8_t(n * 31 + i);
            sector[0] = sector[11] = 0;
            std::memset(sector.data() + 1, 0xff, 10);
            sector[15] = 2;
            file.write(reinterpret_cast<char*>(sector.data()), sector.size());
        }
        assert(file.good());
        std::ofstream sheet(cue);
        sheet << "FILE \"test.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n";
    }
    PS1::ISOReader reference;
    assert(reference.Open(cue.string()));
    void* handle = iso_open(cue.string().c_str());
    assert(handle);
    std::array<uint8_t, 2352> expected{}, actual{};
    auto check = [&](uint32_t lba) {
        const bool ok = reference.ReadRawSector(lba, expected.data());
        assert(iso_read_raw_sector(handle, lba, actual.data(), actual.size()) == int(ok));
        if (ok) {
            expected[100] ^= 0xff;
            assert(actual == expected);
        }
    };
    for (uint32_t lba = 0; lba < 128; ++lba) {
        check(lba);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Seeks, cache-index collisions, EOF and a base-image miss stay equivalent.
    for (uint32_t lba : {2u, 66u, 3u, 127u, 128u, 129u, 0u, 70u}) check(lba);
    assert(iso_read_raw_sector(handle, 200, actual.data(), actual.size()));
    expected.fill(0x5a); expected[100] ^= 0xff;
    assert(actual == expected);
    assert(!iso_read_raw_sector(handle, 1, actual.data(), 2351));
    uint64_t hits = 0, misses = 0;
    iso_prefetch_stats(handle, &hits, &misses);
    const char* mode = std::getenv("PSX_CD_PREFETCH");
    if (mode && *mode == '0') assert(!hits && !misses);
    else assert(hits && misses);
    // Close with outstanding read-ahead, then exercise a fresh reader/cache.
    iso_close(handle);
    handle = iso_open(cue.string().c_str()); assert(handle);
    check(17);
    iso_close(handle);
    reference.Close();
    fs::remove(cue); fs::remove(bin); fs::remove(dir);
    std::printf("PASS: raw bytes, seeks, EOF, media layering, close/reopen (hits=%llu misses=%llu)\n",
        (unsigned long long)hits, (unsigned long long)misses);
}
