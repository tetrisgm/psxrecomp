/* Resolution-independent rewind rendering. Keep the exact ordered renderer
 * inputs and native texture memory, never drawable pixels or fitted geometry.
 * Only complete, queued, rollback-isolated producers may enter this history. */
#define RD_CAPACITY (8u*1024u*1024u)
#define RD_MAGIC 0x31574452u
typedef struct RewindDrawHeader {
    uint32_t magic,bytes,commands,hd_bytes,phase,records;
    uint64_t cycle,pixel_hash;
    int32_t begin[9];
    int wide_w,wide_off,source_path,dither;
    FgState state;
    int depth_need,depth_used,depth_reason;
    float depth_avg;
} RewindDrawHeader;
static struct {
    RewindHistory *history;
    uint8_t *capture,*decoded;
    size_t used;
    uint64_t sequence,rendered,rejected,pixel_checks,pixel_mismatches,last_tick;
    int active,ended,last_rejected_op;
    GLuint texture,spare_texture,raw_texture,raw_fbo;
    uint64_t texture_key,spare_key,cache_hits;
    PassGen texture_generation,spare_generation;
    int spare_w,spare_h;
    uint16_t *native;
    int texture_w,texture_h;
    unsigned pending_restored;
    int pending_generation;
    uint32_t pending_tick;
} RD;
static int rewind_draw_op(uint16_t op) {
    switch(op) {
    case RTH_SEMI:case RTH_MASK:case RTH_TWIN:case RTH_MOD:
    case RTH_PRECISE:case RTH_PERSP:case RTH_DEPTH:case RTH_DITHER:
    case RTH_FILL:case RTH_COPY:case RTH_FLAT_TRI:case RTH_GOURAUD_TRI:
    case RTH_TEX_TRI:case RTH_SHADED_TEX_TRI:case RTH_FLAT_RECT:
    case RTH_TEX_RECT:case RTH_TEX_RECT_SCALED:case RTH_LINE:case RTH_SHADED_LINE:
    case RTH_VRAM_WRITE:case RTH_XFER_IN:case RTH_AREA:case RTH_OFFSET:
    case RTH_WIDE_CONFIGURE:case RTH_WIDE_VIEW:case RTH_WIDE_TARGET:case RTH_WIDE_DISABLE:
    case RTH_WIDE_CLEAR:case RTH_WIDE_CLEAR_MARGINS:case RTH_PROJ_TRI:
    case RTH_WIDE_RECOVERY:case RTH_STATE:case RTH_HD_NOTE:case RTH_WIDE_FEEDBACK:
        return 1;
    default:return 0; /* In particular, never retain pointer-bearing jobs. */
    }
}
static void rewind_draw_shutdown(void) {
    rewind_history_destroy(RD.history);
    free(RD.capture);free(RD.decoded);free(RD.native);
    if(RD.raw_fbo)p_glDeleteFramebuffers(1,&RD.raw_fbo);
    if(RD.raw_texture)glDeleteTextures(1,&RD.raw_texture);
    if(RD.texture)glDeleteTextures(1,&RD.texture);
    if(RD.spare_texture)glDeleteTextures(1,&RD.spare_texture);
    memset(&RD,0,sizeof RD);
}
static void rewind_draw_enable(int enabled) {
    if(!enabled){rewind_draw_shutdown();return;}
    if(RD.history)return;
    RD.history=rewind_history_create((size_t)256*1024*1024,900,RD_CAPACITY);
    RD.capture=malloc(RD_CAPACITY);RD.decoded=malloc(RD_CAPACITY);
    if(!RD.history || !RD.capture || !RD.decoded)rewind_draw_shutdown();
}
static void rewind_draw_interrupt(void) { RD.active=RD.ended=0; }
static void rewind_draw_record(const RtCmd *cmd,const void *payload) {
    if(!RD.history || RV.frozen || s_rewind_command_replay)return;
    if(cmd->op==RTH_PASS_BEGIN) {
        RD.active=RD.ended=0;
        if(cmd->payload!=9*sizeof(int32_t) || !s_hd_native_authority)return;
        const int32_t *v=payload;
        if(!(v[6]&GL_PASS_COMPLETE_IMAGE))return;
        RewindDrawHeader *h=(RewindDrawHeader*)RD.capture;
        memset(h,0,sizeof *h);h->magic=RD_MAGIC;
        memcpy(h->begin,v,sizeof h->begin);
        h->cycle=(uint64_t)(uint32_t)v[7]|((uint64_t)(uint32_t)v[8]<<32);
        h->wide_w=g_wide_w;h->wide_off=g_wide_off;
        h->source_path=s_interp_source_path;h->dither=s_dither_bit;
        fg_state_capture(&h->state);
        size_t at=sizeof *h;
        memcpy(RD.capture+at,s_vram,(size_t)VRAM_W*VRAM_H*sizeof(uint16_t));
        at+=(size_t)VRAM_W*VRAM_H*sizeof(uint16_t);
        uint8_t *hd=NULL;size_t hd_size=0;
        if((gpu_hd_textures_active() && !gpu_hd_textures_residency_save(&hd,&hd_size)) || at+hd_size>RD_CAPACITY) {
            free(hd);RD.last_rejected_op=-1;++RD.rejected;return;
        }
        h->hd_bytes=(uint32_t)hd_size;
        if(hd_size)memcpy(RD.capture+at,hd,hd_size);
        free(hd);at+=hd_size;at=(at+15u)&~(size_t)15u;
        h->commands=(uint32_t)at;RD.used=at;RD.active=1;return;
    }
    if(!RD.active)return;
    if(cmd->op==RTH_PASS_END){RD.ended=1;return;}
    if(!rewind_draw_op(cmd->op)){RD.active=0;RD.last_rejected_op=cmd->op;++RD.rejected;return;}
    if(cmd->op==RTH_WIDE_CONFIGURE) {
        const int32_t *v=payload;const RewindDrawHeader *h=(const RewindDrawHeader*)RD.capture;
        if(cmd->payload!=2*sizeof(int32_t) || v[0]!=h->wide_w || v[1]!=h->wide_off) {
            RD.active=0;RD.last_rejected_op=cmd->op;++RD.rejected;return;
        }
    }
    size_t bytes=(cmd->payload+3u)&~(size_t)3u;
    if(RD.used+sizeof(FgRec)+bytes>RD_CAPACITY){RD.active=0;++RD.rejected;return;}
    FgRec rec={cmd->op,cmd->flags,cmd->payload};
    memcpy(RD.capture+RD.used,&rec,sizeof rec);RD.used+=sizeof rec;
    memcpy(RD.capture+RD.used,payload,cmd->payload);
    memset(RD.capture+RD.used+cmd->payload,0,bytes-cmd->payload);RD.used+=bytes;
    ++((RewindDrawHeader*)RD.capture)->records;
}
/* Opt-in diagnostic: compare complete original/replayed internal pixels.
 * Only the digest enters history; no framebuffer is retained for playback. */
static uint64_t rewind_draw_pixel_hash(GLuint texture,int w,int h) {
    size_t bytes=(size_t)w*h*4;uint8_t *pixels=malloc(bytes);
    if(!pixels)return 0;
    GLint pbo,row,skipr,skipp,alignment;
    glGetIntegerv(0x88ED /* GL_PIXEL_PACK_BUFFER_BINDING */,&pbo);
    glGetIntegerv(GL_PACK_ROW_LENGTH,&row);glGetIntegerv(GL_PACK_SKIP_ROWS,&skipr);
    glGetIntegerv(GL_PACK_SKIP_PIXELS,&skipp);glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
    p_glBindBuffer(RV_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ROW_LENGTH,0);
    glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glBindTexture(GL_TEXTURE_2D,texture);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glPixelStorei(GL_PACK_ROW_LENGTH,row);glPixelStorei(GL_PACK_SKIP_ROWS,skipr);
    glPixelStorei(GL_PACK_SKIP_PIXELS,skipp);glPixelStorei(GL_PACK_ALIGNMENT,alignment);
    p_glBindBuffer(RV_PIXEL_PACK_BUFFER,pbo);
    uint64_t hash=0xcbf29ce484222325ull;size_t at=0;
    for(;at+8<=bytes;at+=8){uint64_t word;memcpy(&word,pixels+at,8);hash=(hash^word)*0x100000001b3ull;}
    for(;at<bytes;at++)hash=(hash^pixels[at])*0x100000001b3ull;
    free(pixels);return hash?hash:1;
}
static uint64_t rewind_draw_seal(uint32_t phase,GLuint texture,int w,int height) {
    if(!RD.active || !RD.ended || s_rewind_command_replay)return 0;
    RD.active=RD.ended=0;
    RewindDrawHeader *h=(RewindDrawHeader*)RD.capture;
    h->phase=phase;h->bytes=(uint32_t)RD.used;
    h->cycle=s_pgen[s_pgen_write].created_cycle;
    h->depth_need=s_pb_depth_need;h->depth_used=s_pb_depth_used;
    h->depth_reason=s_pb_depth_reason;h->depth_avg=s_pb_depth_avg;
    const char *verify=getenv("PSX_REWIND_VERIFY_DRAW");
    if(verify && verify[0]=='1')h->pixel_hash=rewind_draw_pixel_hash(texture,w,height);
    int slot;void *job=rewind_history_reserve(RD.history,RD.used,&slot);
    if(!job)return 0;
    memcpy(job,RD.capture,RD.used);
    RewindHistoryMeta meta={++RD.sequence,h->cycle,0,0};
    rewind_history_submit(RD.history,slot,&meta);
    return meta.key;
}
int gl_renderer_rewind_draw_select(uint64_t key,uint32_t oldest,uint64_t *shown,uint64_t *tick) {
    GL_RT_SYNC("rewind_draw_select");
    if(!RD.history)return 0;
    if(!key || key>RV.complete_latest)key=RV.complete_latest;
    for(unsigned n=0;key && key>=oldest && n<2048;--key,++n) {
        unsigned at=(unsigned)(key%2048);
        if(RV.complete_ticks[at]==key && RV.draw_keys[at] &&
           rewind_history_contains(RD.history,RV.draw_keys[at])) {
            *shown=*tick=key;return 1;
        }
    }
    return 0;
}
static int rewind_draw_valid(const RewindDrawHeader *h,size_t bytes) {
    size_t native_bytes=(size_t)VRAM_W*VRAM_H*sizeof(uint16_t);
    if(bytes<sizeof *h || h->magic!=RD_MAGIC || h->bytes!=bytes ||
       h->commands<sizeof *h+native_bytes || h->commands>bytes ||
       h->hd_bytes>h->commands-sizeof *h-native_bytes || !h->records)return 0;
    size_t at=h->commands;
    for(uint32_t n=0;n<h->records;n++) {
        if(at+sizeof(FgRec)>bytes)return 0;
        FgRec c;memcpy(&c,(const uint8_t*)h+at,sizeof c);at+=sizeof c;
        if(!rewind_draw_op(c.op) || c.bytes>bytes-at)return 0;
        at+=(c.bytes+3u)&~(size_t)3u;
    }
    return at==bytes;
}
static void rewind_draw_software_state(const FgState *state) {
    sw_set_semi_transparency(state->semi_en,state->semi_mode);
    sw_set_mask_bits(state->mask_set,state->mask_check);
    sw_set_texture_window((uint32_t)(state->tw[0]|(state->tw[1]<<5)|(state->tw[2]<<10)|(state->tw[3]<<15)));
    sw_set_color_modulation(state->mod[0],state->mod[1],state->mod[2],state->mod[3]);
    sw_set_draw_area(state->area[0],state->area[1],state->area[2],state->area[3]);
    sw_set_draw_offset(state->off[0],state->off[1]);
    sw_set_precise_triangle(0,0,0,0,0,0,0);sw_set_perspective_triangle(0,0,0,0);
}
static void rewind_draw_cache_clear(void) { RD.texture_key=RD.spare_key=0; }
static void rewind_draw_cache_swap(void) {
    GLuint texture=RD.texture;RD.texture=RD.spare_texture;RD.spare_texture=texture;
    uint64_t key=RD.texture_key;RD.texture_key=RD.spare_key;RD.spare_key=key;
    int w=RD.texture_w,h=RD.texture_h;RD.texture_w=RD.spare_w;RD.texture_h=RD.spare_h;
    RD.spare_w=w;RD.spare_h=h;
    PassGen g=RD.texture_generation;RD.texture_generation=RD.spare_generation;RD.spare_generation=g;
}
static int rewind_draw_render(uint64_t draw_key,int decoded) {
    size_t bytes=0;RewindHistoryMeta meta;
    if(!decoded && (!rewind_history_decode_before(RD.history,draw_key,RD.decoded,RD_CAPACITY,&bytes,&meta) ||
       meta.key!=draw_key || !rewind_draw_valid((RewindDrawHeader*)RD.decoded,bytes)))return 0;
    const RewindDrawHeader *h=(const RewindDrawHeader*)RD.decoded;
    if(h->wide_w!=g_wide_w || h->wide_off!=g_wide_off || h->source_path!=s_interp_source_path ||
       !s_hd_native_authority || s_pass_active)return 0;
    /* Two working images, not pixel history. Backward playback commonly
     * visits the same producer twice; its next forward producer was just
     * displayed and can be restored without drawing it a third time. */
    if(RD.spare_key==draw_key && RD.spare_w==s_interp_w && RD.spare_h==s_interp_h)
        rewind_draw_cache_swap();
    if(RD.texture_key==draw_key && RD.texture_w==s_interp_w && RD.texture_h==s_interp_h) {
        ++RD.cache_hits;return 1;
    }
    FgState saved;fg_state_capture(&saved);
    const int saved_dither=s_dither_bit;
    uint8_t *saved_hd=NULL;size_t saved_hd_bytes=0;
    if(gpu_hd_textures_active() && !gpu_hd_textures_residency_save(&saved_hd,&saved_hd_bytes))return 0;
    if(!RD.native)RD.native=malloc((size_t)VRAM_W*VRAM_H*2);
    if(!RD.raw_texture)RD.raw_texture=make_tex(PSXGL_R16UI,VRAM_W,VRAM_H,PSXGL_RED_INTEGER,GL_UNSIGNED_SHORT);
    if(!RD.raw_fbo && RD.raw_texture)make_fbo(&RD.raw_fbo,RD.raw_texture,0);
    if(!RD.native || !RD.raw_fbo){free(saved_hd);return 0;}
    const int32_t *v=h->begin;
    uint32_t leaks=s_pass_leaks;
    if(!transaction_begin(v[0],v[1],v[2],v[3],1,(uint32_t)v[5],
                          GL_PASS_COMPLETE_IMAGE,0,h->cycle)) {
        free(saved_hd);return 0;
    }
    s_rewind_command_replay=1;
    const uint8_t *native=RD.decoded+sizeof *h;
    /* Native-authority draws sample this 1 MiB texture. A private native
     * mirror avoids copying/restoring the entire high-resolution VRAM FBO. */
    uint16_t *live_native=s_vram;GLuint live_raw=s_raw_tex,live_raw_fbo=s_raw_fbo;
    memcpy(RD.native,native,(size_t)VRAM_W*VRAM_H*2);
    s_vram=RD.native;sw_renderer_rebind_vram(s_vram);gpu_hd_textures_bind_vram(s_vram);
    s_raw_tex=RD.raw_texture;s_raw_fbo=RD.raw_fbo;
    p_glActiveTexture(PSXGL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,s_raw_tex);
    p_glBindBuffer(PSXGL_PIXEL_UNPACK_BUFFER,0);
    glPixelStorei(GL_UNPACK_ALIGNMENT,2);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    /* The previous preview may still sample this texture. Stage through the
     * existing orphaned unpack buffer rather than wait for CPU texture access. */
    upload_texture_pixels(0,0,VRAM_W,VRAM_H,PSXGL_RED_INTEGER,GL_UNSIGNED_SHORT,
                          RD.native,(size_t)VRAM_W*VRAM_H*sizeof(uint16_t));
    int ok=s_pass_leaks==leaks;
    if(ok && h->hd_bytes)
        ok=gpu_hd_textures_residency_load(native+(size_t)VRAM_W*VRAM_H*2,h->hd_bytes);
    fg_state_apply(&h->state,h->state.wide_on?pass_wide_fbo_for(h->state.wide_base):0);
    rewind_draw_software_state(&h->state);
    s_dither_bit=h->dither;
    s_depth_need_clear=h->depth_need;s_depth_used=h->depth_used;
    s_depth_need_reason=h->depth_reason;s_depth_last_avg=h->depth_avg;
    size_t at=h->commands;
    for(uint32_t n=0;ok && n<h->records;n++) {
        FgRec record;memcpy(&record,RD.decoded+at,sizeof record);at+=sizeof record;
        RtCmd cmd={0};cmd.op=record.op;cmd.flags=record.flags;cmd.payload=record.bytes;
        gl_rth_exec(NULL,&cmd,RD.decoded+at);
        at+=(record.bytes+3u)&~(size_t)3u;
        ok=s_pass_leaks==leaks;
    }
    flush_flat_batch();flush_tex_batch();flush_cpu_upload();hiw_flush_queue();
    PassGen produced=s_pgen[s_pgen_write];
    if(ok) {
        rewind_draw_cache_swap();RD.texture_key=0;
        RD.texture_generation=produced;
        if(!RD.texture)glGenTextures(1,&RD.texture);
        if(RD.texture_w!=produced.tex_w || RD.texture_h!=produced.tex_h) {
            glBindTexture(GL_TEXTURE_2D,RD.texture);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,produced.tex_w,produced.tex_h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
            RD.texture_w=produced.tex_w;RD.texture_h=produced.tex_h;
        }
        pass_capture_into(RD.texture,&produced,0);
        if(h->pixel_hash) {
            ++RD.pixel_checks;
            if(rewind_draw_pixel_hash(RD.texture,RD.texture_w,RD.texture_h)!=h->pixel_hash)++RD.pixel_mismatches;
        }
    }
    transaction_restore();s_rewind_command_replay=0;
    s_vram=live_native;s_raw_tex=live_raw;s_raw_fbo=live_raw_fbo;
    sw_renderer_rebind_vram(s_vram);gpu_hd_textures_bind_vram(s_vram);
    fg_state_apply(&saved,saved.wide_on?pass_wide_fbo_for(saved.wide_base):0);
    rewind_draw_software_state(&saved);
    s_dither_bit=saved_dither;

    if(saved_hd_bytes && !gpu_hd_textures_residency_load(saved_hd,saved_hd_bytes))ok=0;
    free(saved_hd);
    if(!ok){++RD.rejected;return 0;}
    RD.texture_key=draw_key;
    return 1;
}
int gl_renderer_rewind_draw_present(uint64_t key,uint32_t oldest,uint64_t *shown,uint64_t *tick) {
    GL_RT_SYNC("rewind_draw_present");
    uint64_t selected,clock;
    if(!gl_renderer_rewind_draw_select(key,oldest,&selected,&clock))return 0;
    uint64_t draw_key=RV.draw_keys[selected%2048];
    if(!rewind_draw_render(draw_key,0))return 0;
    const RewindDrawHeader *h=(const RewindDrawHeader*)RD.decoded;
    RV.command_tick=clock;RV.resume_tick=clock;
    s_rewind_complete_source=1;s_rewind_complete_cycle=h->cycle;
    s_rewind_complete_phase=h->phase;s_rewind_complete_draw_key=draw_key;
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,0);
    interp_present_pair(RD.texture,RD.texture,0,s_interp_blend_mode);
    s_rewind_complete_source=0;RV.command_tick=0;
    if(RV.presented_tick!=clock)return 0;
    ++RD.rendered;RD.last_tick=clock;*shown=selected;*tick=clock;return 1;
}

int gl_renderer_rewind_draw_presented(uint32_t tick) {
    GL_RT_SYNC("rewind_draw_presented");return RD.last_tick==tick && RV.presented_tick==tick;
}
void gl_renderer_rewind_draw_resume(uint32_t tick) {
    GL_RT_SYNC("rewind_draw_resume");
    /* Keep the exact command preview on screen while normal forward drawing
     * rebuilds its pipeline. Native-resolution restage cannot open this gate. */
    RV.frozen=1;RV.resume_tick=tick;RV.resume_complete=1;RV.resume_cycle=0;RV.resume_phase=0;
    rewind_resume_image_start_ns=rt_now_ns();
    atomic_store(&rewind_resume_image_us,UINT64_MAX);
    if(RD.pending_tick) {
        PassGen *g=&s_pgen[RD.pending_generation];
        /* This image was already issued at the restored machine boundary.
         * Publish it now, then hold it until normal presentation reaches its
         * phase. Never expose the native-resolution restage or move backward
         * to the preceding phase-zero image while the queue catches up. */
        RV.command_tick=RD.pending_tick;RV.pending_publish=1;
        s_rewind_complete_source=1;s_rewind_complete_cycle=g->created_cycle;
        s_rewind_complete_phase=g->phase[1];s_rewind_complete_draw_key=g->draw_key[1];
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,0);
        int published=interp_present_pair(s_pgen_tex[RD.pending_generation][1],
                            s_pgen_tex[RD.pending_generation][1],0,s_interp_blend_mode);
        s_rewind_complete_source=0;RV.command_tick=0;RV.pending_publish=0;
        /* Publication is included in release_us; no additional image wait. */
        if(published)atomic_store(&rewind_resume_image_us,0);
        RV.frozen=1;RV.resume_complete=1;RV.resume_tick=RD.pending_tick;
        RV.resume_cycle=g->created_cycle;RV.resume_phase=g->phase[1];
        RD.pending_tick=0;
    }
}
uint64_t gl_renderer_rewind_resume_image_us(void) {
    /* Observing a release must not drain the renderer queue and alter pacing. */
    return atomic_load(&rewind_resume_image_us);
}
/* The machine checkpoint omits GPU producer work already issued before
 * its boundary. Recover only commands from this branch with an eligible
 * issue cycle; future input may never enter the restored timeline. */
static void rewind_draw_restore_pending(uint32_t tick) {
    RD.pending_tick=0;
    if(!RD.history || psx_rewind_state_playback()!=2)return;
    for(unsigned i=1;i<=8;++i) {
        unsigned slot=(tick+i)%2048;
        if(RV.complete_ticks[slot]!=tick+i || !RV.draw_keys[slot])continue;
        uint64_t key=RV.draw_keys[slot];size_t bytes=0;RewindHistoryMeta meta;
        if(!rewind_history_decode_before(RD.history,key,RD.decoded,RD_CAPACITY,&bytes,&meta) ||
           meta.key!=key || !rewind_draw_valid((RewindDrawHeader*)RD.decoded,bytes))continue;
        const RewindDrawHeader *h=(const RewindDrawHeader*)RD.decoded;
        uint64_t issued=(uint32_t)h->begin[7]|((uint64_t)(uint32_t)h->begin[8]<<32);
        if(issued>psx_cycle_count || !h->phase)continue;
        uint32_t phase=h->phase;
        if(!rewind_draw_render(key,1))continue;
        /* Preview transactions are not live forward generations. */
        pass_gens_invalidate();
        uint64_t order[PASS_GENERATIONS];uint32_t valid=0;
        for(int j=0;j<PASS_GENERATIONS;++j) {order[j]=s_pgen[j].order;if(s_pgen[j].valid)valid|=1u<<j;}
        int gi=render_pass_generation_write(s_pgen_cur,valid,order,PASS_GENERATIONS);
        if(!pass_gen_reserve(gi,2,RD.texture_w,RD.texture_h))continue;
        GLuint spare=s_pgen_tex[gi][1];s_pgen_tex[gi][1]=RD.texture;RD.texture=spare;
        PassGen *g=&s_pgen[gi];*g=RD.texture_generation;
        g->valid=1;g->promoted=0;g->order=++s_pgen_order;
        RD.texture_key=0;
        g->n=2;g->complete[0]=0;g->complete[1]=1;
        g->phase[0]=0;g->phase[1]=phase;g->draw_key[0]=0;g->draw_key[1]=key;
        RD.pending_generation=gi;RD.pending_tick=tick+i;
        ++RD.pending_restored;return;
    }
}
static void rewind_draw_stats(char *out,int capacity) {
    RewindHistoryStats s;rewind_history_stats(RD.history,&s);
    snprintf(out,(size_t)capacity,",\"draw_last_tick\":%llu,\"resume_complete_pending\":%d,\"draw_pixel_checks\":%llu,\"draw_pixel_mismatches\":%llu,\"draw_rejected_op\":%d,\"draw_entries\":%u,\"draw_bytes\":%llu,\"draw_rendered\":%llu,\"draw_rejected\":%llu,\"draw_dropped\":%llu",(unsigned long long)RD.last_tick,RV.resume_complete,(unsigned long long)RD.pixel_checks,(unsigned long long)RD.pixel_mismatches,RD.last_rejected_op,s.entries,(unsigned long long)s.bytes,(unsigned long long)RD.rendered,(unsigned long long)RD.rejected,(unsigned long long)s.dropped);
    size_t used=strlen(out);
    if(used<(size_t)capacity)snprintf(out+used,(size_t)capacity-used,",\"pending_restored\":%u,\"draw_cache_hits\":%llu",RD.pending_restored,(unsigned long long)RD.cache_hits);
}
