/* runtime/web/tz.c under test (web/tools/tztest.sh): for each Unix time on
 * the command line prints localtime_r's fields and the mktime round trip, in
 * the zone NFSU2_TZ describes.  The JS side compares with the browser's own
 * local time (Date) for the same zone. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv){
    for (int i = 1; i < argc; i++){
        time_t t = (time_t)strtoll(argv[i], 0, 10);
        struct tm a; localtime_r(&t, &a);
        struct tm b = a; b.tm_isdst = -1; time_t back = mktime(&b);
        printf("%lld %ld %d %04d-%02d-%02d %02d:%02d:%02d %d %d %lld %s\n", (long long)t, (long)a.tm_gmtoff, a.tm_isdst,
               a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min, a.tm_sec, a.tm_wday, a.tm_yday,
               (long long)back, tzname[a.tm_isdst > 0]);
    }
    return 0;
}
