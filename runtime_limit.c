#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>

#include "main.h"
#include "defaults.h"
#include "runtime_limit.h"
#include "error.h"
#include "utils.h"

time_repr_t format_time(time_t t)
{
    time_repr_t out;
    double time_in_sec = (double)t;

    out.unit = 's';

    if (time_in_sec > 250) {
        time_in_sec /= 60.0;
        out.unit = 'm';

        if (time_in_sec > 100) {
            time_in_sec /= 60.0;
            out.unit = 'h';

            if (time_in_sec > 50) {
                time_in_sec /= 24.0;
                out.unit = 'd';
            }
        }
    }

    out.value = time_in_sec;
    return out;
}
/*
static double str2double(const char *str) {
    char *endptr;
    errno = 0;

    double val = strtod(str, &endptr);

    if (str == endptr) {
        return -1;
    }

    if (errno == ERANGE) {
        return -1;
    }

    // 3. 有非法字符（非空格）
    while (*endptr) {
        if (!isspace((unsigned char)*endptr)) {
            return -1;
        }
        endptr++;
    }

    return val;
}
*/

int parse_time(const char *s, time_t *out)
{
    double total = 0.0;

    while (*s) {

        while (isspace((unsigned char)*s))
            s++;

        if (*s == '\0')
            break;

        char *end;

        errno = 0;

        double value = strtod(s, &end);

        if (errno || end == s)
            return -1;

        s = end;

        while (isspace((unsigned char)*s))
            s++;

        double multiplier = 1.0;

        if (*s == '\0') {

            multiplier = 1.0;

        } else if (tolower((unsigned char)*s) == 's') {

            multiplier = 1.0;
            s++;

        } else if (tolower((unsigned char)*s) == 'm') {

            multiplier = 60.0;
            s++;

        } else if (tolower((unsigned char)*s) == 'h') {

            multiplier = 3600.0;
            s++;

        } else if (tolower((unsigned char)*s) == 'd') {

            multiplier = 86400.0;
            s++;

        } else if (tolower((unsigned char)*s) == 'w') {

            multiplier = 604800.0;
            s++;

        } else {

            return -1;
        }

        total += value * multiplier;
    }

    if (total < 0)
        return -1;

    *out = (time_t)(total + 0.5);

    return 0;
}

time_t get_monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

time_t get_max_wall_time() {
  const char* str = getenv("TS_MAX_WALL_TIME");
  if (str == NULL || strlen(str) == 0) {
    ;
  } else {
    int64_t max_walltime = str2int64(str);
    if (max_walltime > 0)
        return (time_t)max_walltime;
  }
  return DEFAULT_MAX_WALL_TIME;
}

time_t get_work_time_by_job(const struct Job* p) { // return in seconds
    if (p->state == FINISHED) {
        return p->info.end_time - p->info.start_time - p->info.pause_duration;
    }

    time_t t = (p->state == PAUSE) ? p->info.pause_time : get_monotonic_sec();
    t -= p->info.start_time + p->info.pause_duration;
    return t;
}
time_t get_pause_time_by_job(const struct Job* p) { // return in seconds
    time_t t_pause = p->info.pause_duration;
    if (p->state == PAUSE && p->info.pause_time != 0) {
        t_pause += get_monotonic_sec() - p->info.pause_time;
    }
    return t_pause;
}

int parse_schedule(const char *s, time_t *out_mono) {
    if (!s || !out_mono) return -1;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return -1;

    time_t boot_offset = time(NULL) - get_monotonic_sec();

    if (*s == '+') {
        /* Relative: +5m, +1h30m */
        time_t sec;
        if (parse_time(s + 1, &sec) != 0) return -1;
        *out_mono = get_monotonic_sec() + sec;
        return 0;
    }

    /* Try absolute wall-clock formats */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    /* "2025-06-01 14:00" */
    if (strptime(s, "%Y-%m-%d %H:%M", &tm) != NULL) {
        time_t wall = mktime(&tm);
        if (wall == (time_t)-1) return -1;
        *out_mono = wall - boot_offset;
        return 0;
    }

    /* "14:00" — today at that time */
    memset(&tm, 0, sizeof(tm));
    if (strptime(s, "%H:%M", &tm) != NULL) {
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        tm.tm_year = local->tm_year;
        tm.tm_mon  = local->tm_mon;
        tm.tm_mday = local->tm_mday;
        time_t wall = mktime(&tm);
        if (wall == (time_t)-1) return -1;
        if (wall <= now) wall += 86400; /* tomorrow if already past */
        *out_mono = wall - boot_offset;
        return 0;
    }

    return -1;
}

const char *format_schedule_delta(time_t mono_target) {
    static char buf[64];
    time_t now = get_monotonic_sec();
    time_t diff = mono_target - now;
    if (diff <= 0) return "now";
    if (diff < 60) {
        snprintf(buf, sizeof(buf), "%lds", (long)diff);
    } else if (diff < 3600) {
        snprintf(buf, sizeof(buf), "%ldm%lds", (long)(diff / 60), (long)(diff % 60));
    } else {
        snprintf(buf, sizeof(buf), "%ldh%ldm", (long)(diff / 3600), (long)((diff % 3600) / 60));
    }
    return buf;
}

time_t get_cpu_time_by_pid(int pid) {
    char path[64];
    FILE *fp;
    long utime, stime;
    long ticks;
    time_t total_time;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    /*
     * /proc/[pid]/stat 格式复杂：
     * 前面很多字段，我们只关心第14和15个：
     * utime 和 stime
     */
    int i;
    char buf[1024];

    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    char *ptr = buf;

    // 跳过前13个字段
    for (i = 0; i < 13; i++) {
        ptr = strchr(ptr, ' ');
        if (!ptr) return -1;
        ptr++;
    }

    // 读取 utime 和 stime
    if (sscanf(ptr, "%ld %ld", &utime, &stime) != 2) {
        return -1;
    }

    // 获取系统 ticks（每秒多少 tick）
    ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) {
        return -1;
    }

    total_time = (time_t)(utime + stime) / ticks;
    // printf("total_time = %f, %ld, %ld %ld\n", total_time, utime, stime, ticks);
    return total_time; // 单位：秒
}