#!/bin/bash
# runtime/web/tz.c (local time from the page's zone) against the JS engine's
# local time, for zones with and without daylight saving, both hemispheres,
# half- and quarter-hour offsets and a 30-minute DST shift.
#   web/tools/tztest.sh [zone ...]
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos 2>/dev/null | tail -1)}"
OUT="$ROOT/out/web/tz"; mkdir -p "$OUT"
CFLAGS="--target=wasm32-wasip1-threads -pthread -O2 -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime"
"$WASI/bin/clang" $CFLAGS -o "$OUT/tztest.wasm" tests/web/tztest.c runtime/web/tz.c \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296
ZONES="${*:-Europe/Amsterdam America/New_York America/Sao_Paulo Australia/Sydney Australia/Lord_Howe Asia/Kolkata Pacific/Chatham Europe/Dublin America/St_Johns UTC}"
fail=0
for z in $ZONES; do TZ="$z" node --no-warnings web/tools/tztest.mjs "$OUT/tztest.wasm" || fail=1; done
exit $fail
