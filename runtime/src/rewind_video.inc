#define RV_PIXEL_PACK_BUFFER 0x88EB
#define RV_STREAM_READ 0x88E1
#define RV_READ_ONLY 0x88B8
/* Full-resolution lossless history, isolated from native VRAM and PGXP.
 * PBO fences are polled, never waited during racing. The compression worker
 * receives complete pixels; no disk or thumbnail path participates. */
typedef GLenum (APIENTRY *RvClientWait)(void *,GLbitfield,uint64_t);
static struct {
    int configured, enabled, frozen, w, h;
    uint64_t resume_tick, presented_tick, complete_ticks[2048], complete_latest, held_incomplete;
    uint64_t complete_cycles[2048], forced_complete;
    uint32_t complete_phases[2048];
    int state_enabled;
    RewindHistory *history;
    GLuint pbo[3], texture, fbo;
    void *fence[3];
    RewindHistoryMeta meta[3];
    uint64_t sequence, gpu_dropped, map_failed;
    unsigned next;
    uint8_t *decoded;
    size_t capacity;
    RvClientWait wait;
} RV;
static void rewind_video_shutdown(void) {
    rewind_history_destroy(RV.history);RV.history=NULL;
    for(int i=0;i<3;i++)if(RV.fence[i]){p_glDeleteSync(RV.fence[i]);RV.fence[i]=NULL;}
    if(RV.pbo[0])p_glDeleteBuffers(3,RV.pbo);
    if(RV.texture)glDeleteTextures(1,&RV.texture);
    if(RV.fbo)p_glDeleteFramebuffers(1,&RV.fbo);
    free(RV.decoded);
    int configured=RV.configured,enabled=RV.enabled;
    memset(&RV,0,sizeof RV);RV.configured=configured;RV.enabled=enabled;
}
static int rewind_video_prepare(int w,int h) {
    if(RV.history && RV.w==w && RV.h==h)return 1;
    rewind_video_shutdown();
    if(w<=0 || h<=0 || (uint64_t)w*h>4096u*4096u)return 0;
    RV.capacity=(size_t)w*h*4;
    RV.history=rewind_history_create((size_t)1024*1024*1024,900,RV.capacity);
    RV.decoded=malloc(RV.capacity);
    RV.wait=(RvClientWait)SDL_GL_GetProcAddress("glClientWaitSync");
    if(!RV.history || !RV.decoded || !RV.wait || !p_glFenceSync || !p_glMapBuffer){rewind_video_shutdown();return 0;}
    RV.w=w;RV.h=h;
    p_glGenBuffers(3,RV.pbo);
    for(int i=0;i<3;i++) {
        p_glBindBuffer(RV_PIXEL_PACK_BUFFER,RV.pbo[i]);
        p_glBufferData(RV_PIXEL_PACK_BUFFER,(ptrdiff_t)RV.capacity,NULL,RV_STREAM_READ);
    }
    p_glBindBuffer(RV_PIXEL_PACK_BUFFER,0);
    return 1;
}
static void rewind_video_capture(void) {
    if(!RV.configured) {
        const char *e=getenv("PSX_REWIND_VIDEO");
        RV.enabled=e && e[0]=='1';RV.configured=1;
    }
    if(!RV.enabled || RV.frozen || !s_ctx)return;
    int w=0,h=0;SDL_GL_GetDrawableSize(s_win,&w,&h);
    GLint pack,alignment,row,skip_rows,skip_pixels,read_fbo,read_buffer;
    glGetIntegerv(0x88ED,&pack);glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH,&row);glGetIntegerv(GL_PACK_SKIP_ROWS,&skip_rows);glGetIntegerv(GL_PACK_SKIP_PIXELS,&skip_pixels);
    glGetIntegerv(0x8CAA,&read_fbo);glGetIntegerv(GL_READ_BUFFER,&read_buffer);
    if(!rewind_video_prepare(w,h))goto restore;
    /* Harvest strictly in source order. A newer ready slot cannot overtake an
     * older pending copy in the immutable history. */
    for(int count=0;count<3;count++) {
        int oldest=-1;
        for(int i=0;i<3;i++)if(RV.fence[i] && (oldest<0 || RV.meta[i].key<RV.meta[oldest].key))oldest=i;
        if(oldest<0)break;
        GLenum ready=RV.wait(RV.fence[oldest],0,0);
        if(ready!=0x911A && ready!=0x911C)break;
        int slot;void *raw=rewind_history_reserve(RV.history,RV.capacity,&slot);
        if(raw) {
            p_glBindBuffer(RV_PIXEL_PACK_BUFFER,RV.pbo[oldest]);
            const void *pixels=p_glMapBuffer(RV_PIXEL_PACK_BUFFER,RV_READ_ONLY);
            if(pixels) {
                memcpy(raw,pixels,RV.capacity);
                if(p_glUnmapBuffer(RV_PIXEL_PACK_BUFFER))rewind_history_submit(RV.history,slot,&RV.meta[oldest]);
                else {rewind_history_cancel(RV.history,slot);++RV.map_failed;}
            } else {rewind_history_cancel(RV.history,slot);++RV.map_failed;}
        }
        p_glDeleteSync(RV.fence[oldest]);RV.fence[oldest]=NULL;
    }
    unsigned i=RV.next;
    if(RV.fence[i]){++RV.gpu_dropped;goto restore;}
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,0);
    glReadBuffer(s_pt_on ? PSXGL_COLOR_ATTACHMENT0 : GL_BACK);
    p_glBindBuffer(RV_PIXEL_PACK_BUFFER,RV.pbo[i]);
    glPixelStorei(GL_PACK_ALIGNMENT,4);glPixelStorei(GL_PACK_ROW_LENGTH,0);
    glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    RV.fence[i]=p_glFenceSync(0x9117,0);
    extern uint32_t psx_rewind_frame(void);
    RV.meta[i]=(RewindHistoryMeta){++RV.sequence,rth_replaying()?s_rth_present_rewind_tick:psx_rewind_frame(),(uint32_t)w,(uint32_t)h};
    RV.next=(i+1)%3;
restore:
    p_glBindBuffer(RV_PIXEL_PACK_BUFFER,(GLuint)pack);
    glPixelStorei(GL_PACK_ALIGNMENT,alignment);glPixelStorei(GL_PACK_ROW_LENGTH,row);
    glPixelStorei(GL_PACK_SKIP_ROWS,skip_rows);glPixelStorei(GL_PACK_SKIP_PIXELS,skip_pixels);
    p_glBindFramebuffer_raw(PSXGL_READ_FRAMEBUFFER,(GLuint)read_fbo);glReadBuffer((GLenum)read_buffer);
}
void gl_renderer_rewind_video_enable(int enabled) {
    GL_RT_SYNC("rewind_video_enable");RV.configured=1;RV.enabled=!!enabled;
    if(!enabled)rewind_video_shutdown();
}
void gl_renderer_rewind_video_freeze(int frozen) {
    GL_RT_SYNC("rewind_video_freeze");RV.frozen=!!frozen;
    rewind_history_freeze(RV.history,frozen);
    RV.resume_tick=0;RV.presented_tick=0;
    if(frozen)for(int i=0;i<3;i++)if(RV.fence[i]){p_glDeleteSync(RV.fence[i]);RV.fence[i]=NULL;}
}
void gl_renderer_rewind_video_clear(void) {
    GL_RT_SYNC("rewind_video_clear");rewind_video_shutdown();
}
int gl_renderer_rewind_video_present_since(uint64_t key,uint32_t oldest,uint64_t *shown,uint64_t *tick) {
    GL_RT_SYNC("rewind_video_present");
    if(!RV.frozen || !RV.history)return 0;
    RewindHistoryMeta m;size_t bytes;
    if(!rewind_history_decode_before(RV.history,key?key:UINT64_MAX,RV.decoded,RV.capacity,&bytes,&m))return 0;
    if(m.clock<oldest)return 0;
    int w,h;SDL_GL_GetDrawableSize(s_win,&w,&h);
    if(w!=(int)m.width || h!=(int)m.height || bytes!=(size_t)w*h*4)return 0;
    if(!RV.texture){glGenTextures(1,&RV.texture);p_glGenFramebuffers(1,&RV.fbo);}
    p_glBindBuffer(PSXGL_PIXEL_UNPACK_BUFFER,0);
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    glBindTexture(GL_TEXTURE_2D,RV.texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,RV.decoded);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,RV.fbo);
    p_glFramebufferTexture2D(PSXGL_READ_FRAMEBUFFER,PSXGL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,RV.texture,0);
    glReadBuffer(PSXGL_COLOR_ATTACHMENT0);p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,0);
    glDisable(GL_SCISSOR_TEST);p_glBlitFramebuffer(0,0,w,h,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    gl_swap_now(0);
    if(shown)*shown=m.key;if(tick)*tick=m.clock;
    return 1;
}
void gl_renderer_rewind_video_stats(char *out,int capacity) {
    GL_RT_SYNC("rewind_video_stats");RewindHistoryStats s;rewind_history_stats(RV.history,&s);
    snprintf(out,(size_t)capacity,"\"enabled\":%d,\"state_enabled\":%d,\"complete_latest\":%llu,\"presented_tick\":%llu,\"held_incomplete\":%llu,\"forced_complete\":%llu,\"frozen\":%d,\"width\":%d,\"height\":%d,\"submitted\":%llu,\"completed\":%llu,\"dropped\":%llu,\"gpu_dropped\":%llu,\"map_failed\":%llu,\"bytes\":%llu,\"entries\":%u,\"pending\":%u",RV.enabled,RV.state_enabled,(unsigned long long)RV.complete_latest,(unsigned long long)RV.presented_tick,(unsigned long long)RV.held_incomplete,(unsigned long long)RV.forced_complete,RV.frozen,RV.w,RV.h,(unsigned long long)s.submitted,(unsigned long long)s.completed,(unsigned long long)s.dropped,(unsigned long long)RV.gpu_dropped,(unsigned long long)RV.map_failed,(unsigned long long)s.bytes,s.entries,s.pending);
}

int gl_renderer_rewind_video_present(uint64_t key,uint64_t *shown,uint64_t *tick) {
    return gl_renderer_rewind_video_present_since(key,0,shown,tick);
}
void gl_renderer_rewind_video_resume(uint64_t key,uint32_t tick) {
    GL_RT_SYNC("rewind_video_resume");
    if(key)rewind_history_drop_after(RV.history,key);
    RV.resume_tick=tick;
}
static int rewind_video_hold(void) {
    if(!RV.frozen)return 0;
    extern uint32_t psx_rewind_frame(void);
    uint64_t tick=rth_replaying()?s_rth_present_rewind_tick:psx_rewind_frame();
    if(!RV.resume_tick || tick<RV.resume_tick)return 1;
    if(RV.state_enabled) {
        unsigned i=(unsigned)(tick%2048);
        if(!s_rewind_complete_source || RV.complete_ticks[i]!=tick ||
           RV.complete_cycles[i]!=s_rewind_complete_cycle ||
           RV.complete_phases[i]!=s_rewind_complete_phase){++RV.held_incomplete;return 1;}
        RV.presented_tick=tick;RV.resume_tick=0;
        return 0;
    }
    RV.resume_tick=0;RV.frozen=0;rewind_history_freeze(RV.history,0);
    return 0;
}

static void rewind_state_note_present(void) {
    if(!RV.state_enabled || RV.frozen || !s_rewind_complete_source)return;
    extern uint32_t psx_rewind_frame(void);
    uint64_t tick=rth_replaying()?s_rth_present_rewind_tick:psx_rewind_frame();
    if(tick) {
        unsigned i=(unsigned)(tick%2048);RV.complete_ticks[i]=tick;
        RV.complete_cycles[i]=s_rewind_complete_cycle;RV.complete_phases[i]=s_rewind_complete_phase;
        RV.complete_latest=tick;
    }
}
void gl_renderer_rewind_state_enable(int enabled) {
    GL_RT_SYNC("rewind_state_enable");RV.state_enabled=!!enabled;
    memset(RV.complete_ticks,0,sizeof RV.complete_ticks);RV.complete_latest=0;
}
int gl_renderer_rewind_state_select(uint64_t key,uint32_t oldest,uint64_t *shown,uint64_t *tick) {
    GL_RT_SYNC("rewind_state_select");
    if(!key || key>RV.complete_latest)key=RV.complete_latest;
    for(unsigned count=0;key && key>=oldest && count<2048;key--,count++) {
        if(RV.complete_ticks[key%2048]==key){*shown=*tick=key;return 1;}
    }
    return 0;
}
int gl_renderer_rewind_state_presented(uint32_t tick) {
    GL_RT_SYNC("rewind_state_presented");
    if(RV.presented_tick==tick)return 1;
    unsigned record=tick%2048;
    if(RV.complete_ticks[record]!=tick)return 0;
    /* Restore changes host presentation deadlines. Select the exact producer
     * cycle and phase recorded at this guest boundary, not a new wall phase. */
    for(int gi=0;gi<PASS_GENERATIONS;gi++) {
        PassGen *g=&s_pgen[gi];
        if(!g->valid || g->created_cycle!=RV.complete_cycles[record] ||
           g->tex_w!=s_interp_w || g->tex_h!=s_interp_h || g->source_path!=s_interp_source_path)continue;
        for(uint32_t slot=1;slot<g->n;slot++) {
            if(!g->complete[slot] || g->phase[slot]!=RV.complete_phases[record])continue;
            s_rewind_complete_source=1;s_rewind_complete_cycle=g->created_cycle;
            s_rewind_complete_phase=g->phase[slot];RV.resume_tick=tick;
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,0);
            interp_present_pair(s_pgen_tex[gi][slot],s_pgen_tex[gi][slot],0,s_interp_blend_mode);
            s_rewind_complete_source=0;
            if(RV.presented_tick==tick){++RV.forced_complete;return 1;}
        }
    }
    return 0;
}

void gl_renderer_rewind_state_branch(uint32_t tick) {
    GL_RT_SYNC("rewind_state_branch");RV.complete_latest=0;
    for(unsigned i=0;i<2048;i++) {
        if(RV.complete_ticks[i]>tick)RV.complete_ticks[i]=0;
        if(RV.complete_ticks[i]>RV.complete_latest)RV.complete_latest=RV.complete_ticks[i];
    }
}
