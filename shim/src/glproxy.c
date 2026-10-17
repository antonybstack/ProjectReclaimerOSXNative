// glproxy.c -- loader side of the native GL helper (shim/docs/gl-helper.md). Included by peload.c right after the
// renderer's G table. PELOAD_GL_HELPER=1: the renderer's GL context lives in shim/build/peload-gl (arm64); its calls
// are encoded into a shared-memory ring here. Every other GL user (the window/present thread, WGL) is untouched.
#include <OpenGL/CGLIOSurface.h>
#include <IOSurface/IOSurface.h>
#include <poll.h>
#include <mach/machine.h>
#include "glproto.h"

static GlpShared* g_glp;                 // shared memory (NULL = helper off)
static int g_glp_on;                     // the renderer's context is the helper's
static __thread int t_glp;               // this thread's current context is the helper's
static uint64_t g_glp_head, g_glp_end;   // producer position (loader-local copy of g_glp->head) / end of the open command
static int g_glp_wake_w=-1, g_glp_present_r=-1; static pid_t g_glp_pid;
static uint32_t g_glp_seq;               // round-trip sequence
static uint32_t g_glp_next[NS_COUNT];    // next object name per namespace
#define GLP_CTX ((CGLContextObj)(uintptr_t)0x0000474C50524F58ull)   // the renderer's context handle in the loader

static void glp_fatal(const char*why){ char m[200]; int k=snprintf(m,sizeof m,"  [glproxy] %s -- the GL helper is gone; exiting\n",why); (void)!write(2,m,k); _exit(70); }
static void glp_check_alive(void){ int st; if(g_glp_pid>0 && waitpid(g_glp_pid,&st,WNOHANG)==g_glp_pid) glp_fatal("helper exited"); }
static void glp_wake(void){ if(__atomic_exchange_n(&g_glp->helper_sleeping,0,__ATOMIC_ACQ_REL)){ char b=1; (void)!write(g_glp_wake_w,&b,1); } }
static void glp_wait_room(uint64_t need){
    if(g_glp_head+need-__atomic_load_n(&g_glp->tail,__ATOMIC_ACQUIRE)<=GLP_RING_WORDS) return;
    __atomic_store_n(&g_glp->head,g_glp_head,__ATOMIC_RELEASE); glp_wake();
    for(unsigned i=0; g_glp_head+need-__atomic_load_n(&g_glp->tail,__ATOMIC_ACQUIRE)>GLP_RING_WORDS; i++){
        if(i<2000) __asm__ volatile("pause"); else { usleep(50); if((i&1023)==0) glp_check_alive(); } } }
// reserve a command with nw payload words; fill them, then glp_end()
static uint32_t* glp_cmd(uint32_t op,uint32_t nw){
    uint64_t need=1+(uint64_t)nw, pos=g_glp_head%GLP_RING_WORDS;
    if(pos+need>GLP_RING_WORDS){ glp_wait_room(GLP_RING_WORDS-pos); g_glp->ring[pos]=GLCMD(GLOP_WRAP,0); g_glp_head+=GLP_RING_WORDS-pos; pos=0; }
    glp_wait_room(need);
    uint32_t*w=&g_glp->ring[pos]; w[0]=GLCMD(op,nw); g_glp_end=g_glp_head+need; return w+1; }
static inline void glp_end(void){ g_glp_head=g_glp_end; __atomic_store_n(&g_glp->head,g_glp_head,__ATOMIC_RELEASE);
    if(__atomic_load_n(&g_glp->helper_sleeping,__ATOMIC_RELAXED)) glp_wake(); }

#include "glproxy_gen.h"   // glr_X pointers, generated dispatchers, the G install

// ---- data arguments (glproto.h GLDATA_*): stage big ones first, then put the marker/inline bytes in the command ----
enum { GLPD_NULL, GLPD_STAGED, GLPD_INLINE, GLPD_OFFSET };
static uint32_t glp_data_prep(const void*d,uint64_t bytes,int offset_mode,int*mode){
    if(offset_mode){ *mode=GLPD_OFFSET; return 3; }
    if(!d){ *mode=GLPD_NULL; return 1; }
    if(bytes>GLDATA_INLINE_MAX){ for(uint64_t off=0; off<bytes; off+=GLDATA_INLINE_MAX){ uint32_t n=(uint32_t)(bytes-off<GLDATA_INLINE_MAX?bytes-off:GLDATA_INLINE_MAX);
            uint32_t*w=glp_cmd(GLOP_STAGE,5+(n+3)/4); memcpy(w,&bytes,8); memcpy(w+2,&off,8); w[4]=n; memcpy(w+5,(const uint8_t*)d+off,n); glp_end(); }
        *mode=GLPD_STAGED; return 1; }
    *mode=GLPD_INLINE; return 1+(uint32_t)((bytes+3)/4); }
static void glp_data_fill(uint32_t*w,const void*d,uint64_t bytes,int mode){
    if(mode==GLPD_NULL) w[0]=GLDATA_NULL; else if(mode==GLPD_STAGED) w[0]=GLDATA_STAGED;
    else if(mode==GLPD_OFFSET){ uint64_t o=(uint64_t)(uintptr_t)d; w[0]=GLDATA_OFFSET; memcpy(w+1,&o,8); }
    else { w[0]=(uint32_t)bytes; memcpy(w+1,d,(size_t)bytes); } }

// ---- round trips: the request's first payload word is its sequence number ----
static uint32_t* glp_rt(uint32_t op,uint32_t nw,uint32_t*seq){ *seq=++g_glp_seq; uint32_t*w=glp_cmd(op,nw); w[0]=*seq; return w; }
static const uint8_t* glp_rt_wait(uint32_t seq,uint32_t*len){
    glp_end(); glp_wake();
    for(unsigned i=0; __atomic_load_n(&g_glp->reply_seq,__ATOMIC_ACQUIRE)!=seq; i++){ if(i<20000) __asm__ volatile("pause"); else { usleep(20); if((i&4095)==0) glp_check_alive(); } }
    if(len) *len=g_glp->reply_len; return g_glp->reply; }
static uint32_t glp_rt_u32(uint32_t op,uint32_t a,uint32_t b,uint32_t c){ uint32_t seq; uint32_t*w=glp_rt(op,4,&seq); w[1]=a; w[2]=b; w[3]=c; uint32_t v=0; memcpy(&v,glp_rt_wait(seq,0),4); return v; }

// ---- pixel transfer sizes (unpack/pack state tracked here; the helper gets the same PixelStorei calls) ----
static int32_t g_glp_unp[6]={4,0,0,0,0,0}, g_glp_pk[6]={4,0,0,0,0,0};   // alignment, row length, image height, skip pixels/rows/images
static uint32_t g_glp_unpack_buf, g_glp_pack_buf;
static int glp_pix_bytes(GLenum fmt,GLenum type){
    int pk=0; switch(type){ case 0x8032: case 0x8362: pk=1; break; case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366: pk=2; break;
        case 0x8035: case 0x8367: case 0x8036: case 0x8368: case 0x84FA: case 0x8C3B: case 0x8C3E: pk=4; break; case 0x8DAD: pk=8; break; }
    if(pk) return pk;
    int ts=(type==0x1400||type==0x1401)?1:(type==0x1402||type==0x1403||type==0x140B)?2:4;
    int nc=1; switch(fmt){ case 0x8227: case 0x8228: nc=2; break; case 0x1907: case 0x80E0: case 0x8D98: case 0x8D9A: nc=3; break; case 0x1908: case 0x80E1: case 0x8D99: case 0x8D9B: nc=4; break; case 0x84F9: nc=2; break; }
    return nc*ts; }
static uint64_t glp_img_bytes(int w,int h,int d,GLenum fmt,GLenum type,const int32_t*st){
    if(w<=0||h<=0||d<=0) return 0; int bpp=glp_pix_bytes(fmt,type); int a=st[0]>0?st[0]:1;
    uint64_t rl=(uint64_t)(st[1]>0?st[1]:w), rowb=(rl*bpp+a-1)/a*a, ih=(uint64_t)(st[2]>0?st[2]:h);
    return ((uint64_t)(st[5]+d-1)*ih+(uint64_t)(st[4]+h-1))*rowb+(uint64_t)(st[3]+w)*bpp; }

// ---- custom encoders (glhelper.c decodes the same layouts) ----
static void glp_BindBuffer(GLenum t,GLuint b){ if(!t_glp){ glr_BindBuffer(t,b); return; }
    if(t==0x88EC) g_glp_unpack_buf=b; else if(t==0x88EB) g_glp_pack_buf=b;
    uint32_t*w=glp_cmd(GLOP_BindBuffer,2); w[0]=t; w[1]=b; glp_end(); }
static void glp_PixelStorei(GLenum pn,GLint v){ if(!t_glp){ glr_PixelStorei(pn,v); return; }
    switch(pn){ case 0x0CF5: g_glp_unp[0]=v; break; case 0x0CF2: g_glp_unp[1]=v; break; case 0x806E: g_glp_unp[2]=v; break; case 0x0CF4: g_glp_unp[3]=v; break; case 0x0CF3: g_glp_unp[4]=v; break; case 0x806D: g_glp_unp[5]=v; break;
        case 0x0D05: g_glp_pk[0]=v; break; case 0x0D02: g_glp_pk[1]=v; break; case 0x806C: g_glp_pk[2]=v; break; case 0x0D04: g_glp_pk[3]=v; break; case 0x0D03: g_glp_pk[4]=v; break; case 0x806B: g_glp_pk[5]=v; break; }
    uint32_t*w=glp_cmd(GLOP_PixelStorei,2); w[0]=pn; w[1]=(uint32_t)v; glp_end(); }
static void glp_BeginQuery(GLenum t,GLuint id){ if(!t_glp){ glr_BeginQuery(t,id); return; }
    uint32_t gen=0; if(id<GLP_QUERIES){ gen=g_glp->qgen[id]+1; if(!gen) gen=1; g_glp->qgen[id]=gen; }
    uint32_t*w=glp_cmd(GLOP_BeginQuery,3); w[0]=t; w[1]=id; w[2]=gen; glp_end(); }
static void glp_EndQuery(GLenum t){ if(!t_glp){ glr_EndQuery(t); return; } uint32_t*w=glp_cmd(GLOP_EndQuery,1); w[0]=t; glp_end(); }
static void glp_GetQueryObjectiv(GLuint id,GLenum pn,GLint*v){ if(!t_glp){ glr_GetQueryObjectiv(id,pn,v); return; } if(!v) return;
    int av=id<GLP_QUERIES && __atomic_load_n(&g_glp->qdone[id],__ATOMIC_ACQUIRE)==g_glp->qgen[id];
    if(pn==0x8867/*RESULT_AVAILABLE*/) *v=av; else *v=av?(GLint)g_glp->qval[id]:0; }
static void glp_GetQueryObjectui64v(GLuint id,GLenum pn,GLuint64*v){ if(!t_glp){ glr_GetQueryObjectui64v(id,pn,v); return; } if(!v) return;
    int av=id<GLP_QUERIES && __atomic_load_n(&g_glp->qdone[id],__ATOMIC_ACQUIRE)==g_glp->qgen[id];
    if(pn==0x8867) *v=(GLuint64)av; else *v=av?g_glp->qval[id]:0; }
#define GLP_GEN(fn,ns) static void glp_##fn(GLsizei n,GLuint*out){ if(!t_glp){ glr_##fn(n,out); return; } if(n<=0||!out) return; \
    for(GLsizei i=0;i<n;i++){ uint32_t c=++g_glp_next[ns]; if(ns==NS_QRY && c>=GLP_QUERIES){ static int w1; if(!w1++) (void)!write(2,"  [glproxy] query names past the result table\n",47); } out[i]=c; } \
    uint32_t*w=glp_cmd(GLOP_##fn,1+(uint32_t)n); w[0]=(uint32_t)n; memcpy(w+1,out,(size_t)n*4); glp_end(); }
GLP_GEN(GenBuffers,NS_BUF) GLP_GEN(GenFramebuffers,NS_FBO) GLP_GEN(GenProgramPipelines,NS_PIPE) GLP_GEN(GenQueries,NS_QRY)
GLP_GEN(GenRenderbuffers,NS_RBO) GLP_GEN(GenSamplers,NS_SMP) GLP_GEN(GenTextures,NS_TEX) GLP_GEN(GenVertexArrays,NS_VAO)
static GLuint glp_CreateShader(GLenum ty){ if(!t_glp) return glr_CreateShader(ty); uint32_t c=++g_glp_next[NS_OBJ]; uint32_t*w=glp_cmd(GLOP_CreateShader,2); w[0]=ty; w[1]=c; glp_end(); return c; }
static GLuint glp_CreateProgram(void){ if(!t_glp) return glr_CreateProgram(); uint32_t c=++g_glp_next[NS_OBJ]; uint32_t*w=glp_cmd(GLOP_CreateProgram,1); w[0]=c; glp_end(); return c; }
// strings: [count, len_0.., bytes] (no terminators)
static uint32_t glp_strings_words(GLsizei c,const GLchar*const*s,const GLint*l,uint32_t*lens){ uint64_t b=0;
    for(GLsizei i=0;i<c;i++){ lens[i]=s[i]?(uint32_t)((l&&l[i]>=0)?l[i]:(GLint)strlen(s[i])):0; b+=lens[i]; } return 1+(uint32_t)c+(uint32_t)((b+3)/4); }
static void glp_strings_fill(uint32_t*w,GLsizei c,const GLchar*const*s,const uint32_t*lens){ w[0]=(uint32_t)c; memcpy(w+1,lens,(size_t)c*4); char*b=(char*)(w+1+c);
    for(GLsizei i=0;i<c;i++){ if(lens[i]) memcpy(b,s[i],lens[i]); b+=lens[i]; } }
static void glp_ShaderSource(GLuint sh,GLsizei c,const GLchar*const*s,const GLint*l){ if(!t_glp){ glr_ShaderSource(sh,c,s,l); return; } if(c<0) return; if(c>64) c=64;
    uint32_t lens[64]; uint32_t nw=glp_strings_words(c,s,l,lens); uint32_t*w=glp_cmd(GLOP_ShaderSource,1+nw); w[0]=sh; glp_strings_fill(w+1,c,s,lens); glp_end(); }
static GLuint glp_CreateShaderProgramv(GLenum ty,GLsizei c,const GLchar*const*s){ if(!t_glp) return glr_CreateShaderProgramv(ty,c,s); if(c<0) c=0; if(c>64) c=64;
    uint32_t cn=++g_glp_next[NS_OBJ]; uint32_t lens[64]; uint32_t nw=glp_strings_words(c,s,0,lens); uint32_t*w=glp_cmd(GLOP_CreateShaderProgramv,2+nw); w[0]=ty; w[1]=cn; glp_strings_fill(w+2,c,s,lens); glp_end(); return cn; }
static void glp_DrawBuffers(GLsizei n,const GLenum*b){ if(!t_glp){ glr_DrawBuffers(n,b); return; } if(n<0||n>16) return; uint32_t*w=glp_cmd(GLOP_DrawBuffers,1+(uint32_t)n); w[0]=(uint32_t)n; memcpy(w+1,b,(size_t)n*4); glp_end(); }
static void glp_ProgramUniform4fv(GLuint pr,GLint loc,GLsizei c,const GLfloat*v){ if(!t_glp){ glr_ProgramUniform4fv(pr,loc,c,v); return; } if(c<0) return;
    uint32_t*w=glp_cmd(GLOP_ProgramUniform4fv,3+4*(uint32_t)c); w[0]=pr; w[1]=(uint32_t)loc; w[2]=(uint32_t)c; memcpy(w+3,v,(size_t)c*16); glp_end(); }
static void glp_SamplerParameterfv(GLuint sm,GLenum pn,const GLfloat*v){ if(!t_glp){ glr_SamplerParameterfv(sm,pn,v); return; }
    uint32_t*w=glp_cmd(GLOP_SamplerParameterfv,6); w[0]=sm; w[1]=pn; memset(w+2,0,16); memcpy(w+2,v,pn==0x1004/*BORDER_COLOR*/?16:4); glp_end(); }
static void glp_BufferData(GLenum t,GLsizeiptr sz,const void*d,GLenum use){ if(!t_glp){ glr_BufferData(t,sz,d,use); return; }
    int m; uint64_t s=(uint64_t)sz; uint32_t dw=glp_data_prep(d,s,0,&m); uint32_t*w=glp_cmd(GLOP_BufferData,4+dw); w[0]=t; w[1]=use; memcpy(w+2,&s,8); glp_data_fill(w+4,d,s,m); glp_end(); }
static void glp_BufferSubData(GLenum t,GLintptr off,GLsizeiptr sz,const void*d){ if(!t_glp){ glr_BufferSubData(t,off,sz,d); return; }
    int m; uint64_t o=(uint64_t)off, s=(uint64_t)sz; uint32_t dw=glp_data_prep(d,s,0,&m); uint32_t*w=glp_cmd(GLOP_BufferSubData,5+dw); w[0]=t; memcpy(w+1,&o,8); memcpy(w+3,&s,8); glp_data_fill(w+5,d,s,m); glp_end(); }
static void glp_TexImage2D(GLenum t,GLint lv,GLint ifmt,GLsizei wd,GLsizei ht,GLint bd,GLenum fmt,GLenum ty,const void*px){ if(!t_glp){ glr_TexImage2D(t,lv,ifmt,wd,ht,bd,fmt,ty,px); return; }
    int m; uint64_t b=glp_img_bytes(wd,ht,1,fmt,ty,g_glp_unp); uint32_t dw=glp_data_prep(px,b,g_glp_unpack_buf!=0,&m);
    uint32_t*w=glp_cmd(GLOP_TexImage2D,8+dw); w[0]=t; w[1]=(uint32_t)lv; w[2]=(uint32_t)ifmt; w[3]=(uint32_t)wd; w[4]=(uint32_t)ht; w[5]=(uint32_t)bd; w[6]=fmt; w[7]=ty; glp_data_fill(w+8,px,b,m); glp_end(); }
static void glp_TexImage3D(GLenum t,GLint lv,GLint ifmt,GLsizei wd,GLsizei ht,GLsizei dp,GLint bd,GLenum fmt,GLenum ty,const void*px){ if(!t_glp){ glr_TexImage3D(t,lv,ifmt,wd,ht,dp,bd,fmt,ty,px); return; }
    int m; uint64_t b=glp_img_bytes(wd,ht,dp,fmt,ty,g_glp_unp); uint32_t dw=glp_data_prep(px,b,g_glp_unpack_buf!=0,&m);
    uint32_t*w=glp_cmd(GLOP_TexImage3D,9+dw); w[0]=t; w[1]=(uint32_t)lv; w[2]=(uint32_t)ifmt; w[3]=(uint32_t)wd; w[4]=(uint32_t)ht; w[5]=(uint32_t)dp; w[6]=(uint32_t)bd; w[7]=fmt; w[8]=ty; glp_data_fill(w+9,px,b,m); glp_end(); }
static void glp_TexSubImage2D(GLenum t,GLint lv,GLint x,GLint y,GLsizei wd,GLsizei ht,GLenum fmt,GLenum ty,const void*px){ if(!t_glp){ glr_TexSubImage2D(t,lv,x,y,wd,ht,fmt,ty,px); return; }
    int m; uint64_t b=glp_img_bytes(wd,ht,1,fmt,ty,g_glp_unp); uint32_t dw=glp_data_prep(px,b,g_glp_unpack_buf!=0,&m);
    uint32_t*w=glp_cmd(GLOP_TexSubImage2D,8+dw); w[0]=t; w[1]=(uint32_t)lv; w[2]=(uint32_t)x; w[3]=(uint32_t)y; w[4]=(uint32_t)wd; w[5]=(uint32_t)ht; w[6]=fmt; w[7]=ty; glp_data_fill(w+8,px,b,m); glp_end(); }
static void glp_TextureSubImage2D(GLuint tx,GLint lv,GLint x,GLint y,GLsizei wd,GLsizei ht,GLenum fmt,GLenum ty,const void*px){ if(!t_glp && glr_TextureSubImage2D) glr_TextureSubImage2D(tx,lv,x,y,wd,ht,fmt,ty,px); }
static void glp_CompressedTexImage2D(GLenum t,GLint lv,GLenum ifmt,GLsizei wd,GLsizei ht,GLint bd,GLsizei isz,const void*d){ if(!t_glp){ glr_CompressedTexImage2D(t,lv,ifmt,wd,ht,bd,isz,d); return; }
    int m; uint32_t dw=glp_data_prep(d,(uint64_t)(isz>0?isz:0),g_glp_unpack_buf!=0,&m);
    uint32_t*w=glp_cmd(GLOP_CompressedTexImage2D,6+dw); w[0]=t; w[1]=(uint32_t)lv; w[2]=ifmt; w[3]=(uint32_t)wd; w[4]=(uint32_t)ht; w[5]=(uint32_t)bd; glp_data_fill(w+6,d,(uint64_t)(isz>0?isz:0),m); glp_end(); }
// maps: the loader writes into its own memory; Unmap sends the bytes (write maps) -- no round trip for the cb/vertex ring
typedef struct { GLenum target; uint64_t off, len; uint32_t access; uint8_t*mem; uint64_t cap; int on; } GlpMap;
static GlpMap g_glp_maps[4];
static int glp_map_slot(GLenum t){ switch(t){ case 0x8892: return 0; case 0x8A11: return 1; case 0x88EB: return 2; default: return 3; } }
static void* glp_MapBufferRange(GLenum t,GLintptr off,GLsizeiptr len,GLbitfield acc){ if(!t_glp) return glr_MapBufferRange(t,off,len,acc); if(len<=0) return 0;
    GlpMap*m=&g_glp_maps[glp_map_slot(t)]; if((uint64_t)len>m->cap){ uint8_t*n=realloc(m->mem,(size_t)len); if(!n) return 0; m->mem=n; m->cap=(uint64_t)len; }
    m->target=t; m->off=(uint64_t)off; m->len=(uint64_t)len; m->access=acc; m->on=1;
    if(acc&0x0001/*READ*/){ uint32_t seq; uint32_t*w=glp_rt(GLOP_MapBufferRange,6,&seq); w[1]=t; memcpy(w+2,&m->off,8); memcpy(w+4,&m->len,8); uint32_t got=0; const uint8_t*r=glp_rt_wait(seq,&got);
        if(g_glp->reply_status!=1){ m->on=0; return 0; } memcpy(m->mem,r,got<m->len?got:m->len); }
    return m->mem; }
static GLboolean glp_UnmapBuffer(GLenum t){ if(!t_glp) return glr_UnmapBuffer(t); GlpMap*m=&g_glp_maps[glp_map_slot(t)]; if(!m->on) return 0; m->on=0;
    if(!(m->access&0x0002/*WRITE*/)) return 1;
    int md; uint32_t dw=glp_data_prep(m->mem,m->len,0,&md); uint32_t*w=glp_cmd(GLOP_UnmapBuffer,6+dw); w[0]=t; memcpy(w+1,&m->off,8); memcpy(w+3,&m->len,8); w[5]=m->access; glp_data_fill(w+6,m->mem,m->len,md); glp_end(); return 1; }
// round trips
static void glp_GetBufferSubData(GLenum t,GLintptr off,GLsizeiptr sz,void*d){ if(!t_glp){ glr_GetBufferSubData(t,off,sz,d); return; }
    uint32_t seq; uint64_t o=(uint64_t)off, s=(uint64_t)sz; uint32_t*w=glp_rt(GLOP_GetBufferSubData,6,&seq); w[1]=t; memcpy(w+2,&o,8); memcpy(w+4,&s,8); uint32_t got=0; const uint8_t*r=glp_rt_wait(seq,&got); if(d) memcpy(d,r,got<s?got:s); }
static void glp_ReadPixels(GLint x,GLint y,GLsizei wd,GLsizei ht,GLenum fmt,GLenum ty,void*px){ if(!t_glp){ glr_ReadPixels(x,y,wd,ht,fmt,ty,px); return; }
    if(g_glp_pack_buf){ uint32_t*w=glp_cmd(GLOP_ReadPixels,9); w[0]=0; w[1]=(uint32_t)x; w[2]=(uint32_t)y; w[3]=(uint32_t)wd; w[4]=(uint32_t)ht; w[5]=fmt; w[6]=ty; uint64_t o=(uint64_t)(uintptr_t)px; memcpy(w+7,&o,8); glp_end(); return; }
    uint64_t b=glp_img_bytes(wd,ht,1,fmt,ty,g_glp_pk); uint32_t seq; uint32_t*w=glp_rt(GLOP_ReadPixels,8,&seq);
    w[1]=(uint32_t)x; w[2]=(uint32_t)y; w[3]=(uint32_t)wd; w[4]=(uint32_t)ht; w[5]=fmt; w[6]=ty; w[7]=(uint32_t)b; uint32_t got=0; const uint8_t*r=glp_rt_wait(seq,&got); if(px) memcpy(px,r,got<b?got:b); }
static void glp_GetTexLevelParameteriv(GLenum t,GLint lv,GLenum pn,GLint*v){ if(!t_glp){ glr_GetTexLevelParameteriv(t,lv,pn,v); return; }
    uint32_t r=glp_rt_u32(GLOP_GetTexLevelParameteriv,t,(uint32_t)lv,pn); if(v) *v=(GLint)r; }
static void glp_GetTexImage(GLenum t,GLint lv,GLenum fmt,GLenum ty,void*px){ if(!t_glp){ glr_GetTexImage(t,lv,fmt,ty,px); return; }
    GLint wd=0,ht=0,dp=1; glp_GetTexLevelParameteriv(t,lv,0x1000/*WIDTH*/,&wd); glp_GetTexLevelParameteriv(t,lv,0x1001/*HEIGHT*/,&ht); if(t==0x806F||t==0x8C1A) glp_GetTexLevelParameteriv(t,lv,0x8071/*DEPTH*/,&dp);
    uint64_t b=glp_img_bytes(wd,ht,dp>0?dp:1,fmt,ty,g_glp_pk); uint32_t seq; uint32_t*w=glp_rt(GLOP_GetTexImage,6,&seq); w[1]=t; w[2]=(uint32_t)lv; w[3]=fmt; w[4]=ty; w[5]=(uint32_t)b;
    uint32_t got=0; const uint8_t*r=glp_rt_wait(seq,&got); if(px) memcpy(px,r,got<b?got:b); }
static int glp_get_count(GLenum pn){ switch(pn){ case 0x0BA2: case 0x0C10: case 0x0C22: case 0x0C23: case 0x8005: return 4; case 0x0B70: case 0x0D3A: case 0x846E: return 2; default: return 1; } }
static void glp_GetIntegerv(GLenum pn,GLint*v){ if(!t_glp){ glr_GetIntegerv(pn,v); return; } uint32_t seq; uint32_t*w=glp_rt(GLOP_GetIntegerv,2,&seq); w[1]=pn; const uint8_t*r=glp_rt_wait(seq,0); if(v) memcpy(v,r,(size_t)glp_get_count(pn)*4); }
static void glp_GetFloatv(GLenum pn,GLfloat*v){ if(!t_glp){ glr_GetFloatv(pn,v); return; } uint32_t seq; uint32_t*w=glp_rt(GLOP_GetFloatv,2,&seq); w[1]=pn; const uint8_t*r=glp_rt_wait(seq,0); if(v) memcpy(v,r,(size_t)glp_get_count(pn)*4); }
static void glp_GetBooleanv(GLenum pn,GLboolean*v){ if(!t_glp){ glr_GetBooleanv(pn,v); return; } uint32_t seq; uint32_t*w=glp_rt(GLOP_GetBooleanv,2,&seq); w[1]=pn; const uint8_t*r=glp_rt_wait(seq,0); if(v) memcpy(v,r,(size_t)glp_get_count(pn)); }
static GLboolean glp_IsEnabled(GLenum c){ if(!t_glp) return glr_IsEnabled(c); return (GLboolean)glp_rt_u32(GLOP_IsEnabled,c,0,0); }
static GLenum glp_CheckFramebufferStatus(GLenum t){ if(!t_glp) return glr_CheckFramebufferStatus(t); return glp_rt_u32(GLOP_CheckFramebufferStatus,t,0,0); }
static const GLubyte* glp_GetString(GLenum n){ if(!t_glp) return glr_GetString(n);
    static struct { GLenum n; char* s; } cache[16]; static int nc; for(int i=0;i<nc;i++) if(cache[i].n==n) return (const GLubyte*)cache[i].s;
    uint32_t seq; uint32_t*w=glp_rt(GLOP_GetString,2,&seq); w[1]=n; uint32_t got=0; const uint8_t*r=glp_rt_wait(seq,&got); if(!got) return 0;
    char*s=malloc(got+1); memcpy(s,r,got); s[got]=0; if(nc<16){ cache[nc].n=n; cache[nc].s=s; nc++; } return (const GLubyte*)s; }
static void glp_GetProgramiv(GLuint p,GLenum pn,GLint*v){ if(!t_glp){ glr_GetProgramiv(p,pn,v); return; } uint32_t r=glp_rt_u32(GLOP_GetProgramiv,p,pn,0); if(v) *v=(GLint)r; }
static void glp_GetShaderiv(GLuint s,GLenum pn,GLint*v){ if(!t_glp){ glr_GetShaderiv(s,pn,v); return; } uint32_t r=glp_rt_u32(GLOP_GetShaderiv,s,pn,0); if(v) *v=(GLint)r; }
#define GLP_LOG(fn) static void glp_##fn(GLuint o,GLsizei cap,GLsizei*len,GLchar*out){ if(!t_glp){ glr_##fn(o,cap,len,out); return; } \
    uint32_t got=0; const uint8_t*r=glp_rt_wait(glp_rt_u32_seq(GLOP_##fn,o,(uint32_t)cap),&got); if(cap<=0||!out){ if(len)*len=0; return; } \
    if(got>(uint32_t)cap-1) got=(uint32_t)cap-1; memcpy(out,r,got); out[got]=0; if(len)*len=(GLsizei)got; }
static uint32_t glp_rt_u32_seq(uint32_t op,uint32_t a,uint32_t b){ uint32_t seq; uint32_t*w=glp_rt(op,3,&seq); w[1]=a; w[2]=b; return seq; }
GLP_LOG(GetProgramInfoLog) GLP_LOG(GetShaderInfoLog) GLP_LOG(GetShaderSource)
static GLint glp_GetUniformLocation(GLuint p,const GLchar*nm){ if(!t_glp) return glr_GetUniformLocation(p,nm); int m; uint64_t b=nm?strlen(nm)+1:0;
    uint32_t dw=glp_data_prep(nm,b,0,&m); uint32_t seq; uint32_t*w=glp_rt(GLOP_GetUniformLocation,2+dw,&seq); w[1]=p; glp_data_fill(w+2,nm,b,m); int32_t v=-1; memcpy(&v,glp_rt_wait(seq,0),4); return v; }
static GLuint glp_GetUniformBlockIndex(GLuint p,const GLchar*nm){ if(!t_glp) return glr_GetUniformBlockIndex(p,nm); int m; uint64_t b=nm?strlen(nm)+1:0;
    uint32_t dw=glp_data_prep(nm,b,0,&m); uint32_t seq; uint32_t*w=glp_rt(GLOP_GetUniformBlockIndex,2+dw,&seq); w[1]=p; glp_data_fill(w+2,nm,b,m); uint32_t v=0xFFFFFFFFu; memcpy(&v,glp_rt_wait(seq,0),4); return v; }
static void glp_GetUniformfv(GLuint p,GLint loc,GLfloat*v){ if(!t_glp){ glr_GetUniformfv(p,loc,v); return; } uint32_t seq; uint32_t*w=glp_rt(GLOP_GetUniformfv,3,&seq); w[1]=p; w[2]=(uint32_t)loc; const uint8_t*r=glp_rt_wait(seq,0); if(v) memcpy(v,r,16); }
// GetError: no round trip (it would wait for everything queued); the helper checks errors itself (PELOAD_GL_HELPER_CHECK)
static GLenum glp_GetError(void){ if(!t_glp) return glr_GetError(); static int sync=-1; if(sync<0) sync=getenv("PELOAD_GL_HELPER_SYNCERR")?1:0;
    if(!sync) return 0; uint32_t seq; (void)glp_rt(GLOP_GetError,1,&seq); uint32_t v=0; memcpy(&v,glp_rt_wait(seq,0),4); return v; }
static void glp_Finish(void){ if(!t_glp){ glr_Finish(); return; } uint32_t seq; (void)glp_rt(GLOP_Finish,1,&seq); (void)glp_rt_wait(seq,0); }
// fences: loader-side handles
static uint32_t g_glp_nsync;
static GLsync glp_FenceSync(GLenum c,GLbitfield f){ if(!t_glp) return glr_FenceSync(c,f); uint32_t h=++g_glp_nsync; uint32_t*w=glp_cmd(GLOP_FenceSync,3); w[0]=h; w[1]=c; w[2]=f; glp_end(); return (GLsync)(uintptr_t)(((uint64_t)h<<4)|0x9); }
static int glp_is_sync(GLsync s){ return ((uintptr_t)s&0xF)==0x9 && (uintptr_t)s<((uint64_t)1<<36); }
static void glp_DeleteSync(GLsync s){ if(!t_glp||!glp_is_sync(s)){ if(glr_DeleteSync) glr_DeleteSync(s); return; } uint32_t*w=glp_cmd(GLOP_DeleteSync,1); w[0]=(uint32_t)((uintptr_t)s>>4); glp_end(); }
static void glp_WaitSync(GLsync s,GLbitfield f,GLuint64 to){ if(!t_glp||!glp_is_sync(s)){ if(glr_WaitSync) glr_WaitSync(s,f,to); return; } uint32_t*w=glp_cmd(GLOP_WaitSync,4); w[0]=(uint32_t)((uintptr_t)s>>4); w[1]=f; memcpy(w+2,&to,8); glp_end(); }


// ---- dlsym for GL functions: the dispatcher when the helper is on ----
static void* glp_dlsym(const char*name){
    void*real=dlsym(RTLD_DEFAULT,name); if(!g_glp_on || !real) return real;
    for(size_t i=0;i<sizeof g_glp_fns/sizeof g_glp_fns[0];i++) if(!strcmp(g_glp_fns[i].name,name)){ if(!*g_glp_fns[i].real) *g_glp_fns[i].real=real; return g_glp_fns[i].disp; }
    static int nw; if(nw++<20){ char m[160]; int k=snprintf(m,sizeof m,"  [glproxy] %s is not proxied (calls on the renderer's context would go nowhere)\n",name); (void)!write(2,m,k); }
    return real; }

// ---- CGL: the renderer's context is a handle in the loader; real contexts pass through ----
static CGLError glp_cgl_setcur(CGLContextObj c){ if(c==GLP_CTX){ t_glp=1; return kCGLNoError; } t_glp=0; return CGLSetCurrentContext(c); }
static CGLContextObj glp_cgl_getcur(void){ return t_glp?GLP_CTX:CGLGetCurrentContext(); }
static CGLError glp_cgl_lock(CGLContextObj c){ return c==GLP_CTX?kCGLNoError:CGLLockContext(c); }
static CGLError glp_cgl_unlock(CGLContextObj c){ return c==GLP_CTX?kCGLNoError:CGLUnlockContext(c); }
static CGLError glp_cgl_flush(CGLContextObj c){ return c==GLP_CTX?kCGLNoError:CGLFlushDrawable(c); }

// ---- start ----
static int glp_wanted(void){ const char*e=getenv("PELOAD_GL_HELPER"); return e && *e && strcmp(e,"0"); }
static int glp_start(void){
    if(g_glp) return 1;
    char exe[1200]={0}; uint32_t es=sizeof exe; if(_NSGetExecutablePath(exe,&es)) return 0;
    char*sl=strrchr(exe,'/'); if(!sl) return 0; snprintf(sl+1,sizeof exe-(size_t)(sl+1-exe),"peload-gl");
    if(access(exe,X_OK)){ char m[1300]; int k=snprintf(m,sizeof m,"  [glproxy] %s missing: rendering in process\n",exe); (void)!write(2,m,k); return 0; }
    char shm[64]; snprintf(shm,sizeof shm,"/peload-gl.%d",(int)getpid()); shm_unlink(shm);
    int fd=shm_open(shm,O_CREAT|O_RDWR,0600); if(fd<0 || ftruncate(fd,(off_t)sizeof(GlpShared))){ if(fd>=0) close(fd); (void)!write(2,"  [glproxy] shared memory failed\n",33); return 0; }
    GlpShared*s=mmap(0,sizeof(GlpShared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd); if(s==MAP_FAILED){ shm_unlink(shm); return 0; }
    s->pq_latest=-1; s->pq_reading=-1;
    int wake[2], pres[2]; if(pipe(wake)||pipe(pres)){ munmap(s,sizeof(GlpShared)); shm_unlink(shm); return 0; }
    fcntl(wake[1],F_SETFL,O_NONBLOCK); fcntl(pres[0],F_SETFL,O_NONBLOCK); fcntl(pres[1],F_SETFL,O_NONBLOCK);
    char a1[16],a2[16],a3[16]; snprintf(a1,sizeof a1,"%d",wake[0]); snprintf(a2,sizeof a2,"%d",pres[1]); snprintf(a3,sizeof a3,"%d",(int)getpid());
    char*av[]={exe,shm,a1,a2,a3,0}; posix_spawnattr_t at; posix_spawnattr_init(&at);
    cpu_type_t arm=CPU_TYPE_ARM64; size_t ocnt=0; posix_spawnattr_setbinpref_np(&at,1,&arm,&ocnt);   // run it natively
    extern char**environ; pid_t pid=0; int rc=posix_spawn(&pid,exe,0,&at,av,environ); posix_spawnattr_destroy(&at);
    close(wake[0]); close(pres[1]);
    // the helper unlinks nothing: the name goes now that both sides have it mapped (it opens it by name at start)
    if(rc){ char m[120]; int k=snprintf(m,sizeof m,"  [glproxy] starting the GL helper failed: %s\n",strerror(rc)); (void)!write(2,m,k); munmap(s,sizeof(GlpShared)); shm_unlink(shm); close(wake[1]); close(pres[0]); return 0; }
    g_glp=s; g_glp_pid=pid; g_glp_wake_w=wake[1]; g_glp_present_r=pres[0]; g_glp_head=0;
    uint32_t*w=glp_cmd(GLOP_HELLO,1); w[0]=GLPROTO_VERSION; glp_end();
    // wait for it to answer (it maps the shared memory and makes its context first), then drop the name
    uint32_t seq; (void)glp_rt(GLOP_SYNC,1,&seq); glp_end(); glp_wake();
    double t0=mono_s(); while(__atomic_load_n(&g_glp->reply_seq,__ATOMIC_ACQUIRE)!=seq){ usleep(1000); int st; if(waitpid(pid,&st,WNOHANG)==pid || mono_s()-t0>10){
            (void)!write(2,"  [glproxy] the GL helper did not start: rendering in process\n",62); shm_unlink(shm); g_glp=0; munmap(s,sizeof(GlpShared)); return 0; } }
    shm_unlink(shm);
    g_glp_on=1; glp_install_G();
    char m[120]; int k=snprintf(m,sizeof m,"  [glproxy] renderer GL runs in the native helper (pid %d)\n",(int)pid); (void)!write(2,m,k);
    return 1; }

// ---- present (render thread): copy the back buffer into a slot; (present thread): show the newest slot ----
static void glp_pq_publish(unsigned fbo,int vw,int vh,uint64_t frame){
    uint32_t*w=glp_cmd(GLOP_PQ_PUBLISH,5); w[0]=fbo; w[1]=(uint32_t)vw; w[2]=(uint32_t)vh; memcpy(w+3,&frame,8); glp_end(); glp_wake(); }
static struct { uint32_t id; IOSurfaceRef surf; unsigned tex; } g_glp_view[GLP_SLOTS];
static unsigned g_glp_prog, g_glp_vao; static int g_glp_sloc=-1, g_glp_szloc=-1;
// present thread: the newest finished slot (marked as being read), or -1 if none arrived within ms
static int glp_present_acquire(unsigned long*seen,int ms,int*slot_w,int*slot_h){
    if(__atomic_load_n(&g_glp->pq_seq,__ATOMIC_ACQUIRE)==*seen){ struct pollfd pf={g_glp_present_r,POLLIN,0}; if(poll(&pf,1,ms)<=0) return -1; }
    { char b[64]; while(read(g_glp_present_r,b,sizeof b)>0){} }
    uint64_t sq=__atomic_load_n(&g_glp->pq_seq,__ATOMIC_ACQUIRE); if(sq==*seen) return -1;
    int slot=__atomic_load_n(&g_glp->pq_latest,__ATOMIC_ACQUIRE); if(slot<0||slot>=GLP_SLOTS) return -1;
    __atomic_store_n(&g_glp->pq_reading,slot,__ATOMIC_RELEASE); *seen=(unsigned long)sq;
    *slot_w=(int)g_glp->slot_w[slot]; *slot_h=(int)g_glp->slot_h[slot]; return slot; }
// present thread (window context current, viewport set): draw the slot's IOSurface
static void glp_present_draw(int slot,int w,int h){
    uint32_t id=g_glp->slot_surface[slot]; CGLContextObj pc=CGLGetCurrentContext();
    if(g_glp_view[slot].id!=id){
        if(g_glp_view[slot].surf) CFRelease(g_glp_view[slot].surf);
        g_glp_view[slot].surf=IOSurfaceLookup(id); g_glp_view[slot].id=id;
        if(!g_glp_view[slot].tex) G.GenTextures(1,&g_glp_view[slot].tex);
        G.BindTexture(0x84F5/*RECTANGLE*/,g_glp_view[slot].tex);
        if(g_glp_view[slot].surf) CGLTexImageIOSurface2D(pc,0x84F5,GLE_RGBA8,w,h,0x80E1/*BGRA*/,0x8367/*8_8_8_8_REV*/,g_glp_view[slot].surf,0);
        G.TexParameteri(0x84F5,0x2801,GLE_LINEAR); G.TexParameteri(0x84F5,0x2800,GLE_LINEAR); G.TexParameteri(0x84F5,0x2802,GLE_CLAMP); G.TexParameteri(0x84F5,0x2803,GLE_CLAMP); }
    if(!g_glp_prog){ const char*vs="#version 410 core\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);uv=p*0.5+0.5;gl_Position=vec4(p,0.0,1.0);}\n";
        const char*fs="#version 410 core\nin vec2 uv;out vec4 c;uniform sampler2DRect s;uniform vec4 sz;void main(){c=vec4(texture(s,uv*sz.xy).rgb,1.0);}\n";
        unsigned v=G.CreateShader(GLE_VS),fr=G.CreateShader(GLE_FS); G.ShaderSource(v,1,&vs,0); G.CompileShader(v); G.ShaderSource(fr,1,&fs,0); G.CompileShader(fr);
        g_glp_prog=G.CreateProgram(); G.AttachShader(g_glp_prog,v); G.AttachShader(g_glp_prog,fr); G.LinkProgram(g_glp_prog); G.DeleteShader(v); G.DeleteShader(fr);
        g_glp_sloc=G.GetUniformLocation(g_glp_prog,"s"); g_glp_szloc=G.GetUniformLocation(g_glp_prog,"sz"); G.GenVertexArrays(1,&g_glp_vao); }
    G.UseProgram(0); G.BindProgramPipeline(0); G.UseProgram(g_glp_prog);
    G.ActiveTexture(GLE_TEX0); G.BindTexture(0x84F5,g_glp_view[slot].tex); if(G.BindSampler) G.BindSampler(0,0);
    if(g_glp_sloc>=0) G.ProgramUniform1i(g_glp_prog,g_glp_sloc,0);
    if(g_glp_szloc>=0){ float sz[4]={(float)w,(float)h,0,0}; G.ProgramUniform4fv(g_glp_prog,g_glp_szloc,1,sz); }
    G.BindVertexArray(g_glp_vao); G.DrawArrays(4,0,3); }
static void glp_present_done(void){ __atomic_store_n(&g_glp->pq_reading,-1,__ATOMIC_RELEASE); }
