# Colin McRae Rally 2005 in the browser

A fan project that aims to run the PC game *Colin McRae Rally 2005* (Czech edition, 2004) in Google Chrome.
The game's x86 code is translated to C ahead of time and compiled to WebAssembly; Win32, Direct3D 9, DirectInput 8
and DirectSound are reimplemented, the graphics run on WebGPU. You bring your own copy of the game.

Built on [wc3-web](https://github.com/Chuck3CZ/wc3-web) (same lifter and runtime, originally from nfsu2-web).

> Not affiliated with or endorsed by Codemasters. This repository contains none of the game's code or data: no EXE,
> DLL, BIG or WMV, no C translated from them, no compiled module (`.gitignore` keeps them out).

**Status:** the plan, the state and the measured numbers are in [docs/PLAN.md](docs/PLAN.md).

## Build (Linux / WSL 2)

```sh
. tools/env.sh
./gen.sh "<CMR 2005 folder>"     # lift CMR5.EXE and the Math*.dll to C (gen/, never committed)
./build.sh                       # compile + link -> out/web/wc3.wasm
```

Needs Python 3 with `pefile` and `capstone` 5, wasi-sdk in `deps/wasi-sdk`, Node.js 24.
