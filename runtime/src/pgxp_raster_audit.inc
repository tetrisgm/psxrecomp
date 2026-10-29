/* Renderer-owned PGXP audit state. Included after renderer state declarations. */
/* Bounded, opt-in CPU-side raster stream comparison at draw-area boundaries. */
#define PGXP_RASTER_FLOATS (1024 * 1024)
static float *s_ra_cur, *s_ra_prev;
static size_t s_ra_n, s_ra_prev_n;
static uint64_t s_ra_seq, s_ra_diff;
static float s_ra_first_a, s_ra_first_b;
static size_t s_ra_first;
static int s_ra_overflow;
static int s_ra_depth_probe;
static GLuint s_ra_queries[2];
static uint64_t s_ra_near, s_ra_failed, s_ra_invalid_w, s_ra_invalid_z;
static uint64_t s_ra_last_invalid_w, s_ra_last_invalid_z, s_ra_prev_invalid_w, s_ra_prev_invalid_z;
static uint64_t s_ra_frame, s_ra_last_near, s_ra_last_failed, s_ra_last_resets[5], s_ra_prev_resets[5];
static uint64_t s_ra_seam_changed, s_ra_seam_matched, s_ra_last_seam_changed, s_ra_last_seam_matched;
#define PGXP_AUDIT_SEAMS 4096
static struct PgxpAuditSeam {uint64_t key; float e[3]; PgxpSnapStrip st[3];}
    s_ra_seams[2][PGXP_AUDIT_SEAMS];
static unsigned s_ra_seam_n, s_ra_seam_prev_n, s_ra_seam_bank;
static void pgxp_raster_seam(const int *xs,const int *ys) {
    if(!s_ra_cur) return;
    if(s_ra_seam_n>=PGXP_AUDIT_SEAMS) {s_ra_overflow=1;return;}
    uint64_t key=1469598103934665603ull;
    for(int i=0;i<3;i++) {
        int32_t words[5]={xs[i]-s_area_x1,ys[i]-s_area_y1,
          (int32_t)llrint((s_pc_x[i]-s_area_x1)*65536.0),
          (int32_t)llrint((s_pc_y[i]-s_area_y1)*65536.0),0};
        memcpy(words+4,s_pz+i,4);
        for(int j=0;j<5;j++){key^=(uint32_t)words[j];key*=1099511628211ull;}
    }
    struct PgxpAuditSeam *a=&s_ra_seams[s_ra_seam_bank][s_ra_seam_n];
    struct PgxpAuditSeam *b=&s_ra_seams[s_ra_seam_bank^1][s_ra_seam_n];
    a->key=key; memcpy(a->e,s_snap_e,sizeof a->e);memcpy(a->st,s_snap_st,sizeof a->st);
    if(s_ra_seam_n<s_ra_seam_prev_n && key==b->key) {
        s_ra_seam_matched++;
        if(memcmp(a->e,b->e,sizeof a->e)||memcmp(a->st,b->st,sizeof a->st))s_ra_seam_changed++;
    }
    s_ra_seam_n++;
}
static void pgxp_raster_frame_end(void) {
    s_ra_last_invalid_w=s_ra_invalid_w-s_ra_prev_invalid_w;s_ra_prev_invalid_w=s_ra_invalid_w;
    s_ra_last_invalid_z=s_ra_invalid_z-s_ra_prev_invalid_z;s_ra_prev_invalid_z=s_ra_invalid_z;
    s_ra_frame++;s_ra_last_near=s_ra_near;s_ra_last_failed=s_ra_failed;s_ra_near=s_ra_failed=0;
    s_ra_last_seam_changed=s_ra_seam_changed;s_ra_last_seam_matched=s_ra_seam_matched;
    s_ra_seam_changed=s_ra_seam_matched=0;
    for(int i=0;i<5;i++){s_ra_last_resets[i]=s_depth_clear_reason[i]-s_ra_prev_resets[i];s_ra_prev_resets[i]=s_depth_clear_reason[i];}
}
static void pgxp_raster_record(const float *v, int n, int stride) {
    if (!s_ra_cur) return;
    for (int i=0;i<n;i++) if(pgxp_vertex_local(v+i*stride,stride)) for(int j=0;j<stride;j++) {
        if(s_ra_n>=PGXP_RASTER_FLOATS) {s_ra_overflow=1;return;}
        s_ra_cur[s_ra_n++]=v[i*stride+j];
    }
}
static void pgxp_raster_boundary(void) {
    if (!s_ra_cur || !s_ra_n) return;
    s_ra_seam_prev_n=s_ra_seam_n;s_ra_seam_n=0;s_ra_seam_bank^=1;
    s_ra_diff=0;s_ra_first=0;s_ra_first_a=s_ra_first_b=0;
    size_t n=s_ra_n<s_ra_prev_n?s_ra_n:s_ra_prev_n;
    for(size_t i=0;i<n;i++) if(s_ra_cur[i]!=s_ra_prev[i]) {
        if(!s_ra_diff){s_ra_first=i;s_ra_first_a=s_ra_prev[i];s_ra_first_b=s_ra_cur[i];}
        s_ra_diff++;
    }
    if(s_ra_seq) s_ra_diff += s_ra_n>s_ra_prev_n?s_ra_n-s_ra_prev_n:s_ra_prev_n-s_ra_n;
    float *p=s_ra_prev;s_ra_prev=s_ra_cur;s_ra_cur=p;s_ra_prev_n=s_ra_n;s_ra_n=0;s_ra_seq++;
}
void gl_renderer_pgxp_raster_json(char *out, size_t cap, int depth_probe) {
    if(depth_probe>=0)s_ra_depth_probe=depth_probe!=0;
    if(!s_ra_cur) {s_ra_cur=calloc(PGXP_RASTER_FLOATS,sizeof(float));s_ra_prev=calloc(PGXP_RASTER_FLOATS,sizeof(float));
        if(!s_ra_cur || !s_ra_prev) {free(s_ra_cur);free(s_ra_prev);s_ra_cur=s_ra_prev=NULL;snprintf(out,cap,"\"error\":\"audit allocation failed\"");return;}
    }
    GLint geometry_vertices=0;
    if(s_raster_ok && s_tex_prog)p_glGetProgramiv(s_tex_prog,0x8916 /* GL_GEOMETRY_VERTICES_OUT */,&geometry_vertices);
    snprintf(out,cap,"\"pipeline_ready\":%d,\"textured_geometry_vertices\":%d,\"sequence\":%llu,\"floats\":%zu,\"changed\":%llu,\"first\":%zu,\"a\":%.9g,\"b\":%.9g,\"overflow\":%d,"
      "\"render_frame\":%llu,\"depth_resets\":[%llu,%llu,%llu,%llu,%llu],\"seam_matched\":%llu,\"seam_changed\":%llu,"
      "\"depth_probe\":%d,\"epsilon\":0.00000095367431640625,\"depth_failed_samples\":%llu,\"depth_near_samples\":%llu,"
      "\"invalid_w\":%llu,\"invalid_z\":%llu,\"invalid_w_total\":%llu,\"invalid_z_total\":%llu",
      s_raster_ok,geometry_vertices,(unsigned long long)s_ra_seq,s_ra_prev_n,(unsigned long long)s_ra_diff,s_ra_first,s_ra_first_a,s_ra_first_b,s_ra_overflow,
      (unsigned long long)s_ra_frame,(unsigned long long)s_ra_last_resets[0],(unsigned long long)s_ra_last_resets[1],
      (unsigned long long)s_ra_last_resets[2],(unsigned long long)s_ra_last_resets[3],(unsigned long long)s_ra_last_resets[4],
      (unsigned long long)s_ra_last_seam_matched,(unsigned long long)s_ra_last_seam_changed,s_ra_depth_probe,
      (unsigned long long)s_ra_last_failed,(unsigned long long)s_ra_last_near,
      (unsigned long long)s_ra_last_invalid_w,(unsigned long long)s_ra_last_invalid_z,
      (unsigned long long)s_ra_invalid_w,(unsigned long long)s_ra_invalid_z);
}


static void pgxp_raster_shutdown(void) {
    free(s_ra_cur);free(s_ra_prev);s_ra_cur=s_ra_prev=NULL;
    s_ra_n=s_ra_prev_n=s_ra_first=0;s_ra_seq=s_ra_diff=0;
    s_ra_first_a=s_ra_first_b=0;s_ra_overflow=s_ra_depth_probe=0;
    s_ra_queries[0]=s_ra_queries[1]=0;
    s_ra_near=s_ra_failed=s_ra_invalid_w=s_ra_invalid_z=0;
    s_ra_last_invalid_w=s_ra_last_invalid_z=s_ra_prev_invalid_w=s_ra_prev_invalid_z=0;
    s_ra_frame=s_ra_last_near=s_ra_last_failed=0;
    memset(s_ra_last_resets,0,sizeof s_ra_last_resets);
    memcpy(s_ra_prev_resets,s_depth_clear_reason,sizeof s_ra_prev_resets);
    s_ra_seam_changed=s_ra_seam_matched=s_ra_last_seam_changed=s_ra_last_seam_matched=0;
    s_ra_seam_n=s_ra_seam_prev_n=s_ra_seam_bank=0;
}
