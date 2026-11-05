# Bulánci 1.80 v prohlížeči – plán a stav

> **Na začátku každého sezení si přečti tento soubor.** Na konci každého kroku ho aktualizuj, commitni a pushni.

## Vstup

`bulanci.exe` 1.80 (SleepTeam Labs, 2002), 22 445 318 B, SHA-256 `2992cca1…6648e39` (celý v `gen.sh`).
Jeden 32bit PE bez DLL a bez `.reloc` (báze 0x400000), `.text` 504 KB. Data hry jsou za koncem PE jako overlay
(`GZIP=` + zlib bloky po 32 KB); hra si je čte sama přes `GetModuleFileNameW` + `CreateFileW`.
Unicode program (W varianty Win32), DirectDraw 1, DirectInput (`DirectInputCreateW`), DirectSound a DirectPlay 4A
přes `CoCreateInstance`, registr `HKCU\Software\SleepTeam\Bulanci`. Řízení chyb přes C++ výjimky (`CDSException`,
`CDSRegKeyException`, `CDSApiException`, `CDSSimpleException`).

## Stav (2026-11-05)

| Krok | Stav |
|---|---|
| lift + build | **hotovo** – 5 524 funkcí, 8 shardů, wasm 6 MB |
| C++ výjimky (`runtime/eh.c`, `tools/lift.py eh_conts`) | **hotovo** – catch, unwind, rethrow |
| W→A vrstva, CP1250 (`runtime/w2a.c`), W registr (`win32_ext.c regw_call`) | **hotovo** |
| DirectDraw 1 (`runtime/ddraw.c`) | **hotovo** – 800×600×32, Blt/BltFast, color key, Lock, Flip |
| DirectSound přes `CoCreateInstance`, `DirectInputCreateW` | napojeno na com.c |
| hlavní menu a nastavení hry v Chrome | **funguje** |
| start kola („Hrát“) | **nefunguje** – viz níže |
| DirectPlay 4A, síťová hra | – |
| ukládání registru, stránka pro hráče, Docker/QNAP | – |

**Další krok:** po „Hrát“ hra hlásí „Došlo k přetečení dat v zásobníku nebo ve frontě“ – `CDSSimpleException`
z `sub_0043c450` (zápis do paměťového streamu s pevnou kapacitou, volá `sub_0043b680`). Podezření: hra i pro hru na
jednom počítači používá síťovou vrstvu (`CDSDirectPlay`) a `CoCreateInstance(CLSID_DirectPlay)` vrací
`REGDB_E_CLASSNOTREG`. Ověřit (kdo plní stream, `EHLOG=1`), pak `IDirectPlay4A` s lokální session.

## Ladění

- `EHLOG=1` – každý throw s backtrace, catch, unwind, pokračování.
- `DDLOG=1` / `DDLOG=2` – volání DirectDraw (2: i Blt/BltFast/Lock/Unlock).
- `REGLOG=1` – registr; `TRACE_MAX=N` (run-node) – každé volání importu.
- Testovací stránka: parametr `env=K=V` předá proměnnou prostředí, `gpu` zapne WebGPU renderer.
