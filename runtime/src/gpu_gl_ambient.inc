/* Scene-light sampling without a framebuffer readback. Transform feedback
 * writes the same three scanlines' weighted samples into a small GPU buffer.
 * The CPU maps only a buffer whose fence has already signaled; busy slots are
 * skipped. This avoids Apple's synchronous default-framebuffer ReadPixels. */
typedef struct AmbientSlot {
    GLuint texture, buffer, feedback;
    void *fence;
    int width, height, count;
    uint64_t order;
} AmbientSlot;
static AmbientSlot s_ambient_slots[3];
static GLuint s_ambient_program, s_ambient_vao;
static uint64_t s_ambient_order, s_ambient_consumed;
static void (APIENTRY *ambient_varyings)(GLuint,GLsizei,const char *const *,GLenum);
static void (APIENTRY *ambient_gen_tf)(GLsizei,GLuint *);
static void (APIENTRY *ambient_bind_tf)(GLenum,GLuint);
static void (APIENTRY *ambient_delete_tf)(GLsizei,const GLuint *);
static void (APIENTRY *ambient_bind_base)(GLenum,GLuint,GLuint);
static void (APIENTRY *ambient_begin)(GLenum);
static void (APIENTRY *ambient_end)(void);
static PgpuClientWait ambient_wait;

static void ambient_shutdown(void) {
    for(unsigned i=0;i<3;++i) {
        AmbientSlot *s=&s_ambient_slots[i];
        if(s->fence && p_glDeleteSync)p_glDeleteSync(s->fence);
        if(s->texture)glDeleteTextures(1,&s->texture);
        if(s->buffer)p_glDeleteBuffers(1,&s->buffer);
        if(s->feedback && ambient_delete_tf)ambient_delete_tf(1,&s->feedback);
    }
    memset(s_ambient_slots,0,sizeof s_ambient_slots);
    if(s_ambient_program)p_glDeleteProgram(s_ambient_program);
    if(s_ambient_vao)p_glDeleteVertexArrays(1,&s_ambient_vao);
    s_ambient_program=s_ambient_vao=0;s_ambient_order=s_ambient_consumed=0;
}
static int ambient_init(void) {
    if(s_ambient_program)return 1;
    GLint major=0;glGetIntegerv(0x821B /* MAJOR_VERSION */,&major);
    if(major<4 && !SDL_GL_ExtensionSupported("GL_ARB_transform_feedback2"))return 0;
#define AMBIENT_LOAD(member,name) member=(void *)SDL_GL_GetProcAddress(name);if(!member)return 0
    AMBIENT_LOAD(ambient_varyings,"glTransformFeedbackVaryings");
    AMBIENT_LOAD(ambient_gen_tf,"glGenTransformFeedbacks");
    AMBIENT_LOAD(ambient_bind_tf,"glBindTransformFeedback");
    AMBIENT_LOAD(ambient_delete_tf,"glDeleteTransformFeedbacks");
    AMBIENT_LOAD(ambient_bind_base,"glBindBufferBase");
    AMBIENT_LOAD(ambient_begin,"glBeginTransformFeedback");
    AMBIENT_LOAD(ambient_end,"glEndTransformFeedback");
    AMBIENT_LOAD(ambient_wait,"glClientWaitSync");
#undef AMBIENT_LOAD
    if(!p_glFenceSync || !p_glDeleteSync || !p_glMapBuffer || !p_glUnmapBuffer)return 0;
    const char *source="#version 330\n"
        "uniform sampler2D source; uniform int first_x; uniform int columns;\n"
        "out float brightness;\n"
        "void main(){ int x=first_x+8*(gl_VertexID%columns);\n"
        "vec3 c=texelFetch(source,ivec2(x,gl_VertexID/columns),0).rgb;\n"
        "brightness=dot(c,vec3(0.3,0.55,0.15));gl_Position=vec4(0,0,0,1); }\n";
    GLuint shader=compile_shader(PSXGL_VERTEX_SHADER,source);
    if(!shader)return 0;
    GLuint program=p_glCreateProgram();p_glAttachShader(program,shader);
    const char *varying="brightness";ambient_varyings(program,1,&varying,0x8C8C /* INTERLEAVED_ATTRIBS */);
    p_glLinkProgram(program);p_glDeleteShader(shader);
    GLint linked=0;p_glGetProgramiv(program,PSXGL_LINK_STATUS,&linked);
    if(!linked) {p_glDeleteProgram(program);return 0;}
    s_ambient_program=program;p_glGenVertexArrays(1,&s_ambient_vao);return 1;
}
int gl_renderer_sample_ambient(int width,int height,float *brightness) {
    /* Called by the host overlay on the existing GL owner. Never acquires a
     * context or waits for the emulation/render thread from an overlay. */
    if(!brightness || width<8 || height<4 || width>s_gl_max_dim || !pgpu_available())return 0;
    /* Transform-feedback objects and their state queries are optional on
     * the renderer's GL 3.3 baseline. Do not issue unsupported enums. */
    if(!ambient_init())return 0;
    GLint active=0;glGetIntegerv(0x8E24 /* TRANSFORM_FEEDBACK_ACTIVE */,&active);
    if(active)return 0;
    PgpuState saved;pgpu_state_save(&saved);
    GLint feedback=0,buffer=0;glGetIntegerv(0x8E25,&feedback);glGetIntegerv(0x8C8F,&buffer);
    int result=0,free_slot=-1;
    for(unsigned i=0;i<3;++i) {
        AmbientSlot *s=&s_ambient_slots[i];
        if(s->fence) {
            GLenum status=ambient_wait(s->fence,0,0);
            if(status!=0x911A && status!=0x911C)continue;
            if(s->order>s_ambient_consumed && s->width==width && s->height==height) {
                p_glBindBuffer(PSXGL_ARRAY_BUFFER,s->buffer);
                const float *samples=(const float *)p_glMapBuffer(PSXGL_ARRAY_BUFFER,0x88B8 /* READ_ONLY */);
                if(samples) {
                    double sum=0;for(int n=0;n<s->count;++n)sum+=samples[n];
                    if(p_glUnmapBuffer(PSXGL_ARRAY_BUFFER)) {
                        *brightness=(float)(sum/s->count);s_ambient_consumed=s->order;result=1;
                    }
                }
            }
            p_glDeleteSync(s->fence);s->fence=NULL;
        }
        if(free_slot<0)free_slot=(int)i;
    }
    if(free_slot>=0) {
        AmbientSlot *s=&s_ambient_slots[free_slot];
        p_glActiveTexture(PSXGL_TEXTURE0);p_glBindBuffer(0x88EC,0);
        if(s->width!=width || !s->texture) {
            if(s->texture)glDeleteTextures(1,&s->texture);
            s->texture=make_tex(GL_RGBA8,width,3,GL_RGBA,GL_UNSIGNED_BYTE);s->width=width;
        }
        glBindTexture(GL_TEXTURE_2D,s->texture);
        const int rows[3]={height*35/100,height*55/100,height*75/100};
        for(int k=0;k<3;++k)glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,k,0,rows[k],width,1);
        const int columns=(width*7/8-width/8+7)/8;
        s->height=height;s->count=columns*3;s->order=++s_ambient_order;
        if(!s->buffer)p_glGenBuffers(1,&s->buffer);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER,s->buffer);
        p_glBufferData(PSXGL_ARRAY_BUFFER,s->count*sizeof(float),NULL,0x88E1 /* STREAM_READ */);
        if(!s->feedback)ambient_gen_tf(1,&s->feedback);
        ambient_bind_tf(0x8E22,s->feedback);ambient_bind_base(0x8C8E,0,s->buffer);
        p_glUseProgram(s_ambient_program);p_glBindVertexArray(s_ambient_vao);
        pgpu_uniform_i(s_ambient_program,"source",0);pgpu_uniform_i(s_ambient_program,"first_x",width/8);
        pgpu_uniform_i(s_ambient_program,"columns",columns);
        glEnable(0x8C89 /* RASTERIZER_DISCARD */);ambient_begin(GL_POINTS);
        glDrawArrays(GL_POINTS,0,s->count);ambient_end();
        s->fence=p_glFenceSync(0x9117,0);
    }
    if(ambient_bind_tf)ambient_bind_tf(0x8E22,(GLuint)feedback);
    p_glBindBuffer(0x8C8E,(GLuint)buffer);pgpu_state_restore(&saved);return result;
}
