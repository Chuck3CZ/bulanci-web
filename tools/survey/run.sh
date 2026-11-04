#!/usr/bin/env bash
# Phase 0 survey of a Warcraft III 1.26a folder: tools/survey/run.sh "<WC3 folder>" [nfsu2-web checkout]
# Writes out/survey/ (never committed): survey.json, apidiff.json and the console reports.
set -euo pipefail
W="$1"; N="${2:-$HOME/nfsu2-web}"; HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p out/survey && cd out/survey
python3 "$HERE/peinfo.py" "$W" war3.exe "Frozen Throne.exe" "Warcraft III.exe" game.dll Storm.dll Mss32.dll ijl15.dll \
  blizzard.ax msvcr80.dll msvcp80.dll redist/miles/Mp3dec.asi redist/miles/Mssdolby.m3d redist/miles/Msseax2.m3d \
  redist/miles/Mssfast.m3d redist/miles/Reverb3.flt > survey.json
python3 "$HERE/dynload.py" "$W/war3.exe" "$W/game.dll" "$W/Storm.dll" "$W/Mss32.dll" "$W/ijl15.dll" | tee dynload.txt
python3 "$HERE/apidiff.py" "$N" | tee apidiff.txt
python3 "$HERE/fncount.py" "$W/war3.exe" "$W/game.dll" "$W/Storm.dll" "$W/Mss32.dll" "$W/ijl15.dll" "$W/msvcr80.dll" | tee fncount.txt
