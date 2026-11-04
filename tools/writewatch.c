/* writewatch.c -- test tool: log every attempt to modify files below a
 * directory, from any code in the process (the runtime, AppKit, Metal...).
 *
 *   clang -dynamiclib -O2 -o out/writewatch.dylib tools/writewatch.c
 *   WRITEWATCH_DIR=<game folder> WRITEWATCH_LOG=<file> \
 *     DYLD_INSERT_LIBRARIES=out/writewatch.dylib <binary> ...
 *
 * dyld interposing replaces these libc entry points for every image in the
 * process: open/openat (with any of O_WRONLY, O_RDWR, O_CREAT, O_TRUNC,
 * O_APPEND), creat, fopen (any mode but "r"/"rb"), mkdir(at), rename(at),
 * unlink(at), rmdir, truncate, chmod, chown, lchown, chflags, utimes,
 * setxattr, removexattr, link, symlink, clonefile, copyfile.  An attempt on
 * a path below WRITEWATCH_DIR (after resolving relative paths against the
 * cwd) is logged as "WRITE <call> <path>" and counted, whether it succeeds
 * or not; the call itself is passed through unchanged.  At exit the log gets
 * "writewatch: <n> write attempts below <dir>" (and the count of all write
 * attempts anywhere, for scale).  WRITEWATCH_ALL=1 also logs every write
 * attempt elsewhere, as "other <call> <path>".  Only works for binaries without the
 * hardened runtime (the ad-hoc signed NFSU2.app qualifies).               */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <sys/clonefile.h>
#include <copyfile.h>

#define DYLD_INTERPOSE(_new, _old) \
    __attribute__((used)) static struct { const void *n, *o; } _ip_##_old \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)_new, (const void *)_old };

static char g_dir[PATH_MAX];
static size_t g_dirlen;
static FILE *g_log;
static long g_hits, g_all;
static int g_log_all;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_busy;              /* our own fopen/fprintf */

static void finish(void){
    if (!g_log) return;
    pthread_mutex_lock(&g_mx);
    fprintf(g_log, "writewatch: %ld write attempts below %s (%ld write attempts anywhere)\n",
            g_hits, g_dir, g_all);
    fflush(g_log);
    pthread_mutex_unlock(&g_mx);
}
__attribute__((constructor)) static void init(void){
    const char *d = getenv("WRITEWATCH_DIR"), *l = getenv("WRITEWATCH_LOG");
    if (!d || !*d) return;
    if (!realpath(d, g_dir)) snprintf(g_dir, sizeof g_dir, "%s", d);
    g_dirlen = strlen(g_dir);
    { const char *a = getenv("WRITEWATCH_ALL"); g_log_all = a && *a && *a != '0'; }
    t_busy = 1;
    g_log = fopen(l && *l ? l : "/dev/stderr", "a");
    t_busy = 0;
    if (g_log){ setvbuf(g_log, NULL, _IOLBF, 0);
        fprintf(g_log, "writewatch: watching %s (pid %d)\n", g_dir, getpid()); }
    atexit(finish);
}
/* A path below the watched dir?  Relative paths are taken against the cwd
 * (or dirfd for the *at calls, approximated by the cwd unless AT_FDCWD). */
static void note(const char *call, int dirfd, const char *path){
    if (!g_dirlen || !path || t_busy) return;
    char abs[PATH_MAX * 2];
    if (path[0] == '/') snprintf(abs, sizeof abs, "%s", path);
    else {
        char base[PATH_MAX] = "";
        if (dirfd != AT_FDCWD && dirfd >= 0 && fcntl(dirfd, F_GETPATH, base) == 0) { }
        else if (!getcwd(base, sizeof base)) base[0] = 0;
        snprintf(abs, sizeof abs, "%s/%s", base, path);
    }
    pthread_mutex_lock(&g_mx);
    g_all++;
    /* Compare against the resolved parent too (symlinks, /private/...). */
    char rp[PATH_MAX], par[PATH_MAX * 2]; const char *hit = NULL;
    if (!strncmp(abs, g_dir, g_dirlen) && (abs[g_dirlen] == '/' || !abs[g_dirlen])) hit = abs;
    else {
        snprintf(par, sizeof par, "%s", abs);
        char *s = strrchr(par, '/');
        if (s && s != par){ *s = 0;
            if (realpath(par, rp) && !strncmp(rp, g_dir, g_dirlen) && (rp[g_dirlen] == '/' || !rp[g_dirlen]))
                hit = abs; }
    }
    if (hit){ g_hits++; if (g_log) fprintf(g_log, "WRITE %s %s\n", call, hit); }
    else if (g_log_all && g_log) fprintf(g_log, "other %s %s\n", call, abs);
    pthread_mutex_unlock(&g_mx);
}
#define WFLAGS (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)

static int w_open(const char *p, int fl, ...){
    mode_t m = 0;
    if (fl & O_CREAT){ va_list ap; va_start(ap, fl); m = (mode_t)va_arg(ap, int); va_end(ap); }
    if (fl & WFLAGS) note("open", AT_FDCWD, p);
    return open(p, fl, m);
}
static int w_openat(int fd, const char *p, int fl, ...){
    mode_t m = 0;
    if (fl & O_CREAT){ va_list ap; va_start(ap, fl); m = (mode_t)va_arg(ap, int); va_end(ap); }
    if (fl & WFLAGS) note("openat", fd, p);
    return openat(fd, p, fl, m);
}
static int w_creat(const char *p, mode_t m){ note("creat", AT_FDCWD, p); return creat(p, m); }
static FILE *w_fopen(const char *p, const char *mode){
    if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) note("fopen", AT_FDCWD, p);
    return fopen(p, mode);
}
static int w_mkdir(const char *p, mode_t m){ note("mkdir", AT_FDCWD, p); return mkdir(p, m); }
static int w_mkdirat(int fd, const char *p, mode_t m){ note("mkdirat", fd, p); return mkdirat(fd, p, m); }
static int w_rename(const char *a, const char *b){ note("rename(from)", AT_FDCWD, a); note("rename(to)", AT_FDCWD, b); return rename(a, b); }
static int w_renameat(int fa, const char *a, int fb, const char *b){
    note("renameat(from)", fa, a); note("renameat(to)", fb, b); return renameat(fa, a, fb, b); }
static int w_unlink(const char *p){ note("unlink", AT_FDCWD, p); return unlink(p); }
static int w_unlinkat(int fd, const char *p, int f){ note("unlinkat", fd, p); return unlinkat(fd, p, f); }
static int w_rmdir(const char *p){ note("rmdir", AT_FDCWD, p); return rmdir(p); }
static int w_truncate(const char *p, off_t l){ note("truncate", AT_FDCWD, p); return truncate(p, l); }
static int w_chmod(const char *p, mode_t m){ note("chmod", AT_FDCWD, p); return chmod(p, m); }
static int w_chown(const char *p, uid_t u, gid_t g){ note("chown", AT_FDCWD, p); return chown(p, u, g); }
static int w_lchown(const char *p, uid_t u, gid_t g){ note("lchown", AT_FDCWD, p); return lchown(p, u, g); }
static int w_chflags(const char *p, unsigned int f){ note("chflags", AT_FDCWD, p); return chflags(p, f); }
static int w_utimes(const char *p, const struct timeval t[2]){ note("utimes", AT_FDCWD, p); return utimes(p, t); }
static int w_setxattr(const char *p, const char *n, const void *v, size_t s, u_int32_t pos, int o){
    note("setxattr", AT_FDCWD, p); return setxattr(p, n, v, s, pos, o); }
static int w_removexattr(const char *p, const char *n, int o){ note("removexattr", AT_FDCWD, p); return removexattr(p, n, o); }
static int w_link(const char *a, const char *b){ note("link(to)", AT_FDCWD, b); return link(a, b); }
static int w_symlink(const char *a, const char *b){ note("symlink(to)", AT_FDCWD, b); return symlink(a, b); }
static int w_clonefile(const char *a, const char *b, uint32_t f){ note("clonefile(to)", AT_FDCWD, b); return clonefile(a, b, f); }
static int w_copyfile(const char *a, const char *b, copyfile_state_t s, copyfile_flags_t f){
    note("copyfile(to)", AT_FDCWD, b); return copyfile(a, b, s, f); }

DYLD_INTERPOSE(w_open, open)
DYLD_INTERPOSE(w_openat, openat)
DYLD_INTERPOSE(w_creat, creat)
DYLD_INTERPOSE(w_fopen, fopen)
DYLD_INTERPOSE(w_mkdir, mkdir)
DYLD_INTERPOSE(w_mkdirat, mkdirat)
DYLD_INTERPOSE(w_rename, rename)
DYLD_INTERPOSE(w_renameat, renameat)
DYLD_INTERPOSE(w_unlink, unlink)
DYLD_INTERPOSE(w_unlinkat, unlinkat)
DYLD_INTERPOSE(w_rmdir, rmdir)
DYLD_INTERPOSE(w_truncate, truncate)
DYLD_INTERPOSE(w_chmod, chmod)
DYLD_INTERPOSE(w_chown, chown)
DYLD_INTERPOSE(w_lchown, lchown)
DYLD_INTERPOSE(w_chflags, chflags)
DYLD_INTERPOSE(w_utimes, utimes)
DYLD_INTERPOSE(w_setxattr, setxattr)
DYLD_INTERPOSE(w_removexattr, removexattr)
DYLD_INTERPOSE(w_link, link)
DYLD_INTERPOSE(w_symlink, symlink)
DYLD_INTERPOSE(w_clonefile, clonefile)
DYLD_INTERPOSE(w_copyfile, copyfile)
