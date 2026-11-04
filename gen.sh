#!/bin/bash
# Lift Bulanci 1.80 (SleepTeam Labs) to C: every module the game runs, each at its fixed
# guest base (docs/PLAN.md "Rozložení paměti").
#   ./gen.sh "<Bulanci folder>" [module ...]     (default: all modules)
# Writes gen/<module>/ (shards, decls.h, fntab.c, impargs.c, modinfo.c,
# stamp.txt) and out/mod/<module>/ (the relocated analysis copy, fixup slots,
# seeds, logs).  Both are made from the game and never committed.
#
# The input files must be the exact Bulanci 1.80 ones: their SHA-256 are pinned
# below and anything else is refused (lifted code bakes in addresses, so
# another build of a DLL would run the wrong code).
#
# Lift options (tools/lift.py), web defaults:
#   X87PC=1   x87 precision control (PC=24 as on Windows)
#   X87OPT=1  x87 stack slots as locals within a block
#   JTSWITCH=1 MSVC jump tables as C switches
#   LREGS=1   guest registers and flags as C locals
# NSHARD_<MODULE> overrides a module's shard count.
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
W="${1:-${WC3_DATA:-}}"
[ -d "$W" ] || { echo "usage: ./gen.sh \"<Bulanci folder>\" [module ...]"; exit 1; }
shift || true
PY="${PY:-python3}"
"$PY" -c 'import pefile, capstone' 2>/dev/null || { echo "gen.sh: needs Python with pefile and capstone 5 (requirements.txt)"; exit 1; }

# module  file                     guest base  prefix    shards  sha256
# (the exe has no .reloc and stays at 0x400000; the math DLLs, one of which
# the game loads by the CPU it finds, go right after it)
MODULES="
exe      bulanci.exe               0x00400000  exe_      8       2992cca12ec9ff452894045729ebf776a335e1dfcd9281260be6d8d9f6648e39
"
WANT=" $* "

export X87PC="${X87PC:-1}" X87OPT="${X87OPT:-1}" JTSWITCH="${JTSWITCH:-1}" LREGS="${LREGS:-1}"
h16(){ sha256sum | cut -c1-16; }
SOURCES="lifter $(cat tools/lift.py tools/lregs.py tools/genall.py tools/prepare_module.py tools/ptrscan.py | h16)"

# find a file case-insensitively in the game folder (Mss32.dll vs mss32.dll)
ifind(){ find "$W" -maxdepth 3 -ipath "$W/$1" -type f | head -1; }

echo "$MODULES" | while read -r MOD FILE BASE PFX NSH SHA; do
  [ -n "$MOD" ] || continue
  [ "$WANT" = "  " ] || [[ "$WANT" == *" $MOD "* ]] || continue
  SRC="$(ifind "$FILE")"
  [ -n "$SRC" ] || { echo "gen.sh: $FILE not found in $W"; exit 1; }
  GOT="$(sha256sum "$SRC" | cut -d' ' -f1)"
  [ "$GOT" = "$SHA" ] || { echo "gen.sh: $FILE is not the Bulanci 1.80 file (sha256 $GOT, want $SHA)"; exit 1; }
  NV="NSHARD_${MOD^^}"; NSH="${!NV:-$NSH}"
  G="gen/$MOD"; O="out/mod/$MOD"
  if [ "${ARGS_ONLY:-}" = 1 ]; then
    # only the import table (argument counts: tools/argcount.py, tools/winapi_nargs.txt);
    # the lifted shards stay, and the stamp keeps naming the lifter they were made by
    [ -f "$G/stamp.txt" ] && ! grep -q '^incomplete' "$G/stamp.txt" || { echo "gen.sh: $MOD was never lifted (run ./gen.sh without ARGS_ONLY)"; exit 1; }
    IMG="$O/image.dll"; IMPD="$SRC"; [ "$MOD" = exe ] && { IMG="$SRC"; IMPD=""; }
    RELOCS="$O/relocs.txt" IMPOUT="$G/impargs.c" SYMPFX="$PFX" IMPDLL="$IMPD" \
      "$PY" tools/argcount.py "$IMG" > "$O/argcount.log" 2>&1 || { tail -20 "$O/argcount.log"; echo "gen.sh: argcount.py failed for $MOD"; exit 1; }
    grep -E '^-> |GUESSED' "$O/argcount.log" | sed "s/^/  $MOD: /"
    sed -i "s/^lifter .*/$SOURCES/" "$G/stamp.txt"   # the import tables are not part of the lift
    continue
  fi
  mkdir -p "$G" "$O"; find "$G" -mindepth 1 -delete
  echo "incomplete: gen.sh has not finished (or failed)" > "$G/stamp.txt"
  T0=$(date +%s)
  if [ "$MOD" = exe ]; then
    # the exe stays at its own base; it has no .reloc, so its pointers are scanned
    IMG="$SRC"
    "$PY" tools/ptrscan.py "$SRC" "$O" "$G" "$PFX" | sed "s/^/  $MOD: /"
  else
    IMG="$O/image.dll"
    "$PY" tools/prepare_module.py "$SRC" "$BASE" "$O" "$G" "$PFX" > "$O/prepare.log"
    sed -E 's/, exports .*//; s/^/  '"$MOD"': /' "$O/prepare.log"
  fi
  GENOUT="$G" RELOCS="$O/relocs.txt" SEEDS="$O/seeds.txt" SYMPFX="$PFX" \
    "$PY" tools/genall.py "$IMG" "$NSH" > "$O/genall.log" 2>&1 || { tail -20 "$O/genall.log"; echo "gen.sh: genall.py failed for $MOD"; exit 1; }
  grep -E "^  [0-9]+ functions|functions  *:|C source size|traps \(|truncated|MISSING|stubs\)" "$O/genall.log" | sed "s/^ */  $MOD: /"
  IMPD="$SRC"; [ "$MOD" = exe ] && IMPD=""          # a DLL's import table is read from the unrelocated file
  RELOCS="$O/relocs.txt" IMPOUT="$G/impargs.c" SYMPFX="$PFX" IMPDLL="$IMPD" \
    "$PY" tools/argcount.py "$IMG" > "$O/argcount.log" 2>&1 || { tail -20 "$O/argcount.log"; echo "gen.sh: argcount.py failed for $MOD"; exit 1; }
  { echo "# gen.sh $(date -u +%Y-%m-%dT%H:%M:%SZ): $MOD lifted from $FILE"
    echo "$SOURCES"
    echo "input $FILE $SHA base $BASE"
    echo "options X87PC=$X87PC X87OPT=$X87OPT JTSWITCH=$JTSWITCH LREGS=$LREGS NSHARD=$NSH"; } > "$G/stamp.txt"
  echo "  $MOD: gen OK -> $G ($(ls "$G"/shard_*.c | wc -l) shards, $(( $(date +%s) - T0 )) s)"
done

# gen/modtab.c: the lifted modules for runtime/module.c, in this table's order
# (the exe first).  A module gen.sh has not lifted (yet) is left out; the
# runtime then refuses to load it.
{ echo "/* generated by gen.sh: the lifted modules (runtime/module.c) */"
  echo '#include "module.h"'
  N=0; ENTRIES=""
  while read -r MOD FILE BASE PFX NSH SHA; do
    [ -n "$MOD" ] && [ -f "gen/$MOD/fntab.c" ] && ! grep -q '^incomplete' "gen/$MOD/stamp.txt" || continue
    echo "extern const ModInfo g_${PFX}modinfo; extern const ImpEnt g_${PFX}imp[]; extern const uint32_t g_${PFX}imp_count;"
    echo "extern const uint32_t g_${PFX}fn_addr[]; extern const mod_fn g_${PFX}fn_ptr[]; extern const uint32_t g_${PFX}fn_count;"
    ENTRIES="$ENTRIES  { \"$MOD\", &g_${PFX}modinfo, g_${PFX}imp, &g_${PFX}imp_count, g_${PFX}fn_addr, g_${PFX}fn_ptr, &g_${PFX}fn_count },\n"
    N=$((N + 1))
  done <<< "$MODULES"
  echo "const ModEntry g_modtab[] = {"; printf "$ENTRIES"; echo "};"
  echo "const int g_modtab_count = $N;"
} > gen/modtab.c
echo "gen.sh: gen/modtab.c ($N modules)"
