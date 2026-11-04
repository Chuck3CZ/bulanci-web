#!/usr/bin/env bash
# The QNAP trial bundle: out/qnap/wc3-web-trial/ (Dockerfile, compose.yml, the server and the built pages) and,
# beside it, out/qnap/module/wc3.wasm.  Neither goes into git (out/ is ignored): the module is built from the
# game's code.  Copy both folders to the NAS (README in deploy/qnap).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
[ -f out/web/wc3.wasm ] || { echo "pack-qnap: out/web/wc3.wasm missing (./build.sh)"; exit 1; }
( cd web && node build.mjs >/dev/null )
B=out/qnap/cmr05-web-trial
rm -rf "$B" out/qnap/module; mkdir -p "$B/app/web/tools" out/qnap/module
cp deploy/qnap/Dockerfile deploy/qnap/compose.yml deploy/qnap/README.md "$B/"
[ -f deploy/qnap/.env ] && cp deploy/qnap/.env "$B/" || cp deploy/qnap/env.example "$B/.env"
cp web/tools/wc3-dev-server.mjs "$B/app/web/tools/"
cp web/landing.html "$B/app/web/"
cp -r web/dist web/dist-dev "$B/app/web/"
cp out/web/wc3.wasm out/qnap/module/
echo "rev $(git rev-parse --short HEAD)$(git diff --quiet || echo +)" > "$B/REVISION"
du -sh "$B" out/qnap/module | sed 's/^/pack-qnap: /'
