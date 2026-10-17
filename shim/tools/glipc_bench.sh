#!/bin/bash
# Feasibility of the native GL helper: glbench's draw loop in process (x86_64 under Rosetta, as the loader runs today;
# arm64 natively) and through a shared-memory ring replayed by a native arm64 process (tools/glipc_bench.c).
#     bash shim/tools/glipc_bench.sh [draws/frame] [frames] [textures/draw] [cbs/draw]
# Run it with no game running (it measures CPU time per draw).
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/glipc.$$"; mkdir -p "$OUT"
for a in x86_64 arm64; do
  clang -O2 -arch $a glbench.c -framework OpenGL -o "$OUT/glbench_$a" || exit 1
  clang -O2 -arch $a glipc_bench.c -framework OpenGL -o "$OUT/glipc_$a" || exit 1
done
for r in 1 2; do
  arch -x86_64 "$OUT/glbench_x86_64" "$@"
  arch -arm64  "$OUT/glbench_arm64"  "$@"
  GLIPC_SERVER="$OUT/glipc_arm64" arch -x86_64 "$OUT/glipc_x86_64" "$@" 2>/dev/null
done
rm -rf "$OUT"
