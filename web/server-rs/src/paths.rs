//! Node's `path` (posix) on the strings the server keeps its paths in: `resolve`, `join`, `dirname`, `basename`,
//! `extname`, all lexical (no file system), as the Node server used them.  Paths are strings here: they go into
//! console lines, die messages and ETags (a file's ETag hashes its real path) as the Node server wrote them.

/// `path.posix.normalize` of an absolute path: "." and ".." resolved, repeated and trailing slashes gone ("/" stays)
pub fn normalize_abs(p: &str) -> String {
    let mut parts: Vec<&str> = Vec::new();
    for seg in p.split('/') {
        match seg {
            "" | "." => {}
            ".." => {
                parts.pop();
            }
            s => parts.push(s),
        }
    }
    let mut out = String::with_capacity(p.len());
    for s in &parts {
        out.push('/');
        out.push_str(s);
    }
    if out.is_empty() {
        out.push('/');
    }
    out
}

/// `path.resolve(base, p)` with an absolute base: p itself when it is absolute
pub fn resolve(base: &str, p: &str) -> String {
    if p.starts_with('/') { normalize_abs(p) } else { normalize_abs(&format!("{base}/{p}")) }
}

/// `path.join(a, b)` for an absolute a
pub fn join(a: &str, b: &str) -> String {
    normalize_abs(&format!("{a}/{b}"))
}

/// `path.posix.dirname(p)` of a normalized path
pub fn dirname(p: &str) -> String {
    let t = p.trim_end_matches('/');
    if t.is_empty() {
        return if p.starts_with('/') { "/".into() } else { ".".into() };
    }
    match t.rfind('/') {
        None => ".".into(),
        Some(i) => {
            let d = t[..i].trim_end_matches('/');
            if d.is_empty() { "/".into() } else { d.to_string() }
        }
    }
}

/// `path.posix.basename(p)`
pub fn basename(p: &str) -> String {
    let t = p.trim_end_matches('/');
    match t.rfind('/') {
        Some(i) => t[i + 1..].to_string(),
        None => t.to_string(),
    }
}

/// `path.posix.extname(p)`: from the last "." of the last part, "" when that part has none, starts with it, or is ".."
pub fn extname(p: &str) -> &str {
    let t = p.trim_end_matches('/');
    let base = match t.rfind('/') {
        Some(i) => &t[i + 1..],
        None => t,
    };
    if base == ".." {
        return "";
    }
    match base.rfind('.') {
        Some(0) | None => "",
        Some(i) => &base[i..],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn as_node_path_does_it() {
        assert_eq!(normalize_abs("/a//b/./c/../d/"), "/a/b/d");
        assert_eq!(normalize_abs("/../.."), "/");
        assert_eq!(resolve("/home/x", "web/../out"), "/home/x/out");
        assert_eq!(resolve("/home/x", "/etc/"), "/etc");
        assert_eq!(resolve("/home/x", ""), "/home/x");
        assert_eq!(join("/", "x"), "/x");
        assert_eq!(join("/a", ""), "/a");
        assert_eq!(dirname("/a/b/c"), "/a/b");
        assert_eq!(dirname("/a/b/"), "/a");
        assert_eq!(dirname("/a"), "/");
        assert_eq!(dirname("/"), "/");
        assert_eq!(basename("/a/b.txt"), "b.txt");
        assert_eq!(basename("/a/b/"), "b");
        assert_eq!(extname("/a/b.HTML"), ".HTML");
        assert_eq!(extname("/a/.bashrc"), "");
        assert_eq!(extname("/a/b."), ".");
        assert_eq!(extname("/a.b/c"), "");
        assert_eq!(extname("/x/a.tar.gz"), ".gz");
        assert_eq!(extname("/x/.."), "");
    }
}
