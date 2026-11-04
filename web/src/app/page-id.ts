// This browser's own id on the server's LAN: random, made once and kept in localStorage, sent in the switch's hello
// (net/index.ts).  The operator's bans name a page by it besides its address (web/server-rs/crates/bans), so that a
// banned player is not back by changing networks.  It says nothing about the player, and the server keeps only a
// keyed hash of it, in a ban.  Without storage (a private window, storage blocked) each load has a new one.

const KEY = 'nfsu2.net.page-id';

export function pageId(): string {
  try {
    const kept = localStorage.getItem(KEY);
    if (kept && /^[A-Za-z0-9_-]{22}$/.test(kept)) return kept;
    const id = make();
    localStorage.setItem(KEY, id);
    return id;
  } catch { return make(); }
}

/** 128 random bits, base64url (22 characters) */
function make(): string {
  const b = crypto.getRandomValues(new Uint8Array(16));
  return btoa(String.fromCharCode(...b)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
