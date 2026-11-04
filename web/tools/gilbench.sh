#!/bin/bash
# The guest lock's hand-off latency and fairness in Chrome (tests/web/gilbench.c):
# FIFO ticket lock (runtime/web/ticketlock.h) vs wasi-libc's pthread mutex.
#   web/tools/gilbench.sh [http://localhost:<port>]   (a running server, started with --dev)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
BASE="${1:-http://localhost:8611}"
[ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/test/hostcheck.html")" = 404 ] && { echo "$BASE does not serve the test pages: start the server with --dev"; exit 2; }
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos 2>/dev/null | tail -1)}"
mkdir -p web/dist-dev/test
"$WASI/bin/clang" --target=wasm32-wasip1-threads -pthread -O2 -Iruntime -o web/dist-dev/test/gilbench.wasm tests/web/gilbench.c \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296
node web/tools/run-page.mjs "$BASE/test/hostcheck.html?wasm=/test/gilbench.wasm&pages=1024&save=core-gilbench&fresh" 120000 \
  | sed -E 's/^\[[0-9]+!?\] //' | tee "$ROOT/out/web/gilbench.log" | grep -vE '^compiled|^Failed to load resource'
grep -q '^EXIT 0' "$ROOT/out/web/gilbench.log"
