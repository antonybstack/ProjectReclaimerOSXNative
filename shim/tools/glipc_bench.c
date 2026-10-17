// glipc_bench: feasibility test for the native GL helper (shim/docs/PERF-PLAN.md option 1). glbench's draw loop, but the
// x86_64 client (Rosetta, like the loader) only ENCODES its GL calls into a shared-memory ring and a native arm64 server
// process replays them on a real GL context. Prints the client's cost per draw (what the loader's render thread would
// pay), the server's, and the end-to-end frame time with the client running at most one frame ahead.
//     bash shim/tools/glipc_bench.sh [draws/frame] [frames] [textures/draw] [cbs/draw]
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>

static double now_us(void){ static mach_timebase_info_data_t tb; if(!tb.denom) mach_timebase_info(&tb); return mach_absolute_time()*(double)tb.numer/tb.denom/1000.0; }

// ---- shared ring: SPSC, 4-byte words, monotonic positions ----
enum { RING_WORDS=1<<22 };   // 16 MB
typedef struct {
    volatile uint64_t head, pad0[7];      // producer position (words)
    volatile uint64_t tail, pad1[7];      // consumer position (words)
    volatile uint64_t frames_done, pad2[7];
    volatile int ready, quit; double server_us; uint64_t server_draws;
    uint32_t w[RING_WORDS];
} Ring;
enum { OP_PROG=1, OP_TEX, OP_UBO, OP_ABLE, OP_DMASK, OP_DRAW, OP_BEGIN, OP_END, OP_SETUP };

// ---------------- client (x86_64) ----------------
static Ring* R; static uint64_t c_head;
static inline uint32_t* put(int n){   // reserve n words (never wraps mid-command: a command that would wrap waits for room at 0)
    uint64_t pos=c_head%RING_WORDS; if(pos+n>RING_WORDS){ while(c_head+(RING_WORDS-pos)+n-__atomic_load_n(&R->tail,__ATOMIC_ACQUIRE)>RING_WORDS) {} R->w[pos]=0; c_head+=RING_WORDS-pos; pos=0; }
    while(c_head+n-__atomic_load_n(&R->tail,__ATOMIC_ACQUIRE)>RING_WORDS) {}
    uint32_t*p=&R->w[pos]; c_head+=n; return p; }
static inline void publish(void){ __atomic_store_n(&R->head,c_head,__ATOMIC_RELEASE); }

static int client(int argc,char**argv,const char*self_arm64){
    int dpf=argc>1?atoi(argv[1]):3000, frames=argc>2?atoi(argv[2]):60, tpd=argc>3?atoi(argv[3]):6, cbpd=argc>4?atoi(argv[4]):2; if(tpd>8) tpd=8; if(cbpd<1) cbpd=1; if(cbpd>4) cbpd=4;
    char shm[64]; snprintf(shm,sizeof shm,"/glipc.%d",getpid()); shm_unlink(shm);
    int fd=shm_open(shm,O_CREAT|O_RDWR,0600); if(fd<0||ftruncate(fd,sizeof(Ring))){ perror("shm"); return 1; }
    R=mmap(0,sizeof(Ring),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd); memset((void*)R,0,4096);
    pid_t pid; char*av[]={(char*)self_arm64,"--server",shm,0}; extern char**environ;
    if(posix_spawn(&pid,self_arm64,0,0,av,environ)){ perror("spawn"); return 1; }
    while(!__atomic_load_n(&R->ready,__ATOMIC_ACQUIRE)) usleep(1000);
    float cb[64]; for(int i=0;i<64;i++) cb[i]=0.01f*i;
    double csum=0, esum=0; int n=0; unsigned seed=1; double tprev=now_us();
    for(int f=0;f<frames+5;f++){
        double t0=now_us();
        put(1)[0]=OP_BEGIN;
        for(int d=0; d<dpf; d++){
            seed=seed*1103515245u+12345u;
            if(d%4==0){ uint32_t*p=put(2); p[0]=OP_PROG; p[1]=(seed>>8); }
            for(int u=0;u<tpd;u++){ uint32_t*p=put(4); p[0]=OP_TEX; p[1]=u; p[2]=((seed>>(u*3))+d); p[3]=(seed>>(u+5)); }
            for(int k=0;k<cbpd;k++){ uint32_t*p=put(2+64); p[0]=OP_UBO; p[1]=k; memcpy(p+2,cb,256); }   // the constants travel in the ring
            if(d%16==0){ uint32_t*p=put(3); p[0]=OP_ABLE; p[1]=(seed>>12)&1; p[2]=(seed>>13)&1; }
            uint32_t*p=put(2); p[0]=OP_DRAW; p[1]=(((seed>>16)%64+1)*3);
            if((d&15)==15) publish();
        }
        put(1)[0]=OP_END; publish();
        double cpu=now_us()-t0;
        // at most one frame ahead (like the loader's present queue)
        while(__atomic_load_n(&R->frames_done,__ATOMIC_ACQUIRE)+1<(uint64_t)f+1) ;
        double t1=now_us();
        if(f>=5){ csum+=cpu; esum+=t1-tprev; n++; } tprev=t1;
    }
    while(__atomic_load_n(&R->frames_done,__ATOMIC_ACQUIRE)<(uint64_t)frames+5) usleep(100);
    R->quit=1; int st; waitpid(pid,&st,0); shm_unlink(shm);
    printf("ipc     %d draws/frame, %d textures + %d cbs/draw: client %.2f us/draw (encode), server %.2f us/draw (replay), end to end %.2f us/draw = %.1f ms/frame\n",
        dpf,tpd,cbpd,csum/n/dpf,R->server_us/R->server_draws,esum/n/dpf,esum/n/1000);
    return 0; }

// ---------------- server (arm64) ----------------
static GLuint sh(GLenum t,const char*src){ GLuint s=glCreateShader(t); glShaderSource(s,1,&src,0); glCompileShader(s); return s; }
static GLuint prog(int variant){
    char fs[2000]; snprintf(fs,sizeof fs,
        "#version 410 core\nuniform sampler2D t0,t1,t2,t3,t4,t5,t6,t7;\nlayout(std140) uniform CB{ vec4 c[16]; };\nin vec2 uv; out vec4 o;\n"
        "void main(){ o=texture(t0,uv)+texture(t1,uv)*c[0]+texture(t2,uv)*c[1]+texture(t3,uv)*c[2]+texture(t4,uv)+texture(t5,uv)+texture(t6,uv)+texture(t7,uv)*%d.0; }\n",variant+1);
    const char*vs="#version 410 core\nlayout(location=0) in vec3 p; layout(location=1) in vec2 t; layout(std140) uniform CB{ vec4 c[16]; }; out vec2 uv;\n"
        "void main(){ uv=t; gl_Position=vec4(p*0.1+c[3].xyz*0.0,1.0); }\n";
    GLuint pr=glCreateProgram(); glAttachShader(pr,sh(GL_VERTEX_SHADER,vs)); glAttachShader(pr,sh(GL_FRAGMENT_SHADER,fs)); glLinkProgram(pr);
    glUseProgram(pr); for(int i=0;i<8;i++){ char nm[4]={'t',(char)('0'+i),0}; glUniform1i(glGetUniformLocation(pr,nm),i); }
    glUniformBlockBinding(pr,glGetUniformBlockIndex(pr,"CB"),0); return pr; }
static int server(const char*shm){
    int fd=shm_open(shm,O_RDWR,0600); if(fd<0){ perror("server shm"); return 1; }
    R=mmap(0,sizeof(Ring),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd);
    CGLPixelFormatAttribute at[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAAccelerated,kCGLPFAColorSize,24,kCGLPFADepthSize,24,0};
    CGLPixelFormatObj pf; GLint np=0; CGLChoosePixelFormat(at,&pf,&np); CGLContextObj ctx; if(CGLCreateContext(pf,0,&ctx)) return 1; CGLSetCurrentContext(ctx);
    GLuint fbo, col, dep; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&col); glBindTexture(GL_TEXTURE_2D,col); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1920,1080,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,col,0);
    glGenTextures(1,&dep); glBindTexture(GL_TEXTURE_2D,dep); glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT32F,1920,1080,0,GL_DEPTH_COMPONENT,GL_FLOAT,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,dep,0); glViewport(0,0,1920,1080);
    enum { NT=64, NS=8, NP=8 }; GLuint tex[NT], smp[NS], pr[NP]; unsigned char*px=malloc(256*256*4); for(int i=0;i<256*256*4;i++) px[i]=(unsigned char)(i*7);
    glGenTextures(NT,tex); for(int i=0;i<NT;i++){ glBindTexture(GL_TEXTURE_2D,tex[i]); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,px); glGenerateMipmap(GL_TEXTURE_2D); }
    glGenSamplers(NS,smp); for(int i=0;i<NS;i++){ glSamplerParameteri(smp[i],GL_TEXTURE_MIN_FILTER,i&1?GL_LINEAR_MIPMAP_LINEAR:GL_LINEAR); glSamplerParameteri(smp[i],GL_TEXTURE_WRAP_S,i&2?GL_CLAMP_TO_EDGE:GL_REPEAT); }
    for(int i=0;i<NP;i++) pr[i]=prog(i);
    float vtx[64*3*5]; for(int i=0;i<64*3*5;i++) vtx[i]=(float)((i*37)%17)/17.0f-0.5f; unsigned short idx[64*3]; for(int i=0;i<64*3;i++) idx[i]=(unsigned short)i;
    GLuint vao, vb, ib, ub; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vb); glBindBuffer(GL_ARRAY_BUFFER,vb); glBufferData(GL_ARRAY_BUFFER,sizeof vtx,vtx,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,20,0); glEnableVertexAttribArray(0); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,20,(void*)12); glEnableVertexAttribArray(1);
    glGenBuffers(1,&ib); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ib); glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof idx,idx,GL_STATIC_DRAW);
    const int UBSZ=4<<20; glGenBuffers(1,&ub); glBindBuffer(GL_UNIFORM_BUFFER,ub); glBufferData(GL_UNIFORM_BUFFER,UBSZ,0,GL_DYNAMIC_DRAW);
    int upo=0; uint64_t tail=0; double fstart=0; uint64_t draws=0;
    __atomic_store_n(&R->ready,1,__ATOMIC_RELEASE);
    for(;;){
        uint64_t head=__atomic_load_n(&R->head,__ATOMIC_ACQUIRE);
        if(tail==head){ if(R->quit) break; continue; }
        while(tail<head){ uint64_t pos=tail%RING_WORDS; uint32_t*p=&R->w[pos]; uint32_t op=p[0]; int n=1;
            switch(op){
            case 0: n=(int)(RING_WORDS-pos); break;   // wrap filler
            case OP_BEGIN: fstart=now_us(); glBindFramebuffer(GL_FRAMEBUFFER,fbo); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST); break;
            case OP_PROG: glUseProgram(pr[p[1]%NP]); n=2; break;
            case OP_TEX: glActiveTexture(GL_TEXTURE0+p[1]); glBindTexture(GL_TEXTURE_2D,tex[p[2]%NT]); glBindSampler(p[1],smp[p[3]%NS]); n=4; break;
            case OP_UBO: { if(upo+256>UBSZ) upo=0; void*m=glMapBufferRange(GL_UNIFORM_BUFFER,upo,256,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT|GL_MAP_UNSYNCHRONIZED_BIT);
                if(m){ memcpy(m,p+2,256); glUnmapBuffer(GL_UNIFORM_BUFFER); } glBindBufferRange(GL_UNIFORM_BUFFER,p[1]?p[1]+1:0,ub,upo,256); upo+=256; n=66; break; }
            case OP_ABLE: if(p[1]) glEnable(GL_BLEND); else glDisable(GL_BLEND); glDepthMask((GLboolean)p[2]); n=3; break;
            case OP_DRAW: glDrawElements(GL_TRIANGLES,(GLsizei)p[1],GL_UNSIGNED_SHORT,0); draws++; n=2; break;
            case OP_END: glFlush(); { double t=now_us()-fstart; if(R->frames_done>=5){ R->server_us+=t; R->server_draws+=draws; } } draws=0;
                __atomic_store_n(&R->tail,tail+1,__ATOMIC_RELEASE); __atomic_add_fetch(&R->frames_done,1,__ATOMIC_ACQ_REL); break;
            }
            tail+=(uint64_t)n; if((tail&255)<(uint64_t)n || op==OP_END) __atomic_store_n(&R->tail,tail,__ATOMIC_RELEASE); }
        __atomic_store_n(&R->tail,tail,__ATOMIC_RELEASE);
    }
    glFinish(); return 0; }

int main(int argc,char**argv){
    if(argc>2 && !strcmp(argv[1],"--server")) return server(argv[2]);
    const char*a64=getenv("GLIPC_SERVER"); if(!a64){ fprintf(stderr,"set GLIPC_SERVER to the arm64 build\n"); return 1; }
    return client(argc,argv,a64); }
