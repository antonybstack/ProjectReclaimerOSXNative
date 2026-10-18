// glhelper_pf.c -- shim/build/peload-gl-pf.dylib: caches Metal's pixel-format capabilities for the native GL helper.
// Apple's GL-on-Metal driver checks every texture the program samples whenever any texture or sampler binding changed
// (GLDContextRec::updateTextureState -> GLDTextureRec::isValidTexture, "can this format be filtered"), and each check asks
// Metal for the format's capabilities: MTLPixelFormatGetInfoForDevice, a table walk with Objective-C supportsFamily:
// calls, ~0.12 us. That is once per sampler on almost every draw (12% of the helper's time in a 60-bot scene). The
// answer depends only on the device and the format, so this dylib interposes the function and remembers it.
// dyld honours __interpose tuples only in dylibs loaded at launch (not in the main executable), hence a separate file:
// peload-gl links it weakly (@executable_path), so a missing dylib just means no cache.
// The result is a 0x38-byte struct returned through x8 (every caller in the driver zeroes 0x38 bytes and passes them).
// Only the helper's GL thread uses the table (glhpf_thread), so it needs no lock; other threads always ask Metal.
#include <pthread.h>
#include <stdint.h>
typedef struct { uint64_t w[7]; } MtlPfInfo;
extern MtlPfInfo MTLPixelFormatGetInfoForDevice(void*device,unsigned long format);
// set by peload-gl (glhelper.c): the GL thread and on/off (PELOAD_GL_HELPER_NOPFCACHE, PELOAD_AB=flag); counters for its stats
volatile int glhpf_on; pthread_t glhpf_thread; uint64_t glhpf_hits, glhpf_miss;
static MtlPfInfo g_pf[1024]; static uint8_t g_ok[1024]; static void* g_dev;
static MtlPfInfo pf_info(void*dev,unsigned long fmt){
    if(!glhpf_on || fmt>=1024 || !glhpf_thread || !pthread_equal(pthread_self(),glhpf_thread)) return MTLPixelFormatGetInfoForDevice(dev,fmt);
    if(dev!=g_dev){ for(int i=0;i<1024;i++) g_ok[i]=0; g_dev=dev; }
    if(g_ok[fmt]){ glhpf_hits++; return g_pf[fmt]; }
    g_pf[fmt]=MTLPixelFormatGetInfoForDevice(dev,fmt); g_ok[fmt]=1; glhpf_miss++; return g_pf[fmt]; }
__attribute__((used,section("__DATA,__interpose"))) static const struct { const void* repl; const void* orig; } g_interpose={ (const void*)pf_info, (const void*)MTLPixelFormatGetInfoForDevice };
