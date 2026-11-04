#!/bin/bash
# Stress of the host's file RPC (tests/web/iostress.c) through the real
# browser host: 8 threads x 20000 small preads of a /data file, checked.
#   web/tools/iostress.sh [http://localhost:<port>]   (a running server, started with --dev)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
BASE="${1:-http://localhost:8611}"
[ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/test/hostcheck.html")" = 404 ] && { echo "$BASE does not serve the test pages: start the server with --dev"; exit 2; }
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos 2>/dev/null | tail -1)}"
mkdir -p web/dist-dev/test
"$WASI/bin/clang" --target=wasm32-wasip1-threads -pthread -O2 -o web/dist-dev/test/iostress.wasm tests/web/iostress.c \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296
node web/tools/run-page.mjs "$BASE/test/hostcheck.html?wasm=/test/iostress.wasm&pages=1024&save=core-iostress&fresh" 120000 | tee "$ROOT/out/web/iostress.log" | grep -E 'OK|FAIL|EXIT|TIMEOUT|TRAP'
grep -q '^EXIT 0' "$ROOT/out/web/iostress.log"
