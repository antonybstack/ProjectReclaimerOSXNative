#!/bin/bash
# Repeatable big-game render benchmark (shim/src/bench.c, shim/docs/bench.md): boots the client with the test
# profile, hosts a local Guardian Custom Slayer from the menu, spawns N bots, measures, prints one RESULT line.
#   bash shim/tools/bench.sh [bots=60] [runs=1] ["<extra env>"]
#   e.g. bash shim/tools/bench.sh 60 3                               # 3 runs, current build
#        bash shim/tools/bench.sh 60 2 "PELOAD_GL_HELPER=0"          # A/B: GL in process
# Results: one JSON line per run appended to /tmp/reclaimer_bench.jsonl (BENCH_OUT=...), log /tmp/reclaimer_bench.log.
# BENCH_MAP=<name> (default guardian = Host Game's default) picks the map through the map picker's search box, e.g.
# valhalla, sandtrap, avalanche, "high ground". BENCH_SECS (default 30) measured seconds; BENCH_WAIT=2 doubles the menu waits. Never runs beside the user's game (same rule as quiet-run.sh).
# Needs the client exe and the MS CRT: from this tree, else from ~/ReclaimerMac (BENCH_EXE / RECLAIMER_CRT_DIR override).
cd "$(dirname "$0")/../.."
BOTS="${1:-60}"; RUNS="${2:-1}"; EXTRA="${3:-}"
OUT="${BENCH_OUT:-/tmp/reclaimer_bench.jsonl}"; LOG=/tmp/reclaimer_bench.log
EXE="${BENCH_EXE:-}"; if [ -z "$EXE" ]; then for c in client/project-reclaimer-client-v0.9.20.exe client/project-reclaimer-client-v0.9.18.exe "$HOME/ReclaimerMac/client/project-reclaimer-client-v0.9.20.exe" "$HOME/ReclaimerMac/client/project-reclaimer-client-v0.9.18.exe"; do [ -f "$c" ] && EXE="$c" && break; done; fi
CRT="${RECLAIMER_CRT_DIR:-}"; [ -z "$CRT" ] && { [ -f shim/crt/ucrtbase.dll ] && CRT="$PWD/shim/crt" || CRT="$HOME/ReclaimerMac/shim/crt"; }
[ -f "$EXE" ] && [ -f "$CRT/ucrtbase.dll" ] || { echo "client exe or CRT not found ($EXE, $CRT)"; exit 2; }
KEEP=" $(cat "$HOME/Library/Application Support/ReclaimerMac/target.pgid" "$HOME/Library/Application Support/ReclaimerMac/dedicated.pgid" 2>/dev/null | tr '\n' ' ') "
others(){ for p in $(pgrep -f '^[^ ]*build/peload'); do case "$KEEP" in *" $(ps -o pgid= -p "$p" | tr -d ' ') "*) continue;; esac; echo "$p"; done; }
mkdir -p "$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/ReclaimerProfileTest"
MAP="${BENCH_MAP:-guardian}"; W="${BENCH_WAIT:-1}"   # BENCH_WAIT=2: every menu wait twice as long (a busy Mac)
w(){ echo "s:$(( $1 * W ))"; }
CLICKS="110,503;$(w 6)"   # Host Game
# map picker (1280x720 coordinates): MAP row, search box, type, first tile, USE <MAP>
[ "$MAP" != guardian ] && CLICKS="$CLICKS;230,185;$(w 3);300,145;t:$MAP;$(w 4);155,265;$(w 3);1047,665;$(w 4)"
CLICKS="$CLICKS;230,625"   # Start Game
for run in $(seq 1 "$RUNS"); do
  if [ -n "$(others)" ]; then echo "a game is already running - not starting"; exit 3; fi
  : > "$LOG"; set -m
  env PELOAD_SKINGUARD=1 PELOAD_VTGUARD=1 PELOAD_ALLOC_GUARD=1 PELOAD_RECLAIMER_PATCHES=all PELOAD_WORKER_INHERIT_STDIO=1 \
    PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_PRESENT_HIDDEN=1 PELOAD_D3DGL=1 PELOAD_FPS_LOG=1 \
    PELOAD_NO_PAD=1 PELOAD_NO_KBM=1 PELOAD_NO_SYS_CURSOR=1 PELOAD_NO_FOCUS=1 PELOAD_AUDIO=0 \
    PELOAD_GL_HELPER="${PELOAD_GL_HELPER:-1}" PELOAD_D3DGL_SUBMIT="${PELOAD_D3DGL_SUBMIT:-auto}" \
    PELOAD_TITLE_PREFIX="BENCH (no input)" RECLAIMER_PROFILE_DIR='C:\ReclaimerProfileTest' RECLAIMER_CRT_DIR="$CRT" \
    PELOAD_AUTOCLICK="$CLICKS" PELOAD_BENCH_BOTS="$BOTS" PELOAD_BENCH_SECS="${BENCH_SECS:-30}" PELOAD_BENCH_OUT="$OUT" $EXTRA \
    arch -x86_64 shim/build/peload "$EXE" game-client --mcc-path "C:\\Program Files\\Steam\\steamapps\\common\\Halo The Master Chief Collection" \
    < /dev/null > "$LOG" 2>&1 &
  PG=$!; set +m; t=0; limit=$(( ${BENCH_SECS:-30} + 170 * W ))
  while [ $t -lt $limit ]; do
    sleep 2; t=$((t+2))
    if grep -aq '\[bench\] RESULT\|\[bench\] .*no bots\|\[CRASH\]' "$LOG" 2>/dev/null; then break; fi
    pgrep -g $PG >/dev/null || break   # (the first client process starts the real one and exits)
  done
  sleep 1; kill -9 -- -$PG 2>/dev/null; sleep 1
  r=$(grep -ah '\[bench\] RESULT' "$LOG" | sed 's/^ *\[bench\] //')
  if [ -n "$r" ]; then echo "run $run ($MAP): $r ${EXTRA:+[$EXTRA]}"
  else echo "run $run: no result after ${t}s -- $(grep -ah '\[bench\]\|\[CRASH\]' "$LOG" | tail -3 | tr '\n' ' ')"; fi
  grep -ah '\[bench\] spawned' "$LOG" | sed 's/^ */      /'
done
if [ -n "$(others)" ]; then echo "WARNING: a game process is still running after cleanup"; fi
