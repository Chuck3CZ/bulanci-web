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
| DirectPlay 4A (`runtime/dplay.c`) | lokální session + virtuální LAN |
| síťová hra (Připojit se, hráči na více PC) | **rozpracováno** – hledání a připojení jde, hostitel odmítne (viz níže) |
| ukládání registru, stránka pro hráče, Docker/QNAP | – |

**Start kola – příčina (2026-11-05):** `DirectInputCreateW` měl v `gen/exe/impargs.c` počet argumentů 0 (Wine
`.spec` tabulka nezná dinput, odhad z pushů se netrefil), správně 4. HLE tedy nechalo 16 B argumentů na zásobníku
a `pop edi/esi/ebx` v konstruktoru klávesnice `sub_004121d0` (volaný ze startu kola `sub_00413ce0`) načetly
argumenty místo uložených registrů. `esi` pak neukazoval na herní objekt, pole síťového objektu (`+0x1dc`) bylo
nenulové a zpráva šla „do fronty“ v odpadcích: jednou přetečení streamu, jindy alokace 1 GB. Oprava: počty
argumentů DirectX vstupů (dinput, ddraw, dplayx) ručně v `tools/argcount.py` (`KNOWN`). Žádný import Bulánků už
není odhadnutý. Ladění: `HOOKS=<adr,...> ./gen.sh` + `env=WC3_HOOKDUMP=esi+0x1dc` vypíše registry na adresách.

**Síťová hra – rozpracováno (2026-11-05, stav při přerušení):**

`runtime/dplay.c` má DirectPlay nad virtuální LAN (jeden UDP socket, port 47624, přes `vlan_*` z `net_lan.c`;
`winsock_dplay_up()` v `winsock.c` zapne LAN, protože hra sama `WSAStartup` nevolá). Protokol: ENUMREQ/ENUMREP
(hledání), JOIN/JOINACK, PADD/PDEL (hráči), DATA (zprávy, i po kusech), LEAVE. Hra ze systémových zpráv čte jen
DESTROYPLAYER, SESSIONLOST a HOST (`sub_00415290`), připojování si dělá vlastními zprávami (typ 0 žádost,
typ 1 odpověď).

Ověřeno ve dvou záložkách (stránka s `?net=<jméno>`): hledání najde hru, `Open(JOIN)`, `CreatePlayer`, zprávy
tečou oběma směry. **Hostitel ale klienta odmítne** (odpověď typ 1, první ID hráče 0, důvod 2 = žádný společný
level). Příčina nalezena, neopravena: žádost klienta nese 6 GUID levelů jako ANSI řetězce po 36 B (od offsetu
0x2a), hostitel je převádí na CStringW přes `sub_00401d00` → `sub_00401290(str, [0x4afbfc]())` a z
`"987EF758-…"` vznikne jen `"9"` (hook na `0x41549e`, `WC3_HOOKDUMP=ecx+0,eax+0`). Hostitelův seznam je
správně (6 GUID v UTF-16). Chyba je tedy v převodu ANSI → UTF-16 (nejspíš naše HLE `MultiByteToWideChar` nebo
to, co vrací `[0x4afbfc]`), ne v síti.

**Další krok:** zjistit, co je `[0x4afbfc]` a co `sub_00401290` volá, opravit převod, pak znovu: hostitel
Vytvořit → 2 hráči / 1 na PC → Dále → **Start** (až pak má vybrané levely) → klient Připojit se → 1 hráč na PC
→ Dále → OK (prázdné = hledat) → vybrat hru → Připojit. Pozor: `gen/` je teď liftnutý s ladicími hooky
(`HOOKS=41549e,415480`), před nasazením `./gen.sh` bez `HOOKS` a `./build.sh`. Rizika: UDP přes switch se může
ztratit při plné frontě; `Open(JOIN)` čeká až 5 s s drženým zámkem hosta.

Pak: zahrát celé kolo na jednom PC i po síti a sepsat, co nejde.

## Ladění

- `EHLOG=1` – každý throw s backtrace, catch, unwind, pokračování.
- `DDLOG=1` / `DDLOG=2` – volání DirectDraw (2: i Blt/BltFast/Lock/Unlock).
- `DPLOG=1` – volání DirectPlay (`dplay.c`); `DPLOG=2` i prázdné `Receive`.
- `REGLOG=1` – registr; `TRACE_MAX=N` (run-node) – každé volání importu.
- Testovací stránka: parametr `env=K=V` předá proměnnou prostředí, `gpu` zapne WebGPU renderer.
