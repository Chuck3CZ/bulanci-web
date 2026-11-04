#!/bin/bash
# The x86 ISA semantics suite (tools/isatest.py) on WebAssembly: the lifter's
# output for the hand-coded test instructions, compiled exactly like the game's
# lifted shards (web/build.sh flags, identity guest mapping, guest span below
# 0x28000000) and run in V8 under node:wasi.  This is the first correctness
# gate of the wasm codegen; the Mac runs the same tests as out/isatest.
#
#   web/tools/isatest.sh            regenerate gen/isatest.c, build, run
#   OPT=-O2 web/tools/isatest.sh    another optimisation level (default -O3)
#   WEB_LIFT=opt web/tools/isatest.sh   with web/gen.sh's lift options (X87OPT, FLAGABI, JTSWITCH, LREGS)
#
# Output: out/web/isa/isatest.wasm and the test report; exit 0 = all passed.
# gen/isatest.c is written into this checkout's own gen/ directory (next to
# the gen/all symlink of a worktree; gen/all itself is never touched).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos "$ROOT"/deps/wasi-sdk-*-x86_64-linux 2>/dev/null | tail -1)}"
[ -x "$WASI/bin/clang" ] || { echo "wasi-sdk not found in deps/ (set WASI=...)"; exit 1; }
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3
OUT="$ROOT/out/web/isa"; mkdir -p "$OUT" gen
# WEB_LIFT=opt: lift the tests the way web/gen.sh lifts the game (X87OPT, JTSWITCH, LREGS and
# FLAGABI's liveness with the test snippet declared flag-returning: the test
# reads its flags after its ret, which a FLAGABI lift otherwise leaves dead)
if [ "${WEB_LIFT:-}" = opt ]; then
  echo '{"live_in": {}, "flags_out": ["0x401000"], "indirect_rd": []}' > "$OUT/flagabi_isa.json"
  export X87OPT=1 FLAGABI="$OUT/flagabi_isa.json" JTSWITCH="${JTSWITCH:-1}" LREGS="${LREGS:-1}"
fi
"$PY" tools/isatest.py
# Same target, features and defines as web/build.sh (keep in step with it).
OPT="${OPT:--O3}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime"
# the lifted code as web/build.sh compiles the shards (SHARDFLAGS there)
"$WASI/bin/clang" $CFLAGS -DNFSU2_GUEST_OBJ -Wno-unused-label -Wno-parentheses-equality -c -o "$OUT/isatest.o" gen/isatest.c
"$WASI/bin/clang" $CFLAGS -c -o "$OUT/mem.o" runtime/mem.c
# dispatch_tail: the lifted DISPATCH_TAIL target; the tests never jump indirectly
printf '#include "cpu.h"\nvoid dispatch_tail(CPU *c){ dispatch_indirect(c, c->eip); }\n' > "$OUT/tail.c"
"$WASI/bin/clang" $CFLAGS -c -o "$OUT/tail.o" "$OUT/tail.c"
"$WASI/bin/clang" $TARGET $OPT -o "$OUT/isatest.wasm" "$OUT/isatest.o" "$OUT/mem.o" "$OUT/tail.o" \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=1073741824,--global-base=671088640 -Wl,--no-stack-first \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks
echo "built $OUT/isatest.wasm ($OPT)"
node --no-warnings web/tools/wasi-run.mjs "$OUT/isatest.wasm"
