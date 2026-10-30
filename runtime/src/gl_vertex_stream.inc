/* Render-owner vertex streams. Each segment is immutable until its fence
 * retires. GPU stalls can park this disposable consumer, never the guest.
 * GL 4.4/ARB_buffer_storage uses coherent persistent mapping. The GL 3.3
 * platform contract (including macOS) uses fixed storage + subdata, with
 * the same explicit segment lifetime; neither path orphans per batch. */
#define VERTEX_STREAM_SEGMENTS 8
#ifndef PSXGL_PIXEL_UNPACK_BUFFER
#define PSXGL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
typedef void (APIENTRY *StreamStorage)(GLenum,ptrdiff_t,const void *,GLbitfield);
typedef void *(APIENTRY *StreamMap)(GLenum,ptrdiff_t,ptrdiff_t,GLbitfield);
typedef void (APIENTRY *StreamSubData)(GLenum,ptrdiff_t,ptrdiff_t,const void *);
typedef GLenum (APIENTRY *StreamWait)(void *,GLbitfield,uint64_t);
typedef void (APIENTRY *StreamBinding)(GLuint,GLuint,ptrdiff_t,GLsizei);
typedef void (APIENTRY *StreamFormat)(GLuint,GLint,GLenum,GLboolean,GLuint);
typedef void (APIENTRY *StreamAttribBinding)(GLuint,GLuint);
typedef struct VertexStream {
    GLuint buffer;
    unsigned segment;
    size_t segment_bytes,used;
    uint8_t *mapped;
    void *fence[VERTEX_STREAM_SEGMENTS];
    int initialized,binding;
    uint64_t uploads,bytes,rotations,waits;
} VertexStream;
static VertexStream s_vertex_stream[3]; /* flat, textured, upload/copy quad */
static VertexStream s_unpack_stream; /* immutable native texture upload bytes */
static StreamStorage s_stream_storage;
static StreamMap s_stream_map;
static StreamSubData s_stream_subdata;
static StreamWait s_stream_wait;
static StreamBinding s_stream_binding;
static StreamFormat s_stream_format;
static StreamAttribBinding s_stream_attrib_binding;
static int s_stream_loaded,s_stream_persistent,s_stream_bindings;
/* Tests shrink segments to exercise reuse without changing raster content. */
static size_t s_stream_test_segment_bytes;
static void vertex_stream_load(void) {
    if(s_stream_loaded)return;
    s_stream_loaded=1;
    s_stream_storage=(StreamStorage)SDL_GL_GetProcAddress("glBufferStorage");
    s_stream_map=(StreamMap)SDL_GL_GetProcAddress("glMapBufferRange");
    s_stream_subdata=(StreamSubData)SDL_GL_GetProcAddress("glBufferSubData");
    s_stream_wait=(StreamWait)SDL_GL_GetProcAddress("glClientWaitSync");
    s_stream_binding=(StreamBinding)SDL_GL_GetProcAddress("glBindVertexBuffer");
    s_stream_format=(StreamFormat)SDL_GL_GetProcAddress("glVertexAttribFormat");
    s_stream_attrib_binding=(StreamAttribBinding)SDL_GL_GetProcAddress("glVertexAttribBinding");
    GLint major=0,minor=0;
    glGetIntegerv(0x821B /* GL_MAJOR_VERSION */,&major);
    glGetIntegerv(0x821C /* GL_MINOR_VERSION */,&minor);
    s_stream_persistent=s_stream_storage && s_stream_map &&
        ((major>4 || (major==4 && minor>=4)) || SDL_GL_ExtensionSupported("GL_ARB_buffer_storage"));
    s_stream_bindings=s_stream_binding && s_stream_format && s_stream_attrib_binding &&
        ((major>4 || (major==4 && minor>=3)) || SDL_GL_ExtensionSupported("GL_ARB_vertex_attrib_binding"));
    if(!s_stream_subdata || !s_stream_wait || !p_glFenceSync || !p_glDeleteSync) {
        fprintf(stderr,"psxrecomp: required GL 3.3 vertex stream API unavailable\n");abort();
    }
}
static void vertex_stream_layout(int kind,size_t offset,int initialize) {
    static const unsigned counts[3]={2,14,1};
    static const GLint sizes[3][14]={{2,4},{2,2,4,2,2,1,1,4,1,1,1,4,1,1},{2}};
    static const unsigned positions[3][14]={{0,2},{0,2,4,8,10,12,13,14,18,19,20,21,25,26},{0}};
    const GLsizei stride=(kind==0?6:kind==1?TEXV:2)*sizeof(float);
    VertexStream *stream=&s_vertex_stream[kind];
    if(stream->binding) {
        if(initialize)for(unsigned i=0;i<counts[kind];++i) {
            s_stream_format(i,sizes[kind][i],GL_FLOAT,GL_FALSE,positions[kind][i]*sizeof(float));
            s_stream_attrib_binding(i,0);
        }
        s_stream_binding(0,stream->buffer,(ptrdiff_t)offset,stride);
    } else for(unsigned i=0;i<counts[kind];++i)
        p_glVertexAttribPointer(i,sizes[kind][i],GL_FLOAT,GL_FALSE,stride,
            (const void *)(uintptr_t)(offset+positions[kind][i]*sizeof(float)));
}
static size_t buffer_stream_upload(VertexStream *stream,GLenum target,GLuint buffer,
                                  size_t segment_bytes,const void *data,size_t bytes) {
    vertex_stream_load();
    if(!stream->initialized) {
        stream->buffer=buffer;
        stream->segment_bytes=segment_bytes;
        p_glBindBuffer(target,stream->buffer);
        size_t capacity=stream->segment_bytes*VERTEX_STREAM_SEGMENTS;
        if(s_stream_persistent) {
            const GLbitfield access=0x0002|0x0040|0x0080; /* WRITE,PERSISTENT,COHERENT */
            s_stream_storage(target,(ptrdiff_t)capacity,NULL,access);
            stream->mapped=s_stream_map(target,0,(ptrdiff_t)capacity,access);
            if(!stream->mapped) { fprintf(stderr,"psxrecomp: persistent vertex mapping failed\n");abort(); }
        } else p_glBufferData(target,(ptrdiff_t)capacity,NULL,PSXGL_STREAM_DRAW);
        stream->initialized=1;
    }
    if(bytes>stream->segment_bytes) { fprintf(stderr,"psxrecomp: vertex batch exceeds stream segment\n");abort(); }
    p_glBindBuffer(target,stream->buffer);
    if(bytes>stream->segment_bytes-stream->used) {
        stream->fence[stream->segment]=p_glFenceSync(0x9117 /* GPU_COMMANDS_COMPLETE */,0);
        if(!stream->fence[stream->segment])abort();
        stream->segment=(stream->segment+1)%VERTEX_STREAM_SEGMENTS;
        stream->used=0;++stream->rotations;
        void *fence=stream->fence[stream->segment];
        if(fence) {
            GLenum result=s_stream_wait(fence,0,0);
            while(result==0x911B /* TIMEOUT_EXPIRED */) {
                ++stream->waits;result=s_stream_wait(fence,1 /* FLUSH_COMMANDS */,1000000);
            }
            if(result!=0x911A /* ALREADY_SIGNALED */ && result!=0x911C /* CONDITION_SATISFIED */)abort();
            p_glDeleteSync(fence);stream->fence[stream->segment]=NULL;
        }
    }
    const size_t offset=stream->segment*stream->segment_bytes+stream->used;
    if(stream->mapped)memcpy(stream->mapped+offset,data,bytes);
    else s_stream_subdata(target,(ptrdiff_t)offset,(ptrdiff_t)bytes,data);
    stream->used+=(bytes+63u)&~(size_t)63u;
    ++stream->uploads;stream->bytes+=bytes;
    return offset;
}
static void vertex_stream_upload(int kind,const void *data,size_t bytes) {
    VertexStream *stream=&s_vertex_stream[kind];
    const int initialize=!stream->initialized;
    const GLuint buffer=kind==0?s_geo_vbo:kind==1?s_tex_vbo:s_blit_vbo;
    const size_t segment_bytes=s_stream_test_segment_bytes?s_stream_test_segment_bytes:
        (kind==1?4u*1024u*1024u:1024u*1024u);
    const size_t offset=buffer_stream_upload(stream,PSXGL_ARRAY_BUFFER,buffer,segment_bytes,data,bytes);
    stream->binding=s_stream_bindings;
    vertex_stream_layout(kind,offset,initialize);
}
static void vertex_stream_reset(void) {
    for(unsigned k=0;k<4;++k) {
        VertexStream *stream=k<3?&s_vertex_stream[k]:&s_unpack_stream;
        const GLenum target=k<3?PSXGL_ARRAY_BUFFER:PSXGL_PIXEL_UNPACK_BUFFER;
        for(unsigned i=0;i<VERTEX_STREAM_SEGMENTS;++i)
            if(stream->fence[i])p_glDeleteSync(stream->fence[i]);
        if(stream->mapped) {
            p_glBindBuffer(target,stream->buffer);
            p_glUnmapBuffer(target);
        }
    }
    memset(s_vertex_stream,0,sizeof s_vertex_stream);
    memset(&s_unpack_stream,0,sizeof s_unpack_stream);s_stream_loaded=0;
}
