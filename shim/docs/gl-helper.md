# Native GL helper (PERF-PLAN option 1)

The D3D11→GL renderer's OpenGL calls run in a native arm64 process (`shim/build/peload-gl`) instead of
Apple's GL driver translated by Rosetta inside the loader. `PELOAD_GL_HELPER=1` turns it on; without it
nothing changes.

## Why (measured, `shim/tools/glipc_bench.sh`, M1 Max, 3,000 draws × 6 textures + 2 constant buffers)

| | µs per draw |
|---|---|
| in process, x86_64 under Rosetta (the loader today) | 5.8–6.0 |
| native arm64, in process | 3.5 |
| ring + native helper, end to end | 3.3 |
| of which the client (Rosetta) pays for encoding | 0.04 |

Encoding is plain stores, which Rosetta translates to near-native code; only the driver's own code is slow
translated. The render thread keeps only the loader's D3D→GL translation, and the driver runs in parallel
in the helper.

Cross-process, cross-architecture IOSurface sharing works (global surface, `IOSurfaceLookup(id)` from an
x86_64 GL context reads what an arm64 GL context rendered; tested 2026-10-17 on macOS 26).

## Design

**What moves.** Only the renderer's own context (`g_d3dgl_cgl`, the render thread and the GL submission
thread). The window's `NSOpenGLContext` (present thread, hitbox overlay) and the client's WGL contexts stay
in the loader and call GL directly.

**Routing.** Every `G.*` entry and every GL function the loader fetches with `dlsym` goes through a
dispatcher: `t_glp` (thread-local, set when the thread makes the render context current) → encode into the
ring; otherwise → the real function. `CGLSetCurrentContext` / `CGLGetCurrentContext` / `CGLLockContext`
are wrapped so the render context is a sentinel handle in the loader.

**Ring.** One single-producer ring of 4-byte words in shared memory (`shm_open`), opcode + arguments +
inline payload. GL allows one thread per context at a time, so the thread that made the render context
current is the only producer; ownership moves at make-current. Commands never wrap; a filler word skips to
the start. Payloads too big for the ring are staged in pieces (`OP_STAGE`) and then referenced.

**Object names.** `Gen*` / `Create*` names are allocated in the loader (one counter per GL namespace;
shaders and programs share one); the helper maps them to its real names. No round trip.

**Calls with results.**
- Hot path, no round trip:
  - `MapBufferRange(WRITE…)` returns loader-side staging memory; `Unmap` sends the bytes
    (the constant/vertex ring `d3dgl_ring_put`, ~2.6 per draw).
  - `GetError` returns `GL_NO_ERROR`; the helper checks errors itself and logs them with the opcode.
  - Occlusion queries: `BeginQuery` bumps a loader-side generation per query, the helper writes
    `{generation, value}` into a shared table when the result is available; `GetQueryObject*` reads the
    table (`RESULT_AVAILABLE` is false until then, as on real GL).
- Round trips (flush the ring, wait for the helper to reach the request, read the reply): shader
  compile/link status and logs, uniform locations, `ReadPixels` into memory, `Map(READ)`,
  `Get{Integer,Float,Boolean}v`, `GetString`, `Finish`, `ClientWaitSync`. These happen at load/link
  time, for captures and debugging, never per frame.

**Uploads.** `BufferData`/`BufferSubData`/`TexImage*`/`TexSubImage*`/`CompressedTexImage*` copy their
data into the ring. Image sizes come from width/height/depth, format/type and the unpack state, which the
loader tracks (`PixelStorei`); with a pixel-unpack buffer bound the pointer is an offset and nothing is
copied.

**Presentation.** The present queue's frame slots (`pq_publish`) become IOSurfaces created by the
helper (global, IDs in shared memory). The helper blits the back buffer into the slot, inserts a fence and
keeps going; when the fence has signalled it marks the slot ready and writes a byte to a pipe. The
loader's present thread waits on the pipe, wraps the slot's IOSurface as a `GL_TEXTURE_RECTANGLE` in the
window context and draws it as today (letterbox, overlay).

**Lifetime.** The loader spawns the helper when the renderer initialises; the helper exits when its parent
does (kqueue `NOTE_EXIT`). If the helper fails to start, the loader logs it and renders in process.

## Files

- `shim/src/glproxy.spec` — every proxied GL function: return type, arguments, and which arguments are
  object names / payloads / outputs.
- `shim/tools/gen_glproxy.py` — generates `shim/src/glproxy_gen.h` (loader encoders + dispatch) and
  `shim/src/glhelper_gen.h` (helper decoders). The generated files are committed; `build.sh` needs
  only clang.
- `shim/src/glproto.h` — opcodes and the shared-memory layout, used by both sides.
- `shim/src/glproxy.c` — loader side (included into peload.c): spawn, ring, round trips, maps, queries,
  name allocation, present slots.
- `shim/src/glhelper.c` — the helper process (arm64).
- `shim/src/glhelper_pf.c` → `shim/build/peload-gl-pf.dylib` — caches Apple's driver's Metal pixel-format queries
  (below). `peload-gl` links it weakly: the helper runs without it, just slower.

## Driver costs worked around

- **Pixel-format queries.** Whenever texture or sampler state is dirty, Apple's driver (`AppleMetalOpenGLRenderer`,
  `GLDContextRec::updateTextureState`) checks every sampler of the bound program with `GLDTextureRec::isValidTexture`,
  which calls `MTLPixelFormatGetInfoForDevice` each time (~0.12 µs, Objective-C `supportsFamily:` calls inside; 12% of
  the helper's time with 60 bots). The answer is fixed per device and format, so `peload-gl-pf.dylib` interposes the
  function (`__DATA,__interpose`) and caches it for the helper's GL thread (13 formats in a 60-bot game, 40 M lookups
  in 100 s). dyld ignores interpose tuples in the main executable; in a dylib loaded at launch they apply to the
  driver bundle, which is not in the shared cache. `PELOAD_GL_HELPER_NOPFCACHE=1` turns it off.
- **sRGB decode re-sent** (loader side, `d3dgl_bind_res`): textures sampled without a sampler object had
  `TEXTURE_SRGB_DECODE` set at every bind (~1,600 a frame); each `TexParameteri` makes the driver re-validate the
  texture at the next draw. It is now sent only when it changes (`PELOAD_D3DGL_NODECODECACHE` = always).

## Switches

- `PELOAD_GL_HELPER=1` — on. `play.sh` sets it by default since 2026-10-17 (`PELOAD_GL_HELPER=0` = in process); other
  launchers (target.sh, quiet-run.sh, dedicated.sh) leave it off. If `peload-gl` is missing or doesn't start, the
  loader renders in process.
- `PELOAD_GL_HELPER_STATS=1` — every 2 s: commands/s, helper busy %, frames published / shown per second, GPU waits
  at publish, GL errors.
- `PELOAD_GL_HELPER_CHECK=1` — the helper calls `glGetError` after every command and logs errors with the function.
- `PELOAD_GL_HELPER_SYNCERR=1` — `glGetError` in the loader becomes a real round trip (slow; debugging only).
- `PELOAD_GL_HELPER_OPS=1` — every 5 s: the helper's decode time per frame and the top 20 GL calls (calls per frame,
  µs each, share), map/unmap upload bytes per frame by size, and the pixel-format cache's counts.
- `PELOAD_GL_HELPER_NOPFCACHE=1` — no pixel-format cache. `PELOAD_AB=flag:<secs>` also switches it (and anything
  else that reads `GlpShared.ab_off` / `g_ab_flag`) off in alternate phases.
- `PELOAD_PRESENT_STATS=1` — window frames/s and the present thread's flush / wait / draw time (both renderers).

## Status

- 2026-10-17: feasibility measured, IOSurface sharing tested, design written.
- 2026-10-17: working, opt-in. Tested on an M1 Max at 2560x1440:
  - direct Guardian match: 103-108 fps at 1,230-1,450 draws/frame (6.5-7.6 µs/draw), vs 74-84 fps at ~1,225 draws
    in process (11.0 µs/draw); every frame reaches the window; helper 60-70% busy; no GL errors with CHECK on;
  - Reclaimer's menu (its UI composited into the frame), Host Game -> match, Settings resolution change
    (ResizeBuffers -> new back buffer and present slots), F8 capture (ReadPixels round trip), the GL submission
    thread (`PELOAD_D3DGL_SUBMIT=1`): all work.
- 2026-10-18: 60-bot benchmark (shim/docs/bench.md), same-run A/B: the sRGB decode cache -7.4% helper CPU per frame
  (+7% fps), the pixel-format cache -10.7% (+12% fps); no GL errors with CHECK on.
  Loader side, constant buffers refilled with unchanged bytes reuse their ring copy: -22.5% (+31% fps).
- Known limits:
  - F9 "visible only" reads its depth buffer back with `Map(READ)` every frame, a round trip under the helper.
  - If the helper dies mid-game the loader exits (the GL state is gone).
  - Not yet tested: online games, long sessions with many map changes, 60+ player games.
