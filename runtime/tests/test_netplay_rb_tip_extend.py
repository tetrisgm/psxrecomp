#!/usr/bin/env python3
"""Exercise the production continue-replay path with delayed old frame commits."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    begin = source.rindex(signature)
    brace = source.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / 'runtime/src/psx_netplay_rb.c').read_text()
    prelude = r'''
#include <assert.h>
#include <stdio.h>
#include "netplay_hash_confirm.h"
enum { nRNetRbPhaseReplay=3, nRNetRbPhaseVerify=4 };
typedef struct { unsigned sim; } RNetSession;
static RNetSession session;
static int rb, *g_rb=&rb, phase=nRNetRbPhaseReplay;
static struct { NetplayHashConfirm *hc; } g_b;
static unsigned g_tip_extend_prime_tick, g_pending_load_tick;
static int g_episode_snap_applied=1, g_pending_load_valid, g_tip_extend_rereplay;
static int g_pending_resume_valid, g_pin_valid=1;
static unsigned g_pin_tick=6048;
static void *g_snaps;
static unsigned rnet_rb_get_load_tick(int *r) { return 6048; }
static unsigned rnet_rb_get_target_tick(int *r) { return 6083; }
static int rnet_rb_get_phase(int *r) { return phase; }
static int rnet_rb_is_active(int *r) { return 1; }
static void rnet_rb_set_phase(int *r,int p) { phase=p; }
static RNetSession *sess(void) { return &session; }
static unsigned rnet_session_sim_tick(RNetSession *s) { return s->sim; }
static int tip_extend_keep_live(unsigned t) { return 0; }
static int netplay_snap_ring_has(void *s,unsigned t) { return 0; }
static void clear_post_handshake(void) {}
'''
    driver = r'''
int main(void) {
    NetplayHashConfirm hc = {0};
    unsigned h;
    g_b.hc=&hc;
    netplay_hc_reset(&hc);
    netplay_hc_prime_after(&hc,6077);
    netplay_hc_note_local(&hc,6078,0x1111);
    netplay_hc_note_peer(&hc,6078,0x1111);
    netplay_hc_note_peer(&hc,6080,0xadb9d88c); /* delayed old input history */
    session.sim=6078;
    schedule_episode_rereplay(6080); /* input 6080 changed; keep prefix */
    assert(!g_pending_load_valid && session.sim==6078);
    assert(netplay_hc_resolved_through(&hc)==6077);
    assert(!netplay_hc_peer_digest(&hc,6080,&h));
    assert(psx_netplay_rb_ignore_peer_frame_commit(6080,0xadb9d88c));
    netplay_hc_note_local(&hc,6080,0x35a83174);
    assert(psx_netplay_rb_ignore_peer_frame_commit(6080,0xadb9d88c));
    assert(!psx_netplay_rb_ignore_peer_frame_commit(6080,0x35a83174));
    netplay_hc_note_peer(&hc,6080,0x35a83174);
    assert(netplay_hc_peer_digest(&hc,6080,&h) && h==0x35a83174);
    /* When already past the changed row, the production path must reload. */
    session.sim=6081;
    schedule_episode_rereplay(6080);
    assert(g_pending_load_valid && g_pending_load_tick==6048);
    return 0;
}
'''
    prelude += r'''
#define RB_MEDIA_SNAP_RING 32u
typedef struct { uint32_t tick; } RbMediaSnap;
static RbMediaSnap g_media_snap[RB_MEDIA_SNAP_RING];
static uint32_t g_media_snap_head, g_media_snap_n, g_media_crc_peer_match_through;
static int g_media_kf_episode=1, seal_count, checksum_count;
static uint8_t snap_bytes[4]={1,2,3,4};
static const uint8_t *netplay_snap_ring_peek(void *ring,uint32_t tick,size_t *size) { *size=4;return snap_bytes; }
static uint32_t rnet_checksum(const void *p,size_t size) { checksum_count++; return 0x12345678; }
static int rb_media_kf_seal_local(uint32_t tick) { seal_count++;return 1; }
'''
    prelude += function(source, 'static void rb_media_snap_note(')
    prelude += function(source, 'static int rb_media_snap_known(')
    prelude += function(source, 'int psx_netplay_rb_media_kf_probe_match(')
    driver = driver.replace('    return 0;', r'''
    rb_media_snap_note(0,4);rb_media_snap_note(1,0);
    assert(g_media_snap_n==0);
    for(unsigned t=1;t<=40;t++)rb_media_snap_note(t,4);
    assert(g_media_snap_n==32 && !rb_media_snap_known(8) && rb_media_snap_known(9));
    unsigned head=g_media_snap_head;
    rb_media_snap_note(40,4);assert(g_media_snap_head==head && checksum_count==0);
    g_snaps=snap_bytes;
    assert(!psx_netplay_rb_media_kf_probe_match(3,0x12345678));
    assert(checksum_count==0 && seal_count==0);
    assert(!psx_netplay_rb_media_kf_probe_match(4,0xdeadbeef));
    assert(checksum_count==1 && seal_count==0);
    assert(psx_netplay_rb_media_kf_probe_match(4,0x12345678));
    assert(checksum_count==2 && seal_count==1 && g_media_crc_peer_match_through==6048);
    return 0;
''')
    program = prelude + function(source, 'static void schedule_episode_rereplay(uint32_t prefer_plus_one)\n{') + '\n' + function(source, 'int psx_netplay_rb_ignore_peer_frame_commit(') + driver
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path/'test.c').write_text(program)
        subprocess.run([args.compiler, '-std=c11', '-I'+str(root/'runtime/include'), '-I'+str(root/'lib/recomp-net/include'), str(path/'test.c'), str(root/'lib/recomp-net/src/rollback/rnet_hash_confirm.c'), '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True)


if __name__ == '__main__':
    main()
