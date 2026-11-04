/* wasm: local time in the page's time zone.
 *
 * WASI has no zone database, so wasi-libc's local time is UTC.  The page
 * reads the zone from the browser (web/src/host/tz.ts: offsets and the exact
 * transition instants for the years around now, plus the zone's names) and
 * passes it in the environment:
 *
 *   NFSU2_TZ = "<standard name>;<daylight name>;<offset>[;<t>:<offset>]..."
 *
 * offsets in seconds east of UTC (like tm_gmtoff), t in Unix seconds: from t
 * on, the offset is the one after it.  compat/wasm_plat.h routes localtime_r,
 * localtime, mktime and tzname here, so the Win32 time-zone HLE
 * (GetTimeZoneInformation, FileTimeToLocalFileTime in win32_ext.c) works
 * unchanged and sees the same zone a Mac build sees through its libc.
 * Without NFSU2_TZ local time stays UTC, as before. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TZ_MAX 64                   /* transitions: 3 years need at most ~6 */
static int64_t g_t[TZ_MAX];         /* transition instants, ascending */
static int32_t g_off[TZ_MAX + 1];   /* g_off[i]: offset before g_t[i]; g_off[n]: after the last */
static int     g_n;
static int32_t g_std, g_dst;        /* the zone's standard and daylight offsets */
static char    g_name[2][64] = { "UTC", "UTC" };
const char    *nfsu2_tzname[2] = { g_name[0], g_name[1] };
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void tz_load(void){
    const char *e = getenv("NFSU2_TZ");
    if (!e || !*e) return;
    const char *s1 = strchr(e, ';'), *s2 = s1 ? strchr(s1 + 1, ';') : 0;
    if (!s2){ fprintf(stderr, "tz: NFSU2_TZ malformed, local time is UTC\n"); return; }
    snprintf(g_name[0], sizeof g_name[0], "%.*s", (int)(s1 - e), e);
    snprintf(g_name[1], sizeof g_name[1], "%.*s", (int)(s2 - s1 - 1), s1 + 1);
    char *p;
    g_off[0] = (int32_t)strtol(s2 + 1, &p, 10);
    while (*p == ';' && g_n < TZ_MAX){
        char *q; long long t = strtoll(p + 1, &q, 10);
        if (*q != ':') break;
        g_t[g_n] = t; g_off[g_n + 1] = (int32_t)strtol(q + 1, &p, 10); g_n++;
    }
    /* standard = the smaller offset, daylight = the larger (Windows' view;
     * also right for the southern hemisphere) */
    g_std = g_dst = g_off[0];
    for (int i = 1; i <= g_n; i++){ if (g_off[i] < g_std) g_std = g_off[i]; if (g_off[i] > g_dst) g_dst = g_off[i]; }
    const char *nm = getenv("NFSU2_TZ_NAME");
    fprintf(stderr, "tz: %s, UTC%+.4g h%s (%d transitions)\n", nm && *nm ? nm : g_name[0], g_std / 3600.0,
            g_dst != g_std ? ", daylight saving" : "", g_n);
}
static int32_t tz_off(int64_t t){
    pthread_once(&g_once, tz_load);
    int i = 0;
    while (i < g_n && t >= g_t[i]) i++;
    return g_off[i];
}

struct tm *nfsu2_localtime_r(const time_t *t, struct tm *out){
    int32_t off = tz_off((int64_t)*t);
    time_t lt = *t + off;
    if (!gmtime_r(&lt, out)) return 0;
    out->tm_isdst = g_dst != g_std && off != g_std;
    out->tm_gmtoff = off;
    out->tm_zone = nfsu2_tzname[out->tm_isdst > 0];
    return out;
}
struct tm *nfsu2_localtime(const time_t *t){
    static _Thread_local struct tm r;
    return nfsu2_localtime_r(t, &r);
}
/* Local broken-down time -> time_t.  tm_isdst < 0: find the offset that
 * applies (in the hour skipped at a spring transition the earlier offset
 * wins, as in most libcs); 0 or 1: take the standard or daylight offset. */
time_t nfsu2_mktime(struct tm *tm){
    pthread_once(&g_once, tz_load);
    int isdst = tm->tm_isdst;
    time_t u = timegm(tm);                    /* the fields as if they were UTC (normalises tm) */
    if (u == (time_t)-1) return u;
    time_t t;
    if (isdst > 0 && g_dst != g_std)  t = u - g_dst;
    else if (isdst == 0)              t = u - g_std;
    else { t = u - tz_off((int64_t)u - g_std); t = u - tz_off((int64_t)t); }
    nfsu2_localtime_r(&t, tm);
    return t;
}
