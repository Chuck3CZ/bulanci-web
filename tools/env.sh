# source tools/env.sh: the toolchains this repo builds with (installed under $HOME, no sudo)
#   wasi-sdk 34 in deps/wasi-sdk, Node.js 24 in ~/.local/node24 (wasm exnref; 20 in ~/.local/node), Rust in ~/.cargo
_WC3_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export WASI="${WASI:-$_WC3_ROOT/deps/wasi-sdk}"
export PATH="$HOME/.local/node24/bin:$HOME/.local/node/bin:$HOME/.cargo/bin:$PATH"
