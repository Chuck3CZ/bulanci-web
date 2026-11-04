# Colin McRae Rally 2005 (CZ) v prohlížeči – plán a stav

> **Na začátku každého sezení si přečti tento soubor.** Na konci každého kroku ho aktualizuj, commitni a pushni.

Stejná metoda jako [wc3-web](https://github.com/Chuck3CZ/wc3-web) a nfsu2-web: statická rekompilace x86 → C → wasm,
HLE Win32 / Direct3D 9 / DirectInput 8 / DirectSound, vykreslování přes WebGPU. Repo vzniklo jako kopie wc3-web
(rev 311420a) – runtime, lifter i webová část jsou odtud; `docs/ORIGIN.md` popisuje původ z nfsu2-web.

## Vstupní soubory (CZ vydání, 2004)

| soubor | báze | SHA-256 |
|---|---|---|
| `CMR5.EXE` (7,9 MB, bez .reloc) | 0x00400000 | `749090010147b9c4fb1d88c75ee362691f0a4241ff3b52c12f7f90dd1d57f8f0` |
| `MathCPU.dll` | 0x00BA0000 | `b913baecb125d4458f3183e2593c8390531de54eaf36cce125df46281bd951d6` |
| `MathPIII.dll` | 0x00BB0000 | `6b0a646f1cb556b1f2a5624574c1ed572635be4609244ed0e81d12dcecd7d1de` |
| `Math3dNow.dll` | 0x00BC0000 | `ce41ccda8b820401c55e3906756d939e521859265cb79367b8c6c7d89c832faf` |

Data (2,9 GB): `data/` (Cars, Tracks, Textures, Sounds, `Rally_Shaders/shaders.big`, videa `*.wmv`), `fonts/`,
savy v `SG/`. Nic z toho nesmí do gitu (`.gitignore`).

## Průzkum (2026-11-02)

- Importy exe: `d3d9` (Direct3DCreate9), `DINPUT8` (DirectInput8Create), `DSOUND` (ordinály 1, 2), `WSOCK32`,
  kernel32 (105), user32 (27), gdi32, advapi32, ole32, version. Dynamicky: `Math*.dll` (13 exportů `MathDLL_*`),
  `NETAPI32`, `SHELL32`, `d3d9d.dll`, `libittnotify.dll`, `mscoree.dll`.
- `.text` nešifrovaný (entropie 6,7), žádný StarForce/SecuROM wrapper.
- Sekce `.chntex` (0x00B76000, 0x23C bajtů) + `.chndat` + `.chnrel`: patch českého vydání (převodní tabulky znaků),
  7 skoků/volání z `.text` do ní. Lifter (`tools/lift.py` `code_ranges`) proto bere i spustitelné sekce za mezerou.
- Videa jsou WMV (DirectShow) → zatím `NOVIDEO` na příkazové řádce; později offline převod do WebM.

## Rozložení paměti

- `CMR5.EXE` 0x00400000–0x00B9A000 (.data 5,5 MB, .chnrel), math DLL 0x00BA0000–0x00BD0000.
- TEB/PEB 0x00C00000, zásobníky, halda 0x08000000 – beze změny z wc3-web.
- Dispatch (`runtime/cpu.h`): tabulka HI = 0x00401000–0x00BD0000 (celý kód), LO jen zástupná.

## Stav

| Fáze | Stav |
|---|---|
| 1 – kostra repa z wc3-web, lifter, `gen.sh` | **hotovo** – exe: 7 822 funkcí, 64 shardů, 395 trapů (většinou data dekódovaná jako kód: `aam`, `iretd`) |
| 2 – build + link (`./build.sh`) | **hotovo** – `out/web/wc3.wasm` 23 MB |
| 3 – start pod Node / v Chrome, chybějící Win32 importy | **rozpracováno** – pod Node: okno `PC_CMR5`, D3D9 zařízení, DirectInput, MathPIII.dll, textury, první `DrawIndexedPrimitive`/`EndScene`; pak volání přes NULL (viz níže) |
| 4 – D3D9 (z nfsu2 části runtime), menu | **hotovo** – menu, závody, HUD, zvuk (hudba, motor, spolujezdec), klávesnice |
| 4b – známé chyby | šipka rozpisu v rámečku zvětšená/špatně, mapa při startu hry; viz níže |
| 5 – závod, výkon, DInput8 (klávesnice/gamepad/volant), zvuk | – |
| 6 – videa (WMV → WebM) | – |
| 7 – nasazení na QNAP (jako wc3-web, jiný port) | – |

### Krok 3 – stav (2026-11-02)

`tools/run-node.sh /mnt/c/Users/Chuck/Desktop/colin [COMTRACE=1] [BT=1]` → `out/node-run.log`.

Opraveno cestou:
- CRT `_output` (`printf`): tabulka skoků `jmp [eax*4+0x5b4ec9]` za `cmp eax, ecx` – JTSWITCH ji nebere,
  `ptrscan.py` teď přidá položky takových tabulek jako sloty → cíle jsou funkce.
- `GetDeviceCaps` mazal 0x150 bajtů místo 304 (sizeof D3DCAPS9) → přepsal CMR uložené registry na zásobníku
  (`ebp`, `esi`, `edi` = 0 po `sub_00588850`).
- `MathPIII.dll` – hra kontroluje verzi 1.06 přes VERSION.dll → `GetFileVersionInfoSizeA/GetFileVersionInfoA/
  VerQueryValueA` (resource RT_VERSION ze souboru).
- Cesty: hra skládá `C:\Program Files\Codemasters\Colin McRae Rally 2005\...` → to je teď virtuální složka hry.
- `DSOUND` ordinál 2 (`DirectSoundEnumerateA`) neměl počet argumentů → zásobník o 8 B mimo → /GS „Buffer overrun“.

**Další:** po prvním `EndScene` `sub_00583800` volá `GetSurfaceLevel` (vtable+0x48) na textuře s NULL na +0x18
(objekt z `sub_004647f0(0)`, volající `sub_00466c30`). Větev závisí na `[0xb674c0]` = render-to-texture z caps
(`sub_00588850` @0x588e90: DevCaps2 & 0x10 a DeclTypes 0x200 / 0x2020000). Zjistit, co textura měla obsahovat
(CreateTexture s RENDERTARGET? D3DX?). Pak testovací stránka (`hostcheck`) v Chrome, vlákna, zvuk.

### Krok 4 – stav (2026-11-02)

Hra běží v Chrome (`hostcheck` na portu 8087) od úvodního dema přes menu po závod se zvukem. Opraveno:
TextureCaps bez SQUAREONLY, proměnné prostředí (Intel OpenMP), velikost zásobníku `CreateThread`, 4 CPU,
DirectInput klávesnice (`runtime/dinput_kbd.c`), zařízení v rozlišení, které hra chce (HUD), `DirectSoundEnumerateA`
se zařízením s GUID, `IDirectSoundNotify`/`3DBuffer`/`3DListener` (IKsPropertySet odmítnut), notifikace z mixéru,
`GetStatus` z mixéru, `MsgWaitForMultipleObjects`, `CompareString`, 1024 zvukových slotů, viewport u shaderů,
`SetRenderTarget` resetuje viewport i scissor, scissor test, **`DrawPrimitive`** (chyběl úplně: trasa, body,
stínový jezdec), fixed function s vertex declaration (POSITION přes WVP).

**Otevřené:** šipka spolujezdce v rámečku je pořád zvětšená (rámeček z atlasu HUD je správně, šipka je 3D model –
kreslení ještě neodchyceno; ladicí přepínače `BOXLOG=x0,y0,x1,y1`, `BOXDUMP=0`, `RTDRAWLOG=1`, `VPLOG=1`, `TEXID=`,
`TEXDUMP=1`; dekodér `[texdump]` → PNG byl ve scratchpadu), mapa při startu hry. Videa WMV (`NOVIDEO`).

### Nasazení na QNAP (2026-11-02)

`\QNAP-TVs\Public\Colin`: `game/` (data), `module/wc3.wasm`, `logs/`, `cmr05-web-trial.tar` (image z Docker
Desktop), `container-station.yml` (port 8087). Postup jako u wc3-web: `tools/pack-qnap.sh`, `docker build` ve Windows
(kontext zkopírovat z WSL), `docker save` na share, Container Station: Images → Import, Applications → Create.

## Příkazy

```sh
. tools/env.sh
./gen.sh /mnt/c/Users/Chuck/Desktop/colin     # lift -> gen/
./build.sh                                     # -> out/web/wc3.wasm
```

## Zbytky WC3, které je třeba projít

- `runtime/web/main_wc3.c`, `win32.c` (příkazová řádka `CMR5.EXE NOVIDEO`), `fileio.c` (`C:\Program Files\Codemasters\Colin McRae Rally 2005`).
- `runtime/module.c` – `resolve_module` hledá i v `redist\miles` (neškodí).
- `runtime/com.c` – NFSU2 ladicí háčky na pevných adresách (jen s env proměnnými), D3D8 vrstva (`d3d8.c`) CMR nepotřebuje.
- `web/tools/wc3-dev-server.mjs`, testovací stránka – názvy a výchozí cesty.
