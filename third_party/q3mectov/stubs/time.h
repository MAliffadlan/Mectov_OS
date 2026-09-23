/* <time.h> stub — ioquake3 subset on Mectov OS kernel (v38.98) */
#ifndef Q3STUB_TIME_H
#define Q3STUB_TIME_H

#include "stddef.h"

typedef long q3_time_t;
typedef q3_time_t time_t;
struct tm {
	int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
	int tm_wday, tm_yday, tm_isdst;
};
typedef struct tm tm;

time_t q3_time(time_t *t);
struct tm *q3_localtime(const time_t *t);
size_t q3_strftime(char *s, size_t max, const char *fmt, const struct tm *tmv);

#define time      q3_time
#define localtime q3_localtime
#define strftime  q3_strftime

#endif /* Q3STUB_TIME_H */
