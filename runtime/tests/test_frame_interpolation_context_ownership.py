#!/usr/bin/env python3
"""Pin temporal blending to the renderer's original thread and GL context."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RENDERER = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(
    encoding="utf-8"
)

# The only second context is the present thread's ([video] present_thread,
# docs/RENDER_THREAD.md), created in pt_begin for the presentation consumer.
assert RENDERER.count("SDL_GL_CreateContext(") == 2, (
    "temporal blending must not create a second GL context"
)
_pt = RENDERER[RENDERER.index("static int pt_begin(void)"):]
_pt = _pt[:_pt.index("\n}\n")]
assert "SDL_GL_CreateContext(" in _pt, "the second GL context belongs to the present thread"
# CPU seam preparation has a worker and mutex; it owns no GL context. Scope
# this guard to temporal presentation instead of banning unrelated CPU work.
_interp = RENDERER[RENDERER.index("static void interp_reset_history_unlocked(void)"):
                   RENDERER.index("/* ==== Render passes")]
assert "SDL_CreateThread(" not in _interp, (
    "temporal blending must not create a presentation thread"
)
assert "SDL_LockMutex(" not in _interp, (
    "the GL renderer must not hold an application mutex across driver work"
)
# gl_window_swap holds the swap once per SDL backend (#if PSX_SDL3 / #else).
_swap = RENDERER[RENDERER.index("static void gl_window_swap(void)"):]
_swap = _swap[:_swap.index("\n}\n")]
assert RENDERER.count("SDL_GL_SwapWindow(") == _swap.count("SDL_GL_SwapWindow(") >= 1, (
    "all OpenGL presentation must converge on one owned swap call"
)
assert "gl_renderer_interpolation_owns_cadence" in RENDERER
assert "no motion vectors" in RENDERER

print("single-context temporal blending ownership guard passed")
