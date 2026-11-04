# WC3 v prohlížeči na QNAP (zkušební nasazení)

Server je tentýž Node server jako při vývoji (`web/tools/wc3-dev-server.mjs`): stránky, `wc3.wasm`, herní data
(Range), virtuální LAN switch na `/lan`, hlavičky COOP/COEP. Image neobsahuje kód ani data hry, ty jsou na
datovém svazku NASu.

## Na PC (WSL)

    ./build.sh && tools/pack-qnap.sh

vznikne `out/qnap/wc3-web-trial/` a `out/qnap/module/wc3.wasm`.

## Na NAS

Složky na datovém svazku (ne na systémovém disku):

    /share/CACHEDEV1_DATA/wc3/game      složka Warcraft III (war3.exe, game.dll, Storm.dll, Mss32.dll, *.mpq, Maps, redist, ...)
    /share/CACHEDEV1_DATA/wc3/module    wc3.wasm
    /share/CACHEDEV1_DATA/wc3/logs      logy testovací stránky
    /share/CACHEDEV1_DATA/wc3/server    obsah out/qnap/wc3-web-trial

Pak v `server/`:

    docker compose up -d --build
    docker compose logs -f server

Hra: `http://<NAS>:8086/` funguje jen z NASu samotného nebo přes HTTPS — Chrome povolí WebGPU a vlákna
(SharedArrayBuffer) jen na zabezpečené stránce. Pro hráče tedy Cloudflare Tunnel:

1. Cloudflare Zero Trust → Networks → Tunnels → Create tunnel (cloudflared), zkopírovat token do `.env`
   (`TUNNEL_TOKEN=`).
2. Public hostname: např. `wc3.example.cz`, service `http://server:8086`.
3. `docker compose --profile tunnel up -d`

Kdo zná adresu, může hrát a stáhnout si herní soubory: doporučeno Cloudflare Access (Zero Trust →
Access → Applications) s povolenými e-maily hráčů.

Nová verze: na PC znovu `tools/pack-qnap.sh`, nahrát `wc3-web-trial/` a `wc3.wasm`, `docker compose up -d --build`.
Všichni hráči musí mít stejnou verzi (LAN je lockstep): po nasazení obnovit stránku.
