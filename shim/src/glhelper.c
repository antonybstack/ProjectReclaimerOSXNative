// glhelper.c -- the native GL helper process (arm64): replays the loader's renderer GL calls natively.
// shim/docs/gl-helper.md. Built by shim/build.sh as shim/build/peload-gl; started by the loader (glproxy.c):
//     peload-gl <shm name> <wake pipe read fd> <present pipe write fd> <parent pid>
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/CGLIOSurface.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "glproto.h"
#include "glhelper_fns.h"

static GlpShared* S;
static int g_wake_fd=-1, g_present_fd=-1, g_check;
static void logf_(const char*fmt,...){ char m[512]; va_list a; va_start(a,fmt); int k=vsnprintf(m,sizeof m,fmt,a); va_end(a); if(k>0){ if(k>(int)sizeof m-1) k=sizeof m-1; (void)!write(2,m,(size_t)k); } }
static double mono(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

// ---- argument helpers (glhelper_gen.h) ----
static inline float glh_f32(const uint32_t*p){ float f; memcpy(&f,p,4); return f; }
static inline double glh_f64(const uint32_t*p){ double d; memcpy(&d,p,8); return d; }
static inline uint64_t glh_u64(const uint32_t*p){ uint64_t v; memcpy(&v,p,8); return v; }

// ---- object names: loader name -> real name, per namespace ----
static GLuint* g_map[NS_COUNT]; static uint32_t g_mapcap[NS_COUNT];
static GLsync* g_syncs; static uint32_t g_synccap;
static void map_set(int ns,uint32_t c,GLuint real){ if(c>=g_mapcap[ns]){ uint32_t nc=g_mapcap[ns]?g_mapcap[ns]:1024; while(nc<=c) nc*=2;
        GLuint*m=realloc(g_map[ns],nc*sizeof *m); if(!m){ logf_("  [glhelper] out of memory (names)\n"); return; } memset(m+g_mapcap[ns],0,(nc-g_mapcap[ns])*sizeof *m); g_map[ns]=m; g_mapcap[ns]=nc; }
    g_map[ns][c]=real; }
static inline GLuint glh_name(int ns,uint32_t c){ return c && c<g_mapcap[ns] ? g_map[ns][c] : 0; }
static void sync_set(uint32_t c,GLsync s){ if(c>=g_synccap){ uint32_t nc=g_synccap?g_synccap*2:256; while(nc<=c) nc*=2; GLsync*m=realloc(g_syncs,nc*sizeof *m); if(!m) return; memset(m+g_synccap,0,(nc-g_synccap)*sizeof *m); g_syncs=m; g_synccap=nc; } g_syncs[c]=s; }
static GLsync sync_get(uint32_t c){ return c<g_synccap?g_syncs[c]:0; }

// ---- data arguments: [bytes] + words, or staged / NULL / offset ----
static uint8_t* g_stage; static uint64_t g_stage_cap, g_stage_len;
// returns the data pointer; *used = words consumed at p
static const void* glh_data(const uint32_t*p,uint32_t*used,uint64_t*bytes){
    uint32_t b=p[0];
    if(b==GLDATA_NULL){ *used=1; if(bytes)*bytes=0; return 0; }
    if(b==GLDATA_STAGED){ *used=1; if(bytes)*bytes=g_stage_len; return g_stage; }
    if(b==GLDATA_OFFSET){ *used=3; if(bytes)*bytes=0; return (const void*)(uintptr_t)glh_u64(p+1); }
    *used=1+(b+3)/4; if(bytes)*bytes=b; return p+1; }
static void hx_stage(const uint32_t*p){ uint64_t total=glh_u64(p), off=glh_u64(p+2); uint32_t n=p[4];
    if(total>g_stage_cap){ uint8_t*s=realloc(g_stage,total); if(!s){ logf_("  [glhelper] staging %llu bytes failed\n",(unsigned long long)total); return; } g_stage=s; g_stage_cap=total; }
    if(off+n<=total) memcpy(g_stage+off,p+5,n); g_stage_len=total; }

// ---- replies ----
static void reply(uint32_t seq,const void*data,uint32_t len,int32_t status){
    if(len>GLP_REPLY_BYTES) len=GLP_REPLY_BYTES; if(data&&len) memcpy(S->reply,data,len);
    S->reply_len=len; S->reply_status=status; __atomic_store_n(&S->reply_seq,(uint64_t)seq,__ATOMIC_RELEASE); }

// ---- occlusion queries: polled until available, then published to the shared table ----
typedef struct { uint32_t id, gen; GLuint q; } PendQ;
static PendQ* g_pq; static int g_npq, g_cappq; static uint32_t g_active_id[4], g_active_gen[4];
static int qslot(GLenum t){ return t==GL_SAMPLES_PASSED?0:t==GL_ANY_SAMPLES_PASSED?1:t==0x8D6A/*ANY_SAMPLES_PASSED_CONSERVATIVE*/?2:3; }
static void poll_queries(void){
    for(int i=0;i<g_npq;){ GLint av=0; glh_fn.GetQueryObjectiv(g_pq[i].q,GL_QUERY_RESULT_AVAILABLE,&av);
        if(!av){ i++; continue; }
        GLuint64 v=0; if(glh_fn.GetQueryObjectui64v) glh_fn.GetQueryObjectui64v(g_pq[i].q,GL_QUERY_RESULT,&v); else { GLint r=0; glh_fn.GetQueryObjectiv(g_pq[i].q,GL_QUERY_RESULT,&r); v=(GLuint)r; }
        if(g_pq[i].id<GLP_QUERIES){ S->qval[g_pq[i].id]=v; __atomic_store_n(&S->qdone[g_pq[i].id],g_pq[i].gen,__ATOMIC_RELEASE); }
        g_pq[i]=g_pq[--g_npq]; } }

// ---- present slots: IOSurface-backed rectangle textures the loader's window shows ----
typedef struct { IOSurfaceRef surf; GLuint tex, fbo; int w, h; GLsync fence; uint64_t frame; } Slot;
static Slot g_slot[GLP_SLOTS]; static CGLContextObj g_ctx;
static int slot_make(Slot*s,int w,int h){
    if(s->surf && s->w==w && s->h==h) return 1;
    if(s->surf){ CFRelease(s->surf); s->surf=0; }
    int bpe=4, fmt='BGRA'; CFNumberRef W=CFNumberCreate(0,kCFNumberIntType,&w),H=CFNumberCreate(0,kCFNumberIntType,&h),B=CFNumberCreate(0,kCFNumberIntType,&bpe),F=CFNumberCreate(0,kCFNumberIntType,&fmt);
    const void*k[]={kIOSurfaceWidth,kIOSurfaceHeight,kIOSurfaceBytesPerElement,kIOSurfacePixelFormat,kIOSurfaceIsGlobal}; const void*v[]={W,H,B,F,kCFBooleanTrue};
    CFDictionaryRef d=CFDictionaryCreate(0,k,v,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    s->surf=IOSurfaceCreate(d); CFRelease(d); CFRelease(W); CFRelease(H); CFRelease(B); CFRelease(F);
    if(!s->surf){ logf_("  [glhelper] IOSurfaceCreate %dx%d failed\n",w,h); return 0; }
    if(!s->tex) glGenTextures(1,&s->tex); if(!s->fbo) glGenFramebuffers(1,&s->fbo);
    GLint ot=0; glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE,&ot); glBindTexture(GL_TEXTURE_RECTANGLE,s->tex);
    CGLError e=CGLTexImageIOSurface2D(g_ctx,GL_TEXTURE_RECTANGLE,GL_RGBA8,w,h,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,s->surf,0);
    glBindTexture(GL_TEXTURE_RECTANGLE,(GLuint)ot);
    if(e){ logf_("  [glhelper] CGLTexImageIOSurface2D err %d\n",e); return 0; }
    GLint of=0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&of); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s->fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_RECTANGLE,s->tex,0); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)of);
    s->w=w; s->h=h; return 1; }
static int g_slot_pending=-1;   // filled, fence not signalled yet
static int g_prev_latest=-1;    // never refill the slot the present thread may have just picked
static void present_poll(int wait){
    if(g_slot_pending<0) return; Slot*s=&g_slot[g_slot_pending];
    GLenum r=glClientWaitSync(s->fence,wait?GL_SYNC_FLUSH_COMMANDS_BIT:0,wait?50000000ull:0);
    if(r==GL_TIMEOUT_EXPIRED) return;
    glDeleteSync(s->fence); s->fence=0; int i=g_slot_pending; g_slot_pending=-1;
    S->slot_surface[i]=IOSurfaceGetID(s->surf); S->slot_w[i]=(uint32_t)s->w; S->slot_h[i]=(uint32_t)s->h; S->slot_frame[i]=s->frame;
    g_prev_latest=__atomic_load_n(&S->pq_latest,__ATOMIC_ACQUIRE); __atomic_store_n(&S->pq_latest,i,__ATOMIC_RELEASE); __atomic_add_fetch(&S->pq_seq,1,__ATOMIC_ACQ_REL); S->published++;
    { struct timespec ct; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&ct); S->cpu_ns=(uint64_t)ct.tv_sec*1000000000ull+(uint64_t)ct.tv_nsec; }
    char b=1; (void)!write(g_present_fd,&b,1); }
static void hx_pq_publish(const uint32_t*p){
    GLuint fbo=glh_name(NS_FBO,p[0]); int vw=(int)p[1], vh=(int)p[2]; uint64_t frame=glh_u64(p+3);
    if(vw<1||vh<1) return;
    if(g_slot_pending>=0){ double t0=mono(); present_poll(1); S->fence_waits++; S->fence_wait_s+=mono()-t0; }   // one frame in flight at a time (as the loader's present queue)
    int latest=__atomic_load_n(&S->pq_latest,__ATOMIC_ACQUIRE), reading=__atomic_load_n(&S->pq_reading,__ATOMIC_ACQUIRE), i;
    for(i=0;i<GLP_SLOTS;i++) if(i!=latest && i!=reading && i!=g_prev_latest) break;
    if(i>=GLP_SLOTS) return;
    Slot*s=&g_slot[i]; if(!slot_make(s,vw,vh)) return;
    GLint rf=0, df=0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&rf); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&df);
    GLboolean sc=glIsEnabled(GL_SCISSOR_TEST), fs=glIsEnabled(GL_FRAMEBUFFER_SRGB); if(sc) glDisable(GL_SCISSOR_TEST); if(fs) glDisable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s->fbo);
    // D3D row 0 (GL row 0 of the back buffer) lands at the IOSurface's top row: the loader draws it top-down
    glBlitFramebuffer(0,0,vw,vh, 0,vh,vw,0, GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)rf); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)df);
    if(sc) glEnable(GL_SCISSOR_TEST); if(fs) glEnable(GL_FRAMEBUFFER_SRGB);
    s->fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0); s->frame=frame; glFlush(); g_slot_pending=i; }

// ---- Metal pixel-format capabilities, cached: peload-gl-pf.dylib (glhelper_pf.c), weakly linked ----
extern volatile int glhpf_on __attribute__((weak_import)); extern pthread_t glhpf_thread __attribute__((weak_import));
extern uint64_t glhpf_hits __attribute__((weak_import)), glhpf_miss __attribute__((weak_import));
static int g_pf_off;   // PELOAD_GL_HELPER_NOPFCACHE
static void pf_sync(void){ if(&glhpf_on) glhpf_on=!g_pf_off && !S->ab_off; }

// ---- the hand-written decoders (glproxy.c encodes the same layouts) ----
static void hx_BindBuffer(const uint32_t*p,uint32_t n){ (void)n; glh_fn.BindBuffer(p[0],glh_name(NS_BUF,p[1])); }
static void hx_PixelStorei(const uint32_t*p,uint32_t n){ (void)n; glh_fn.PixelStorei(p[0],(GLint)p[1]); }
static void hx_BeginQuery(const uint32_t*p,uint32_t n){ (void)n; uint32_t id=p[1]; GLuint q=glh_name(NS_QRY,id); glh_fn.BeginQuery(p[0],q); int k=qslot(p[0]); g_active_id[k]=id; g_active_gen[k]=p[2]; }
static void hx_EndQuery(const uint32_t*p,uint32_t n){ (void)n; glh_fn.EndQuery(p[0]); int k=qslot(p[0]); uint32_t id=g_active_id[k]; if(!id) return;
    if(g_npq==g_cappq){ int nc=g_cappq?g_cappq*2:256; PendQ*q=realloc(g_pq,(size_t)nc*sizeof *q); if(!q) return; g_pq=q; g_cappq=nc; }
    g_pq[g_npq++]=(PendQ){id,g_active_gen[k],glh_name(NS_QRY,id)}; g_active_id[k]=0; }
#define GEN(fn,ns) static void hx_##fn(const uint32_t*p,uint32_t n){ (void)n; int c=(int)p[0]; if(c<=0) return; GLuint tmp[256]; \
    for(int o=0;o<c;o+=256){ int k=c-o<256?c-o:256; glh_fn.fn(k,tmp); for(int i=0;i<k;i++) map_set(ns,p[1+o+i],tmp[i]); } }
GEN(GenBuffers,NS_BUF) GEN(GenFramebuffers,NS_FBO) GEN(GenProgramPipelines,NS_PIPE) GEN(GenQueries,NS_QRY)
GEN(GenRenderbuffers,NS_RBO) GEN(GenSamplers,NS_SMP) GEN(GenTextures,NS_TEX) GEN(GenVertexArrays,NS_VAO)
static void hx_CreateShader(const uint32_t*p,uint32_t n){ (void)n; map_set(NS_OBJ,p[1],glh_fn.CreateShader(p[0])); }
static void hx_CreateProgram(const uint32_t*p,uint32_t n){ (void)n; map_set(NS_OBJ,p[0],glh_fn.CreateProgram()); }
// strings: [count, len_0..len_{count-1}, bytes...]
static int strings_of(const uint32_t*p,const GLchar**s,GLint*l,int max){ int c=(int)p[0]; if(c>max) c=max; const char*b=(const char*)(p+1+p[0]);
    for(int i=0;i<c;i++){ s[i]=b; l[i]=(GLint)p[1+i]; b+=p[1+i]; } return c; }
static void hx_ShaderSource(const uint32_t*p,uint32_t n){ (void)n; const GLchar*s[64]; GLint l[64]; int c=strings_of(p+1,s,l,64); glh_fn.ShaderSource(glh_name(NS_OBJ,p[0]),c,s,l); }
static void hx_CreateShaderProgramv(const uint32_t*p,uint32_t n){ (void)n; const GLchar*s[64]; GLint l[64]; int c=strings_of(p+2,s,l,64);
    // glCreateShaderProgramv takes NUL-terminated strings: copy each with a terminator
    char* z[64]; for(int i=0;i<c;i++){ z[i]=malloc((size_t)l[i]+1); memcpy(z[i],s[i],(size_t)l[i]); z[i][l[i]]=0; }
    map_set(NS_OBJ,p[1],glh_fn.CreateShaderProgramv(p[0],c,(const GLchar*const*)z)); for(int i=0;i<c;i++) free(z[i]); }
static void hx_DrawBuffers(const uint32_t*p,uint32_t n){ (void)n; glh_fn.DrawBuffers((GLsizei)p[0],p+1); }
static void hx_ProgramUniform4fv(const uint32_t*p,uint32_t n){ (void)n; glh_fn.ProgramUniform4fv(glh_name(NS_OBJ,p[0]),(GLint)p[1],(GLsizei)p[2],(const GLfloat*)(p+3)); }
static void hx_SamplerParameterfv(const uint32_t*p,uint32_t n){ (void)n; glh_fn.SamplerParameterfv(glh_name(NS_SMP,p[0]),p[1],(const GLfloat*)(p+2)); }
static void hx_BufferData(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; const void*d=glh_data(p+4,&u,0); glh_fn.BufferData(p[0],(GLsizeiptr)glh_u64(p+2),d,p[1]); }
static void hx_BufferSubData(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; const void*d=glh_data(p+5,&u,0); glh_fn.BufferSubData(p[0],(GLintptr)glh_u64(p+1),(GLsizeiptr)glh_u64(p+3),d); }
static void hx_TexImage2D(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; const void*d=glh_data(p+8,&u,0);
    glh_fn.TexImage2D(p[0],(GLint)p[1],(GLint)p[2],(GLsizei)p[3],(GLsizei)p[4],(GLint)p[5],p[6],p[7],d); }
static void hx_TexImage3D(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; const void*d=glh_data(p+9,&u,0);
    glh_fn.TexImage3D(p[0],(GLint)p[1],(GLint)p[2],(GLsizei)p[3],(GLsizei)p[4],(GLsizei)p[5],(GLint)p[6],p[7],p[8],d); }
static void hx_TexSubImage2D(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; const void*d=glh_data(p+8,&u,0);
    glh_fn.TexSubImage2D(p[0],(GLint)p[1],(GLint)p[2],(GLint)p[3],(GLsizei)p[4],(GLsizei)p[5],p[6],p[7],d); }
static void hx_TextureSubImage2D(const uint32_t*p,uint32_t n){ (void)p;(void)n; }   // GL 4.5: not on Apple's GL; never sent
static void hx_CompressedTexImage2D(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; uint64_t b=0; const void*d=glh_data(p+6,&u,&b);
    glh_fn.CompressedTexImage2D(p[0],(GLint)p[1],p[2],(GLsizei)p[3],(GLsizei)p[4],(GLint)p[5],(GLsizei)b,d); }
// Unmap of a loader-side mapping: [target, off_lo, off_hi, len_lo, len_hi, access, data]
static void hx_UnmapBuffer(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; uint64_t b=0; const void*d=glh_data(p+6,&u,&b);
    GLintptr off=(GLintptr)glh_u64(p+1); GLsizeiptr len=(GLsizeiptr)glh_u64(p+3); GLbitfield acc=(p[5]&~(GLbitfield)GL_MAP_READ_BIT)|GL_MAP_WRITE_BIT;
    if(!d||!len) return; void*m=glh_fn.MapBufferRange(p[0],off,len,acc);
    if(m){ memcpy(m,d,(size_t)(b<(uint64_t)len?b:(uint64_t)len)); glh_fn.UnmapBuffer(p[0]); } else glh_fn.BufferSubData(p[0],off,len,d); }
// round trips: p[0] = seq
static void hx_MapBufferRange(const uint32_t*p,uint32_t n){ (void)n; GLsizeiptr len=(GLsizeiptr)glh_u64(p+4); if(len>(GLsizeiptr)GLP_REPLY_BYTES) len=GLP_REPLY_BYTES;
    void*m=glh_fn.MapBufferRange(p[1],(GLintptr)glh_u64(p+2),len,GL_MAP_READ_BIT); if(m){ reply(p[0],m,(uint32_t)len,1); glh_fn.UnmapBuffer(p[1]); } else reply(p[0],0,0,0); }
static void hx_GetBufferSubData(const uint32_t*p,uint32_t n){ (void)n; GLsizeiptr len=(GLsizeiptr)glh_u64(p+4); if(len>(GLsizeiptr)GLP_REPLY_BYTES) len=GLP_REPLY_BYTES;
    glh_fn.GetBufferSubData(p[1],(GLintptr)glh_u64(p+2),len,S->reply); reply(p[0],0,(uint32_t)len,1); }
static void hx_ReadPixels(const uint32_t*p,uint32_t n){ (void)n;
    if(p[0]==0){ glh_fn.ReadPixels((GLint)p[1],(GLint)p[2],(GLsizei)p[3],(GLsizei)p[4],p[5],p[6],(void*)(uintptr_t)glh_u64(p+7)); return; }   // into the bound pack buffer
    uint32_t bytes=p[7]; if(bytes>GLP_REPLY_BYTES){ reply(p[0],0,0,0); return; }
    glh_fn.ReadPixels((GLint)p[1],(GLint)p[2],(GLsizei)p[3],(GLsizei)p[4],p[5],p[6],S->reply); reply(p[0],0,bytes,1); }
static void hx_GetTexImage(const uint32_t*p,uint32_t n){ (void)n; uint32_t bytes=p[5]; if(bytes>GLP_REPLY_BYTES){ reply(p[0],0,0,0); return; }
    glh_fn.GetTexImage(p[1],(GLint)p[2],p[3],p[4],S->reply); reply(p[0],0,bytes,1); }
static void hx_GetTexLevelParameteriv(const uint32_t*p,uint32_t n){ (void)n; GLint v=0; glh_fn.GetTexLevelParameteriv(p[1],(GLint)p[2],p[3],&v); reply(p[0],&v,4,1); }
static void hx_GetIntegerv(const uint32_t*p,uint32_t n){ (void)n; GLint v[16]={0}; glh_fn.GetIntegerv(p[1],v); reply(p[0],v,sizeof v,1); }
static void hx_GetFloatv(const uint32_t*p,uint32_t n){ (void)n; GLfloat v[16]={0}; glh_fn.GetFloatv(p[1],v); reply(p[0],v,sizeof v,1); }
static void hx_GetBooleanv(const uint32_t*p,uint32_t n){ (void)n; GLboolean v[16]={0}; glh_fn.GetBooleanv(p[1],v); reply(p[0],v,sizeof v,1); }
static void hx_IsEnabled(const uint32_t*p,uint32_t n){ (void)n; int32_t v=glh_fn.IsEnabled(p[1]); reply(p[0],&v,4,1); }
static void hx_CheckFramebufferStatus(const uint32_t*p,uint32_t n){ (void)n; uint32_t v=glh_fn.CheckFramebufferStatus(p[1]); reply(p[0],&v,4,1); }
static void hx_GetString(const uint32_t*p,uint32_t n){ (void)n; const GLubyte*s=glh_fn.GetString(p[1]); reply(p[0],s,s?(uint32_t)strlen((const char*)s)+1:0,s!=0); }
static void hx_GetProgramiv(const uint32_t*p,uint32_t n){ (void)n; GLint v=0; glh_fn.GetProgramiv(glh_name(NS_OBJ,p[1]),p[2],&v); reply(p[0],&v,4,1); }
static void hx_GetShaderiv(const uint32_t*p,uint32_t n){ (void)n; GLint v=0; glh_fn.GetShaderiv(glh_name(NS_OBJ,p[1]),p[2],&v); reply(p[0],&v,4,1); }
#define LOG(fn) static void hx_##fn(const uint32_t*p,uint32_t n){ (void)n; GLsizei cap=(GLsizei)p[2]; if(cap>(GLsizei)GLP_REPLY_BYTES) cap=GLP_REPLY_BYTES; GLsizei len=0; \
    if(cap>0) glh_fn.fn(glh_name(NS_OBJ,p[1]),cap,&len,(GLchar*)S->reply); reply(p[0],0,(uint32_t)len,1); }
LOG(GetProgramInfoLog) LOG(GetShaderInfoLog) LOG(GetShaderSource)
static void hx_GetUniformLocation(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; const char*s=glh_data(p+2,&u,0); int32_t v=s?glh_fn.GetUniformLocation(glh_name(NS_OBJ,p[1]),s):-1; reply(p[0],&v,4,1); }
static void hx_GetUniformBlockIndex(const uint32_t*p,uint32_t n){ (void)n; uint32_t u; const char*s=glh_data(p+2,&u,0); uint32_t v=s?glh_fn.GetUniformBlockIndex(glh_name(NS_OBJ,p[1]),s):GL_INVALID_INDEX; reply(p[0],&v,4,1); }
static void hx_GetUniformfv(const uint32_t*p,uint32_t n){ (void)n; GLfloat v[16]={0}; glh_fn.GetUniformfv(glh_name(NS_OBJ,p[1]),(GLint)p[2],v); reply(p[0],v,sizeof v,1); }
static void hx_GetError(const uint32_t*p,uint32_t n){ (void)n; uint32_t v=glh_fn.GetError(); reply(p[0],&v,4,1); }
static void hx_Finish(const uint32_t*p,uint32_t n){ (void)n; glh_fn.Finish(); reply(p[0],0,0,1); }
// handled on the loader side (never sent)
static void hx_GetQueryObjectiv(const uint32_t*p,uint32_t n){ (void)p;(void)n; }
static void hx_GetQueryObjectui64v(const uint32_t*p,uint32_t n){ (void)p;(void)n; }
// fences: [handle, condition, flags] / [handle] / [handle, flags, timeout_lo, timeout_hi]
static void hx_FenceSync(const uint32_t*p,uint32_t n){ (void)n; sync_set(p[0],glh_fn.FenceSync(p[1],p[2])); }
static void hx_DeleteSync(const uint32_t*p,uint32_t n){ (void)n; GLsync s=sync_get(p[0]); if(s){ glh_fn.DeleteSync(s); sync_set(p[0],0); } }
static void hx_WaitSync(const uint32_t*p,uint32_t n){ (void)n; GLsync s=sync_get(p[0]); if(s) glh_fn.WaitSync(s,p[1],glh_u64(p+2)); }

// ---- the decode loop ----
static void decode(uint32_t op,const uint32_t*p,uint32_t n){
    switch(op){
#include "glhelper_gen.h"
    case GLOP_STAGE: hx_stage(p); break;
    case GLOP_SYNC: reply(p[0],0,0,1); break;
    case GLOP_PQ_PUBLISH: hx_pq_publish(p); pf_sync(); break;
    case GLOP_HELLO: if(p[0]!=GLPROTO_VERSION) logf_("  [glhelper] protocol %u, expected %u\n",p[0],GLPROTO_VERSION); break;
    case GLOP_QUIT: exit(0);
    default: logf_("  [glhelper] unknown opcode %u\n",op); break; }
    if(g_check && op>=GLOP_FIRST_GL){ GLenum e=glGetError(); if(e){ S->gl_errors++; static int ne; if(ne++<50) logf_("  [glhelper] GL error 0x%x after %s\n",e,glop_names[op-GLOP_FIRST_GL]); } } }

// PELOAD_GL_HELPER_OPS=1: every 5 s the opcodes that took the most time (count per frame, us each, share of the time
// spent decoding). Draws carry the driver's deferred state validation, so the state calls that cause it look cheap.
static uint64_t g_op_n[256], g_op_t[256]; static double g_op_tb;
static void ops_report(void){ double now=mono(); if(!g_op_tb){ g_op_tb=now; return; } if(now-g_op_tb<5.0) return;
    static uint64_t pub0; uint64_t fr=S->published-pub0; pub0=S->published; if(!fr) fr=1;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); uint64_t tot=0; for(int i=0;i<256;i++) tot+=g_op_t[i];
    char m[2048]; int k=snprintf(m,sizeof m,"  [glhelper ops] %.0f s, %llu frames, decode %.1f ms/frame:",now-g_op_tb,(unsigned long long)fr,tot*1e-6*tb.numer/tb.denom/(double)fr);
    for(int r=0;r<20;r++){ int bi=-1; uint64_t bt=0; for(int i=0;i<256;i++) if(g_op_t[i]>bt){ bt=g_op_t[i]; bi=i; } if(bi<0) break;
        const char*nm=bi>=GLOP_FIRST_GL&&bi-GLOP_FIRST_GL<(int)(sizeof glop_names/sizeof glop_names[0])?glop_names[bi-GLOP_FIRST_GL]:bi==GLOP_PQ_PUBLISH?"pq_publish":bi==GLOP_STAGE?"stage":"?";
        k+=snprintf(m+k,sizeof m-k," %s %.0f/f %.2fus %.0f%%,",nm,(double)g_op_n[bi]/fr,g_op_n[bi]?bt*1e-3*tb.numer/tb.denom/(double)g_op_n[bi]:0,100.0*bt/(double)(tot?tot:1));
        g_op_t[bi]=0; if(k>(int)sizeof m-120) break; }
    k+=snprintf(m+k,sizeof m-k," format queries cached %llu, asked %llu%s",&glhpf_hits?(unsigned long long)glhpf_hits:0,&glhpf_miss?(unsigned long long)glhpf_miss:0,&glhpf_on?"":" (no peload-gl-pf.dylib)");
    memset(g_op_n,0,sizeof g_op_n); memset(g_op_t,0,sizeof g_op_t); m[k++]='\n'; (void)!write(2,m,(size_t)k); g_op_tb=now; }

static void* parent_watch(void*a){ pid_t pp=(pid_t)(intptr_t)a; int kq=kqueue(); struct kevent ev; EV_SET(&ev,pp,EVFILT_PROC,EV_ADD|EV_ONESHOT,NOTE_EXIT,0,0);
    if(kq<0 || kevent(kq,&ev,1,0,0,0)<0){ while(getppid()==pp) sleep(1); _exit(0); }
    struct kevent out; kevent(kq,0,0,&out,1,0); _exit(0); }

int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"usage: peload-gl <shm> <wake fd> <present fd> <parent pid>\n"); return 2; }
    int fd=shm_open(argv[1],O_RDWR,0600); if(fd<0){ logf_("  [glhelper] shm_open %s: %s\n",argv[1],strerror(errno)); return 1; }
    S=mmap(0,sizeof(GlpShared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd); if(S==MAP_FAILED){ logf_("  [glhelper] mmap failed\n"); return 1; }
    g_wake_fd=atoi(argv[2]); g_present_fd=atoi(argv[3]); signal(SIGPIPE,SIG_IGN);
    pthread_t t; pthread_create(&t,0,parent_watch,(void*)(intptr_t)atoi(argv[4])); pthread_detach(t);
    g_check=getenv("PELOAD_GL_HELPER_CHECK")!=0; int opstats=getenv("PELOAD_GL_HELPER_OPS")!=0;
    CGLPixelFormatAttribute at[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAAccelerated,kCGLPFAColorSize,(CGLPixelFormatAttribute)24,kCGLPFAAlphaSize,(CGLPixelFormatAttribute)8,kCGLPFADepthSize,(CGLPixelFormatAttribute)24,kCGLPFAStencilSize,(CGLPixelFormatAttribute)8,(CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf=0; GLint np=0; if(CGLChoosePixelFormat(at,&pf,&np)||!pf){ CGLPixelFormatAttribute b[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,(CGLPixelFormatAttribute)0}; CGLChoosePixelFormat(b,&pf,&np); }
    if(!pf || CGLCreateContext(pf,0,&g_ctx) || !g_ctx){ logf_("  [glhelper] no GL context\n"); return 1; }
    if(&glhpf_thread){ g_pf_off=getenv("PELOAD_GL_HELPER_NOPFCACHE")!=0; glhpf_thread=pthread_self(); pf_sync(); }
    CGLSetCurrentContext(g_ctx); glh_load();
    { pthread_attr_t a; (void)a; pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0); }
    logf_("  [glhelper] native GL helper ready: %s\n",(const char*)glGetString(GL_RENDERER));
    uint64_t tail=S->tail; double t_idle=0, tb=mono(); int spins=0;
    for(;;){
        uint64_t head=__atomic_load_n(&S->head,__ATOMIC_ACQUIRE);
        if(tail==head){
            if(g_npq) poll_queries(); if(g_slot_pending>=0) present_poll(0);
            if(++spins<20000){ __asm__ volatile("yield"); continue; }   // ~ a few hundred us of polling before sleeping
            double t0=mono(); S->helper_sleeping=1; __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if(__atomic_load_n(&S->head,__ATOMIC_ACQUIRE)==tail){ struct pollfd pf={g_wake_fd,POLLIN,0}; int to=(g_npq||g_slot_pending>=0)?1:100;
                if(poll(&pf,1,to)>0){ char buf[64]; (void)!read(g_wake_fd,buf,sizeof buf); } }
            S->helper_sleeping=0; t_idle+=mono()-t0; spins=0; continue; }
        spins=0;
        while(tail!=head){
            uint64_t pos=tail%GLP_RING_WORDS; uint32_t h=S->ring[pos], op=GLCMD_OP(h), nw=GLCMD_NW(h);
            if(op==GLOP_WRAP){ tail+=GLP_RING_WORDS-pos; continue; }
            if(__builtin_expect(opstats,0)){ uint64_t t0=mach_absolute_time(); decode(op,&S->ring[pos+1],nw); g_op_t[op&255]+=mach_absolute_time()-t0; g_op_n[op&255]++; if(op==GLOP_PQ_PUBLISH) ops_report(); }
            else decode(op,&S->ring[pos+1],nw);
            tail+=1+nw; S->cmds++;
            if((S->cmds&255)==0){ __atomic_store_n(&S->tail,tail,__ATOMIC_RELEASE); if(g_npq) poll_queries(); }
        }
        __atomic_store_n(&S->tail,tail,__ATOMIC_RELEASE);
        if((S->cmds&1023)<16){ double now=mono(); S->idle_s=t_idle; S->busy_s=now-tb-t_idle; }
    }
}
