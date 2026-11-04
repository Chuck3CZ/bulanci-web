//! TLS with a certificate given to the server (--tls-cert/--tls-key): rustls, TLS 1.2 and 1.3.  The handshake goes
//! through only for a server name (SNI) the certificate covers (exact, or "*.zone" for one label), so a scanner that
//! connects to the bare address, or asks for another name, never sees the certificate or the domain in it (the
//! audit's F4); a certificate that names an IP address is shown for handshakes without a name too (that is how
//! browsers ask for an IP).  `reload()` reads the files again (a renewed certificate, picked up without a restart).
//!
//! A refused handshake looks as Node's did: without a name, OpenSSL had no certificate and sent a fatal
//! handshake_failure alert; for a name the SNI callback refused, the socket was destroyed without a byte.  (rustls
//! itself would send access_denied for both, which tells a scanner that a name is what it lacks.)  An SNI that is an
//! IP address counts as none (rustls's reading; Node's callback refused it as another name).
//!
//! With a client CA (`load_with_client_ca`), a peer the service names (`Service::client_certificate`: a proxy at
//! addresses others connect from too, Cloudflare's) must show a client certificate that CA signed, or its handshake
//! fails; nobody else is asked for one.  Its handshakes have session tickets and a session cache of their own, so a
//! session made without a certificate never resumes as one that showed it.

use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use arc_swap::ArcSwap;
use rustls::server::{
    Acceptor, ClientHello, ProducesTickets, ResolvesServerCert, ServerSessionMemoryCache, StoresServerSessions, WebPkiClientVerifier,
};
use rustls::sign::CertifiedKey;
use rustls::{RootCertStore, ServerConfig, pki_types::CertificateDer};
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;
use tokio_rustls::StartHandshake;
use tokio_rustls::server::TlsStream;

/// The names a certificate is for (its subjectAltName): DNS names in lower case, and IP addresses (bare, as
/// nfsu2_headers::bare_ip gives them).
#[derive(Clone, Debug, Default)]
pub struct CertNames {
    pub dns: Vec<String>,
    pub ips: Vec<String>,
}

/// The certificate in use, swapped by `reload()` while connections come in.
pub struct TlsState {
    cert: PathBuf,
    key: PathBuf,
    /// what signs the client certificates of the peers that must show one (the crate doc)
    client_ca: Option<Arc<RootCertStore>>,
    current: ArcSwap<Current>,
    /// kept across reloads: a client resumes its session after a renewal too (each kind of handshake its own)
    tickets: Tickets,
    proved_tickets: Tickets,
}

/// the session tickets and the session cache of one kind of handshake
#[derive(Clone)]
struct Tickets {
    tickets: Arc<dyn ProducesTickets>,
    sessions: Arc<dyn StoresServerSessions>,
}

impl Tickets {
    fn new() -> Result<Tickets, String> {
        Ok(Tickets {
            tickets: rustls::crypto::ring::Ticketer::new().map_err(|e| e.to_string())?,
            sessions: ServerSessionMemoryCache::new(SESSION_CACHE),
        })
    }
}

/// A certificate's context and the names its handshakes are for, swapped as one.
struct Current {
    config: Arc<ServerConfig>,
    /// the same, for a peer that must show a client certificate (with a client CA only)
    proved: Option<Arc<ServerConfig>>,
    dns: Vec<String>,
    /// it names an address: shown to handshakes without a name too
    bare: bool,
}

/// sessions a reconnecting client (a page's six connections, Cloudflare's) may resume without a full handshake
const SESSION_CACHE: usize = 4096;

/// a fatal handshake_failure alert in a TLS 1.2 record: what Node (OpenSSL) sent for a handshake it had no
/// certificate for
const HANDSHAKE_FAILURE: [u8; 7] = [0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x28];

impl TlsState {
    /// Reads and checks both files.  Err: why they cannot be used ("<cert> names no host (no subjectAltName)", a
    /// broken file), for the server's "the certificate files cannot be used: ..."
    pub fn load(cert: &Path, key: &Path) -> Result<(std::sync::Arc<TlsState>, CertNames), String> {
        Self::load_with(cert, key, None)
    }
    /// `load`, and a peer the service names (`Service::client_certificate`) must show a client certificate that
    /// `client_ca` (PEM: one certificate or more) signed (the crate doc)
    pub fn load_with_client_ca(cert: &Path, key: &Path, client_ca: &[u8]) -> Result<(std::sync::Arc<TlsState>, CertNames), String> {
        let mut roots = RootCertStore::empty();
        for c in rustls_pemfile::certs(&mut &client_ca[..]) {
            roots.add(c.map_err(|e| format!("the client CA: {e}"))?).map_err(|e| format!("the client CA: {e}"))?;
        }
        if roots.is_empty() {
            return Err("the client CA: no certificate in it".into());
        }
        Self::load_with(cert, key, Some(Arc::new(roots)))
    }
    fn load_with(cert: &Path, key: &Path, client_ca: Option<Arc<RootCertStore>>) -> Result<(std::sync::Arc<TlsState>, CertNames), String> {
        let (tickets, proved_tickets) = (Tickets::new()?, Tickets::new()?);
        let (current, names) = build(cert, key, client_ca.as_ref(), &tickets, &proved_tickets)?;
        let state = TlsState {
            cert: cert.to_path_buf(),
            key: key.to_path_buf(),
            client_ca,
            current: ArcSwap::from_pointee(current),
            tickets,
            proved_tickets,
        };
        Ok((Arc::new(state), names))
    }
    /// The files again; Ok: the new names (the server's host check takes them).  On Err the old certificate stays.
    pub fn reload(&self) -> Result<CertNames, String> {
        let (current, names) = build(&self.cert, &self.key, self.client_ca.as_ref(), &self.tickets, &self.proved_tickets)?;
        self.current.store(Arc::new(current));
        Ok(names)
    }
    /// the rustls acceptor of the current certificate (with the SNI rule above, but rustls's own refusals)
    pub fn acceptor(&self) -> tokio_rustls::TlsAcceptor {
        tokio_rustls::TlsAcceptor::from(self.current.load().config.clone())
    }

    /// The handshake of a new connection, refused as Node refused it (above): the ClientHello first, then the rest
    /// only for a name the certificate covers.  `prove`: the peer must show a client certificate the client CA
    /// signed (refused without a client CA).  None: refused, or broken (the connection goes).  The caller bounds the
    /// time (`handshake_timeout`, for both parts).
    pub async fn handshake(&self, mut tcp: TcpStream, prove: bool) -> Option<TlsStream<TcpStream>> {
        let accepted = client_hello(&mut tcp).await?;
        let current = self.current.load_full();
        let config = if prove { current.proved.clone()? } else { current.config.clone() };
        let ok = match accepted.client_hello().server_name() {
            Some(name) => covers(&current.dns, name),
            None if current.bare => true,
            None => {
                let _ = tcp.write_all(&HANDSHAKE_FAILURE).await;
                return None;
            }
        };
        if !ok {
            return None;
        }
        StartHandshake::from_parts(accepted, tcp).into_stream(config).await.ok()
    }
}

/// a non-blocking read, for rustls's reading of the ClientHello
struct TryRead<'a>(&'a TcpStream);
impl io::Read for TryRead<'_> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.0.try_read(buf)
    }
}

/// The ClientHello, as rustls reads it.  One it cannot read gets rustls's alert, except a server name that cannot be
/// one (rustls's illegal_parameter): Node's SNI callback refused that as any other name, without a byte.
async fn client_hello(tcp: &mut TcpStream) -> Option<rustls::server::Accepted> {
    let mut acceptor = Acceptor::default();
    loop {
        tcp.readable().await.ok()?;
        match acceptor.read_tls(&mut TryRead(tcp)) {
            Ok(0) => return None,
            Ok(_) => {}
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => continue,
            Err(_) => return None,
        }
        match acceptor.accept() {
            Ok(Some(accepted)) => return Some(accepted),
            Ok(None) => {}
            Err((rustls::Error::PeerMisbehaved(rustls::PeerMisbehaved::ServerNameMustContainOneHostName), _)) => return None,
            Err((_, mut alert)) => {
                let mut out = Vec::new();
                if alert.write_all(&mut out).is_ok() {
                    let _ = tcp.write_all(&out).await;
                }
                return None;
            }
        }
    }
}

/// the key, the certificate, its names (none: refused), the context (and with a client CA, the context that asks for
/// a client certificate)
fn build(
    cert: &Path,
    key: &Path,
    client_ca: Option<&Arc<RootCertStore>>,
    tickets: &Tickets,
    proved_tickets: &Tickets,
) -> Result<(Current, CertNames), String> {
    let key_pem = std::fs::read(key).map_err(|e| fs_message(&e, key))?;
    let cert_pem = std::fs::read(cert).map_err(|e| fs_message(&e, cert))?;
    let chain: Vec<CertificateDer<'static>> =
        rustls_pemfile::certs(&mut &cert_pem[..]).collect::<Result<_, _>>().map_err(|e| format!("{}: {e}", cert.display()))?;
    let first = chain.first().ok_or_else(|| format!("{}: no certificate in the file", cert.display()))?;
    let names = cert_names(first).map_err(|e| format!("{}: {e}", cert.display()))?;
    if names.dns.is_empty() && names.ips.is_empty() {
        return Err(format!("{} names no host (no subjectAltName)", cert.display()));
    }
    let key_der = rustls_pemfile::private_key(&mut &key_pem[..])
        .map_err(|e| format!("{}: {e}", key.display()))?
        .ok_or_else(|| format!("{}: no private key in the file (PKCS#8, RSA or EC PEM)", key.display()))?;
    let signer = rustls::crypto::ring::sign::any_supported_type(&key_der).map_err(|e| format!("{}: {e}", key.display()))?;
    let certified = CertifiedKey::new(chain, signer);
    // as OpenSSL's "key values mismatch"
    certified.keys_match().map_err(|e| format!("the key does not belong to the certificate: {e}"))?;
    let resolver = Arc::new(Resolver { key: Arc::new(certified), dns: names.dns.clone(), bare: !names.ips.is_empty() });
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let versions = ServerConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(&[&rustls::version::TLS13, &rustls::version::TLS12])
        .map_err(|e| e.to_string())?;
    let finish = |mut config: ServerConfig, t: &Tickets| {
        // Node's https server: ALPN http/1.1
        config.alpn_protocols = vec![b"http/1.1".to_vec()];
        config.ticketer = t.tickets.clone();
        config.session_storage = t.sessions.clone();
        Arc::new(config)
    };
    let config = finish(versions.clone().with_no_client_auth().with_cert_resolver(resolver.clone()), tickets);
    let proved = match client_ca {
        Some(roots) => {
            let verifier =
                WebPkiClientVerifier::builder_with_provider(roots.clone(), provider).build().map_err(|e| format!("the client CA: {e}"))?;
            Some(finish(versions.with_client_cert_verifier(verifier).with_cert_resolver(resolver), proved_tickets))
        }
        None => None,
    };
    let current = Current { config, proved, dns: names.dns.clone(), bare: !names.ips.is_empty() };
    Ok((current, names))
}

/// The handshake's certificate: only for a server name it covers; without a name only if it names an address.  (The
/// same rule as `handshake`'s, which refuses before rustls is asked: this one is what a handshake through
/// `acceptor()` has, and a second guard.)
#[derive(Debug)]
struct Resolver {
    key: Arc<CertifiedKey>,
    dns: Vec<String>,
    bare: bool,
}

impl ResolvesServerCert for Resolver {
    fn resolve(&self, hello: ClientHello<'_>) -> Option<Arc<CertifiedKey>> {
        match hello.server_name() {
            Some(name) => covers(&self.dns, name).then(|| self.key.clone()),
            None => self.bare.then(|| self.key.clone()),
        }
    }
}

/// the SNI rule: the name in lower case, without a trailing dot, then sni_match
fn covers(dns: &[String], name: &str) -> bool {
    let mut n = name.to_ascii_lowercase();
    if n.ends_with('.') {
        n.pop();
    }
    sni_match(&n, dns)
}

/// a name of the certificate, or under one of its "*.zone" names by one label
pub(crate) fn sni_match(name: &str, list: &[String]) -> bool {
    list.iter().any(|d| {
        if d == name {
            return true;
        }
        match d.strip_prefix('*') {
            Some(zone) if d.starts_with("*.") => {
                name.len() >= zone.len() && name.ends_with(zone) && !name[..name.len() - zone.len()].contains('.')
            }
            _ => false,
        }
    })
}

/// the certificate's names: its subjectAltName as Node's X509Certificate wrote it ("DNS:a, IP Address:1.2.3.4"),
/// read back the way the Node server read it
pub(crate) fn cert_names(der: &CertificateDer<'_>) -> Result<CertNames, String> {
    let (_, x) = x509_parser::parse_x509_certificate(der.as_ref()).map_err(|e| format!("not a certificate: {e}"))?;
    let mut names = CertNames::default();
    let san = x.subject_alternative_name().map_err(|e| format!("its subjectAltName cannot be read: {e}"))?;
    let Some(san) = san else { return Ok(names) };
    for g in &san.value.general_names {
        match g {
            x509_parser::extensions::GeneralName::DNSName(d) => names.dns.push(node_dns(d).to_lowercase()),
            x509_parser::extensions::GeneralName::IPAddress(b) => {
                if let Some(ip) = node_ip(b) {
                    names.ips.push(ip);
                }
            }
            _ => {}
        }
    }
    Ok(names)
}

/// a DNS name as Node prints it in subjectAltName: as it is, or JSON-quoted when it holds a character that would make
/// the list ambiguous (quotes, backslash, comma, a control: Node's PrintAltName)
fn node_dns(d: &str) -> String {
    let safe = d.bytes().all(|b| (0x20..=0x7e).contains(&b) && !matches!(b, b'"' | b'\\' | b',' | b'\''));
    if safe {
        return d.to_string();
    }
    let mut s = String::from("\"");
    for b in d.bytes() {
        match b {
            b'"' | b'\\' => {
                s.push('\\');
                s.push(b as char);
            }
            0x20..=0x7e if b != b',' => s.push(b as char),
            // (Node formats the byte as a signed char: bytes from 0x80 are ffffffxx)
            _ => s.push_str(&format!("\\u{:04x}", b as i8 as i32 as u32)),
        }
    }
    s.push('"');
    s
}

/// an IP address as Node prints it ("1.2.3.4", or eight groups "0:0:0:0:0:0:0:1"), then as nfsu2_headers::bare_ip
/// gives it (lower case)
fn node_ip(b: &[u8]) -> Option<String> {
    match b.len() {
        4 => Some(format!("{}.{}.{}.{}", b[0], b[1], b[2], b[3])),
        16 => Some(b.chunks(2).map(|g| format!("{:x}", u16::from(g[0]) << 8 | u16::from(g[1]))).collect::<Vec<_>>().join(":")),
        // Node: "<invalid length=N>", not an address
        _ => None,
    }
}

/// Node's message for a file it cannot read: "ENOENT: no such file or directory, open '<path>'"
fn fs_message(e: &io::Error, path: &Path) -> String {
    let (code, text) = match e.raw_os_error() {
        Some(libc::ENOENT) => ("ENOENT", "no such file or directory"),
        Some(libc::EACCES) => ("EACCES", "permission denied"),
        Some(libc::EISDIR) => return "EISDIR: illegal operation on a directory, read".to_string(),
        _ => return format!("{e}, open '{}'", path.display()),
    };
    format!("{code}: {text}, open '{}'", path.display())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn l(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn sni_matches_a_name_or_one_label_under_a_wildcard() {
        let names = l(&["game.test", "*.wild.test"]);
        assert!(sni_match("game.test", &names));
        assert!(sni_match("a.wild.test", &names));
        assert!(!sni_match("a.b.wild.test", &names));
        assert!(!sni_match("wild.test", &names));
        assert!(!sni_match("scan.invalid", &names));
        assert!(!sni_match("xgame.test", &names));
        // (an empty label: what the Node server's rule says too)
        assert!(sni_match(".wild.test", &names));
    }

    /// the Node server's sniMatch itself, over names built on the edges of the rule
    #[test]
    fn sni_as_the_js_matches_it() {
        let labels = ["a", "b", "game", "test", "wild", "x-y", "", "*"];
        let mut rng = crate::node::Rng(11);
        let name = |rng: &mut crate::node::Rng| (0..1 + rng.below(4)).map(|_| rng.pick(&labels)).collect::<Vec<_>>().join(".");
        let cases: Vec<(String, Vec<String>)> =
            (0..3000).map(|_| (name(&mut rng), (0..rng.below(4)).map(|_| name(&mut rng)).collect())).collect();
        let input = serde_json::to_string(&cases).unwrap_or_default();
        let script = r#"
            const sniMatch = (name, list) => list.some(d => d === name || (d.startsWith('*.') && name.endsWith(d.slice(1)) && !name.slice(0, -(d.length - 1)).includes('.')));
            let s = ''; process.stdin.on('data', d => { s += d; });
            process.stdin.on('end', () => process.stdout.write(JSON.stringify(JSON.parse(s).map(([n, l]) => sniMatch(n, l)))));"#;
        let Some(out) = crate::node::run(script, &input) else {
            eprintln!("skipped: no node to compare with");
            return;
        };
        let want: Vec<bool> = serde_json::from_str(&out).unwrap_or_default();
        assert_eq!(want.len(), cases.len());
        for ((n, l), w) in cases.iter().zip(want) {
            assert_eq!(sni_match(n, l), w, "{n:?} {l:?}");
        }
    }

    #[test]
    fn names_as_node_prints_them() {
        assert_eq!(node_ip(&[127, 0, 0, 1]).unwrap(), "127.0.0.1");
        assert_eq!(node_ip(&[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]).unwrap(), "0:0:0:0:0:0:0:1");
        assert_eq!(node_ip(&[0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xab, 0xcd, 0xef]).unwrap(), "2001:db8:0:0:0:0:ab:cdef");
        assert_eq!(node_ip(&[1, 2, 3]), None);
        assert_eq!(node_dns("Game.Test"), "Game.Test");
        assert_eq!(node_dns("a,b"), "\"a\\u002cb\"");
    }
}
