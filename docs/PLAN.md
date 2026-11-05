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
| start kola („Start“ v nastavení hry) | **opraveno** (2026-11-05) – chyba nevyskočí, zbytek kola ověřit hraním |
| DirectPlay 4A (`runtime/dplay.c`, lokální session) | napsáno, **neověřeno** – hra na jednom PC ho nevolá |
| síťová hra (Připojit se, hráči na více PC) | – |
| ukládání registru, stránka pro hráče, Docker/QNAP | – |

**Start kola – příčina (2026-11-05):** `DirectInputCreateW` měl v `gen/exe/impargs.c` počet argumentů 0 (Wine
`.spec` tabulka nezná dinput, odhad z pushů se netrefil), správně 4. HLE tedy nechalo 16 B argumentů na zásobníku
a `pop edi/esi/ebx` v konstruktoru klávesnice `sub_004121d0` (volaný ze startu kola `sub_00413ce0`) načetly
argumenty místo uložených registrů. `esi` pak neukazoval na herní objekt, pole síťového objektu (`+0x1dc`) bylo
nenulové a zpráva šla „do fronty“ v odpadcích: jednou přetečení streamu, jindy alokace 1 GB. Oprava: počty
argumentů DirectX vstupů (dinput, ddraw, dplayx) ručně v `tools/argcount.py` (`KNOWN`). Žádný import Bulánků už
není odhadnutý. Ladění: `HOOKS=<adr,...> ./gen.sh` + `env=WC3_HOOKDUMP=esi+0x1dc` vypíše registry na adresách.

**Další krok:** zahrát celé kolo (pohyb, střelba, konec kola, statistiky) a sepsat, co nejde. Potom síťová hra:
`dplay.c` má jen lokální session (EnumSessions nic nenajde, Open(JOIN) → `DPERR_NOSESSIONS`); hra po síti
potřebuje přenos mezi prohlížeči (LAN vrstva runtime, `runtime/web/net_lan.c`).

## Ladění

- `EHLOG=1` – každý throw s backtrace, catch, unwind, pokračování.
- `DDLOG=1` / `DDLOG=2` – volání DirectDraw (2: i Blt/BltFast/Lock/Unlock).
- `DPLOG=1` – volání DirectPlay (`dplay.c`); `DPLOG=2` i prázdné `Receive`.
- `REGLOG=1` – registr; `TRACE_MAX=N` (run-node) – každé volání importu.
- Testovací stránka: parametr `env=K=V` předá proměnnou prostředí, `gpu` zapne WebGPU renderer.
