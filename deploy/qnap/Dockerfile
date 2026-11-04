# CMR 2005 in the browser, the trial server: the Node server the tests run (web/tools/wc3-dev-server.mjs) with the
# built pages.  The image holds no game code or data: the game folder and the game module (wc3.wasm) are
# mounted (compose.yml).  Built from the bundle tools/pack-qnap.sh makes (its folder is the build context).
FROM node:24-alpine
WORKDIR /app
COPY app/ /app/
RUN cd /app/web && npm install --omit=dev --no-audit --no-fund ws@8.22.0 && mkdir -p /app/out
USER node
EXPOSE 8086
CMD ["node", "/app/web/tools/wc3-dev-server.mjs", "--data", "/game", "--wasm", "/module/wc3.wasm", "--out", "/logs", "--port", "8086", "--host", "0.0.0.0"]
