# Bulánci v prohlížeči na QNAP

Server je tentýž Node server jako při vývoji (`web/tools/wc3-dev-server.mjs`): stránky, `wc3.wasm`, herní data
(Range), virtuální LAN switch na `/lan`, hlavičky COOP/COEP. Image neobsahuje kód ani data hry, ty jsou ve
sdílené složce NASu.

## Na PC (WSL)

    ./build.sh && tools/pack-qnap.sh

vznikne `out/qnap/bulanci-web/` (server) a `out/qnap/module/wc3.wasm`.

## Na NAS

Složky ve sdílené složce Public (z Windows `\\192.168.16.16\Public\Bulanci`):

    /share/Public/Bulanci/game      bulanci.exe (1.80, data jsou v něm)
    /share/Public/Bulanci/module    wc3.wasm
    /share/Public/Bulanci/logs      logy testovací stránky (zapisovatelné pro kontejner)
    /share/Public/Bulanci/server    obsah out/qnap/bulanci-web

Pak přes SSH (`ssh Martin@192.168.16.16`):

    export PATH=$PATH:/share/CACHEDEV1_DATA/.qpkg/container-station/bin
    cd /share/Public/Bulanci/server && chmod 777 ../logs && docker compose up -d --build
    docker compose logs -f server

Hra: `http://192.168.16.16:8088/`. Chrome povolí WebGPU a vlákna (SharedArrayBuffer) jen na zabezpečené stránce,
takže po síti přes http to samo nepůjde. Na zkoušku v LAN: `chrome://flags/#unsafely-treat-insecure-origin-as-secure`
s `http://192.168.16.16:8088`, restart Chrome. Pro hráče zvenku Cloudflare Tunnel:

1. Cloudflare Zero Trust → Networks → Tunnels → Create tunnel (cloudflared), token do `.env` (`TUNNEL_TOKEN=`).
2. Public hostname: např. `bulanci.example.cz`, service `http://server:8088`.
3. `docker compose --profile tunnel up -d`

Kdo zná adresu, může hrát a stáhnout si herní soubor: doporučeno Cloudflare Access s povolenými e-maily hráčů.

Nová verze: na PC znovu `./build.sh && tools/pack-qnap.sh`, nahrát `bulanci-web/` do `server/` a `wc3.wasm` do
`module/`, na NASu `docker compose up -d --build`, v prohlížeči obnovit stránku.
