# Bot benchmark (big-game render load, repeatable)

`bash shim/tools/bench.sh [bots=60] [runs=1] ["<extra env>"]`, e.g.

```bash
BENCH_MAP=valhalla bash shim/tools/bench.sh 60 3                         # 3 runs of the current build
BENCH_MAP=valhalla PELOAD_GL_HELPER=0 bash shim/tools/bench.sh 60 1      # A/B against a switch
```

The script boots the client with the test profile (`C:\ReclaimerProfileTest`, input-less, visible window), hosts a
LOCAL Custom Slayer from the menu (`PELOAD_AUTOCLICK`; `BENCH_MAP` types the map's name into the map picker's search
box), and sets `PELOAD_BENCH_BOTS`. Each run prints one line and appends a JSON line to `/tmp/reclaimer_bench.jsonl`
(`BENCH_OUT`); the log is `/tmp/reclaimer_bench.log`. A run takes about 2.5 minutes. It refuses to start while another
game process runs (like quiet-run.sh).

What the loader does (`shim/src/bench.c`), all on the engine main thread at `main_loop_body` inside halo3's game-state
lock, host only (simulation type 5 or 1):

1. Camera spot: of every weapon / equipment item lying on the map (all on real floor, the same set every load) and 36
   headings, the one with the most open points on a 7 x 5 grid ahead (6-30 units, +-6 sideways; "open" =
   `scenario_location_from_point` finds a cluster at knee and head height). The local player is teleported there.
2. Bots: n bipeds with the local player's own biped tag, standing on that grid (several per point in rings), facing the
   camera, each given starting profile 0 (the map's loadout: an assault rifle on Valhalla). They are not players: no
   AI, no network traffic, no nameplates; they stand and idle-animate.
3. After `PELOAD_BENCH_SETTLE` s (8; bots land, first-use shaders build) the frame times of `PELOAD_BENCH_SECS` s (30)
   are measured: fps mean / median / p10 / p1 / min, draws per frame, frame-time percentiles, and CPU time per frame
   of the thread that issues the draws, of the whole loader process, and of the native GL helper (the helper writes
   its process CPU time to `GlpShared.cpu_ns` at every publish).

Same map + same n + same build = the same scene: draws per frame agree to +-2 across runs. **Read the CPU columns
first**: wall-clock fps moves with whatever else the Mac is doing (26-39 fps for identical runs with Chrome busy, load
average ~15; 41.3 / 41.3 on a quiet machine), the helper's CPU per frame much less (24.6-26.4 ms busy, 24.56 / 24.64
quiet).

## Results (M1 Max, 2560x1440, Valhalla, 60 bots, 2026-10-17)

| build | fps mean | p10 | draws/frame | CPU ms/frame: draw thread / process / helper |
|---|---|---|---|---|
| GL helper (default) | 41.3 | 36.6 | 5,084 | 17.2 / 74.6 / 24.6 |
| `PELOAD_GL_HELPER=0` | 14.9 | 14.1 | 5,011 | 66.2 / 213.2 / - |

The helper is 100% busy: in this scene the frame rate is set by the helper's GL work, ~4.8 us of native driver time
per draw. The loader's side (D3D -> GL translation and encoding) is 17.2 ms, ~3.4 us per draw. Next steps that this
loop can measure: drop redundant GL state calls before they are encoded (the state cache was A/B neutral in process,
where the loader's own translation dominated; with the helper the driver work is the limit), fewer GL calls per draw.

## Optimizing with it (2026-10-18)

Measure a change with `PELOAD_AB=flag:<secs>` inside one run: d3dsubmit.c flips `g_ab_flag` (loader) and
`GlpShared.ab_off` (helper) every `<secs>` and logs `[ab]` lines with fps, draws and the helper's CPU ms per frame for
each phase (the first phase is a warm-up and is not counted). On a busy Mac this is the only reliable comparison;
`BENCH_WAIT=2` doubles the menu waits when the machine is slow. `PELOAD_GL_HELPER_OPS=1` shows where the helper's time
goes, per GL call.

| change | helper CPU ms/frame (on / off) | fps (on / off) | phases |
|---|---|---|---|
| sRGB decode set only when it changes (loader) | 27.4 / 29.2 (-7.4%) | +7.1% | 9 / 8 |
| Metal pixel-format cache (`peload-gl-pf.dylib`) | 24.2 / 27.3 (-10.7%) | 39.0 / 34.0 (+12.2%) | 8 / 9 |
| constant buffers refilled with the same bytes reuse their ring copy (loader, `cb_reuse`) | 18.9 / 24.4 (-22.5%) | 54.4 / 41.6 (+30.8%) | 9 / 8 |

Tried, not worth it: the loader's GL state cache under the helper (-1.4%, noise); remapping texture units
(`PELOAD_TEXSLOT_CENSUS`: of ~12,000 texture binds a frame only 17% rebind a slot's previous texture; the rest are real
texture changes between draws).

The constant-buffer finding: Halo 3 maps and refills most of its cbuffers for every draw; 78% of the uploads (73% of the
bytes) equalled that buffer's previous upload and another 20% an upload of a different buffer shortly before. The ring is
append-only within a generation, so an unchanged buffer now points at its old copy (exact `memcmp` against the bytes it
last uploaded; `PELOAD_D3DGL_NOCBDEDUP` = always upload). Map/unmap uploads went from 8.9 to ~3 MB a frame. Captures
with it on and off are identical; no GL errors with `PELOAD_GL_HELPER_CHECK`.

Before the constant-buffer change, the helper's time went to (`PELOAD_GL_HELPER_OPS`, per frame): glDrawElements 3,566 x 3.5-4.2 µs ~52%,
glUnmapBuffer 5,200 x 0.4 µs ~8%, glDrawArrays 900 x 2 µs 7%, glBindBuffer 39,650 x 0.04 µs 6%, glClear 21 x 55-60 µs
(render-pass ends) 5%, glBindBufferRange 19,800 4%, glBlitFramebuffer 3 x 240-310 µs 3%.

For reference, real games: 60-player BTB ~2,000-3,000 draws/frame in view, 100-player Infection ~3,000 (PERF-PLAN.md).
This view (60 armed Spartans in the open) is heavier than an average BTB frame, closer to a big fight.

## halo3.dll functions used (static RE, capstone, 2026-10-17)

| RVA | function | evidence |
|---|---|---|
| `0x341508` | `object_placement_data_new(data, tag, owner_object, r9)` | memsets 0x180 bytes; tag at +0, forward +0x28 and up +0x34 from globals, scale 1.0 at +0x58, owner fields from the owner object (+0x178/+0x174 for units), r9 (12 bytes, NULL ok) to +0x68. Position at +0x1C (zeroed; the Halo Online layout). Called right before `object_new` at 36 of 47 call sites |
| `0x341ae8` | `object_new(data)` -> handle (-1 on failure) | the scenario path `0x34b294` builds placement data with `0x37867c` and calls it, then the post-create hook `0x37a3f4` |
| `0x357ca8` | `unit_add_equipment(unit, starting_profile, reset, primary)` | the `unit_add_equipment` script function's evaluate `0x1e65c8`; reads scenario (`*(halo3+0xa46a08)`) starting profiles: count +0x240, 0x58-byte entries, **no bounds check** (bench.c checks the count) |
| `0x258cf8` / `0x258548` | `object_create_anew(name)` / create by scenario name | `object_create_anew` evaluate `0x1e2b58`; `0x342ed8` object_delete first |

Teleport `0x259410`, `scenario_location_from_point` `0x9bc78` and the lock `0x12c948` / `0x12c9a0`: shim/docs/teleport.md.
bench.c checks the prologue bytes of `0x341508` (including its `mov r8d, 0x180`) and `0x259410` and does nothing on a
different build.
