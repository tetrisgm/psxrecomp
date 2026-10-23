/* psx_exec_ctx_bench.c - the `ctx_draw` debug command (docs/THREADED_CONTEXTS.md,
 * "Checking a title"): run a guest draw call sequence in draw contexts from
 * one snapshot of the live machine, once serially and once on N threads at
 * the same time, and compare the captured GPU command lists. Optionally time
 * the same sequence on the main context with the full faithful code.
 *
 * Main emulation thread only (the debug server's command pump). The draw
 * contexts never touch the machine; the optional full-code run does, inside a
 * zero-guest-time call with RAM and scratchpad put back afterwards. */

#include "psx_exec_ctx.h"
#include "psx_cycle_freeze.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern uint32_t psx_mod_call_guest_uncharged(CPUState *cpu, uint32_t function,
                                             uint32_t return_address, uint32_t a0,
                                             uint32_t a1, uint32_t a2, uint32_t a3,
                                             uint32_t budget_cycles, int *charged);

#define BENCH_MAX_CALLS  64
#define BENCH_MAX_WRITES 64
#define BENCH_MAX_THREADS PSX_CTX_MAX

typedef struct { uint32_t fn; uint32_t args[4]; int nargs; } BenchCall;
typedef struct { uint32_t addr, value; } BenchWrite;

typedef struct {
    BenchCall calls[BENCH_MAX_CALLS];
    int ncalls;
    BenchWrite writes[BENCH_MAX_WRITES];
    int nwrites;
    uint32_t sp;
    int reps;
} BenchPlan;

typedef struct {
    PsxExecCtx *ctx;
    const PsxCtxSnapshot *snap;
    const BenchPlan *plan;
    PsxCtxStatus status;
    uint32_t fault_pc;
    const char *fault_what;
    int failed_call;
    size_t words;
    uint64_t hash;
    double ms;           /* total over reps */
    uint32_t trail[16];
    int trail_n;
    uint32_t fault_ra;
    uint32_t fault_gpr[32];
    PsxCtxStats stats;
} BenchJob;

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static uint64_t hash_words(const PsxCtxGpuWord *w, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        /* Captures are fully initialized, including precision/projection.
         * Hash exactly the immutable receipt compared by memcmp below. */
        const uint8_t *p = (const uint8_t *)&w[i];
        for (size_t k = 0; k < sizeof w[i]; k++) { h ^= p[k]; h *= 1099511628211ull; }
    }
    return h;
}

/* "fn:a0,a1;fn" */
static int parse_calls(const char *s, BenchPlan *p) {
    p->ncalls = 0;
    while (s && *s) {
        if (p->ncalls >= BENCH_MAX_CALLS) return 0;
        BenchCall *c = &p->calls[p->ncalls];
        char *end;
        c->fn = (uint32_t)strtoul(s, &end, 0);
        if (end == s) return 0;
        c->nargs = 0;
        s = end;
        if (*s == ':') {
            s++;
            while (*s && *s != ';') {
                if (c->nargs >= 4) return 0;
                c->args[c->nargs++] = (uint32_t)strtoul(s, &end, 0);
                if (end == s) return 0;
                s = end;
                if (*s == ',') s++;
            }
        }
        p->ncalls++;
        if (*s == ';') s++;
    }
    return p->ncalls > 0;
}

/* "addr=value;addr=value" */
static int parse_writes(const char *s, BenchPlan *p) {
    p->nwrites = 0;
    while (s && *s) {
        if (p->nwrites >= BENCH_MAX_WRITES) return 0;
        char *end;
        p->writes[p->nwrites].addr = (uint32_t)strtoul(s, &end, 0);
        if (end == s || *end != '=') return 0;
        s = end + 1;
        p->writes[p->nwrites].value = (uint32_t)strtoul(s, &end, 0);
        if (end == s) return 0;
        s = end;
        p->nwrites++;
        if (*s == ';') s++;
    }
    return 1;
}

static void run_job(BenchJob *j) {
    const BenchPlan *p = j->plan;
    j->status = PSX_CTX_OK;
    j->failed_call = -1;
    double t = 0;
    for (int rep = 0; rep < p->reps; rep++) {
        const double t0 = now_ms();
        if (!psx_ctx_begin(j->ctx, j->snap)) {
            j->status = PSX_CTX_FAULT_BAD_ARG;
            j->fault_what = "context snapshot import failed";
            break;
        }
        CPUState *cpu = psx_ctx_cpu(j->ctx);
        for (int i = 0; i < p->nwrites; i++)
            psx_ctx_write32(j->ctx, p->writes[i].addr, p->writes[i].value);
        for (int i = 0; i < p->ncalls; i++) {
            cpu->gpr[29] = p->sp;
            const PsxCtxStatus s = psx_ctx_call(j->ctx, p->calls[i].fn, p->calls[i].args,
                                                p->calls[i].nargs, 0, NULL);
            if (s != PSX_CTX_OK) {
                j->status = s;
                j->failed_call = i;
                psx_ctx_status(j->ctx, &j->fault_pc, &j->fault_what);
                j->trail_n = psx_ctx_trail(j->ctx, j->trail, 16, &j->fault_ra);
                memcpy(j->fault_gpr, cpu->gpr, sizeof j->fault_gpr);
                break;
            }
        }
        t += now_ms() - t0;
        if (j->status != PSX_CTX_OK) break;
    }
    j->ms = t;
    const PsxCtxGpuWord *w = psx_ctx_gpu_words(j->ctx, &j->words);
    j->hash = hash_words(w, j->words);
    j->stats = *psx_ctx_stats(j->ctx);
}

#ifdef _WIN32
static DWORD WINAPI job_thread(LPVOID arg) { run_job((BenchJob *)arg); return 0; }
#else
static void *job_thread(void *arg) { run_job((BenchJob *)arg); return NULL; }
#endif

static int emit_job(char *out, size_t cap, int pos, const char *name, const BenchJob *j) {
    char trail[16 * 13 + 4];
    int tp = 0;
    trail[0] = 0;
    for (int i = 0; i < j->trail_n; i++)
        tp += snprintf(trail + tp, sizeof trail - (size_t)tp, "%s\"0x%08X\"", i ? "," : "", j->trail[i]);
    pos += snprintf(out + pos, cap - (size_t)pos, "%s{\"trail\":[%s],\"fault_ra\":\"0x%08X\",",
                    name, trail, j->fault_ra);
    if (j->status != PSX_CTX_OK) {
        pos += snprintf(out + pos, cap - (size_t)pos, "\"fault_gpr\":[");
        for (unsigned i = 0; i < 32; i++)
            pos += snprintf(out + pos, cap - (size_t)pos, "%s\"0x%08X\"", i ? "," : "", j->fault_gpr[i]);
        pos += snprintf(out + pos, cap - (size_t)pos, "],");
    }
    name = "";
    return pos + snprintf(out + pos, cap - (size_t)pos,
        "%s\"status\":\"%s\",\"failed_call\":%d,\"fault_pc\":\"0x%08X\",\"fault\":\"%s\","
        "\"words\":%u,\"hash\":\"%016llx\",\"ms\":%.3f,\"dispatches\":%llu,"
        "\"dma_gpu\":%llu,\"dma_otc\":%llu,\"gp0_port\":%llu,\"gp1\":%llu,"
        "\"dropped_writes\":%llu,\"hooks_stripped\":%llu}",
        name, psx_ctx_status_name(j->status), j->failed_call, j->fault_pc,
        j->fault_what ? j->fault_what : "", (unsigned)j->words,
        (unsigned long long)j->hash, j->ms,
        (unsigned long long)j->stats.dispatches,
        (unsigned long long)j->stats.dma_gpu, (unsigned long long)j->stats.dma_otc,
        (unsigned long long)j->stats.gp0_port_words, (unsigned long long)j->stats.gp1_words,
        (unsigned long long)j->stats.dropped_writes,
        (unsigned long long)j->stats.hooks_stripped);
}

/* Full faithful code on the main context, for timing only: zero guest time,
 * RAM and scratchpad restored. Device state the calls touch (GPU commands,
 * DMA registers) is not restored: the game rebuilds it every frame, but run
 * this on a throwaway session. */
static double run_full(CPUState *cpu, const BenchPlan *p, uint32_t ram_bytes) {
    uint8_t *ram = memory_get_ram_ptr(), *spad = memory_get_scratchpad_ptr();
    uint8_t *ram_save = (uint8_t *)malloc(ram_bytes);
    uint8_t spad_save[1024];
    if (!ram_save) return -1.0;
    memcpy(ram_save, ram, ram_bytes);
    memcpy(spad_save, spad, sizeof spad_save);
    CPUState cpu_save = *cpu;
    double total = 0;
    for (int rep = 0; rep < p->reps; rep++) {
        for (int i = 0; i < p->nwrites; i++) {
            const uint32_t a = p->writes[i].addr & 0x1FFFFFFFu;
            if (a < ram_bytes) memcpy(ram + a, &p->writes[i].value, 4);
            else if ((a & 0xFFFFFC00u) == 0x1F800000u) memcpy(spad + (a & 0x3FFu), &p->writes[i].value, 4);
        }
        const double t0 = now_ms();
        for (int i = 0; i < p->ncalls; i++) {
            const BenchCall *c = &p->calls[i];
            cpu->gpr[29] = p->sp;
            int charged = 0;
            (void)psx_mod_call_guest_uncharged(cpu, c->fn, PSX_CTX_STOP_PC,
                c->args[0], c->args[1], c->args[2], c->args[3], 0x7FFFFFFFu, &charged);
        }
        total += now_ms() - t0;
        memcpy(ram, ram_save, ram_bytes);
        memcpy(spad, spad_save, sizeof spad_save);
        *cpu = cpu_save;
    }
    free(ram_save);
    return total;
}

int psx_ctx_bench_json(CPUState *cpu, const char *calls, const char *writes,
                       int threads, int reps, int full, uint32_t sp,
                       const char *dump_path, char *out, size_t cap) {
    BenchPlan *plan = (BenchPlan *)calloc(1, sizeof *plan);
    BenchJob *jobs = (BenchJob *)calloc(BENCH_MAX_THREADS + 1, sizeof *jobs);
    PsxExecCtx *ctxs[BENCH_MAX_THREADS + 1] = {0};
    PsxCtxSnapshot *snap = NULL;
    int pos = 0, ok = 0;
    if (!plan || !jobs) { snprintf(out, cap, "\"error\":\"alloc\""); goto done; }
    if (!parse_calls(calls, plan) || !parse_writes(writes, plan)) {
        snprintf(out, cap, "\"error\":\"bad calls/writes spec\"");
        goto done;
    }
    if (threads < 1) threads = 1;
    if (threads > BENCH_MAX_THREADS - 1) threads = BENCH_MAX_THREADS - 1;
    plan->reps = reps < 1 ? 1 : reps;
    /* The live $sp may be the kernel's exception stack when the command
     * runs; default to the top of user RAM (context RAM is private, so the
     * calls' frames cannot hurt the machine). */
    plan->sp = sp ? sp : 0x801FFF00u;
    if (!psx_ctx_available()) { snprintf(out, cap, "\"error\":\"no draw clones in this build\""); goto done; }

    const double ts0 = now_ms();
    snap = psx_ctx_snapshot_capture(cpu, PSX_CTX_SNAP_PGXP);
    const double snap_ms = now_ms() - ts0;
    if (!snap) { snprintf(out, cap, "\"error\":\"snapshot\""); goto done; }
    for (int i = 0; i <= threads; i++) {
        ctxs[i] = psx_ctx_create();
        if (!ctxs[i]) { snprintf(out, cap, "\"error\":\"context create\""); goto done; }
        jobs[i].ctx = ctxs[i];
        jobs[i].snap = snap;
        jobs[i].plan = plan;
    }

    /* serial reference */
    run_job(&jobs[0]);

    /* N concurrent contexts from the same snapshot */
    const double tw0 = now_ms();
#ifdef _WIN32
    HANDLE th[BENCH_MAX_THREADS];
    for (int i = 0; i < threads; i++)
        th[i] = CreateThread(NULL, 4u << 20, job_thread, &jobs[1 + i], 0, NULL);
    for (int i = 0; i < threads; i++) {
        if (th[i]) { WaitForSingleObject(th[i], INFINITE); CloseHandle(th[i]); }
        else run_job(&jobs[1 + i]);
    }
#else
    pthread_t th[BENCH_MAX_THREADS];
    int started[BENCH_MAX_THREADS];
    for (int i = 0; i < threads; i++) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4u << 20);
        started[i] = pthread_create(&th[i], &attr, job_thread, &jobs[1 + i]) == 0;
        pthread_attr_destroy(&attr);
    }
    for (int i = 0; i < threads; i++) {
        if (started[i]) pthread_join(th[i], NULL);
        else run_job(&jobs[1 + i]);
    }
#endif
    const double wall_ms = now_ms() - tw0;

    int identical = 1;
    for (int i = 1; i <= threads; i++) {
        size_t n0 = 0, ni = 0;
        const PsxCtxGpuWord *w0 = psx_ctx_gpu_words(ctxs[0], &n0);
        const PsxCtxGpuWord *wi = psx_ctx_gpu_words(ctxs[i], &ni);
        if (jobs[i].status != jobs[0].status || n0 != ni ||
            (n0 && memcmp(w0, wi, n0 * sizeof *w0) != 0))
            identical = 0;
    }

    if (dump_path && *dump_path) {
        FILE *f = fopen(dump_path, "wb");
        if (f) {
            size_t n = 0;
            const PsxCtxGpuWord *w = psx_ctx_gpu_words(ctxs[0], &n);
            for (size_t i = 0; i < n; i++)
                fprintf(f, "%08X %08X\n", w[i].word, w[i].src);
            fclose(f);
        }
    }

    double full_ms = -1.0;
    if (full) {
        uint32_t ram_bytes = 0;
        (void)psx_ctx_snapshot_ram(snap, &ram_bytes);
        full_ms = run_full(cpu, plan, ram_bytes);
    }

    pos += snprintf(out + pos, cap - (size_t)pos,
                    "\"threads\":%d,\"reps\":%d,\"calls\":%d,\"sp\":\"0x%08X\","
                    "\"snapshot_ms\":%.3f,\"identical\":%s,\"parallel_wall_ms\":%.3f,"
                    "\"lean_ms_per_rep\":%.4f,\"full_ms_per_rep\":%.4f,",
                    threads, plan->reps, plan->ncalls, plan->sp, snap_ms,
                    identical ? "true" : "false", wall_ms,
                    jobs[0].ms / plan->reps, full >= 1 ? full_ms / plan->reps : -1.0);
    pos = emit_job(out, cap, pos, "\"serial\":", &jobs[0]);
    pos += snprintf(out + pos, cap - (size_t)pos, ",\"parallel\":[");
    for (int i = 1; i <= threads && pos < (int)cap - 1200; i++)
        pos = emit_job(out, cap, pos, i > 1 ? "," : "", &jobs[i]);
    pos += snprintf(out + pos, cap - (size_t)pos, "]");
    ok = jobs[0].status == PSX_CTX_OK && identical;

done:
    for (int i = 0; i <= BENCH_MAX_THREADS; i++) psx_ctx_destroy(ctxs[i]);
    psx_ctx_snapshot_release(snap);
    free(jobs);
    free(plan);
    return ok;
}
