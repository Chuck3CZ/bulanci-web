//! The state folder's lan-addresses.json (the switch's store): `[{"v":"10.77.0.2","h":"<base64url of the token's
//! SHA-256>"},...]`, the addresses held and then the ones reserved, so that the players come back to the same
//! addresses after a restart.  Written whole to a .tmp file (mode 0600), then renamed over the old one.

use std::path::PathBuf;

use parking_lot::Mutex;
use serde_json::Value;

use crate::text::js_string;

pub(crate) struct Store {
    path: PathBuf,
    /// the newest list written (saves run on blocking threads: an older one never overwrites a newer one)
    written: Mutex<u64>,
}

impl Store {
    pub(crate) fn new(path: PathBuf) -> Self {
        Store { path, written: Mutex::new(0) }
    }

    /// what the switch starts with: the addresses and their hashes (none when the file is missing or broken; an entry
    /// that is not an address the switch hands out with a hash of 32 bytes is skipped)
    pub(crate) fn load(&self) -> Vec<(u32, [u8; 32])> {
        let Ok(bytes) = std::fs::read(&self.path) else { return Vec::new() };
        let text = String::from_utf8_lossy(&bytes);
        let Ok(Value::Array(list)) = serde_json::from_str::<Value>(&text) else { return Vec::new() };
        let mut out = Vec::new();
        for e in &list {
            let Value::Object(e) = e else { continue };
            let v = crate::vip_num(&js_string(e.get("v")));
            let Some(Value::String(h)) = e.get("h") else { continue };
            let h = nfsu2_util::js::from_base64url(h);
            if v != 0 && h.len() == 32 {
                let mut hash = [0u8; 32];
                hash.copy_from_slice(&h);
                out.push((v, hash));
            }
        }
        out
    }

    /// the list as it was at `seq` (the next change tries again when this fails)
    pub(crate) fn save(&self, seq: u64, json: &str) {
        let mut written = self.written.lock();
        if seq <= *written {
            return;
        }
        let mut tmp = self.path.clone().into_os_string();
        tmp.push(".tmp");
        let tmp = PathBuf::from(tmp);
        if write_private(&tmp, json.as_bytes()).is_ok() && std::fs::rename(&tmp, &self.path).is_ok() {
            *written = seq;
        }
    }
}

/// the file written whole, readable by the server's user alone (0600, the mode a file made here gets; one that was
/// there already keeps its own)
fn write_private(path: &PathBuf, data: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    let mut o = std::fs::OpenOptions::new();
    o.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        o.mode(0o600);
    }
    o.open(path)?.write_all(data)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_what_it_wrote_and_skips_what_is_broken() {
        let dir = tempfile::tempdir().unwrap();
        let s = Store::new(dir.path().join("lan-addresses.json"));
        assert!(s.load().is_empty());
        let h = nfsu2_util::js::base64url(&[7u8; 32]);
        let json = format!(
            r#"[{{"v":"10.77.0.2","h":"{h}"}},{{"v":"10.77.0.1","h":"{h}"}},{{"v":"10.77.1.2","h":"short"}},7,null,{{"v":["10.77.2.2"],"h":"{h}"}}]"#
        );
        s.save(1, &json);
        assert_eq!(std::fs::read_to_string(dir.path().join("lan-addresses.json")).unwrap(), json);
        assert_eq!(s.load(), vec![(0x0A4D_0002, [7u8; 32]), (0x0A4D_0202, [7u8; 32])]);
        // an older list never overwrites a newer one
        s.save(1, "[]");
        assert_eq!(s.load().len(), 2);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(dir.path().join("lan-addresses.json")).unwrap().permissions().mode();
            assert_eq!(mode & 0o777, 0o600);
        }
        std::fs::write(dir.path().join("lan-addresses.json"), "{not json").unwrap();
        assert!(s.load().is_empty());
    }
}
