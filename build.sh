#!/bin/bash
# Build out/web/wc3.wasm from the lifted modules (gen/<module>, made by
# ./gen.sh) and the runtime.
#   ./build.sh                       every module gen.sh made, the runtime, link
#   ./build.sh <module> ...          only compile those modules' shards (no link)
# Objects go to out/web/obj/<module>/ and out/web/rt/; a content-addressed
# cache (~/.cache/wc3-web/obj, keyed by the shard, cpu.h and the flags) makes
# a rebuild after a partial re-lift cheap.
#
# A module is refused when gen/<module>/stamp.txt is missing or incomplete,
# or names another lifter than this checkout's (the lift and the runtime must
# agree on cpu.h's macros).
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
WASI="${WASI:-$ROOT/deps/wasi-sdk}"
[ -x "$WASI/bin/clang" ] || { echo "build.sh: wasi-sdk not found in deps/wasi-sdk (set WASI=...)"; exit 1; }
CC="$WASI/bin/clang"
J="${J:-$(nproc)}"
OPT="${OPT:--O3}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime"
# the lifted shards: guest memory as an object of its own (cpu.h)
SHARDFLAGS="-DNFSU2_GUEST_OBJ -Wno-unused-label -Wno-parentheses-equality"
CACHE="${OBJCACHE:-$HOME/.cache/wc3-web/obj}"; mkdir -p "$CACHE"
HDRSUM="$( (cat runtime/cpu.h runtime/module.h runtime/web/compat/wasm_plat.h; "$CC" --version | head -1; echo "$CFLAGS $SHARDFLAGS") | sha256sum | cut -c1-16)"
LIFTER="lifter $(cat tools/lift.py tools/lregs.py tools/genall.py tools/prepare_module.py tools/ptrscan.py | sha256sum | cut -c1-16)"

MODS="$*"; LINK=0
[ -n "$MODS" ] || { MODS="$(ls -d gen/*/ 2>/dev/null | xargs -n1 basename | tr '\n' ' ')"; LINK=1; }
[ -n "$MODS" ] || { echo "build.sh: nothing in gen/ (run ./gen.sh first)"; exit 1; }

# one shard -> one object (run by xargs; prints "<status> <seconds> <bytes> <shard>")
compile_one(){
  src="$1"; mod="$2"; obj="$ROOT/out/web/obj/$mod/$(basename "${src%.c}").o"
  key="$( (cat "$src" "gen/$mod/decls.h"; echo "$HDRSUM") | sha256sum | cut -c1-32)"
  if [ -f "$CACHE/$key.o" ]; then cp "$CACHE/$key.o" "$obj"; echo "cached 0 $(stat -c%s "$obj") $src"; return; fi
  t0=$(date +%s.%N)
  if ! err=$($CC $CFLAGS $SHARDFLAGS -I"gen/$mod" -c -o "$obj" "$src" 2>&1); then
    echo "$err" | head -30 >&2; echo "FAILED 0 0 $src"; return 1
  fi
  cp "$obj" "$CACHE/$key.o.tmp$$" && mv "$CACHE/$key.o.tmp$$" "$CACHE/$key.o"
  echo "built $(awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN{printf "%.2f", a-b}') $(stat -c%s "$obj") $src"
}
export -f compile_one; export ROOT CC CFLAGS SHARDFLAGS CACHE HDRSUM

FAIL=0
for MOD in $MODS; do
  G="gen/$MOD"
  [ -f "$G/stamp.txt" ] && ! grep -q '^incomplete' "$G/stamp.txt" || { echo "build.sh: $G is missing or incomplete (./gen.sh)"; exit 1; }
  grep -qxF "$LIFTER" "$G/stamp.txt" || { echo "build.sh: $G was lifted by another lifter than this checkout's (re-run ./gen.sh)"; exit 1; }
  mkdir -p "out/web/obj/$MOD"
  T0=$(date +%s)
  ls "$G"/*.c | xargs -P "$J" -I{} bash -c 'compile_one "$@"' _ {} "$MOD" > "out/web/obj/$MOD/build.log" || FAIL=1
  read -r NB NC NF SZ CPU <<<"$(awk '{n[$1]++; s+=$3; t+=$2} END{printf "%d %d %d %d %.0f", n["built"], n["cached"], n["FAILED"], s, t}' "out/web/obj/$MOD/build.log")"
  printf "  %-8s %4d objects (%d built, %d cached, %d FAILED)  %6.1f MB  %5d s wall, %6.0f s CPU\n" \
    "$MOD" $((NB + NC + NF)) "$NB" "$NC" "$NF" "$(awk -v s="$SZ" 'BEGIN{print s/1048576}')" $(( $(date +%s) - T0 )) "$CPU"
done
[ "$FAIL" = 0 ] || { echo "build.sh: some shards failed (out/web/obj/<module>/build.log)"; exit 1; }
[ "$LINK" = 1 ] || exit 0

# ---- runtime ----
# nfsu2-web's generic runtime (docs/ORIGIN.md) and the WC3 parts; runtime/web/
# holds the browser stand-ins for the Mac's Cocoa/Metal/CoreAudio parts.
# runtime/nfsu2_compat/: TEMPORARY, nfsu2-web's interfaces the imported
# runtime still calls (widescreen, enhancements, frame pacer, DirectInput,
# D3DX, ...), as headers and neutral stand-ins; docs/PLAN.md lists what is
# left to take out.
RT="mem dispatch genexec heap ffp watchdog win32 win32_ext com debug res fileio vsemu oracle lifecycle keyboard dinput_kbd winsock module d3d8 user32 nfsu2_compat/stubs"
RTFLAGS="$CFLAGS -Iruntime/nfsu2_compat"
WEB="$(cd runtime/web && ls *.c | sed 's/\.c$//')"
mkdir -p out/web/rt; rm -f out/web/rt/*.o
pids=""
for f in $RT; do
  o="out/web/rt/$(echo "$f" | tr / _)"
  $CC $RTFLAGS -c -o "$o.o" "runtime/$f.c" > "$o.log" 2>&1 & pids="$pids $!"
done
for f in $WEB; do
  case "$f" in thread_exit) fl="$TARGET $OPT $FEAT" ;; *) fl="$RTFLAGS" ;; esac
  $CC $fl -c -o "out/web/rt/web_$f.o" "runtime/web/$f.c" > "out/web/rt/web_$f.log" 2>&1 & pids="$pids $!"
done
$CC $CFLAGS -c -o out/web/rt/modtab.o gen/modtab.c > out/web/rt/modtab.log 2>&1 & pids="$pids $!"
RTFAIL=0; for p in $pids; do wait "$p" || RTFAIL=1; done
if [ "$RTFAIL" = 1 ]; then
  for l in out/web/rt/*.log; do grep -q "error:" "$l" && { echo "== ${l%.log}.c"; grep -E "error:" "$l" | head -12; }; done
  echo "build.sh: the runtime did not compile"; exit 1
fi
echo "  runtime  $(ls out/web/rt/*.o | wc -l) objects"

# ---- link ----
# The guest owns [0, GUEST_END) of linear memory (runtime/cpu.h); the
# runtime's data starts there.  Initial memory covers the guest span plus room
# for the runtime heap; it can grow to 4 GB.
GUEST_END=$((0x38000000))
INITIAL_MB="${INITIAL_MB:-1280}"
rm -f out/web/wc3.wasm
$CC $TARGET $OPT $FEAT -o out/web/wc3.wasm out/web/obj/*/*.o out/web/rt/*.o \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=$(( INITIAL_MB * 1048576 )),--global-base=$GUEST_END \
  -Wl,-z,stack-size=$(( 8 * 1048576 )) -Wl,--no-stack-first \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks > out/web/link.log 2>&1 || true
if [ ! -f out/web/wc3.wasm ]; then
  echo "link FAILED ($(grep -c "undefined symbol" out/web/link.log) undefined symbols):"
  grep -oE "undefined symbol: [A-Za-z0-9_]+" out/web/link.log | sort | uniq -c | sort -rn | head -60
  exit 1
fi
SIZE=$(stat -c%s out/web/wc3.wasm)
REV="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo +)"
printf '{"pages":%d,"maxPages":65536,"guestEnd":%d,"rev":"%s","size":%d,"opt":"%s"}\n' \
  $(( INITIAL_MB * 16 )) "$GUEST_END" "$REV" "$SIZE" "$OPT" > out/web/wc3.json
echo "link OK: out/web/wc3.wasm ($(( SIZE / 1048576 )) MB, rev $REV)"
# a C declaration that disagrees with the definition links on wasm but traps
# when called ("signature_mismatch")
if grep -q "signature mismatch" out/web/link.log; then
  echo "WARNING: function signature mismatches (they trap when called):"; grep -A2 "signature mismatch" out/web/link.log | head -40; fi
