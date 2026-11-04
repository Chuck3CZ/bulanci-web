#!/bin/bash
# tests/web/x87pc.c on WebAssembly (web/build.sh's target and flags, node:wasi):
# the inlined X87PC rounding against its exact out-of-line path.  Exit 0 = pass.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos 2>/dev/null | tail -1)}"
OUT="$ROOT/out/web/x87pc"; mkdir -p "$OUT"
OPT="${OPT:--O3}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime"
"$WASI/bin/clang" $CFLAGS -o "$OUT/x87pc.wasm" tests/web/x87pc.c runtime/mem.c \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=1073741824,--global-base=671088640 -Wl,--no-stack-first \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks
node --no-warnings web/tools/wasi-run.mjs "$OUT/x87pc.wasm"
