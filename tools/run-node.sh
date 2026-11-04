#!/bin/bash
# Start-up test of out/web/wc3.wasm under node:wasi, without the browser:
#   tools/run-node.sh "<CMR 2005 folder>" [env K=V ...]
# The game folder is /data (read-only for the game: writes go to /save,
# out/node-save/), the guest runs on the main thread (WC3_NOTHREAD=1: node's
# wasi has no thread-spawn), so it gets as far as the game's first thread.
# The log is out/node-run.log.
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
. tools/env.sh
W="${1:?usage: tools/run-node.sh \"<CMR 2005 folder>\" [K=V ...]}"; shift
[ -f out/web/wc3.wasm ] || { echo "no out/web/wc3.wasm (./build.sh)"; exit 1; }
mkdir -p out/node-save
export WASI_PREOPEN="/data=$W,/save=$ROOT/out/node-save" WASI_PAGES=$((1280 * 16))
export WC3_NOTHREAD=1 WC3_SAVEDIR=/save WC3_READONLY_DATA=1
for kv in "$@"; do export "$kv"; done
timeout "${TIMEOUT:-120}" node --no-warnings --stack-size=8000 web/tools/wasi-run.mjs out/web/wc3.wasm /data/CMR5.EXE \
  2>&1 | tee out/node-run.log | grep -E --line-buffered "^\[module\]|^\[dll\]|^\[trap\]|UNRESOLVED|FAULT|exited|returned|NOT IMPLEMENTED|^win32:|^dispatch:|error" | head -${LINES_MAX:-200}
echo "--- exit ${PIPESTATUS[0]}; full log: out/node-run.log ($(wc -l < out/node-run.log) lines)"
