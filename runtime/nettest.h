/* nettest.h -- one rule for "a test or debug run, not someone playing", for
 * both network stacks: LAN play (winsock.c) and free-roam multiplayer
 * (roam_bsd.c).  In such a run neither may reach the real network: LAN play
 * becomes loopback (or stays off / the virtual LAN), free roam keeps to
 * 127.0.0.1 without LAN discovery, and anything else is refused and logged as
 * "[net] BLOCKED" / "[roam] BLOCKED" (tests/run_suite.sh fails on either).
 *
 * Playing means the app or ./run.sh's window mode, which both set ALARM=0
 * and none of the test variables.  A test run is any run with
 * NFSU2_NET_HARNESS or NFSU2_ROAM_HARNESS (tools/autorun.sh and
 * tools/apptest.sh set both), an input script, NFSU2_TIMEOUT,
 * NFSU2_QUIT_FRAMES, NOFOCUS, or a missing / non-zero ALARM (boot.c then
 * stops the run after ALARM s).  NFSU2_NET_ALLOW_LAN=1 lifts the rule for a
 * user's own scripted run on the real LAN, never under a harness flag.  The
 * test runs are also wrapped in tests/net/loopback-only.sb, so a bug here
 * still fails closed. */
#ifndef NETTEST_H
#define NETTEST_H
#include <stdlib.h>
#include <string.h>
static inline int nettest_on(const char *k){ const char *v = getenv(k); return v && *v && strcmp(v, "0") != 0; }
static inline int nfsu2_test_run(void){
    if (nettest_on("NFSU2_NET_HARNESS") || nettest_on("NFSU2_ROAM_HARNESS")) return 1;
    if (nettest_on("NFSU2_NET_ALLOW_LAN")) return 0;
    if (nettest_on("NFSU2_INPUT_SCRIPT") || nettest_on("NFSU2_TIMEOUT") || nettest_on("NFSU2_QUIT_FRAMES") ||
        nettest_on("NOFOCUS")) return 1;
    const char *al = getenv("ALARM");
    return !al || strcmp(al, "0") != 0;
}
#endif
