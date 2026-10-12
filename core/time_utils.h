#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static inline bool calendar_valid(int y, int m, int d, int h, int minute, int sec) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1970 || y > 2069 || m < 1 || m > 12 || d < 1 || h < 0 || h > 23 ||
        minute < 0 || minute > 59 || sec < 0 || sec > 59) return false;
    int max = days[m-1] + (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
    return d <= max;
}
static inline int64_t calendar_epoch(int y, int m, int d, int h, int minute, int sec) {
    // Gregorian civil date to UTC days since 1970, independent of process TZ.
    y -= m <= 2;
    int era = y / 400, year = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = year * 365 + year / 4 - year / 100 + doy;
    return ((int64_t)era * 146097 + doe - 719468) * 86400 + h * 3600 + minute * 60 + sec;
}
static inline bool iso_epoch(const char *s, int64_t *out) {
    if (!s || !out) return false;
    int y,m,d,h,min,sec,n=0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y,&m,&d,&h,&min,&sec,&n) != 6 ||
        !calendar_valid(y,m,d,h,min,sec)) return false;
    const char *p = s+n;
    if (*p == '.') { ++p; if (*p < '0' || *p > '9') return false; while (*p >= '0' && *p <= '9') ++p; }
    int offset = 0;
    if (*p == 'Z' && !p[1]) {}
    else if (*p == '+' || *p == '-') {
        int oh,om,used=0;
        if (sscanf(p+1, "%2d:%2d%n", &oh,&om,&used) != 2 || p[1+used] ||
            oh < 0 || oh > 23 || om < 0 || om > 59) return false;
        offset = (oh*60+om)*60 * (*p == '-' ? -1 : 1);
    } else return false;
    *out = calendar_epoch(y,m,d,h,min,sec) - offset;
    return true;
}
