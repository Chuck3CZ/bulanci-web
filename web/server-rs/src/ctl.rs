//! `nfsu2-server ctl [--state <dir>] <command> [words...]`: an operator's command to the server that runs with that
//! state folder, through its control socket (app/control.rs: the commands).  The folder: --state, else NFSU2_STATE,
//! else the server's own default (out/web/server in the repository).  The words after the command are taken as
//! they come, so a message needs no quotes: `ctl kick Bob Stop ramming people`.  Exit code 0, or 1 when the server
//! said no (or is not there), 2 for no command.

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

use crate::app::control::{SOCKET, USAGE, read_answer};
use crate::paths;

pub fn run(argv: &[String], env: &dyn Fn(&str) -> Option<String>, repo: &str, cwd: &str) -> i32 {
    let (state, words) = match argv {
        [flag, dir, rest @ ..] if flag == "--state" => (dir.clone(), rest),
        _ => (env("NFSU2_STATE").filter(|s| !s.is_empty()).unwrap_or_else(|| paths::join(repo, "out/web/server")), argv),
    };
    if words.is_empty() {
        eprintln!("usage: nfsu2-server ctl [--state <dir>] <command> [words...]\n{USAGE}");
        return 2;
    }
    let path = paths::join(&paths::resolve(cwd, &state), SOCKET);
    let ask = || -> std::io::Result<String> {
        let mut s = UnixStream::connect(&path)?;
        s.set_read_timeout(Some(Duration::from_secs(15)))?;
        let mut line = serde_json::to_string(words).map_err(std::io::Error::other)?;
        line.push('\n');
        s.write_all(line.as_bytes())?;
        let mut answer = String::new();
        s.read_to_string(&mut answer)?;
        Ok(answer)
    };
    match ask().map(|a| read_answer(&a)) {
        Ok(Some((true, text))) => {
            println!("{text}");
            0
        }
        Ok(Some((false, text))) => {
            eprintln!("{text}");
            1
        }
        Ok(None) => {
            eprintln!("{path}: no answer from the server");
            1
        }
        Err(e) => {
            eprintln!("{path}: {e} (is the server running, with this state folder?)");
            1
        }
    }
}
