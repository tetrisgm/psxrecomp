/* Hidden GL ownership probe: two worker contexts render independent shared
 * textures concurrently. The owner reads them in frame order and compares
 * exact pixels against the same stream rendered serially. This tests context
 * sharing and per-context FBO ownership, not the renderer's mutable state. */
#include "psx_sdl.h"
#include <SDL3/SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pgxp_seam_snap.h"

#define W 64
#define H 48
#define FRAMES 12
typedef void (APIENTRY *bind_fb_fn)(unsigned, unsigned);
typedef void (APIENTRY *attach_fn)(unsigned, unsigned, unsigned, unsigned, int);
typedef void (APIENTRY *gen_fb_fn)(int, unsigned *);
typedef void (APIENTRY *del_fb_fn)(int, const unsigned *);
typedef unsigned (APIENTRY *fb_status_fn)(unsigned);
static bind_fb_fn bind_fb;
static attach_fn attach;
static gen_fb_fn gen_fb;
static del_fb_fn del_fb;
static fb_status_fn fb_status;
static const unsigned FB = 0x8D40, COLOR0 = 0x8CE0;
static SDL_Window *window;
static SDL_GLContext contexts[3];
static unsigned textures[FRAMES];
static uint64_t serial_hash[FRAMES], parallel_hash[FRAMES], owner_hash[FRAMES];
static int failed;

/* The renderer's actual PGXP seam index must remain private to each output.
 * Two road rows meet at integer y=20 but have a 0.6 native-pixel PGXP gap. */
static int seam_gap(PgxpSnap *snap, float *gap) {
    static const float x[3][3] = {{0, 40, 40}, {0, 40, 0}, {0, 40, 40}};
    static const float y[3][3] = {{10, 10, 19.6f}, {10, 19.6f, 19.6f},
                                 {20.2f, 20.2f, 30}};
    const float z[3] = {1000, 1000, 1000};
    const uint32_t word[3] = {0, 0, 0};
    pgxp_snap_reset(snap);
    for (int t = 0; t < 3; ++t) {
        int32_t ix[3], iy[3], fx[3], fy[3];
        float edge[3];
        for (int k = 0; k < 3; ++k) {
            ix[k] = (int32_t)floorf(x[t][k] + 0.5f);
            iy[k] = (int32_t)floorf(y[t][k] + 0.5f);
            fx[k] = (int32_t)lrintf(x[t][k] * 65536.0f);
            fy[k] = (int32_t)lrintf(y[t][k] * 65536.0f);
        }
        int n = pgxp_snap_triangle(snap, ix, iy, fx, fy, z, word, edge);
        if (t == 2) {
            *gap = edge[0];
            return n == 1 && fabsf(*gap - 0.6f) < 0.001f;
        }
        if (n != 0) return 0;
    }
    return 0;
}

static uint64_t digest(const unsigned char *pixels) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (unsigned i = 0; i < W * H * 4; ++i)
        h = (h ^ pixels[i]) * UINT64_C(1099511628211);
    return h;
}

static int draw(int frame, unsigned tex, PgxpSnap *snap, uint64_t *hash) {
    unsigned fb;
    unsigned char pixels[W * H * 4];
    float gap = 0;
    if (!seam_gap(snap, &gap)) return 0;
    gen_fb(1, &fb);
    bind_fb(FB, fb);
    attach(FB, COLOR0, GL_TEXTURE_2D, tex, 0);
    if (fb_status(FB) != GL_FRAMEBUFFER_COMPLETE) return 0;
    glViewport(0, 0, W, H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, W, H);
    glClearColor((frame % 7) / 8.0f, 0.125f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(frame % 17, frame % 11, 13 + frame, 19);
    glClearColor(0.75f, (frame % 5) / 6.0f, gap / 2.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    *hash = digest(pixels);
    bind_fb(FB, 0);
    del_fb(1, &fb);
    glFinish(); /* shared texture is complete before its owner is released */
    return glGetError() == GL_NO_ERROR;
}

typedef struct Worker { int id; int ok; PgxpSnap *snap; } Worker;
static int worker(void *arg) {
    Worker *w = (Worker *)arg;
    w->ok = SDL_GL_MakeCurrent(window, contexts[w->id + 1]) == 0;
    if (w->ok) {
        for (int f = w->id; f < FRAMES; f += 2)
            if (!draw(f, textures[f], w->snap, &parallel_hash[f])) w->ok = 0;
        SDL_GL_MakeCurrent(window, NULL);
    }
    return 0;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    window = SDL_CreateWindow("shared-context fixture", 0, 0, W, H,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) return 77;
    contexts[0] = SDL_GL_CreateContext(window);
    if (!contexts[0]) return 77;
    bind_fb = (bind_fb_fn)SDL_GL_GetProcAddress("glBindFramebuffer");
    attach = (attach_fn)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    gen_fb = (gen_fb_fn)SDL_GL_GetProcAddress("glGenFramebuffers");
    del_fb = (del_fb_fn)SDL_GL_GetProcAddress("glDeleteFramebuffers");
    fb_status = (fb_status_fn)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    if (!bind_fb || !attach || !gen_fb || !del_fb || !fb_status) return 77;
    glGenTextures(FRAMES, textures);
    PgxpSnap *serial_snap = calloc(1, sizeof *serial_snap);
    if (!serial_snap) return 2;
    uint64_t serial_start = SDL_GetTicksNS();
    for (int f = 0; f < FRAMES; ++f) {
        glBindTexture(GL_TEXTURE_2D, textures[f]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        if (!draw(f, textures[f], serial_snap, &serial_hash[f])) failed = 1;
    }
    uint64_t serial_ns = SDL_GetTicksNS() - serial_start;
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    for (int i = 1; i < 3; ++i) {
        contexts[i] = SDL_GL_CreateContext(window);
        if (!contexts[i]) return 77;
        SDL_GL_MakeCurrent(window, contexts[0]);
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(window, NULL);
    uint64_t parallel_start = SDL_GetTicksNS();
    Worker workers[2] = {{0, 0, calloc(1, sizeof *serial_snap)},
                         {1, 0, calloc(1, sizeof *serial_snap)}};
    if (!workers[0].snap || !workers[1].snap) return 2;
    SDL_Thread *threads[2];
    for (int i = 0; i < 2; ++i)
        threads[i] = SDL_CreateThread(worker, "gl-shared-worker", &workers[i]);
    for (int i = 0; i < 2; ++i) {
        if (!threads[i]) failed = 1;
        else SDL_WaitThread(threads[i], NULL);
        if (!workers[i].ok) failed = 1;
    }
    SDL_GL_MakeCurrent(window, contexts[0]);
    uint64_t parallel_ns = SDL_GetTicksNS() - parallel_start;
    free(workers[0].snap); free(workers[1].snap); free(serial_snap);
    for (int f = 0; f < FRAMES; ++f) {
        unsigned fb;
        unsigned char pixels[W * H * 4];
        gen_fb(1, &fb);
        bind_fb(FB, fb);
        attach(FB, COLOR0, GL_TEXTURE_2D, textures[f], 0);
        if (fb_status(FB) != GL_FRAMEBUFFER_COMPLETE) failed = 1;
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        owner_hash[f] = digest(pixels);
        bind_fb(FB, 0);
        del_fb(1, &fb);
        if (serial_hash[f] != parallel_hash[f] ||
            serial_hash[f] != owner_hash[f]) failed = 1;
        printf("frame=%d serial=%016llx worker=%016llx owner=%016llx\n", f,
               (unsigned long long)serial_hash[f],
               (unsigned long long)parallel_hash[f],
               (unsigned long long)owner_hash[f]);
    }
    glDeleteTextures(FRAMES, textures);
    SDL_GL_MakeCurrent(window, NULL);
    for (int i = 2; i >= 0; --i) SDL_GL_DeleteContext(contexts[i]);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("shared_contexts=%s serial_ms=%.3f parallel_ms=%.3f\n",
           failed ? "FAIL" : "PASS", serial_ns / 1e6, parallel_ns / 1e6);
    return failed ? 1 : 0;
}
