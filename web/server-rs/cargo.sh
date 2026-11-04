#!/bin/bash
# cargo in this workspace, with a target folder of the caller's own (~/.cache/nfsu2-rs/<name>), so that builds that
# run at the same time never wait on one another's lock:
#   web/server-rs/cargo.sh <name> <cargo arguments...>
# e.g. from Windows: MSYS_NO_PATHCONV=1 wsl -d Ubuntu -- bash -l ~/nsfu2-web-wt/rust-server/web/server-rs/cargo.sh lan test -p nfsu2-lan
set -e
cd "$(dirname "$0")"
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
name="${1:?usage: cargo.sh <name> <cargo arguments...>}"; shift
export CARGO_TARGET_DIR="$HOME/.cache/nfsu2-rs/$name"
exec cargo "$@"
