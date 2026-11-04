// Node's https server with the Node server's TLS rules (certNames, sniCallback, a default context only for a
// certificate that names an address, minVersion, handshakeTimeout, onClientError): the reference for how a handshake
// the server refuses looks on the wire (tests/tls.rs).
//   node tls_server.mjs <cert> <key>     prints "listening <port>" once it is up (on 127.0.0.1, port 0)
import https from 'node:https';
import tls from 'node:tls';
import fs from 'node:fs';
import crypto from 'node:crypto';

const [certFile, keyFile] = process.argv.slice(2);
const key = fs.readFileSync(keyFile), cert = fs.readFileSync(certFile);
const x = new crypto.X509Certificate(cert);
const dns = [], ips = [];
for (const part of String(x.subjectAltName ?? '').split(/,\s*/)) {
  if (part.startsWith('DNS:')) dns.push(part.slice(4).toLowerCase());
  else if (part.startsWith('IP Address:')) ips.push(part.slice(11));
}
const certCtx = tls.createSecureContext({ key, cert, minVersion: 'TLSv1.2' });
const sniMatch = (name, list) => list.some(d => d === name || (d.startsWith('*.') && name.endsWith(d.slice(1)) && !name.slice(0, -(d.length - 1)).includes('.')));
function sniCallback(servername, cb) {
  const n = String(servername ?? '').toLowerCase().replace(/\.$/, '');
  if (sniMatch(n, dns)) return cb(null, certCtx);
  cb(new Error('not a name of this server'));
}
const tlsFiles = { SNICallback: sniCallback, ...(ips.length ? { key, cert } : {}) };
const srv = https.createServer({ ...tlsFiles, minVersion: 'TLSv1.2', handshakeTimeout: 15_000 }, (q, s) => s.end('ok'));
// onClientError: no HTTP to answer a handshake with, the connection goes at once
srv.on('clientError', (e, sock) => sock.destroy());
srv.listen(0, '127.0.0.1', () => console.log(`listening ${srv.address().port}`));
process.on('SIGTERM', () => process.exit(0));
