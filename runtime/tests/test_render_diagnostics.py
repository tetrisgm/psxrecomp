"""Verify renderer-only TCP queries never acquire GL on the guest thread.

Run against an already running hidden debug runtime with render_thread enabled.
The caller owns its process. This test changes no graphics settings.
"""
import argparse
import json
import os
import socket
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=int(os.getenv("PSX_RUNTIME_TEST_PORT", "0")))
    args = parser.parse_args()
    if not args.port:
        print("SKIP: provide --port or PSX_RUNTIME_TEST_PORT for an owned hidden runtime")
        return 77

    def command(name, **values):
        with socket.create_connection(("127.0.0.1", args.port), timeout=20) as sock:
            sock.sendall((json.dumps(dict(cmd=name, **values)) + "\n").encode())
            with sock.makefile("rb") as stream:
                result = json.loads(stream.readline())
        assert result.get("ok"), (name, result)
        return result

    before = command("render_thread")
    assert before["active"] and not before["held"], before
    results = {}
    for name in ("gl_coh_ring", "gl_present_ring", "post_aa",
                 "post_shader", "bloom", "batch_sites", "gl_interp", "gl_ws_ablate",
                 "present_image_ring_stats", "display_ring_stats", "gl_fbo_peek", "gl_vram_diff"):
        result = command(name)
        results[name] = {k: v for k, v in result.items() if k != "events"}
    after = command("render_thread")
    assert not after["held"], after
    for name in ("acquires", "releases"):
        assert after[name] == before[name], (name, before, after)
    print(json.dumps({"queries": results, "acquires": after["acquires"] - before["acquires"],
                      "releases": after["releases"] - before["releases"]}))


if __name__ == "__main__":
    sys.exit(main())
