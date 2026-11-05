# Bulánci in the browser

A fan project that aims to run the PC game *Bulánci* (*Combat Pillows*, version 1.80, SleepTeam Labs 2002) in
Google Chrome. The game's x86 code is translated to C ahead of time and compiled to WebAssembly; Win32, DirectDraw,
DirectInput and DirectSound are reimplemented, the picture goes through WebGPU. You bring your own copy of the game.

Built on [cmr05-web](https://github.com/Chuck3CZ/cmr05-web) (same lifter and runtime, from
[wc3-web](https://github.com/Chuck3CZ/wc3-web) and nfsu2-web).

> Not affiliated with or endorsed by SleepTeam Labs. This repository contains none of the game's code or data: no
> EXE, no C translated from it, no compiled module (`.gitignore` keeps them out).

**Status:** the main menu and the game setup run in Chrome; starting a round does not work yet. The plan and the
state are in [docs/PLAN.md](docs/PLAN.md).

## What you need

- **The game:** `bulanci.exe` 1.80 (one file, its data appended; SHA-256 in `gen.sh`). Other versions will not work.
- **To build:** Linux or WSL 2, Python 3 with `pefile` and `capstone` 5, wasi-sdk in `deps/wasi-sdk`, Node.js 24.

## Build and run

```sh
. tools/env.sh
./gen.sh "<folder with bulanci.exe>"     # lift bulanci.exe to C (gen/, never committed)
./build.sh                               # compile + link -> out/web/wc3.wasm
(cd web && node build.mjs)               # the host and the test page
node web/tools/wc3-dev-server.mjs --data "<folder with bulanci.exe>" --port 8088
```

Then in Chrome:
`http://localhost:8088/test/hostcheck.html?wasm=/wc3.wasm&pages=20480&arg=/data/bulanci.exe&env=WC3_SAVEDIR=/save&env=WC3_READONLY_DATA=1&gpu&res=800x600&fresh`

`tools/run-node.sh "<folder>"` runs it under Node (one thread, no picture) for quick checks.

## What is new here

- **`runtime/eh.c`:** MSVC C++ exceptions for the lifted code (the SEH chain, FuncInfo, unwind actions, catch
  funclets, rethrow); the lifter (`tools/lift.py`, `eh_conts`) gives functions with catch blocks a resume point.
- **`runtime/ddraw.c`:** DirectDraw 1 (surfaces in guest memory, Blt/BltFast with colour keys, Lock, Flip,
  palettes), shown through the WebGPU host.
- **`runtime/w2a.c`:** the W (UTF-16) Win32 calls of a Unicode game on the runtime's A ones, code page 1250.

## License

MIT ([LICENSE](LICENSE)). *Bulánci* is a trademark of SleepTeam Labs.
