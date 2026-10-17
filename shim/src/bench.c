// bench.c -- repeatable big-game render benchmark (included by peload.c after hitbox.c).
// PELOAD_BENCH_BOTS=<n>: in a match THIS client hosts, n bipeds (the local player's own biped, with the map's
// starting loadout) are spawned around the weapons and equipment lying on the map, the local player is moved to a
// fixed spot (the map item nearest the items' centre, facing the farthest item), and the frame rate is measured over
// a fixed window. Same map + same n = the same scene every run, which live games never give. Prints one line:
//   [bench] RESULT bots=<spawned>/<asked> fps mean .. median .. p10 .. p1 .. min .. | draws/frame .. | ...
// and appends it as JSON to PELOAD_BENCH_OUT (if set). shim/tools/bench.sh drives it.
//   PELOAD_BENCH_DELAY  s after "gameplay presentation ready" before spawning (default 4)
//   PELOAD_BENCH_SETTLE s after spawning before measuring (default 8: bots fall to the floor, first-use shaders)
//   PELOAD_BENCH_SECS   measured seconds (default 30)
// Engine calls (halo3.dll, static RE 2026-10-17, shim/docs/bench.md): object_placement_data_new 0x341508 (data, tag,
// owner object, r9 NULL), object_new 0x341ae8 (data) -> handle, unit_add_equipment 0x357ca8 (unit, starting profile,
// reset, primary), teleport 0x259410, scenario_location_from_point 0x9bc78, game-state lock 0x12c948 / 0x12c9a0.
static volatile int g_bench_pending;       // (declared in peload.c too) the engine job is due (set at Present, run at main_loop_body)
static volatile int g_bench_state;         // 0 waiting for gameplay, 1 spawn requested, 2 spawned, 3 measuring, 4 done
static volatile int g_bench_spawned, g_bench_asked;
static double g_bench_t;                   // time of the last state change
typedef void     (__attribute__((ms_abi)) *BenchPlaceFn)(void*,uint32_t,uint32_t,void*);
typedef uint32_t (__attribute__((ms_abi)) *BenchNewFn)(void*);
typedef void     (__attribute__((ms_abi)) *BenchEquipFn)(uint32_t,int16_t,uint8_t,uint8_t);
typedef void     (__attribute__((ms_abi)) *BenchTpFn)(uint32_t,const float*,const float*,const float*,uint8_t);
typedef void     (__attribute__((ms_abi)) *BenchLocFn)(uint16_t*,const float*);
static int bench_inside(uint8_t*h3,const float*p){ BenchLocFn loc=(BenchLocFn)(h3+0x9bc78); uint16_t a=0xFFFF,b=0xFFFF;
    float p1[3]={p[0],p[1],p[2]+0.3f}, p2[3]={p[0],p[1],p[2]+1.2f}; loc(&a,p1); loc(&b,p2); return a!=0xFFFF && b!=0xFFFF; }
static int bench_cmp_pos(const void*a,const void*b){ const float*x=a,*y=b; if(x[0]!=y[0]) return x[0]<y[0]?-1:1; if(x[1]!=y[1]) return x[1]<y[1]?-1:1; return x[2]<y[2]?-1:x[2]>y[2]; }
static void bench_engine_job(void){
    if(!__atomic_exchange_n(&g_bench_pending,0,__ATOMIC_ACQ_REL)) return;
    char m[300]; int k; uint8_t*h3=hb_halo3_base(); if(!h3) return; uint32_t idx=*(volatile uint32_t*)(h3+0xa39f9c); if(idx>=128) return;
    void**tarr; __asm__ volatile("movq %%gs:0x58,%0":"=r"(tarr)); if(!tarr || !tarr[idx]) return;
    uint8_t*tls=(uint8_t*)tarr[idx]; uint8_t*arr=*(uint8_t**)(tls+0x38); if(!arr || *(uint32_t*)(arr+0x2c)!=0x64407440 || memcmp(arr,"object",7)) return;
    // the functions must be the ones the RE found (the same build), else do nothing
    static const uint8_t pro_place[8]={0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C}, pro_tp[8]={0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C};
    if(memcmp(h3+0x341508,pro_place,8) || memcmp(h3+0x259410,pro_tp,8) || h3[0x341523]!=0x41 || *(uint32_t*)(h3+0x341525)!=0x180){
        (void)!write(2,"  [bench] halo3 object functions not recognised: no bots\n",57); g_bench_state=4; return; }
    uint8_t*gg=*(uint8_t**)(tls+0x48); int st=gg?gg[0x11]:-1;
    if(st!=5 && st!=1){ k=snprintf(m,sizeof m,"  [bench] this client does not host the game (simulation type %d): no bots\n",st); (void)!write(2,m,k); g_bench_state=4; return; }
    if(!*(uint64_t*)(h3+0x8f4028) || *(int32_t*)(tls+0x18)<0 || *(int32_t*)(tls+0x18)>=32){ g_bench_pending=1; return; }   // not ready: next frame
    int tok=((HbLockFn)(h3+0x12c948))();
    uint8_t*data=*(uint8_t**)(arr+0x48); int hi=*(int32_t*)(arr+0x3c);
    // the local player's body (players table via the output-user mapping, as hb_engine_jobs_locked)
    uint32_t me=0xFFFFFFFFu; uint8_t*mp=0;
    { uint8_t*pm=*(uint8_t**)(tls+0x110), *parr=*(uint8_t**)(tls+0x10);
      if(pm && parr && *(uint32_t*)(parr+0x2c)==0x64407440){ int32_t ph=*(int32_t*)(pm+0xB8), es=*(int32_t*)(parr+0x24), pn=*(int32_t*)(parr+0x3c); uint8_t*pd=*(uint8_t**)(parr+0x48);
          if(ph!=-1 && pd && (ph&0xFFFF)<pn) me=*(uint32_t*)(pd+(size_t)(ph&0xFFFF)*es+0x28); }
      if(me!=0xFFFFFFFFu && data && (int)(me&0xFFFF)<hi){ uint8_t*e=data+(size_t)(me&0xFFFF)*0x18; if(e[3]==0 && *(uint16_t*)e==(uint16_t)(me>>16)) mp=*(uint8_t**)(e+0x10); } }
    if(!mp || *(uint32_t*)(mp+0x10)!=0xFFFFFFFFu){ ((HbUnlockFn)(h3+0x12c9a0))(&tok); g_bench_pending=1; return; }   // no body yet / in a vehicle: retry
    uint32_t tag=*(uint32_t*)mp;
    // the map items (weapons, equipment lying free): on real floor, the same set every load
    enum { NIT=256 }; static float it[NIT][3]; int ni=0; double cx=0,cy=0;
    for(int i=0;i<hi && ni<NIT;i++){ uint8_t*e=data+(size_t)i*0x18; if(!*(uint16_t*)e || !(e[3]==2||e[3]==3)) continue; uint8_t*p=*(uint8_t**)(e+0x10); if(!p || *(uint32_t*)(p+0x10)!=0xFFFFFFFFu) continue;
        memcpy(it[ni],p+0x50,12); cx+=it[ni][0]; cy+=it[ni][1]; ni++; }
    if(ni<2){ ((HbUnlockFn)(h3+0x12c9a0))(&tok); (void)!write(2,"  [bench] too few map items to place bots\n",42); g_bench_state=4; return; }
    qsort(it,(size_t)ni,sizeof it[0],bench_cmp_pos); cx/=ni; cy/=ni;
    // camera spot + heading: of every map item (as the spot) and 36 headings, the one with the most open points on a
    // grid ahead (6..30 units, +-6 sideways, in map space at knee and head height) -- the bots stand on that grid
    enum { GD=7, GL=5, NG=GD*GL };
    float cam[3]={0}, cf[3]={1,0,0}, up[3]={0,0,1}; int best=-1;
    for(int c=0;c<ni;c++){ float ccz=it[c][2]+0.5f;
        for(int hd=0;hd<36;hd++){ float an=(float)(hd*10*M_PI/180), hx=cosf(an), hy=sinf(an); int cnt=0;
            for(int g=0;g<NG;g++){ float d=6.0f+4.0f*(float)(g/GL), lt=-6.0f+3.0f*(float)(g%GL);
                float p[3]={it[c][0]+hx*d-hy*lt, it[c][1]+hy*d+hx*lt, ccz}; cnt+=bench_inside(h3,p); }
            if(cnt>best){ best=cnt; cam[0]=it[c][0]; cam[1]=it[c][1]; cam[2]=ccz; cf[0]=hx; cf[1]=hy; } } }
    static float gp[NG][3]; int nc=0;
    for(int g=0;g<NG;g++){ float d=6.0f+4.0f*(float)(g/GL), lt=-6.0f+3.0f*(float)(g%GL);
        float p[3]={cam[0]+cf[0]*d-cf[1]*lt, cam[1]+cf[1]*d+cf[0]*lt, cam[2]}; if(bench_inside(h3,p)){ memcpy(gp[nc],p,12); nc++; } }
    if(!nc){ ((HbUnlockFn)(h3+0x12c9a0))(&tok); (void)!write(2,"  [bench] no open space found for the bots\n",43); g_bench_state=4; return; }
    ((BenchTpFn)(h3+0x259410))(me,cam,cf,up,1);
    // starting profiles (scenario +0x240 count, 0x58-byte entries): unit_add_equipment reads entry 0 unchecked
    uint8_t*scn=*(uint8_t**)(h3+0xa46a08); int nprof=scn?*(int32_t*)(scn+0x240):0;
    int n=g_bench_asked, made=0, skipped=0; uint8_t pd[0x200];
    for(int b=0;b<n;b++){
        const float*base=gp[b%nc]; int ring=b/nc; float pos[3]; int ok=0;   // several bots per grid point: a ring around it
        for(int t=0;t<12 && !ok;t++){ float a=(float)(ring*2.39996 + t*0.5236), r=ring?0.9f+0.35f*(float)(t/6):0.35f*(float)(t/6);
            pos[0]=base[0]+r*cosf(a); pos[1]=base[1]+r*sinf(a); pos[2]=base[2];
            float dx=pos[0]-cam[0], dy=pos[1]-cam[1]; if(dx*dx+dy*dy<4.0f) continue;   // keep the camera's spot clear
            ok=bench_inside(h3,pos); }
        if(!ok){ skipped++; continue; }
        float fw[3]={cam[0]-pos[0],cam[1]-pos[1],0}, l=sqrtf(fw[0]*fw[0]+fw[1]*fw[1]); if(l>1e-4f){ fw[0]/=l; fw[1]/=l; } else { fw[0]=1; fw[1]=0; }
        memset(pd,0,sizeof pd); ((BenchPlaceFn)(h3+0x341508))(pd,tag,0xFFFFFFFFu,0);
        memcpy(pd+0x1C,pos,12); memcpy(pd+0x28,fw,12); memcpy(pd+0x34,up,12);
        uint32_t h=((BenchNewFn)(h3+0x341ae8))(pd); if(h==0xFFFFFFFFu){ skipped++; continue; }
        if(nprof>0) ((BenchEquipFn)(h3+0x357ca8))(h,0,1,1);
        made++; }
    ((HbUnlockFn)(h3+0x12c9a0))(&tok);
    g_bench_spawned=made; g_bench_t=mono_s(); __atomic_store_n(&g_bench_state,2,__ATOMIC_RELEASE);
    k=snprintf(m,sizeof m,"  [bench] spawned %d of %d bots (biped %s, %d skipped) on %d open grid points (%d map items); camera at (%.2f %.2f %.2f) facing (%.2f %.2f); %d starting profiles\n",
        made,n,hb_tag_name(tag),skipped,nc,ni,cam[0],cam[1],cam[2],cf[0],cf[1],nprof); (void)!write(2,m,k); }
#include <mach/thread_act.h>
static double bench_thread_cpu(mach_port_t t){ if(!t) return 0; thread_basic_info_data_t bi; mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;
    if(thread_info(t,THREAD_BASIC_INFO,(thread_info_t)&bi,&n)!=KERN_SUCCESS) return 0;
    return bi.user_time.seconds+bi.user_time.microseconds*1e-6+bi.system_time.seconds+bi.system_time.microseconds*1e-6; }
static int bench_cmp_d(const void*a,const void*b){ double x=*(const double*)a, y=*(const double*)b; return x<y?-1:x>y; }
// render thread, every engine Present
static void bench_tick(void){
    static int on=-1; if(on<0){ const char*e=getenv("PELOAD_BENCH_BOTS"); on=e?1:0; if(on) g_bench_asked=atoi(e)<0?0:atoi(e)>500?500:atoi(e); } if(!on) return;
    extern double g_gameplay_t0; double now=mono_s(); int s=__atomic_load_n(&g_bench_state,__ATOMIC_ACQUIRE);
    static double settle=-1, secs, delay; if(settle<0){ const char*e; delay=(e=getenv("PELOAD_BENCH_DELAY"))?atof(e):4; settle=(e=getenv("PELOAD_BENCH_SETTLE"))?atof(e):8; secs=(e=getenv("PELOAD_BENCH_SECS"))?atof(e):30; if(secs<2) secs=2; }
    if(s==0){ if(g_gameplay_t0 && now-g_gameplay_t0>delay){ g_bench_state=1; g_bench_t=now; __atomic_store_n(&g_bench_pending,1,__ATOMIC_RELEASE); } return; }
    if(s==1){ if(now-g_bench_t>20){ (void)!write(2,"  [bench] the spawn job never ran (no main_loop_body hook?)\n",60); g_bench_state=4; } return; }
    static double *ft; static size_t nft, cap; static double last; static unsigned long d0; static double b0,i0;
    static double c_thr0, c_proc0, c_help0; struct timespec ct;   // CPU time: this (render) thread, this process, the helper
    #define BENCH_CPU(id) (clock_gettime(id,&ct),ct.tv_sec+ct.tv_nsec*1e-9)
    static mach_port_t dport;
    if(s==2){ if(now-g_bench_t<settle) return; g_bench_state=3; g_bench_t=now; last=now; nft=0; d0=g_draws_total; if(g_glp){ b0=g_glp->busy_s; i0=g_glp->idle_s; c_help0=g_glp->cpu_ns*1e-9; }
        dport=g_draw_pthread?pthread_mach_thread_np(g_draw_pthread):0; c_thr0=bench_thread_cpu(dport); c_proc0=BENCH_CPU(CLOCK_PROCESS_CPUTIME_ID); return; }
    if(s!=3) return;
    if(nft==cap){ cap=cap?cap*2:8192; ft=realloc(ft,cap*sizeof *ft); if(!ft){ g_bench_state=4; return; } }
    ft[nft++]=now-last; last=now;
    if(now-g_bench_t<secs) return;
    g_bench_state=4; double tot=now-g_bench_t; unsigned long dr=g_draws_total-d0;
    double cthr=(bench_thread_cpu(dport)-c_thr0)*1000/nft, cproc=(BENCH_CPU(CLOCK_PROCESS_CPUTIME_ID)-c_proc0)*1000/nft;
    double chelp=(g_glp && g_glp->cpu_ns && c_help0>0)?(g_glp->cpu_ns*1e-9-c_help0)*1000/nft:-1;
    double*so=malloc(nft*sizeof *so); if(!so) return; memcpy(so,ft,nft*sizeof *so); qsort(so,nft,sizeof *so,bench_cmp_d);
    double med=so[nft/2], p90=so[(size_t)(nft*0.90)], p99=so[(size_t)(nft*0.99)], mx=so[nft-1]; free(so);
    double hb=-1; if(g_glp){ double b=g_glp->busy_s-b0, i=g_glp->idle_s-i0; if(b+i>0) hb=100*b/(b+i); }
    char gl[40]; if(g_glp_on) snprintf(gl,sizeof gl,"GL helper busy %.0f%%",hb); else snprintf(gl,sizeof gl,"GL in process");
    char m[600]; int k=snprintf(m,sizeof m,"  [bench] RESULT bots=%d/%d fps mean %.1f median %.1f p10 %.1f p1 %.1f min %.1f | draws/frame %.0f | frame ms mean %.2f p90 %.2f p99 %.2f | CPU ms/frame draw thread %.2f process %.2f helper %.2f | %s | %.0f s, %zu frames\n",
        g_bench_spawned,g_bench_asked,nft/tot,1/med,1/p90,1/p99,1/mx,(double)dr/nft,tot*1000/nft,p90*1000,p99*1000,cthr,cproc,chelp,gl,tot,nft); (void)!write(2,m,k);
    const char*out=getenv("PELOAD_BENCH_OUT"); if(out&&*out){ FILE*f=fopen(out,"a"); if(f){
        fprintf(f,"{\"bots\":%d,\"asked\":%d,\"fps_mean\":%.2f,\"fps_median\":%.2f,\"fps_p10\":%.2f,\"fps_p1\":%.2f,\"fps_min\":%.2f,\"draws_per_frame\":%.1f,\"frame_ms_mean\":%.3f,\"frame_ms_p90\":%.3f,\"frame_ms_p99\":%.3f,\"cpu_ms_draw_thread\":%.3f,\"cpu_ms_process\":%.3f,\"cpu_ms_helper\":%.3f,\"gl_helper\":%d,\"helper_busy_pct\":%.1f,\"secs\":%.1f,\"frames\":%zu}\n",
            g_bench_spawned,g_bench_asked,nft/tot,1/med,1/p90,1/p99,1/mx,(double)dr/nft,tot*1000/nft,p90*1000,p99*1000,cthr,cproc,chelp,g_glp_on,hb,tot,nft); fclose(f); } } }
