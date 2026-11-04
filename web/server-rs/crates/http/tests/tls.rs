//! TLS: the certificate only for the names it covers (the audit's F4), for handshakes without a name only when it
//! names an address; its names (DNS names in lower case, addresses in their plain form); refused handshakes as
//! Node's https server refuses them; the key formats; a renewed certificate taken without a restart; a handshake that
//! never finishes; HTTP over it.  Certificates are made with the openssl CLI as web/test/cf-sim.mjs makeTestCerts and
//! audit.test.mjs make them; skipped (with a message) without openssl.

mod common;

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Arc;
use std::time::Duration;

use common::TestService;
use nfsu2_http::Limits;
use nfsu2_http::tls::TlsState;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{DigitallySignedStruct, SignatureScheme};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

fn ssl(dir: &Path, args: &[&str]) -> bool {
    Command::new("openssl").args(args).current_dir(dir).output().map(|o| o.status.success()).unwrap_or(false)
}

struct Certs {
    _dir: tempfile::TempDir,
    dir: PathBuf,
}

impl Certs {
    /// cf-sim.mjs makeTestCerts (a CA, and a server certificate for localhost, nfsu2.test and 127.0.0.1), and
    /// audit.test.mjs's F4 certificate (game.test and *.wild.test, no address), and a few more
    fn make() -> Option<Certs> {
        let tmp = tempfile::tempdir().ok()?;
        let d = tmp.path().to_path_buf();
        let ec = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes"];
        let ok = ssl(&d, &[&["req", "-x509"][..], &ec, &["-keyout", "ca.key", "-out", "ca.crt", "-days", "30", "-subj", "/CN=NFSU2 deploy test CA", "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0", "-addext", "keyUsage=critical,keyCertSign"]].concat())
            && std::fs::write(d.join("server.ext"), "subjectAltName=DNS:localhost,DNS:nfsu2.test,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n").is_ok()
            && ssl(&d, &[&["req"][..], &ec, &["-keyout", "server.key", "-out", "server.csr", "-subj", "/CN=nfsu2.test"]].concat())
            && ssl(&d, &["x509", "-req", "-in", "server.csr", "-CA", "ca.crt", "-CAkey", "ca.key", "-CAcreateserial", "-out", "server.crt", "-days", "30", "-extfile", "server.ext"])
            && ssl(&d, &[&["req", "-x509"][..], &ec, &["-keyout", "k.pem", "-out", "c.pem", "-days", "5", "-subj", "/CN=game.test", "-addext", "subjectAltName=DNS:game.test,DNS:*.wild.test"]].concat())
            && ssl(&d, &[&["req", "-x509"][..], &ec, &["-keyout", "k2.pem", "-out", "c2.pem", "-days", "5", "-subj", "/CN=game.test", "-addext", "subjectAltName=DNS:game.test"]].concat())
            && ssl(&d, &[&["req", "-x509"][..], &ec, &["-keyout", "nosan.key", "-out", "nosan.crt", "-days", "5", "-subj", "/CN=nosan.test"]].concat())
            && ssl(&d, &[&["req", "-x509"][..], &ec, &["-keyout", "v6.key", "-out", "v6.crt", "-days", "5", "-subj", "/CN=v6", "-addext", "subjectAltName=IP:::1,DNS:Upper.Test"]].concat())
            && ssl(&d, &["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "rsa8.key", "-out", "rsa.crt", "-days", "5", "-subj", "/CN=rsa.test", "-addext", "subjectAltName=DNS:rsa.test"])
            && ssl(&d, &["rsa", "-in", "rsa8.key", "-traditional", "-out", "rsa1.key"])
            && ssl(&d, &["ec", "-in", "k.pem", "-out", "sec1.key"]);
        if !ok {
            return None;
        }
        let chain = [std::fs::read(d.join("server.crt")).ok()?, std::fs::read(d.join("ca.crt")).ok()?].concat();
        std::fs::write(d.join("chain.crt"), chain).ok()?;
        Some(Certs { _dir: tmp, dir: d })
    }
    fn p(&self, name: &str) -> PathBuf {
        self.dir.join(name)
    }
}

/// a client that takes any certificate (the tests look at which one it was)
#[derive(Debug)]
struct Any(Arc<rustls::crypto::CryptoProvider>);
impl ServerCertVerifier for Any {
    fn verify_server_cert(
        &self,
        _: &CertificateDer<'_>,
        _: &[CertificateDer<'_>],
        _: &ServerName<'_>,
        _: &[u8],
        _: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }
    fn verify_tls12_signature(
        &self,
        m: &[u8],
        c: &CertificateDer<'_>,
        d: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(m, c, d, &self.0.signature_verification_algorithms)
    }
    fn verify_tls13_signature(
        &self,
        m: &[u8],
        c: &CertificateDer<'_>,
        d: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(m, c, d, &self.0.signature_verification_algorithms)
    }
    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.0.signature_verification_algorithms.supported_schemes()
    }
}

fn connector(alpn: &[&[u8]], versions: &[&'static rustls::SupportedProtocolVersion]) -> tokio_rustls::TlsConnector {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut config = rustls::ClientConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(versions)
        .expect("versions")
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(Any(provider)))
        .with_no_client_auth();
    config.alpn_protocols = alpn.iter().map(|a| a.to_vec()).collect();
    tokio_rustls::TlsConnector::from(Arc::new(config))
}

/// A handshake with this server name (None: none, as for an address): the certificate the server showed.
async fn hello(addr: std::net::SocketAddr, name: Option<&str>) -> Result<Vec<u8>, String> {
    let tcp = TcpStream::connect(addr).await.map_err(|e| e.to_string())?;
    let sn = match name {
        Some(n) => ServerName::try_from(n.to_string()).map_err(|e| e.to_string())?,
        None => ServerName::IpAddress(std::net::IpAddr::from([127, 0, 0, 1]).into()),
    };
    let tls =
        tokio::time::timeout(Duration::from_secs(5), connector(&[], &[&rustls::version::TLS13, &rustls::version::TLS12]).connect(sn, tcp))
            .await
            .map_err(|_| "timeout".to_string())?
            .map_err(|e| e.to_string())?;
    let (_, conn) = tls.get_ref();
    Ok(conn.peer_certificates().and_then(|c| c.first()).map(|c| c.as_ref().to_vec()).unwrap_or_default())
}

fn der(pem: &Path) -> Vec<u8> {
    let data = std::fs::read(pem).expect("pem");
    rustls_pemfile::certs(&mut &data[..]).next().expect("a certificate").expect("pem").as_ref().to_vec()
}

async fn tls_engine(state: Arc<TlsState>, limits: Limits) -> std::net::SocketAddr {
    let (l, _) = nfsu2_http::bind("127.0.0.1", 0, None).await.expect("bind");
    let addr = l.local_addr().expect("addr");
    nfsu2_http::serve(l, Some(state), Arc::new(limits), TestService::new(Duration::from_secs(65)));
    addr
}

macro_rules! certs_or_skip {
    () => {
        match Certs::make() {
            Some(c) => c,
            None => {
                eprintln!("skipped: no openssl to make test certificates");
                return;
            }
        }
    };
}

#[tokio::test]
async fn f4_no_certificate_without_the_servers_name_or_for_another() {
    let c = certs_or_skip!();
    let (state, names) = TlsState::load(&c.p("c.pem"), &c.p("k.pem")).expect("load");
    assert_eq!(names.dns, vec!["game.test", "*.wild.test"]);
    assert!(names.ips.is_empty());
    let addr = tls_engine(state, Limits::default()).await;
    let own = der(&c.p("c.pem"));
    assert!(hello(addr, None).await.is_err(), "no SNI: no certificate");
    assert!(hello(addr, Some("scan.invalid")).await.is_err(), "another name: no certificate");
    assert!(hello(addr, Some("a.b.wild.test")).await.is_err(), "two labels under the wildcard");
    assert!(hello(addr, Some("wild.test")).await.is_err(), "the wildcard's zone itself");
    assert_eq!(hello(addr, Some("game.test")).await.as_deref(), Ok(own.as_slice()));
    assert_eq!(hello(addr, Some("a.wild.test")).await.as_deref(), Ok(own.as_slice()), "a wildcard name of the certificate");
    assert_eq!(hello(addr, Some("GAME.TEST")).await.as_deref(), Ok(own.as_slice()), "case does not matter");
}

/// A ClientHello with this server name (None: none), as rustls's client writes it.
fn client_hello(name: Option<&str>, versions: &[&'static rustls::SupportedProtocolVersion]) -> Vec<u8> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let config = rustls::ClientConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(versions)
        .expect("versions")
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(Any(provider)))
        .with_no_client_auth();
    let sn = match name {
        Some(n) => ServerName::try_from(n.to_string()).expect("name"),
        None => ServerName::IpAddress(std::net::IpAddr::from([127, 0, 0, 1]).into()),
    };
    let mut c = rustls::ClientConnection::new(Arc::new(config), sn).expect("client");
    let mut hello = Vec::new();
    while c.wants_write() {
        c.write_tls(&mut hello).expect("hello");
    }
    hello
}

/// A ClientHello sent on its own: what came back, and whether the connection ended in a reset.  (Only for handshakes
/// the server refuses: it ends the connection.)
async fn refusal(addr: std::net::SocketAddr, hello: &[u8]) -> (Vec<u8>, bool) {
    let mut s = TcpStream::connect(addr).await.expect("connect");
    s.write_all(hello).await.expect("send");
    let mut got = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        match tokio::time::timeout(Duration::from_secs(5), s.read(&mut buf)).await {
            Ok(Ok(0)) => return (got, false),
            Ok(Ok(n)) => got.extend_from_slice(&buf[..n]),
            Ok(Err(e)) => return (got, e.kind() == std::io::ErrorKind::ConnectionReset),
            Err(_) => panic!("the server neither refused nor went on: {got:?}"),
        }
    }
}

/// a fatal handshake_failure alert in a TLS 1.2 record, as OpenSSL sends it for a handshake it has no certificate for
const HANDSHAKE_FAILURE: [u8; 7] = [0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x28];

#[tokio::test]
async fn refused_handshakes_look_as_nodes_do() {
    // Node (OpenSSL) has no certificate for a handshake without a name (tls_server.mjs gives it none): a fatal
    // handshake_failure alert; for a name its SNI callback refuses, the socket is destroyed: closed without a byte.
    // (rustls sent access_denied for both.)  No certificate either way.
    let c = certs_or_skip!();
    let both = [&rustls::version::TLS13, &rustls::version::TLS12];
    let twelve = [&rustls::version::TLS12];
    // a name rustls refuses as none at all (illegal_parameter); OpenSSL takes any bytes but NUL, and the SNI callback
    // refused it as another name
    let mut odd = client_hello(Some("x.test"), &both);
    if let Some(i) = odd.windows(6).position(|w| w == b"x.test") {
        odd[i] = b' ';
    }
    let cases: [(&str, &str, Vec<u8>, bool); 7] = [
        ("c.pem", "k.pem", client_hello(None, &both), true),
        ("c.pem", "k.pem", client_hello(None, &twelve), true),
        ("c.pem", "k.pem", client_hello(Some("scan.invalid"), &both), false),
        ("c.pem", "k.pem", client_hello(Some("a.b.wild.test"), &twelve), false),
        ("c.pem", "k.pem", odd, false),
        // a certificate that names an address: shown without a name, but not for another name
        ("chain.crt", "server.key", client_hello(Some("other.test"), &both), false),
        ("chain.crt", "server.key", client_hello(Some("other.test"), &twelve), false),
    ];
    for (i, (cert, key, hello, alert)) in cases.iter().enumerate() {
        let (state, _) = TlsState::load(&c.p(cert), &c.p(key)).expect("load");
        let addr = tls_engine(state, Limits::default()).await;
        let got = refusal(addr, hello).await;
        let want = if *alert { (HANDSHAKE_FAILURE.to_vec(), false) } else { (Vec::new(), false) };
        assert_eq!(got, want, "case {i}: {cert}");
        // Node's own server on the same certificate, the same ClientHello
        let node = common::NodeRef::script("tls_server.mjs", &[c.p(cert).display().to_string(), c.p(key).display().to_string()]).await;
        match node {
            Some(n) => assert_eq!(refusal(n.addr, hello).await, got, "node, case {i}: {cert}"),
            None => eprintln!("skipped: no node to compare with"),
        }
    }
}

#[tokio::test]
async fn a_certificate_that_names_an_address_is_shown_without_a_name_too() {
    let c = certs_or_skip!();
    let (state, names) = TlsState::load(&c.p("chain.crt"), &c.p("server.key")).expect("load");
    assert_eq!(names.dns, vec!["localhost", "nfsu2.test"]);
    assert_eq!(names.ips, vec!["127.0.0.1"]);
    let addr = tls_engine(state, Limits::default()).await;
    let leaf = der(&c.p("server.crt"));
    assert_eq!(hello(addr, None).await.as_deref(), Ok(leaf.as_slice()));
    assert_eq!(hello(addr, Some("nfsu2.test")).await.as_deref(), Ok(leaf.as_slice()));
    assert!(hello(addr, Some("other.test")).await.is_err(), "a name it does not cover still fails");
}

#[tokio::test]
async fn names_as_node_prints_them() {
    let c = certs_or_skip!();
    let (_, names) = TlsState::load(&c.p("v6.crt"), &c.p("v6.key")).expect("load");
    // Node: "IP Address:0:0:0:0:0:0:0:1" (bareIp: lower case), DNS names in lower case
    assert_eq!(names.ips, vec!["0:0:0:0:0:0:0:1"]);
    assert_eq!(names.dns, vec!["upper.test"]);
}

#[tokio::test]
async fn the_key_formats_a_certificate_comes_with() {
    let c = certs_or_skip!();
    for (cert, key) in [("c.pem", "k.pem"), ("c.pem", "sec1.key"), ("rsa.crt", "rsa8.key"), ("rsa.crt", "rsa1.key")] {
        let (state, names) = TlsState::load(&c.p(cert), &c.p(key)).unwrap_or_else(|e| panic!("{key}: {e}"));
        let addr = tls_engine(state, Limits::default()).await;
        let name = names.dns[0].clone();
        assert_eq!(hello(addr, Some(&name)).await.as_deref(), Ok(der(&c.p(cert)).as_slice()), "{key}");
    }
    // the first line of the PEM says which: PKCS#8, SEC1, PKCS#1
    let first = |f: &str| std::fs::read_to_string(c.p(f)).unwrap_or_default().lines().next().unwrap_or_default().to_string();
    assert_eq!(first("k.pem"), "-----BEGIN PRIVATE KEY-----");
    assert_eq!(first("sec1.key"), "-----BEGIN EC PRIVATE KEY-----");
    assert_eq!(first("rsa1.key"), "-----BEGIN RSA PRIVATE KEY-----");
}

#[tokio::test]
async fn files_that_cannot_be_used() {
    let c = certs_or_skip!();
    let missing = c.p("missing.key");
    let e = TlsState::load(&c.p("c.pem"), &missing).err().unwrap_or_default();
    assert_eq!(e, format!("ENOENT: no such file or directory, open '{}'", missing.display()));
    let e = TlsState::load(&c.p("nosan.crt"), &c.p("nosan.key")).err().unwrap_or_default();
    assert_eq!(e, format!("{} names no host (no subjectAltName)", c.p("nosan.crt").display()));
    let e = TlsState::load(&c.p("c.pem"), &c.p("server.key")).err().unwrap_or_default();
    assert!(e.contains("does not belong"), "{e}");
    std::fs::write(c.p("junk.pem"), "not a certificate\n").unwrap();
    assert!(TlsState::load(&c.p("junk.pem"), &c.p("k.pem")).is_err());
    assert!(TlsState::load(&c.p("c.pem"), &c.p("junk.pem")).is_err());
}

#[tokio::test]
async fn a_renewed_certificate_without_a_restart() {
    let c = certs_or_skip!();
    let (crt, key) = (c.p("live.crt"), c.p("live.key"));
    std::fs::copy(c.p("c.pem"), &crt).unwrap();
    std::fs::copy(c.p("k.pem"), &key).unwrap();
    let (state, _) = TlsState::load(&crt, &key).expect("load");
    let addr = tls_engine(state.clone(), Limits::default()).await;
    assert_eq!(hello(addr, Some("game.test")).await.as_deref(), Ok(der(&c.p("c.pem")).as_slice()));
    std::fs::copy(c.p("c2.pem"), &crt).unwrap();
    std::fs::copy(c.p("k2.pem"), &key).unwrap();
    let names = state.reload().expect("reload");
    assert_eq!(names.dns, vec!["game.test"]);
    assert_eq!(hello(addr, Some("game.test")).await.as_deref(), Ok(der(&c.p("c2.pem")).as_slice()), "the new one");
    assert!(hello(addr, Some("a.wild.test")).await.is_err(), "the old one's names are gone");
    // a broken renewal: the certificate in use stays
    std::fs::write(&crt, "broken").unwrap();
    assert!(state.reload().is_err());
    assert_eq!(hello(addr, Some("game.test")).await.as_deref(), Ok(der(&c.p("c2.pem")).as_slice()));
}

#[tokio::test]
async fn a_handshake_that_never_finishes_is_closed_after_the_handshake_time() {
    let c = certs_or_skip!();
    let (state, _) = TlsState::load(&c.p("chain.crt"), &c.p("server.key")).expect("load");
    let limits = Limits { handshake_timeout: Duration::from_millis(300), ..Limits::default() };
    let addr = tls_engine(state, limits).await;
    let mut socks = Vec::new();
    for _ in 0..50 {
        socks.push(TcpStream::connect(addr).await.unwrap());
    }
    let t0 = tokio::time::Instant::now();
    for s in &mut socks {
        let (got, closed) = common::read_all(s, 2000).await;
        assert!(closed && got.is_empty(), "closed without a byte");
    }
    let dt = t0.elapsed();
    assert!(dt >= Duration::from_millis(250) && dt < Duration::from_millis(1200), "{dt:?}");
    // meanwhile and after, a real client gets in
    assert!(hello(addr, Some("nfsu2.test")).await.is_ok());
}

/// A GET over TLS (these versions, this client certificate and key, or none): whether an answer came.  (TLS 1.3's
/// client finishes its handshake before the server checks its certificate: a refusal shows when it reads.)
async fn get_with(
    addr: std::net::SocketAddr,
    versions: &[&'static rustls::SupportedProtocolVersion],
    cert: Option<(&Path, &Path)>,
) -> bool {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let b = rustls::ClientConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(versions)
        .expect("versions")
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(Any(provider)));
    let config = match cert {
        Some((c, k)) => {
            let chain = rustls_pemfile::certs(&mut &std::fs::read(c).unwrap()[..]).collect::<Result<Vec<_>, _>>().unwrap();
            let key = rustls_pemfile::private_key(&mut &std::fs::read(k).unwrap()[..]).unwrap().unwrap();
            b.with_client_auth_cert(chain, key).expect("client certificate")
        }
        None => b.with_no_client_auth(),
    };
    let tcp = TcpStream::connect(addr).await.unwrap();
    let go = async {
        let mut s =
            tokio_rustls::TlsConnector::from(Arc::new(config)).connect(ServerName::try_from("nfsu2.test").unwrap(), tcp).await.ok()?;
        s.write_all(b"GET /x HTTP/1.1\r\nHost: nfsu2.test\r\nConnection: close\r\n\r\n").await.ok()?;
        let mut all = Vec::new();
        s.read_to_end(&mut all).await.ok()?;
        Some(all.starts_with(b"HTTP/1.1 200"))
    };
    tokio::time::timeout(Duration::from_secs(5), go).await.ok().flatten().unwrap_or(false)
}

/// A peer the service names must show a client certificate that the client CA signed (with --trusted-proxies
/// cloudflare, Cloudflare's origin-pull certificate: any of its customers connects from its addresses too, the second
/// security audit): without one, or with one another CA signed, nothing gets through; the other peers are asked for
/// none; without a client CA, a peer that must show one is refused.
#[tokio::test]
async fn a_peer_that_must_prove_itself_shows_a_certificate_of_the_client_ca() {
    let c = certs_or_skip!();
    let d = &c.dir;
    let ec = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes"];
    assert!(
        ssl(
            d,
            &[
                &["req", "-x509"][..],
                &ec,
                &[
                    "-keyout",
                    "cca.key",
                    "-out",
                    "cca.crt",
                    "-days",
                    "30",
                    "-subj",
                    "/CN=test origin-pull CA",
                    "-addext",
                    "basicConstraints=critical,CA:TRUE",
                    "-addext",
                    "keyUsage=critical,keyCertSign"
                ]
            ]
            .concat()
        ) && std::fs::write(
            d.join("client.ext"),
            "basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n"
        )
        .is_ok()
            && ssl(d, &[&["req"][..], &ec, &["-keyout", "client.key", "-out", "client.csr", "-subj", "/CN=origin-pull"]].concat())
            && ssl(
                d,
                &[
                    "x509",
                    "-req",
                    "-in",
                    "client.csr",
                    "-CA",
                    "cca.crt",
                    "-CAkey",
                    "cca.key",
                    "-CAcreateserial",
                    "-out",
                    "client.crt",
                    "-days",
                    "30",
                    "-extfile",
                    "client.ext"
                ]
            )
            && ssl(
                d,
                &[
                    &["req", "-x509"][..],
                    &ec,
                    &[
                        "-keyout",
                        "rogue.key",
                        "-out",
                        "rogue.crt",
                        "-days",
                        "5",
                        "-subj",
                        "/CN=origin-pull",
                        "-addext",
                        "extendedKeyUsage=clientAuth"
                    ]
                ]
                .concat()
            ),
        "client certificates"
    );
    let ca = std::fs::read(c.p("cca.crt")).unwrap();
    let (state, _) = TlsState::load_with_client_ca(&c.p("chain.crt"), &c.p("server.key"), &ca).expect("load");
    let (l, _) = nfsu2_http::bind("127.0.0.1", 0, None).await.expect("bind");
    let proving = l.local_addr().unwrap();
    let svc = TestService::new(Duration::from_secs(65));
    svc.prove.store(true, std::sync::atomic::Ordering::Relaxed);
    nfsu2_http::serve(l, Some(state.clone()), Arc::new(Limits::default()), svc);
    let (client, rogue) = ((c.p("client.crt"), c.p("client.key")), (c.p("rogue.crt"), c.p("rogue.key")));
    for v in [&[&rustls::version::TLS13][..], &[&rustls::version::TLS12][..]] {
        assert!(!get_with(proving, v, None).await, "{v:?}: no client certificate");
        assert!(!get_with(proving, v, Some((&rogue.0, &rogue.1))).await, "{v:?}: one another CA signed");
        assert!(get_with(proving, v, Some((&client.0, &client.1))).await, "{v:?}: the client CA's");
    }
    // the same certificate state for peers the service does not name: no client certificate asked
    let others = tls_engine(state, Limits::default()).await;
    assert!(get_with(others, &[&rustls::version::TLS13], None).await);
    // no client CA: nothing to prove with
    let (plain, _) = TlsState::load(&c.p("chain.crt"), &c.p("server.key")).expect("load");
    let (l, _) = nfsu2_http::bind("127.0.0.1", 0, None).await.expect("bind");
    let refused = l.local_addr().unwrap();
    let svc = TestService::new(Duration::from_secs(65));
    svc.prove.store(true, std::sync::atomic::Ordering::Relaxed);
    nfsu2_http::serve(l, Some(plain), Arc::new(Limits::default()), svc);
    assert!(!get_with(refused, &[&rustls::version::TLS13], Some((&client.0, &client.1))).await);
    // Cloudflare's own CA (built in) is one the server takes; a file without a certificate is not
    assert!(TlsState::load_with_client_ca(&c.p("chain.crt"), &c.p("server.key"), nfsu2_headers::CLOUDFLARE_ORIGIN_PULL_CA).is_ok());
    assert!(TlsState::load_with_client_ca(&c.p("chain.crt"), &c.p("server.key"), b"junk").is_err());
}

#[tokio::test]
async fn http_over_tls() {
    let c = certs_or_skip!();
    let (state, _) = TlsState::load(&c.p("chain.crt"), &c.p("server.key")).expect("load");
    let addr = tls_engine(state, Limits::default()).await;
    for versions in [&[&rustls::version::TLS13][..], &[&rustls::version::TLS12][..]] {
        let tcp = TcpStream::connect(addr).await.unwrap();
        let mut s =
            connector(&[b"h2", b"http/1.1"], versions).connect(ServerName::try_from("nfsu2.test").unwrap(), tcp).await.expect("handshake");
        assert_eq!(s.get_ref().1.alpn_protocol(), Some(&b"http/1.1"[..]), "Node's ALPN");
        // keep-alive, then a pipelined pair, then a large answer
        let mut all = Vec::new();
        s.write_all(b"GET /conninfo HTTP/1.1\r\nHost: nfsu2.test\r\n\r\n").await.unwrap();
        let mut buf = vec![0u8; 1 << 16];
        let n = s.read(&mut buf).await.unwrap();
        let first = common::mask_dates(&buf[..n]);
        assert!(first.contains("\r\n\r\n127.0.0.1 true 127.0.0.1:"), "{first}");
        s.write_all(b"GET /x HTTP/1.1\r\n\r\nGET /big HTTP/1.1\r\nConnection: close\r\n\r\n").await.unwrap();
        s.read_to_end(&mut all).await.unwrap_or_default();
        let t = common::mask_dates(&all);
        assert!(t.contains("\"u\":\"/x\""), "{}", &t[..t.len().min(300)]);
        assert!(t.ends_with(&"a".repeat(200_000)), "the whole large answer");
    }
    // an upgrade over TLS: the halves whole again, and what came after the head
    let tcp = TcpStream::connect(addr).await.unwrap();
    let mut s =
        connector(&[], &[&rustls::version::TLS13]).connect(ServerName::try_from("nfsu2.test").unwrap(), tcp).await.expect("handshake");
    s.write_all(b"GET /lan HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\nPRE").await.unwrap();
    let mut all = Vec::new();
    let _ = tokio::time::timeout(Duration::from_secs(3), s.read_to_end(&mut all)).await;
    let t = common::mask_dates(&all);
    assert!(t.starts_with("HTTP/1.1 101 Switching Protocols\r\n") && t.contains("x-pre: 505245\r\n"), "{t}");
}
