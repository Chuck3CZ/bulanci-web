#!/usr/bin/env bash
# Copy the generic parts of nfsu2-web (MIT) into this tree.
#   tools/import_nfsu2.sh [nfsu2-web checkout]     (default ~/nfsu2-web)
# The list below is the whole import: what is not named here (career, drift,
# free roam, horns, Online, DirectInput, macOS/Metal, D3DX, the NFSU2 web app
# pages, the Discord bot, ...) is specific to NFSU2 and stays out.
# docs/ORIGIN.md records the source commit.  Run it once; later changes to
# these files are this project's own.
set -euo pipefail
SRC="${1:-$HOME/nfsu2-web}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
[ -f "$SRC/tools/lift.py" ] || { echo "no nfsu2-web checkout at $SRC"; exit 1; }

FILES=(
  requirements.txt
  # the lifter and its analyses
  tools/lift.py tools/genall.py tools/lregs.py tools/flagabi.py tools/argcount.py tools/prepare_module.py
  tools/inline_pick.py tools/lift_profile.py tools/isatest.py tools/fuzzisa.py tools/disasm.py tools/bcmp.py
  tools/genstamp.sh tools/manifest.py tools/vlanhub.py tools/lan_trace.py tools/writewatch.c
  # runtime: CPU, memory, dispatch, modules
  runtime/cpu.h runtime/dispatch.c runtime/genexec.c runtime/mem.c runtime/heap.c runtime/modheap.c
  runtime/module.c runtime/module.h runtime/oracle.c runtime/debug.c runtime/watchdog.c
  runtime/perflog.c runtime/perflog.h runtime/res.c runtime/xxhash.h runtime/nettest.h
  # runtime: Win32, files, input, lifecycle
  runtime/win32.c runtime/win32_ext.c runtime/fileio.c runtime/keyboard.c runtime/kbd.h runtime/lifecycle.c
  # runtime: Direct3D 9 / DirectSound COM, shader translation
  runtime/com.c runtime/d3d9_tables.h runtime/gfx.h runtime/vsemu.c runtime/vsemu.h
  # runtime: Winsock and the virtual LAN
  runtime/winsock.c runtime/winsock.h runtime/net_vlan.c
  # tests that do not depend on the game
  tests/web/aliastest.c tests/web/gilbench.c tests/web/iostress.c tests/web/tztest.c tests/web/x87pc.c
  # web: build, host, GPU, audio, net, input
  web/build.mjs web/build.sh web/package.json web/package-lock.json web/tsconfig.json web/start.sh
  web/src/main.ts web/src/sw.ts
  web/src/test/hostcheck.ts web/src/test/shadercheck.ts
  web/src/input/index.ts web/src/input/keymap.ts web/src/input/proto.ts web/src/input/synth.ts
  web/src/app/loader.ts web/src/app/logs.ts web/src/app/lastgame.ts web/src/app/shell.ts web/src/app/settings.ts
  web/src/app/savefs.ts web/src/app/status.ts web/src/app/display.ts web/src/app/device.ts web/src/app/devicelog.ts
  web/src/app/overlay.ts web/src/app/i18n.ts web/src/app/predownload.ts web/src/app/instance.ts
  web/src/app/page-id.ts web/src/app/checks.ts web/src/app/contract.ts web/src/app/subsystems.ts
  web/src/app/rendercheck.ts web/src/app/text.d.ts web/src/app/zip.ts web/src/app/privacy.ts
  # web tools
  web/tools/aliasguard.sh web/tools/aliastest.sh web/tools/isatest.sh web/tools/run-page.mjs web/tools/wasi-run.mjs
  web/tools/check-proto.mjs web/tools/check-gpu-proto.mjs web/tools/dev-server.mjs web/tools/gilbench.sh
  web/tools/iostress.sh web/tools/tztest.sh web/tools/tztest.mjs web/tools/x87pctest.sh web/tools/main_all.sh
  web/tools/shader-check.mjs web/tools/wgsl-xl.c web/tools/threadcpu.c web/tools/threadcpu-win.ps1 web/tools/stats.mjs
  # deployment (Docker, Caddy, Cloudflare Tunnel)
  web/deploy/Caddyfile web/deploy/cloudflared.yml web/deploy/compose.yml web/deploy/compose.tunnel.yml
  web/deploy/compose.cloudflare.yml web/deploy/compose.origin.yml web/deploy/compose.443.yml web/deploy/README.md
  # docs that describe the shared machinery
  docs/web/gpu-abi.md
)
DIRS=(
  runtime/web                      # the browser runtime (roam_web.c, texrep_web.c removed below)
  web/src/host web/src/gpu web/src/audio web/src/net
  web/server-rs                    # crates online, roam, roamload, vault, names removed below
)
DROP=(
  runtime/web/roam_web.c runtime/web/texrep_web.c
  web/server-rs/crates/online web/server-rs/crates/roam web/server-rs/crates/roamload
  web/server-rs/crates/vault web/server-rs/crates/names
  web/src/host/movie.ts            # VP6/AV1 movie hooks of NFSU2; WC3's movies come later
)

for f in "${FILES[@]}"; do mkdir -p "$(dirname "$f")"; cp -p "$SRC/$f" "$f"; done
for d in "${DIRS[@]}"; do mkdir -p "$(dirname "$d")"; rm -rf -- "./$d"; cp -rp "$SRC/$d" "$d"; done
for d in "${DROP[@]}"; do rm -rf -- "./$d"; done
rm -rf -- ./web/server-rs/target

REV=$(git -C "$SRC" rev-parse HEAD); DATE=$(git -C "$SRC" log -1 --format=%cs)
mkdir -p docs
{
  echo "# Origin"
  echo
  echo "The files that \`tools/import_nfsu2.sh\` lists were copied from"
  echo "[nfsu2-web](https://github.com/Chuck3CZ/nfsu2-web) commit \`$REV\` ($DATE), MIT."
  echo "Everything changed or added after that is this project's own (\`git log\`)."
} > docs/ORIGIN.md
echo "imported ${#FILES[@]} files and ${#DIRS[@]} directories from $SRC at $REV"
