//! The operator's bans (the server's control socket: `nfsu2-server ctl ban ...`): who may not play with others, for
//! a while or for good, and the message their page shows them (nfsu2-lan's Kicks and bans).
//!
//! There are no accounts, so a ban names a client the two ways the server knows one, each as a keyed hash
//! (HMAC-SHA256 under `bans.key` in the state folder, base64url, 22 characters): `bans.json` holds no address and no
//! id, and means nothing without the key.
//!
//! - Its address, as the server's limits count it (an IPv4 address, or an IPv6 /56), for at most [`ADDR_MS`] (7 days)
//!   after it was last the banned player's: addresses change hands (a phone network gives many players one address in
//!   turn, a home's address goes to another home), and an address banned for good would one day turn a stranger away.
//! - The id its page keeps in the browser's storage (the hello's "id", web/src/net), for as long as the ban lasts.
//!
//! A connection that matches either is turned away ([`Bans::by_addr`] when it connects, [`Bans::by_page`] at its
//! hello).  A banned page id seen at an address the ban does not name adds that address to it (the same player on
//! another network: a VPN, a phone); an address never adds an id (whoever shares it would be banned for good).
//! Neither stops a player who clears the site's data and changes networks: without accounts no ban can.
//!
//! A new ban takes the place of the bans that name the same page id (for a page without one: the same address), so
//! `ban Bob perm` after `ban Bob 1d` leaves one ban, for good.  Bans and addresses whose time is up go at the next
//! change or listing.  The file is written whole: a temporary file beside it (0600), renamed over it.

use std::io::{self, Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

use hmac::{Hmac, Mac};
use parking_lot::Mutex;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use sha2::Sha256;

/// how long an address stays banned after it was last the banned player's (ms)
pub const ADDR_MS: i64 = 7 * DAY;
/// the longest message a kick or a ban carries (characters)
pub const MSG_MAX: usize = 200;
/// the longest ban short of one for good (ms): ten years
pub const FOR_MAX: i64 = 3650 * DAY;
/// an address seen again this soon after its time was last set is not written again
const ADDR_SLACK: i64 = 3_600_000;
const MIN: i64 = 60_000;
const HOUR: i64 = 60 * MIN;
const DAY: i64 = 24 * HOUR;

/// One ban.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Ban {
    /// its number (`unban #4`)
    pub id: u64,
    /// the player's name when it was banned, for the operator (anyone can take any name: a ban never matches by one)
    pub name: String,
    /// what the player's page shows
    #[serde(default)]
    pub msg: String,
    /// when it was made, and until when it lasts (ms since 1970; None: for good)
    pub at: i64,
    pub until: Option<i64>,
    /// the hashes of the page ids it names
    #[serde(default)]
    pub pages: Vec<String>,
    /// and of the addresses, each until its own time
    #[serde(default)]
    pub addrs: Vec<Addr>,
}

/// An address a ban names.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Addr {
    /// its hash (`Bans::addr`)
    pub h: String,
    /// banned until (ms since 1970): `ADDR_MS` after it was last seen as the player's, at most the ban's end
    pub until: i64,
}

impl Ban {
    pub fn live(&self, now: i64) -> bool {
        self.until.is_none_or(|u| u > now)
    }
    /// its page's control message: {"t":"ban","msg":...,"until":ms}, without until for good
    pub fn message(&self) -> String {
        let mut m = Map::new();
        m.insert("t".into(), "ban".into());
        m.insert("msg".into(), self.msg.clone().into());
        if let Some(u) = self.until {
            m.insert("until".into(), u.into());
        }
        Value::Object(m).to_string()
    }
    /// whether it names this address (its hash) or page id (its hash) now
    fn names(&self, addr: Option<&str>, page: Option<&str>, now: i64) -> bool {
        addr.is_some_and(|a| self.addrs.iter().any(|x| x.h == a && x.until > now))
            || page.is_some_and(|p| self.pages.iter().any(|x| x == p))
    }
    fn addr_until(&self, now: i64) -> i64 {
        self.until.map_or(now + ADDR_MS, |u| u.min(now + ADDR_MS))
    }
}

/// a kick's control message: {"t":"kick","msg":...}
pub fn kick_message(msg: &str) -> String {
    let mut m = Map::new();
    m.insert("t".into(), "kick".into());
    m.insert("msg".into(), clean_msg(msg).into());
    Value::Object(m).to_string()
}

/// Who a new ban is for: the name it shows, the address's hash (`Bans::addr`), the page's id as the hello named it
pub struct Target<'a> {
    pub name: &'a str,
    pub addr: Option<&'a str>,
    pub page: Option<&'a str>,
}

#[derive(Default, Serialize, Deserialize)]
struct File {
    /// the last ban's number
    next: u64,
    bans: Vec<Ban>,
}

/// The bans of a state folder.  Every call takes one lock; the ones that change something write the file under it
/// (rare: the operator's commands, and a banned page at a new address).
pub struct Bans {
    path: PathBuf,
    key: Vec<u8>,
    file: Mutex<File>,
}

impl Bans {
    /// The bans in `dir` (bans.json, and its key bans.key, made at the first start).  Err: a file that cannot be read.
    pub fn open(dir: &Path) -> io::Result<Bans> {
        let key = load_key(&dir.join("bans.key"))?;
        let path = dir.join("bans.json");
        let file = match std::fs::read(&path) {
            Ok(b) => serde_json::from_slice(&b).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?,
            Err(e) if e.kind() == io::ErrorKind::NotFound => File::default(),
            Err(e) => return Err(e),
        };
        Ok(Bans { path, key, file: Mutex::new(file) })
    }

    /// an address's hash: what bans name it by, and what the switch keeps of it (`AttachOpts::ident`)
    pub fn addr(&self, key: &str) -> String {
        self.hash("addr", key)
    }

    fn page(&self, id: &str) -> String {
        self.hash("page", id)
    }

    fn hash(&self, kind: &str, v: &str) -> String {
        let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(&self.key).expect("HMAC takes any key");
        mac.update(kind.as_bytes());
        mac.update(b"\0");
        mac.update(v.as_bytes());
        nfsu2_util::js::base64url(&mac.finalize().into_bytes())[..22].to_string()
    }

    /// the live ban an address (its hash) is under now, the one that lasts longest
    pub fn by_addr(&self, addr: &str, now: i64) -> Option<Ban> {
        let f = self.file.lock();
        longest(&f.bans, |b| b.live(now) && b.names(Some(addr), None, now)).map(|i| f.bans[i].clone())
    }

    /// the live ban a page id is under now, the one that lasts longest; the address it came from (its hash) is added
    /// to that ban, or kept another `ADDR_MS`
    pub fn by_page(&self, id: &str, addr: Option<&str>, now: i64) -> Option<Ban> {
        let h = self.page(id);
        let mut f = self.file.lock();
        let i = longest(&f.bans, |b| b.live(now) && b.names(None, Some(&h), now))?;
        let b = &mut f.bans[i];
        let mut changed = false;
        if let Some(a) = addr {
            let until = b.addr_until(now);
            match b.addrs.iter_mut().find(|x| x.h == a) {
                Some(x) if x.until >= until - ADDR_SLACK => {}
                Some(x) => {
                    x.until = until;
                    changed = true;
                }
                None => {
                    b.addrs.push(Addr { h: a.to_string(), until });
                    changed = true;
                }
            }
        }
        let ban = b.clone();
        if changed {
            self.save_or_say(&mut f, now);
        }
        Some(ban)
    }

    /// whether `ban` names this address (its hash) or page id now: the other connections a new ban removes
    pub fn covers(&self, ban: &Ban, addr: Option<&str>, page: Option<&str>, now: i64) -> bool {
        ban.names(addr, page.map(|p| self.page(p)).as_deref(), now)
    }

    /// A new ban, with `msg`, until `until` (None: for good).  It takes the place of the bans that name the target's
    /// page id (a target without one: its address); their numbers come back with it.
    pub fn add(&self, t: Target, msg: &str, until: Option<i64>, now: i64) -> io::Result<(Ban, Vec<u64>)> {
        let page = t.page.map(|p| self.page(p));
        let mut f = self.file.lock();
        let mut replaced = Vec::new();
        f.bans.retain(|b| {
            let same = match &page {
                Some(p) => b.pages.contains(p),
                None => t.addr.is_some_and(|a| b.addrs.iter().any(|x| x.h == a)),
            };
            if same && b.live(now) {
                replaced.push(b.id);
            }
            !same
        });
        f.next += 1;
        let mut b = Ban {
            id: f.next,
            name: t.name.to_string(),
            msg: clean_msg(msg),
            at: now,
            until,
            pages: page.into_iter().collect(),
            addrs: vec![],
        };
        if let Some(a) = t.addr {
            b.addrs.push(Addr { h: a.to_string(), until: b.addr_until(now) });
        }
        f.bans.push(b.clone());
        self.save(&mut f, now)?;
        Ok((b, replaced))
    }

    /// The bans `sel` names, lifted: "#4" or "4" (a number), or a name (any case: every ban of that name).
    pub fn remove(&self, sel: &str, now: i64) -> io::Result<Vec<Ban>> {
        let sel = sel.trim();
        let num = sel.strip_prefix('#').unwrap_or(sel).parse::<u64>().ok();
        let mut f = self.file.lock();
        let mut gone = Vec::new();
        f.bans.retain(|b| {
            let hit = match num {
                Some(n) => b.id == n,
                None => b.name.to_lowercase() == sel.to_lowercase(),
            };
            if hit && b.live(now) {
                gone.push(b.clone());
            }
            !hit
        });
        if !gone.is_empty() {
            self.save(&mut f, now)?;
        }
        Ok(gone)
    }

    /// the live bans, oldest first (the ones whose time is up go)
    pub fn list(&self, now: i64) -> Vec<Ban> {
        let mut f = self.file.lock();
        let before = (f.bans.len(), f.bans.iter().map(|b| b.addrs.len()).sum::<usize>());
        prune(&mut f, now);
        if before != (f.bans.len(), f.bans.iter().map(|b| b.addrs.len()).sum::<usize>()) {
            self.save_or_say(&mut f, now);
        }
        f.bans.clone()
    }

    fn save_or_say(&self, f: &mut File, now: i64) {
        if let Err(e) = self.save(f, now) {
            nfsu2_util::log::error(&format!("{}: {e}", self.path.display()));
        }
    }

    /// the file written whole, without what is over
    fn save(&self, f: &mut File, now: i64) -> io::Result<()> {
        prune(f, now);
        let text = serde_json::to_string_pretty(&*f).map_err(io::Error::other)?;
        let tmp = self.path.with_extension(format!("json.tmp-{}", std::process::id()));
        let mut out = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
        out.write_all(text.as_bytes())?;
        out.write_all(b"\n")?;
        out.sync_all()?;
        std::fs::rename(&tmp, &self.path)
    }
}

/// the bans whose time is up go, and so do their addresses whose time is up
fn prune(f: &mut File, now: i64) {
    f.bans.retain(|b| b.live(now));
    for b in &mut f.bans {
        b.addrs.retain(|a| a.until > now);
    }
}

/// of the bans `f` takes, the index of the one that lasts longest (for good first)
fn longest(bans: &[Ban], f: impl Fn(&Ban) -> bool) -> Option<usize> {
    bans.iter().enumerate().filter(|(_, b)| f(b)).max_by_key(|(_, b)| b.until.unwrap_or(i64::MAX)).map(|(i, _)| i)
}

/// the key (32 random bytes), made 0600 at the first start
fn load_key(p: &Path) -> io::Result<Vec<u8>> {
    if let Ok(mut file) = std::fs::File::open(p) {
        let mut k = Vec::new();
        if file.read_to_end(&mut k).is_ok() && k.len() >= 32 {
            return Ok(k);
        }
    }
    let k = nfsu2_util::rand::bytes(32);
    let mut file = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(p)?;
    file.write_all(&k)?;
    Ok(k)
}

/// a message as the page shows it: one line, no control or direction characters, at most `MSG_MAX` characters
pub fn clean_msg(s: &str) -> String {
    let bidi = |c: char| matches!(c, '\u{200B}'..='\u{200F}' | '\u{202A}'..='\u{202E}' | '\u{2060}'..='\u{2069}' | '\u{FEFF}');
    let t: String = s.chars().map(|c| if c.is_whitespace() { ' ' } else { c }).filter(|&c| !c.is_control() && !bidi(c)).collect();
    t.split(' ').filter(|w| !w.is_empty()).collect::<Vec<_>>().join(" ").chars().take(MSG_MAX).collect()
}

/// How long a ban lasts, as the operator writes it: "30m", "12h", "3d", "2w" (a whole number and its unit; "min" for
/// m too), or "perm" (for good: None).  Not one of those (or over `FOR_MAX`): Err.
pub fn parse_for(s: &str) -> Result<Option<i64>, String> {
    let s = s.trim().to_lowercase();
    if matches!(s.as_str(), "perm" | "permanent" | "forever") {
        return Ok(None);
    }
    let at = s.find(|c: char| !c.is_ascii_digit()).unwrap_or(s.len());
    let (n, unit) = s.split_at(at);
    let unit_ms = match unit {
        "m" | "min" => MIN,
        "h" => HOUR,
        "d" => DAY,
        "w" => 7 * DAY,
        _ => return Err(format!("\"{s}\": how long, as 30m, 12h, 3d, 2w or perm")),
    };
    match n.parse::<i64>() {
        Ok(n) if n > 0 && n.checked_mul(unit_ms).is_some_and(|ms| ms <= FOR_MAX) => Ok(Some(n * unit_ms)),
        _ => Err(format!("\"{s}\": how long, as 30m, 12h, 3d, 2w or perm (at most ten years)")),
    }
}

/// a span of time, for the operator: "45 min", "5 h 10 min", "2 d 23 h"
pub fn span(ms: i64) -> String {
    let m = (ms.max(0) + MIN - 1) / MIN;
    let (d, h, mi) = (m / (24 * 60), m / 60 % 24, m % 60);
    match (d, h, mi) {
        (0, 0, mi) => format!("{mi} min"),
        (0, h, 0) => format!("{h} h"),
        (0, h, mi) => format!("{h} h {mi} min"),
        (d, 0, _) => format!("{d} d"),
        (d, h, _) => format!("{d} d {h} h"),
    }
}

/// a time, for the operator: "2026-10-27 14:05 UTC"
pub fn when(ms: i64) -> String {
    let (y, mo, d, h, mi, _, _) = nfsu2_util::js::utc_parts(ms);
    format!("{y:04}-{mo:02}-{d:02} {h:02}:{mi:02} UTC")
}

#[cfg(test)]
mod tests {
    use super::*;

    const T0: i64 = 1_790_000_000_000;

    fn bans() -> (tempfile::TempDir, Bans) {
        let dir = tempfile::tempdir().unwrap();
        let b = Bans::open(dir.path()).unwrap();
        (dir, b)
    }

    #[test]
    fn a_ban_turns_away_its_address_and_its_page() {
        let (_d, b) = bans();
        let (a1, a2) = (b.addr("203.0.113.5"), b.addr("198.51.100.7"));
        assert_ne!(a1, a2);
        assert_eq!(a1.len(), 22);
        let bob = Target { name: "Bob", addr: Some(&a1), page: Some("page-1") };
        let (ban, replaced) = b.add(bob, " Ramming,\n3 days ", Some(T0 + 3 * DAY), T0).unwrap();
        assert!(replaced.is_empty());
        assert_eq!((ban.id, ban.msg.as_str()), (1, "Ramming, 3 days"));
        assert_eq!(b.by_addr(&a1, T0 + 1).map(|x| x.id), Some(1));
        assert_eq!(b.by_addr(&a2, T0 + 1), None);
        assert_eq!(b.by_page("page-2", Some(&a2), T0 + 1), None);
        // the banned page at another address: that address is banned too
        assert_eq!(b.by_page("page-1", Some(&a2), T0 + 1).map(|x| x.id), Some(1));
        assert_eq!(b.by_addr(&a2, T0 + 2).map(|x| x.id), Some(1));
        // and its time is up with the ban's
        assert_eq!(b.by_addr(&a1, T0 + 3 * DAY), None);
        assert_eq!(b.by_page("page-1", None, T0 + 3 * DAY), None);
        assert!(b.list(T0 + 3 * DAY).is_empty());
    }

    #[test]
    fn an_address_is_banned_for_a_week_after_it_was_last_the_players_and_never_bans_a_page() {
        let (_d, b) = bans();
        let a = b.addr("203.0.113.5");
        b.add(Target { name: "Bob", addr: Some(&a), page: Some("p") }, "", None, T0).unwrap();
        // whoever else comes from that address is turned away, but their page is not banned for it
        assert!(b.by_addr(&a, T0 + DAY).is_some());
        assert_eq!(b.by_page("someone-else", Some(&a), T0 + DAY), None);
        assert!(b.by_addr(&a, T0 + ADDR_MS).is_none(), "the address goes after a week");
        // the page comes back from it after two weeks: a week more for the address
        assert!(b.by_page("p", Some(&a), T0 + 14 * DAY).is_some());
        assert!(b.by_addr(&a, T0 + 20 * DAY).is_some());
        assert!(b.by_addr(&a, T0 + 21 * DAY).is_none());
        // for good: the page still
        assert!(b.by_page("p", None, T0 + 3000 * DAY).is_some());
        assert_eq!(b.list(T0 + 3000 * DAY)[0].addrs, vec![]);
    }

    #[test]
    fn a_new_ban_of_the_same_page_takes_the_place_of_the_old_one() {
        let (_d, b) = bans();
        let a = b.addr("203.0.113.5");
        b.add(Target { name: "Bob", addr: Some(&a), page: Some("p") }, "a day", Some(T0 + DAY), T0).unwrap();
        b.add(Target { name: "Eve", addr: Some(&a), page: Some("q") }, "eve", Some(T0 + DAY), T0).unwrap();
        let (ban, replaced) = b.add(Target { name: "Bob", addr: Some(&a), page: Some("p") }, "for good", None, T0 + 1).unwrap();
        assert_eq!((ban.id, replaced), (3, vec![1]));
        let names: Vec<_> = b.list(T0 + 2).iter().map(|x| (x.id, x.name.clone())).collect();
        assert_eq!(names, vec![(2, "Eve".to_string()), (3, "Bob".to_string())]);
        // a player without a page id: by address
        let c = b.addr("192.0.2.1");
        b.add(Target { name: "Old", addr: Some(&c), page: None }, "", Some(T0 + DAY), T0).unwrap();
        let (_, replaced) = b.add(Target { name: "Old", addr: Some(&c), page: None }, "", None, T0).unwrap();
        assert_eq!(replaced, vec![4]);
        // the longest of two that match is the one a page is told
        assert_eq!(b.by_addr(&a, T0 + 2).map(|x| x.until), Some(None));
    }

    #[test]
    fn bans_are_lifted_by_number_or_name() {
        let (_d, b) = bans();
        let a = b.addr("203.0.113.5");
        b.add(Target { name: "Bob", addr: Some(&a), page: Some("p") }, "", None, T0).unwrap();
        b.add(Target { name: "bob", addr: None, page: Some("q") }, "", None, T0).unwrap();
        b.add(Target { name: "Eve", addr: None, page: Some("r") }, "", None, T0).unwrap();
        assert_eq!(b.remove("#3", T0).unwrap().len(), 1);
        assert_eq!(b.remove("BOB", T0).unwrap().iter().map(|x| x.id).collect::<Vec<_>>(), vec![1, 2]);
        assert!(b.remove("4", T0).unwrap().is_empty());
        assert!(b.by_addr(&a, T0).is_none());
    }

    #[test]
    fn the_file_keeps_hashes_only_and_outlives_a_restart() {
        let dir = tempfile::tempdir().unwrap();
        let b = Bans::open(dir.path()).unwrap();
        let a = b.addr("203.0.113.5");
        b.add(Target { name: "Bob", addr: Some(&a), page: Some("page-secret") }, "msg", Some(T0 + DAY), T0).unwrap();
        let text = std::fs::read_to_string(dir.path().join("bans.json")).unwrap();
        assert!(!text.contains("203.0.113") && !text.contains("page-secret"), "{text}");
        use std::os::unix::fs::PermissionsExt;
        for f in ["bans.json", "bans.key"] {
            assert_eq!(std::fs::metadata(dir.path().join(f)).unwrap().permissions().mode() & 0o777, 0o600, "{f}");
        }
        drop(b);
        let b = Bans::open(dir.path()).unwrap();
        assert_eq!(b.addr("203.0.113.5"), a, "the same key");
        assert_eq!(b.by_page("page-secret", None, T0 + 1).map(|x| x.msg), Some("msg".into()));
        let (next, _) = b.add(Target { name: "Eve", addr: None, page: Some("x") }, "", None, T0).unwrap();
        assert_eq!(next.id, 2);
        // a broken file: not taken for no bans
        std::fs::write(dir.path().join("bans.json"), "{").unwrap();
        assert!(Bans::open(dir.path()).is_err());
    }

    #[test]
    fn messages_for_the_page() {
        let (_d, b) = bans();
        let (ban, _) = b.add(Target { name: "B", addr: None, page: Some("p") }, "Bye \"now\"", Some(T0 + DAY), T0).unwrap();
        assert_eq!(ban.message(), format!(r#"{{"t":"ban","msg":"Bye \"now\"","until":{}}}"#, T0 + DAY));
        let (ban, _) = b.add(Target { name: "B", addr: None, page: Some("p") }, "", None, T0).unwrap();
        assert_eq!(ban.message(), r#"{"t":"ban","msg":""}"#);
        assert_eq!(kick_message("a\u{202E}b\tc"), r#"{"t":"kick","msg":"ab c"}"#);
        assert_eq!(clean_msg(&"x".repeat(300)).len(), MSG_MAX);
    }

    #[test]
    fn how_long_as_the_operator_writes_it() {
        assert_eq!(parse_for("30m"), Ok(Some(30 * MIN)));
        assert_eq!(parse_for("30min"), Ok(Some(30 * MIN)));
        assert_eq!(parse_for("12h"), Ok(Some(12 * HOUR)));
        assert_eq!(parse_for("3D"), Ok(Some(3 * DAY)));
        assert_eq!(parse_for("2w"), Ok(Some(14 * DAY)));
        assert_eq!(parse_for("perm"), Ok(None));
        assert_eq!(parse_for("forever"), Ok(None));
        for bad in ["", "3", "d", "0d", "-1d", "1.5d", "3 days", "99999999999999w", "3651d"] {
            assert!(parse_for(bad).is_err(), "{bad}");
        }
        assert_eq!(span(45 * MIN), "45 min");
        assert_eq!(span(5 * HOUR + 10 * MIN), "5 h 10 min");
        assert_eq!(span(3 * DAY - 1), "3 d");
        assert_eq!(span(2 * DAY + 23 * HOUR), "2 d 23 h");
        assert_eq!(span(12 * HOUR), "12 h");
        assert_eq!(when(1_793_109_900_000), "2026-10-27 14:05 UTC");
    }
}
