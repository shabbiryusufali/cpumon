/*
 * cpumon - Linux CPU usage monitor
 *
 * Reads /proc/stat (and hostname/uname/loadavg/uptime/cpuinfo, CPU
 * pressure from /proc/pressure/cpu, per-core frequency from cpufreq,
 * CPU temperature from hwmon/thermal zones, and per-process CPU time
 * from /proc/[pid]/stat) and renders a scrollable, man-page-styled
 * report either as an interactive ncurses TUI (with a "watch"-style
 * auto refresh) or, in --daemon mode, as periodic snapshots appended to
 * a daily log file (text, JSON Lines or CSV) and/or sent to the systemd
 * journal. The daemon mode is intended to be run under systemd; SIGHUP
 * makes it re-read its config file and reopen its log file.
 *
 * Daily log files older than --log-retention-days (default 30, 0
 * disables) are deleted automatically, and files older than
 * --compress-after-days are gzip-compressed, whenever the log rolls
 * over to a new day. --replay prints past snapshots back out of the log
 * directory (compressed or not), optionally limited with --since/--until.
 *
 * CPU usage is inherently a rate, not a snapshot, so every measurement
 * is a delta between two /proc/stat reads: a short (200ms) bootstrap
 * pair for the first frame of --once/--daemon/interactive/--plain, and
 * thereafter the delta between consecutive interval ticks.
 *
 * Build:   make
 * Run:     ./cpumon                     (interactive, 5s refresh)
 *          ./cpumon -i 5m                (interactive, 5 minute refresh)
 *          ./cpumon --once               (single snapshot to stdout, no ncurses)
 *          ./cpumon --once --format json (same, as JSON)
 *          ./cpumon --daemon -i 5m       (headless logger, for systemd)
 *          ./cpumon --replay --since 2h  (print the last 2 hours of logs)
 *
 * License: do whatever you want with it (see LICENSE).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ctype.h>
#include <locale.h>
#include <langinfo.h>
#include <dirent.h>
#include <syslog.h>
#include <zlib.h>

#ifdef USE_NCURSES
#include <ncurses.h>
#endif

#define PROGNAME        "cpumon"
#ifndef VERSION
/* Normally supplied by the Makefile from the top-level VERSION file
 * (-DVERSION="..."); this is only a fallback for ad hoc `cc cpumon.c`
 * builds that bypass the Makefile. See VERSION for the single place
 * to bump the app version. */
#define VERSION         "0.0.0-unversioned"
#endif
#ifndef SYSCONFDIR
#define SYSCONFDIR      "/etc"
#endif
#define SYSTEM_CONFIG   SYSCONFDIR "/cpumon/cpumon.conf"
#define USER_CONFIG_REL "cpumon/cpumon.conf"      /* under $XDG_CONFIG_HOME or ~/.config */
#define STAT_PATH       "/proc/stat"
#define LOADAVG_PATH    "/proc/loadavg"
#define UPTIME_PATH     "/proc/uptime"
#define CPUINFO_PATH    "/proc/cpuinfo"
#define PSI_CPU_PATH    "/proc/pressure/cpu"
#define SYSFS_CPU_DIR   "/sys/devices/system/cpu"
#define HWMON_DIR       "/sys/class/hwmon"
#define THERMAL_DIR     "/sys/class/thermal"
#ifndef JOURNAL_SOCKET
#define JOURNAL_SOCKET  "/run/systemd/journal/socket"
#endif
#define MAX_CORES       512
#define MAX_TEMPS       32
#define MAX_TOP         50
#define DEFAULT_TOP     5
#define HIST_LEN        120                      /* samples kept for sparklines */
#define DEFAULT_LOGDIR  "/var/log/cpumon"
#define FALLBACK_LOGDIR ".local/share/cpumon"   /* under $HOME */
#define DEFAULT_LOG_RETENTION_DAYS 30            /* 0 disables auto-delete */
#define PAD_LINES        2400
#define PAD_COLS         220
#define BOOTSTRAP_USEC    200000                 /* 200ms warm-up sample pair */

/* ---------------------------------------------------------------------
 * Small file helpers
 * ------------------------------------------------------------------- */

/* Reads up to buflen-1 bytes of a (small, usually /proc or /sys) file
 * into buf and NUL-terminates it. Returns bytes read, or -1. */
static int read_small_file(const char *path, char *buf, size_t buflen)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, buflen - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static int read_ll_file(const char *path, long long *out)
{
    char b[64];
    if (read_small_file(path, b, sizeof(b)) <= 0) return -1;
    char *end;
    errno = 0;
    long long v = strtoll(b, &end, 10);
    if (end == b || errno) return -1;
    *out = v;
    return 0;
}

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------------
 * /proc/stat parsing
 * ------------------------------------------------------------------- */

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
} CpuJiffies;

typedef struct {
    CpuJiffies total;                 /* aggregate "cpu" line */
    CpuJiffies cores[MAX_CORES];
    int core_id[MAX_CORES];           /* N from "cpuN" - offline CPUs leave gaps */
    int ncores;                       /* number of per-core lines parsed */
    unsigned long long ctxt;
    unsigned long long processes;     /* forks since boot */
    unsigned long procs_running;
    unsigned long procs_blocked;
    time_t sampled_at;
    double mono;                      /* CLOCK_MONOTONIC seconds, for sub-second deltas */
} CpuStat;

static int parse_cpu_line(const char *line, CpuJiffies *j)
{
    char label[16];
    unsigned long long u = 0, n = 0, s = 0, i = 0, io = 0, ir = 0, so = 0, st = 0, g = 0, gn = 0;
    int fields = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                         label, &u, &n, &s, &i, &io, &ir, &so, &st, &g, &gn);
    if (fields < 5) return -1; /* need at least label+user+nice+system+idle */
    j->user = u; j->nice = n; j->system = s; j->idle = i; j->iowait = io;
    j->irq = ir; j->softirq = so; j->steal = st; j->guest = g; j->guest_nice = gn;
    return 0;
}

static int stat_read(CpuStat *cs)
{
    FILE *f = fopen(STAT_PATH, "r");
    if (!f) return -1;

    memset(cs, 0, sizeof(*cs));
    cs->sampled_at = time(NULL);
    cs->mono = mono_now();

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) == 0) {
            if (isdigit((unsigned char)line[3])) {
                if (cs->ncores < MAX_CORES &&
                    parse_cpu_line(line, &cs->cores[cs->ncores]) == 0) {
                    cs->core_id[cs->ncores] = atoi(line + 3);
                    cs->ncores++;
                }
            } else if (line[3] == ' ' || line[3] == '\t') {
                parse_cpu_line(line, &cs->total);
            }
        } else if (strncmp(line, "ctxt ", 5) == 0) {
            sscanf(line + 5, "%llu", &cs->ctxt);
        } else if (strncmp(line, "processes ", 10) == 0) {
            sscanf(line + 10, "%llu", &cs->processes);
        } else if (strncmp(line, "procs_running ", 14) == 0) {
            sscanf(line + 14, "%lu", &cs->procs_running);
        } else if (strncmp(line, "procs_blocked ", 14) == 0) {
            sscanf(line + 14, "%lu", &cs->procs_blocked);
        }
    }
    fclose(f);
    return 0;
}

/* read the 1/5/15 min load average, returns 0 on success */
static int loadavg_read(double *l1, double *l5, double *l15)
{
    FILE *f = fopen(LOADAVG_PATH, "r");
    if (!f) return -1;
    int rc = fscanf(f, "%lf %lf %lf", l1, l5, l15);
    fclose(f);
    return (rc == 3) ? 0 : -1;
}

static int uptime_read(double *secs)
{
    FILE *f = fopen(UPTIME_PATH, "r");
    if (!f) return -1;
    int rc = fscanf(f, "%lf", secs);
    fclose(f);
    return (rc == 1) ? 0 : -1;
}

static void cpu_model_read(char *buf, size_t buflen)
{
    snprintf(buf, buflen, "unknown");
    FILE *f = fopen(CPUINFO_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                chomp(colon);
                snprintf(buf, buflen, "%s", colon);
            }
            break;
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------------
 * CPU pressure stall information (/proc/pressure/cpu, kernel 4.20+)
 * ------------------------------------------------------------------- */

typedef struct {
    int have_some, have_full;
    double some[3], full[3];          /* avg10, avg60, avg300 (percent) */
} Psi;

static int psi_parse(const char *text, Psi *p)
{
    memset(p, 0, sizeof(*p));
    const char *line = text;
    while (line && *line) {
        if (sscanf(line, "some avg10=%lf avg60=%lf avg300=%lf",
                   &p->some[0], &p->some[1], &p->some[2]) == 3)
            p->have_some = 1;
        else if (sscanf(line, "full avg10=%lf avg60=%lf avg300=%lf",
                        &p->full[0], &p->full[1], &p->full[2]) == 3)
            p->have_full = 1;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return p->have_some ? 0 : -1;
}

static int psi_read(Psi *p)
{
    char buf[512];
    memset(p, 0, sizeof(*p));
    if (read_small_file(PSI_CPU_PATH, buf, sizeof(buf)) <= 0) return -1;
    return psi_parse(buf, p);
}

/* ---------------------------------------------------------------------
 * Per-core frequency: cpufreq's scaling_cur_freq (kHz), falling back to
 * the "cpu MHz" lines in /proc/cpuinfo (e.g. VMs without cpufreq).
 * ------------------------------------------------------------------- */

/* Fills mhz[i] for each core id ids[i] from a /proc/cpuinfo stream.
 * Entries without a match are left untouched. Returns matches found. */
static int cpuinfo_mhz_parse(FILE *f, const int *ids, int n, double *mhz)
{
    char line[512];
    int cur = -1, found = 0;
    while (fgets(line, sizeof(line), f)) {
        const char *colon = strchr(line, ':');
        if (!colon) continue;
        if (strncmp(line, "processor", 9) == 0) {
            cur = atoi(colon + 1);
        } else if (strncmp(line, "cpu MHz", 7) == 0 && cur >= 0) {
            double v = atof(colon + 1);
            for (int i = 0; i < n; i++) {
                if (ids[i] == cur) { mhz[i] = v; found++; break; }
            }
        }
    }
    return found;
}

static int freq_read(const int *ids, int n, double *mhz)
{
    int got = 0;
    for (int i = 0; i < n; i++) {
        char path[128];
        long long khz;
        snprintf(path, sizeof(path), SYSFS_CPU_DIR "/cpu%d/cpufreq/scaling_cur_freq", ids[i]);
        if (read_ll_file(path, &khz) == 0 && khz > 0) {
            mhz[i] = (double)khz / 1000.0;
            got++;
        } else {
            mhz[i] = NAN;
        }
    }
    if (got == 0) {
        FILE *f = fopen(CPUINFO_PATH, "r");
        if (f) {
            got = cpuinfo_mhz_parse(f, ids, n, mhz);
            fclose(f);
        }
    }
    return got;
}

/* ---------------------------------------------------------------------
 * CPU temperature: hwmon drivers that report CPU/package sensors, then
 * thermal zones as a fallback.
 * ------------------------------------------------------------------- */

typedef struct {
    char label[48];
    double celsius;
} TempSensor;

static int is_cpu_hwmon(const char *name)
{
    static const char *const names[] = {
        "coretemp", "k10temp", "k8temp", "zenpower", "fam15h_power",
        "cpu_thermal", "cpu-thermal", "soc_thermal", "via_cputemp", NULL
    };
    for (int i = 0; names[i]; i++)
        if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

static int is_cpu_thermal_zone(const char *type)
{
    return strstr(type, "x86_pkg_temp") || strstr(type, "cpu") ||
           strstr(type, "soc") || strstr(type, "acpitz");
}

static int temps_read(TempSensor *out, int max)
{
    int n = 0;
    DIR *dir = opendir(HWMON_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && n < max) {
            if (ent->d_name[0] == '.') continue;
            char path[512], name[64];
            snprintf(path, sizeof(path), HWMON_DIR "/%s/name", ent->d_name);
            if (read_small_file(path, name, sizeof(name)) <= 0) continue;
            chomp(name);
            if (!is_cpu_hwmon(name)) continue;
            for (int k = 1; k <= 64 && n < max; k++) {
                long long milli;
                snprintf(path, sizeof(path), HWMON_DIR "/%s/temp%d_input", ent->d_name, k);
                if (read_ll_file(path, &milli) != 0) continue;
                char label[40];
                snprintf(path, sizeof(path), HWMON_DIR "/%s/temp%d_label", ent->d_name, k);
                if (read_small_file(path, label, sizeof(label)) > 0) chomp(label);
                else snprintf(label, sizeof(label), "temp%d", k);
                snprintf(out[n].label, sizeof(out[n].label), "%.15s %.31s", name, label);
                out[n].celsius = (double)milli / 1000.0;
                n++;
            }
        }
        closedir(dir);
    }
    if (n > 0) return n;

    dir = opendir(THERMAL_DIR);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max) {
        if (strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;
        char path[512], type[40];
        long long milli;
        snprintf(path, sizeof(path), THERMAL_DIR "/%s/type", ent->d_name);
        if (read_small_file(path, type, sizeof(type)) <= 0) continue;
        chomp(type);
        if (!is_cpu_thermal_zone(type)) continue;
        snprintf(path, sizeof(path), THERMAL_DIR "/%s/temp", ent->d_name);
        if (read_ll_file(path, &milli) != 0) continue;
        snprintf(out[n].label, sizeof(out[n].label), "%s", type);
        out[n].celsius = (double)milli / 1000.0;
        n++;
    }
    closedir(dir);
    return n;
}

/* ---------------------------------------------------------------------
 * Per-process CPU time (/proc/[pid]/stat), for the top-processes list
 * ------------------------------------------------------------------- */

typedef struct {
    int pid;
    char state;
    unsigned long long ticks;         /* utime + stime, all threads */
    unsigned long long start;         /* starttime, detects pid reuse */
    char comm[32];
} ProcEntry;

typedef struct {
    ProcEntry *v;
    size_t n, cap;
} ProcTable;

typedef struct {
    int pid;
    char state;
    double pct;                       /* % of one CPU, like top(1) */
    char comm[32];
} TopProc;

/* Parses the contents of /proc/[pid]/stat. comm is wrapped in parens and
 * may itself contain spaces or parens, so fields are counted from the
 * LAST ')' rather than split naively. */
static int proc_parse_stat(const char *buf, ProcEntry *e)
{
    const char *lp = strchr(buf, '(');
    const char *rp = strrchr(buf, ')');
    if (!lp || !rp || rp < lp || rp[1] == '\0') return -1;
    memset(e, 0, sizeof(*e));
    e->pid = atoi(buf);
    size_t len = (size_t)(rp - lp - 1);
    if (len >= sizeof(e->comm)) len = sizeof(e->comm) - 1;
    memcpy(e->comm, lp + 1, len);
    e->comm[len] = '\0';
    unsigned long long utime, stime, start;
    char state;
    /* fields 3..22: state ppid pgrp session tty_nr tpgid flags minflt cminflt
     * majflt cmajflt utime stime cutime cstime priority nice num_threads
     * itrealvalue starttime */
    if (sscanf(rp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu "
                       "%*d %*d %*d %*d %*d %*d %llu",
               &state, &utime, &stime, &start) != 4)
        return -1;
    e->state = state;
    e->ticks = utime + stime;
    e->start = start;
    return 0;
}

static int proc_cmp_pid(const void *a, const void *b)
{
    int pa = ((const ProcEntry *)a)->pid, pb = ((const ProcEntry *)b)->pid;
    return (pa > pb) - (pa < pb);
}

static void proc_table_read(ProcTable *t)
{
    t->n = 0;
    DIR *dir = opendir("/proc");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        char path[288], buf[1024];
        snprintf(path, sizeof(path), "/proc/%s/stat", ent->d_name);
        if (read_small_file(path, buf, sizeof(buf)) <= 0) continue; /* raced with exit */
        ProcEntry e;
        if (proc_parse_stat(buf, &e) != 0) continue;
        if (t->n == t->cap) {
            size_t ncap = t->cap ? t->cap * 2 : 512;
            ProcEntry *nv = realloc(t->v, ncap * sizeof(*nv));
            if (!nv) break;
            t->v = nv;
            t->cap = ncap;
        }
        t->v[t->n++] = e;
    }
    closedir(dir);
    qsort(t->v, t->n, sizeof(*t->v), proc_cmp_pid);
}

static void proc_table_free(ProcTable *t)
{
    free(t->v);
    memset(t, 0, sizeof(*t));
}

static int top_cmp_desc(const void *a, const void *b)
{
    double pa = ((const TopProc *)a)->pct, pb = ((const TopProc *)b)->pct;
    if (pa != pb) return pa < pb ? 1 : -1;
    return ((const TopProc *)a)->pid - ((const TopProc *)b)->pid;
}

/* Fills out[] with up to n processes that used the most CPU between two
 * process tables taken elapsed seconds apart. A process missing from
 * prev (or whose pid was reused) started during the interval, so all its
 * CPU time belongs to this interval. Returns the number filled. */
static int top_compute(const ProcTable *prev, const ProcTable *cur, double elapsed,
                       long hz, int n, TopProc *out)
{
    if (n <= 0 || cur->n == 0 || elapsed <= 0 || hz <= 0) return 0;
    TopProc *cand = malloc(cur->n * sizeof(*cand));
    if (!cand) return 0;
    size_t nc = 0;
    for (size_t i = 0; i < cur->n; i++) {
        const ProcEntry *c = &cur->v[i];
        const ProcEntry *p = prev->n ? bsearch(c, prev->v, prev->n, sizeof(*c), proc_cmp_pid) : NULL;
        unsigned long long d;
        if (p && p->start == c->start)
            d = (c->ticks >= p->ticks) ? c->ticks - p->ticks : 0;
        else
            d = c->ticks;
        if (d == 0) continue;
        cand[nc].pid = c->pid;
        cand[nc].state = c->state;
        cand[nc].pct = 100.0 * (double)d / ((double)hz * elapsed);
        snprintf(cand[nc].comm, sizeof(cand[nc].comm), "%s", c->comm);
        nc++;
    }
    qsort(cand, nc, sizeof(*cand), top_cmp_desc);
    int m = (int)nc < n ? (int)nc : n;
    memcpy(out, cand, (size_t)m * sizeof(*out));
    free(cand);
    return m;
}

/* ---------------------------------------------------------------------
 * Interval parsing: accepts plain seconds ("5"), or suffixed
 * "5s", "5m", "1h", "1d".
 * ------------------------------------------------------------------- */

static long parse_interval(const char *s, char *errbuf, size_t errbuf_len)
{
    if (!s || !*s) {
        snprintf(errbuf, errbuf_len, "empty interval");
        return -1;
    }
    char *end;
    double val = strtod(s, &end);
    if (end == s || val < 0) {
        snprintf(errbuf, errbuf_len, "invalid interval '%s'", s);
        return -1;
    }
    long mult = 1;
    if (*end != '\0') {
        switch (tolower((unsigned char)*end)) {
            case 's': mult = 1; break;
            case 'm': mult = 60; break;
            case 'h': mult = 3600; break;
            case 'd': mult = 86400; break;
            default:
                snprintf(errbuf, errbuf_len,
                         "unknown interval suffix '%c' (use s/m/h/d)", *end);
                return -1;
        }
        if (*(end + 1) != '\0') {
            snprintf(errbuf, errbuf_len, "trailing characters after interval");
            return -1;
        }
    }
    long secs = (long)(val * mult);
    if (secs < 1) secs = 1;
    return secs;
}

static void format_interval(long secs, char *buf, size_t buflen)
{
    /* buf is expected to be reasonably sized (>=24 bytes) by callers */
    if (secs % 86400 == 0 && secs >= 86400)
        snprintf(buf, buflen, "%ldd", secs / 86400);
    else if (secs % 3600 == 0 && secs >= 3600)
        snprintf(buf, buflen, "%ldh", secs / 3600);
    else if (secs % 60 == 0 && secs >= 60)
        snprintf(buf, buflen, "%ldm", secs / 60);
    else
        snprintf(buf, buflen, "%lds", secs);
}

static void format_duration(double secs, char *buf, size_t buflen)
{
    long s = (long)secs;
    long d = s / 86400; s %= 86400;
    long h = s / 3600;  s %= 3600;
    long m = s / 60;    s %= 60;
    if (d > 0)
        snprintf(buf, buflen, "%ldd %ldh %ldm", d, h, m);
    else if (h > 0)
        snprintf(buf, buflen, "%ldh %ldm %llds", h, m, (long long)s);
    else
        snprintf(buf, buflen, "%ldm %llds", m, (long long)s);
}

/* Parses a --since/--until value: "now", "today", "yesterday", an
 * absolute local time ("YYYY-MM-DD", "YYYY-MM-DD HH:MM[:SS]", or with a
 * 'T' separator), or a relative interval meaning that long ago ("30m",
 * "2h", "7d"). A bare date (or "today"/"yesterday") used as the end of a
 * range (is_end) means the end of that day rather than its first second. */
static int parse_when(const char *s, time_t now, int is_end, time_t *out, char *err, size_t errlen)
{
    struct tm tm;
    if (strcmp(s, "now") == 0) { *out = now; return 0; }
    if (strcmp(s, "today") == 0 || strcmp(s, "yesterday") == 0) {
        localtime_r(&now, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        if (s[0] == 'y') tm.tm_mday -= 1;
        if (is_end) { tm.tm_hour = 23; tm.tm_min = 59; tm.tm_sec = 59; }
        tm.tm_isdst = -1;
        *out = mktime(&tm);
        return 0;
    }
    static const char *const fmts[] = {
        "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S",
        "%Y-%m-%d %H:%M", "%Y-%m-%dT%H:%M", "%Y-%m-%d", NULL
    };
    for (int i = 0; fmts[i]; i++) {
        memset(&tm, 0, sizeof(tm));
        const char *end = strptime(s, fmts[i], &tm);
        if (end && *end == '\0') {
            if (is_end && !fmts[i + 1]) { tm.tm_hour = 23; tm.tm_min = 59; tm.tm_sec = 59; }
            tm.tm_isdst = -1;
            *out = mktime(&tm);
            return 0;
        }
    }
    char ibuf[128];
    long secs = parse_interval(s, ibuf, sizeof(ibuf));
    if (secs >= 0) { *out = now - secs; return 0; }
    snprintf(err, errlen, "invalid time '%s' (use YYYY-MM-DD[ HH:MM[:SS]], "
             "today, yesterday, now, or an interval like 2h)", s);
    return -1;
}

/* ---------------------------------------------------------------------
 * Configuration: built-in defaults < config file < CPUMON_* environment
 * variables < command-line options. The config file uses the same
 * KEY=VALUE names as the environment variables.
 * ------------------------------------------------------------------- */

enum { FMT_TEXT = 0, FMT_JSON, FMT_CSV };
enum { TARGET_FILE = 1, TARGET_JOURNAL = 2 };

typedef struct {
    long interval_secs;
    char logdir[512];                 /* empty = pick automatically */
    int log_retention_days;           /* 0 disables auto-delete */
    int compress_after_days;          /* 0 disables compression */
    int format;                       /* FMT_* */
    int log_target;                   /* TARGET_* bitmask */
    int top_n;                        /* 0 disables process scanning */
    double alert_pct;                 /* <= 0 disables alerts */
    long alert_secs;                  /* how long usage must stay high */
    int no_log;                       /* interactive modes only */
} Config;

static const char *const kConfigKeys[] = {
    "CPUMON_INTERVAL", "CPUMON_LOG_DIR", "CPUMON_LOG_RETENTION_DAYS",
    "CPUMON_COMPRESS_AFTER_DAYS", "CPUMON_FORMAT", "CPUMON_LOG_TARGET",
    "CPUMON_TOP", "CPUMON_ALERT_THRESHOLD", "CPUMON_ALERT_DURATION",
    "CPUMON_NO_LOG", NULL
};

static const char *const kFormatNames[] = { "text", "json", "csv" };

static void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->interval_secs = 5;
    c->log_retention_days = DEFAULT_LOG_RETENTION_DAYS;
    c->compress_after_days = 0;
    c->format = FMT_TEXT;
    c->log_target = TARGET_FILE;
    c->top_n = DEFAULT_TOP;
    c->alert_pct = 0;
    c->alert_secs = 0;
}

static int parse_int_range(const char *s, long lo, long hi, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static int parse_bool(const char *s, int *out)
{
    if (!strcasecmp(s, "1") || !strcasecmp(s, "yes") || !strcasecmp(s, "true") || !strcasecmp(s, "on")) {
        *out = 1; return 0;
    }
    if (!strcasecmp(s, "0") || !strcasecmp(s, "no") || !strcasecmp(s, "false") || !strcasecmp(s, "off")) {
        *out = 0; return 0;
    }
    return -1;
}

/* Returns 0 on success, -1 for an invalid value, -2 for an unknown key. */
static int config_set(Config *c, const char *key, const char *val, char *err, size_t errlen)
{
    long v;
    if (strcmp(key, "CPUMON_INTERVAL") == 0) {
        char ibuf[128];
        long secs = parse_interval(val, ibuf, sizeof(ibuf));
        if (secs < 0) { snprintf(err, errlen, "%s", ibuf); return -1; }
        c->interval_secs = secs;
    } else if (strcmp(key, "CPUMON_LOG_DIR") == 0) {
        if (strlen(val) >= sizeof(c->logdir)) { snprintf(err, errlen, "log dir path too long"); return -1; }
        snprintf(c->logdir, sizeof(c->logdir), "%s", val);
    } else if (strcmp(key, "CPUMON_LOG_RETENTION_DAYS") == 0) {
        if (parse_int_range(val, 0, 1000000, &v) != 0) {
            snprintf(err, errlen, "invalid log retention '%s' (expected days, 0 = keep forever)", val);
            return -1;
        }
        c->log_retention_days = (int)v;
    } else if (strcmp(key, "CPUMON_COMPRESS_AFTER_DAYS") == 0) {
        if (parse_int_range(val, 0, 1000000, &v) != 0) {
            snprintf(err, errlen, "invalid compress-after-days '%s' (expected days, 0 = never)", val);
            return -1;
        }
        c->compress_after_days = (int)v;
    } else if (strcmp(key, "CPUMON_FORMAT") == 0) {
        int f = -1;
        for (int i = 0; i < 3; i++)
            if (strcasecmp(val, kFormatNames[i]) == 0) f = i;
        if (f < 0) { snprintf(err, errlen, "invalid format '%s' (use text, json or csv)", val); return -1; }
        c->format = f;
    } else if (strcmp(key, "CPUMON_LOG_TARGET") == 0) {
        if (!strcasecmp(val, "file")) c->log_target = TARGET_FILE;
        else if (!strcasecmp(val, "journal")) c->log_target = TARGET_JOURNAL;
        else if (!strcasecmp(val, "both")) c->log_target = TARGET_FILE | TARGET_JOURNAL;
        else { snprintf(err, errlen, "invalid log target '%s' (use file, journal or both)", val); return -1; }
    } else if (strcmp(key, "CPUMON_TOP") == 0) {
        if (parse_int_range(val, 0, MAX_TOP, &v) != 0) {
            snprintf(err, errlen, "invalid top process count '%s' (0-%d)", val, MAX_TOP);
            return -1;
        }
        c->top_n = (int)v;
    } else if (strcmp(key, "CPUMON_ALERT_THRESHOLD") == 0) {
        char *end;
        double p = strtod(val, &end);
        if (*end == '%') end++;
        if (end == val || *end != '\0' || p < 0 || p > 100) {
            snprintf(err, errlen, "invalid alert threshold '%s' (percent 0-100, 0 = off)", val);
            return -1;
        }
        c->alert_pct = p;
    } else if (strcmp(key, "CPUMON_ALERT_DURATION") == 0) {
        char ibuf[128];
        if (strcmp(val, "0") == 0) { c->alert_secs = 0; return 0; }
        long secs = parse_interval(val, ibuf, sizeof(ibuf));
        if (secs < 0) { snprintf(err, errlen, "%s", ibuf); return -1; }
        c->alert_secs = secs;
    } else if (strcmp(key, "CPUMON_NO_LOG") == 0) {
        int b;
        if (parse_bool(val, &b) != 0) { snprintf(err, errlen, "invalid boolean '%s'", val); return -1; }
        c->no_log = b;
    } else {
        snprintf(err, errlen, "unknown setting '%s'", key);
        return -2;
    }
    return 0;
}

/* Loads KEY=VALUE lines (# comments, blank lines and optional quotes
 * allowed). Bad lines are reported and skipped. Returns -1 only if the
 * file can't be opened (errno is set). */
static int config_load_file(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        chomp(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        if (strncmp(p, "export ", 7) == 0) p += 7;
        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "%s: %s:%d: expected KEY=VALUE, ignoring\n", PROGNAME, path, lineno);
            continue;
        }
        *eq = '\0';
        char *key = p, *val = eq + 1;
        chomp(key);
        while (*val == ' ' || *val == '\t') val++;
        size_t vl = strlen(val);
        if (vl >= 2 && (val[0] == '"' || val[0] == '\'') && val[vl - 1] == val[0]) {
            val[vl - 1] = '\0';
            val++;
        }
        char err[160];
        if (config_set(c, key, val, err, sizeof(err)) != 0)
            fprintf(stderr, "%s: %s:%d: %s, ignoring\n", PROGNAME, path, lineno, err);
    }
    fclose(f);
    return 0;
}

static void config_load_env(Config *c)
{
    for (int i = 0; kConfigKeys[i]; i++) {
        const char *v = getenv(kConfigKeys[i]);
        if (!v || !*v) continue;
        char err[160];
        if (config_set(c, kConfigKeys[i], v, err, sizeof(err)) != 0)
            fprintf(stderr, "%s: environment %s: %s, ignoring\n", PROGNAME, kConfigKeys[i], err);
    }
}

static int user_config_path(char *buf, size_t buflen)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) { snprintf(buf, buflen, "%s/" USER_CONFIG_REL, xdg); return 0; }
    const char *home = getenv("HOME");
    if (home && *home) { snprintf(buf, buflen, "%s/.config/" USER_CONFIG_REL, home); return 0; }
    return -1;
}

/* ---------------------------------------------------------------------
 * Derived stats - percentages are always a delta between two CpuStat
 * samples, never a single reading. Everything else a frame shows
 * (load, PSI, frequency, temperature) is read alongside the sample.
 * ------------------------------------------------------------------- */

typedef struct {
    double total_pct, user_pct, nice_pct, system_pct, idle_pct;
    double iowait_pct, irq_pct, softirq_pct, steal_pct;
} CoreDerived;

typedef struct {
    CoreDerived agg;
    CoreDerived core[MAX_CORES];
    int core_id[MAX_CORES];
    double core_mhz[MAX_CORES];       /* NAN when unknown */
    int ncores;
    double ctxt_per_sec;
    double forks_per_sec;
    unsigned long procs_running, procs_blocked;
    double elapsed_secs;
    time_t ts;
    double mono_start, mono_end;      /* the interval this frame covers */

    int have_load;
    double load[3];
    int have_uptime;
    double uptime;
    Psi psi;
    int have_freq;
    double freq_avg, freq_min, freq_max;
    TempSensor temps[MAX_TEMPS];
    int ntemps;
    double temp_max;                  /* NAN when no sensors */
    TopProc top[MAX_TOP];
    int ntop;
    int alert_active;
    double alert_threshold;
} Derived;

static unsigned long long jiffies_total(const CpuJiffies *j)
{
    return j->user + j->nice + j->system + j->idle + j->iowait +
           j->irq + j->softirq + j->steal;
    /* guest/guest_nice are already accounted for within user/nice on
     * modern kernels; excluded here to avoid double counting. */
}

static void derive_core(const CpuJiffies *prev, const CpuJiffies *cur, CoreDerived *d)
{
    memset(d, 0, sizeof(*d));
    unsigned long long pt = jiffies_total(prev), ct = jiffies_total(cur);
    if (ct < pt) { d->idle_pct = 100.0; return; } /* counters reset/wrapped */
    unsigned long long dt = ct - pt;
    if (dt == 0) { d->idle_pct = 100.0; return; }

    unsigned long long duser = cur->user - prev->user;
    unsigned long long dnice = cur->nice - prev->nice;
    unsigned long long dsys  = cur->system - prev->system;
    unsigned long long didle = cur->idle - prev->idle;
    unsigned long long diow  = cur->iowait - prev->iowait;
    unsigned long long dirq  = cur->irq - prev->irq;
    unsigned long long dsoft = cur->softirq - prev->softirq;
    unsigned long long dstl  = cur->steal - prev->steal;

    d->user_pct    = 100.0 * (double)duser / (double)dt;
    d->nice_pct    = 100.0 * (double)dnice / (double)dt;
    d->system_pct  = 100.0 * (double)dsys  / (double)dt;
    d->idle_pct    = 100.0 * (double)didle / (double)dt;
    d->iowait_pct  = 100.0 * (double)diow  / (double)dt;
    d->irq_pct     = 100.0 * (double)dirq  / (double)dt;
    d->softirq_pct = 100.0 * (double)dsoft / (double)dt;
    d->steal_pct   = 100.0 * (double)dstl  / (double)dt;
    d->total_pct   = 100.0 - d->idle_pct;
    if (d->total_pct < 0) d->total_pct = 0;
    if (d->total_pct > 100) d->total_pct = 100;
}

static void derive(const CpuStat *prev, const CpuStat *cur, Derived *d)
{
    memset(d, 0, sizeof(*d));
    derive_core(&prev->total, &cur->total, &d->agg);

    d->ncores = cur->ncores;
    for (int i = 0; i < cur->ncores; i++) {
        /* Match cores by id, not position, so a CPU going offline or
         * coming back between samples doesn't shift every later core. */
        const CpuJiffies *p = NULL;
        if (i < prev->ncores && prev->core_id[i] == cur->core_id[i]) {
            p = &prev->cores[i];
        } else {
            for (int j = 0; j < prev->ncores; j++)
                if (prev->core_id[j] == cur->core_id[i]) { p = &prev->cores[j]; break; }
        }
        derive_core(p ? p : &cur->cores[i], &cur->cores[i], &d->core[i]);
        d->core_id[i] = cur->core_id[i];
        d->core_mhz[i] = NAN;
    }

    d->ts = cur->sampled_at;
    d->mono_start = prev->mono;
    d->mono_end = cur->mono;
    d->elapsed_secs = cur->mono - prev->mono;
    if (d->elapsed_secs <= 0) d->elapsed_secs = 1;

    unsigned long long dctxt = (cur->ctxt >= prev->ctxt) ? cur->ctxt - prev->ctxt : 0;
    unsigned long long dproc = (cur->processes >= prev->processes) ? cur->processes - prev->processes : 0;
    d->ctxt_per_sec  = (double)dctxt / d->elapsed_secs;
    d->forks_per_sec = (double)dproc / d->elapsed_secs;
    d->procs_running = cur->procs_running;
    d->procs_blocked = cur->procs_blocked;
    d->temp_max = NAN;
}

/* ---------------------------------------------------------------------
 * Sampler - owns the previous/current /proc/stat and process-table
 * samples and turns them into a Derived frame.
 * ------------------------------------------------------------------- */

typedef struct {
    CpuStat prev, cur;
    ProcTable pprev, pcur;
    int procs;                        /* scan /proc/[pid] for top processes */
} Sampler;

static void sampler_start(Sampler *s, int procs)
{
    memset(s, 0, sizeof(*s));
    s->procs = procs;
    stat_read(&s->prev);
    if (procs) proc_table_read(&s->pprev);
    usleep(BOOTSTRAP_USEC);
    stat_read(&s->cur);
    if (procs) proc_table_read(&s->pcur);
}

static int sampler_tick(Sampler *s)
{
    CpuStat next;
    if (stat_read(&next) != 0) return -1;
    s->prev = s->cur;
    s->cur = next;
    ProcTable t = s->pprev;
    s->pprev = s->pcur;
    s->pcur = t;
    if (s->procs) proc_table_read(&s->pcur);
    else s->pcur.n = 0;
    return 0;
}

/* Turning process scanning on needs a baseline, or the next frame would
 * charge every process its whole lifetime's CPU time. */
static void sampler_set_procs(Sampler *s, int procs)
{
    if (procs && !s->procs) proc_table_read(&s->pcur);
    s->procs = procs;
}

static void sampler_free(Sampler *s)
{
    proc_table_free(&s->pprev);
    proc_table_free(&s->pcur);
}

static void sampler_frame(const Sampler *s, const Config *c, Derived *d)
{
    derive(&s->prev, &s->cur, d);

    d->have_load = loadavg_read(&d->load[0], &d->load[1], &d->load[2]) == 0;
    d->have_uptime = uptime_read(&d->uptime) == 0;
    psi_read(&d->psi);

    if (freq_read(d->core_id, d->ncores, d->core_mhz) > 0) {
        double sum = 0;
        int n = 0;
        d->freq_min = INFINITY;
        d->freq_max = -INFINITY;
        for (int i = 0; i < d->ncores; i++) {
            double m = d->core_mhz[i];
            if (isnan(m)) continue;
            sum += m;
            n++;
            if (m < d->freq_min) d->freq_min = m;
            if (m > d->freq_max) d->freq_max = m;
        }
        if (n > 0) {
            d->have_freq = 1;
            d->freq_avg = sum / n;
        }
    }

    d->ntemps = temps_read(d->temps, MAX_TEMPS);
    for (int i = 0; i < d->ntemps; i++)
        if (isnan(d->temp_max) || d->temps[i].celsius > d->temp_max)
            d->temp_max = d->temps[i].celsius;

    if (s->procs && c->top_n > 0)
        d->ntop = top_compute(&s->pprev, &s->pcur, d->elapsed_secs,
                              sysconf(_SC_CLK_TCK), c->top_n, d->top);
}

/* ---------------------------------------------------------------------
 * History ring buffer + sparklines
 * ------------------------------------------------------------------- */

typedef struct {
    int count, head, ncores;
    unsigned char total[HIST_LEN];
    unsigned char core[MAX_CORES][HIST_LEN];
} History;

static History g_hist;
static int g_utf8 = 0;

static unsigned char pct_byte(double p)
{
    if (!(p >= 0)) return 0;
    if (p > 100) return 100;
    return (unsigned char)(p + 0.5);
}

static void history_push(History *h, const Derived *d)
{
    if (d->ncores != h->ncores) {
        memset(h, 0, sizeof(*h));
        h->ncores = d->ncores;
    }
    h->total[h->head] = pct_byte(d->agg.total_pct);
    for (int i = 0; i < d->ncores; i++)
        h->core[i][h->head] = pct_byte(d->core[i].total_pct);
    h->head = (h->head + 1) % HIST_LEN;
    if (h->count < HIST_LEN) h->count++;
}

/* Copies the newest (up to max) samples, oldest first, for core index
 * `core` (-1 = aggregate). Returns how many were copied. */
static int history_series(const History *h, int core, int max, unsigned char *out)
{
    if (core >= h->ncores) return 0;
    int n = h->count < max ? h->count : max;
    int start = (h->head - n + HIST_LEN) % HIST_LEN;
    const unsigned char *src = core < 0 ? h->total : h->core[core];
    for (int i = 0; i < n; i++)
        out[i] = src[(start + i) % HIST_LEN];
    return n;
}

static void sparkline(char *buf, size_t buflen, const unsigned char *vals, int n, int utf8)
{
    static const char *const blocks[] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
    };
    static const char ascii[] = "_.-:=+*#%@";
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        int v = vals[i] > 100 ? 100 : vals[i];
        if (utf8) {
            const char *b = blocks[(v * 7 + 50) / 100];
            if (used + 4 > buflen) break;
            memcpy(buf + used, b, 3);
            used += 3;
        } else {
            if (used + 2 > buflen) break;
            buf[used++] = ascii[(v * 9 + 50) / 100];
        }
        buf[used] = '\0';
    }
}

static void history_stats(const unsigned char *v, int n, int *mn, int *avg, int *mx)
{
    *mn = 100; *mx = 0;
    long sum = 0;
    for (int i = 0; i < n; i++) {
        if (v[i] < *mn) *mn = v[i];
        if (v[i] > *mx) *mx = v[i];
        sum += v[i];
    }
    *avg = n ? (int)(sum / n) : 0;
    if (n == 0) *mn = 0;
}

/* ---------------------------------------------------------------------
 * Alerts - raised when aggregate usage stays at/above a threshold for
 * a configurable duration, cleared once it drops back below.
 * ------------------------------------------------------------------- */

enum { ALERT_NONE = 0, ALERT_RAISED, ALERT_CLEARED };

typedef struct {
    int active;
    double above_since;               /* monotonic; < 0 when below */
    double active_since;
} AlertState;

static AlertState g_alert = { 0, -1, 0 };
static char g_alert_msg[256] = "";

/* A frame's usage is the average over [start, end], so "above since"
 * starts at the beginning of the first high interval. */
static int alert_update(AlertState *a, double threshold, long hold_secs,
                        double pct, double start, double end)
{
    if (threshold <= 0) {
        int was = a->active;
        a->active = 0;
        a->above_since = -1;
        return was ? ALERT_CLEARED : ALERT_NONE;
    }
    if (pct >= threshold) {
        if (a->above_since < 0) a->above_since = start;
        if (!a->active && end - a->above_since >= (double)hold_secs) {
            a->active = 1;
            a->active_since = a->above_since;
            return ALERT_RAISED;
        }
        return ALERT_NONE;
    }
    a->above_since = -1;
    if (a->active) {
        a->active = 0;
        return ALERT_CLEARED;
    }
    return ALERT_NONE;
}

static void alert_message(int event, const AlertState *a, const Config *c, const Derived *d,
                          char *buf, size_t buflen)
{
    char dur[64];
    if (event == ALERT_RAISED) {
        format_duration(d->mono_end - a->active_since, dur, sizeof(dur));
        int n = snprintf(buf, buflen, "CPU usage %.1f%% has been >= %.0f%% for %s",
                         d->agg.total_pct, c->alert_pct, dur);
        if (d->ntop > 0 && n > 0 && (size_t)n < buflen)
            snprintf(buf + n, buflen - (size_t)n, " (top: %s[%d] %.1f%%)",
                     d->top[0].comm, d->top[0].pid, d->top[0].pct);
    } else {
        format_duration(d->mono_end - a->active_since, dur, sizeof(dur));
        snprintf(buf, buflen, "CPU usage back below %.0f%% (now %.1f%%) after %s",
                 c->alert_pct, d->agg.total_pct, dur);
    }
}

/* ---------------------------------------------------------------------
 * Output formatting helpers
 * ------------------------------------------------------------------- */

typedef struct {
    char hostname[256];
    char kernel[128];
    char model[192];
} HostInfo;

static void host_info_read(HostInfo *h)
{
    snprintf(h->hostname, sizeof(h->hostname), "unknown");
    gethostname(h->hostname, sizeof(h->hostname) - 1);
    struct utsname uts;
    snprintf(h->kernel, sizeof(h->kernel), "unknown");
    if (uname(&uts) == 0)
        snprintf(h->kernel, sizeof(h->kernel), "%s", uts.release);
    cpu_model_read(h->model, sizeof(h->model));
}

/* JSON string. Non-ASCII bytes become '?' so arbitrary process names
 * can never produce invalid UTF-8 in the output. */
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                else if (*p >= 0x80) fputc('?', f);
                else fputc(*p, f);
        }
    }
    fputc('"', f);
}

static void json_num(FILE *f, double v, int prec)
{
    if (isnan(v) || isinf(v)) fputs("null", f);
    else fprintf(f, "%.*f", prec, v);
}

static void csv_str(FILE *f, const char *s)
{
    if (!strpbrk(s, ",\"\n\r")) { fputs(s, f); return; }
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void csv_num(FILE *f, double v, int prec)
{
    if (!(isnan(v) || isinf(v))) fprintf(f, "%.*f", prec, v);
}

static void iso_time(time_t t, char *buf, size_t buflen)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%S%z", &tmv);
}

static void print_bar_plain(FILE *out, double pct, int width)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)((pct / 100.0) * width + 0.5);
    fputc('[', out);
    for (int i = 0; i < width; i++)
        fputc(i < filled ? '#' : '-', out);
    fprintf(out, "] %5.1f%%", pct);
}

/* Prints "Label1: value1   Label2: value2" (or just the first pair when
 * lbl2 is NULL), column-aligned so daily log files scan as easily as the
 * interactive display instead of one dense key=value line per metric. */
static void log_two_col(FILE *f, const char *lbl1, const char *val1,
                         const char *lbl2, const char *val2)
{
    char l1[24];
    snprintf(l1, sizeof(l1), "%s:", lbl1);
    if (lbl2) {
        char l2[24];
        snprintf(l2, sizeof(l2), "%s:", lbl2);
        fprintf(f, "  %-18s%-12s%-18s%s\n", l1, val1, l2, val2);
    } else {
        fprintf(f, "  %-18s%s\n", l1, val1);
    }
}

static void core_cell(const Derived *d, int i, char *buf, size_t buflen)
{
    char lbl[16], mhz[24] = "";
    snprintf(lbl, sizeof(lbl), "cpu%d", d->core_id[i]);
    if (!isnan(d->core_mhz[i]))
        snprintf(mhz, sizeof(mhz), "  %5.0f MHz", d->core_mhz[i]);
    snprintf(buf, buflen, "%-7s%6.1f%%%s", lbl, d->core[i].total_pct, mhz);
}

/* ---------------------------------------------------------------------
 * Snapshot writers: text (man-page styled sections), JSON (one object
 * per line) and CSV (one row per snapshot).
 * ------------------------------------------------------------------- */

static void write_text_snapshot(FILE *f, const Derived *d, const HostInfo *h)
{
    char ts[64];
    struct tm tmv;
    localtime_r(&d->ts, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %Z", &tmv);

    fprintf(f, "======================================================================\n");
    fprintf(f, "%s   host=%s   kernel=%s\n", ts, h->hostname, h->kernel);
    fprintf(f, "======================================================================\n");

    fprintf(f, "CPU OVERVIEW  (%d core%s - %s)\n  ", d->ncores, d->ncores == 1 ? "" : "s", h->model);
    print_bar_plain(f, d->agg.total_pct, 40);
    fputc('\n', f);
    char user_s[16], sys_s[16], idle_s[16], iow_s[16];
    snprintf(user_s, sizeof(user_s), "%.1f%%", d->agg.user_pct);
    snprintf(sys_s, sizeof(sys_s), "%.1f%%", d->agg.system_pct);
    snprintf(idle_s, sizeof(idle_s), "%.1f%%", d->agg.idle_pct);
    snprintf(iow_s, sizeof(iow_s), "%.1f%%", d->agg.iowait_pct);
    log_two_col(f, "User", user_s, "System", sys_s);
    log_two_col(f, "Idle", idle_s, "IOWait", iow_s);
    if (d->alert_active)
        fprintf(f, "  ALERT: usage >= %.0f%% threshold\n", d->alert_threshold);
    fputc('\n', f);

    if (d->have_freq) {
        char avg_s[24], rng_s[40];
        snprintf(avg_s, sizeof(avg_s), "%.0f MHz", d->freq_avg);
        snprintf(rng_s, sizeof(rng_s), "%.0f-%.0f MHz", d->freq_min, d->freq_max);
        fprintf(f, "FREQUENCY\n");
        log_two_col(f, "Average", avg_s, "Range", rng_s);
        fputc('\n', f);
    }

    if (d->ntemps > 0) {
        fprintf(f, "TEMPERATURE\n");
        for (int i = 0; i < d->ntemps; i++)
            fprintf(f, "  %-32s%.1f C\n", d->temps[i].label, d->temps[i].celsius);
        fputc('\n', f);
    }

    if (d->have_load) {
        fprintf(f, "LOAD AVERAGE\n");
        fprintf(f, "  1m: %.2f   5m: %.2f   15m: %.2f\n\n", d->load[0], d->load[1], d->load[2]);
    }

    if (d->psi.have_some) {
        fprintf(f, "PRESSURE (PSI)\n");
        fprintf(f, "  some  avg10: %6.2f%%   avg60: %6.2f%%   avg300: %6.2f%%\n",
                d->psi.some[0], d->psi.some[1], d->psi.some[2]);
        if (d->psi.have_full)
            fprintf(f, "  full  avg10: %6.2f%%   avg60: %6.2f%%   avg300: %6.2f%%\n",
                    d->psi.full[0], d->psi.full[1], d->psi.full[2]);
        fputc('\n', f);
    }

    fprintf(f, "PROCESSES & CONTEXT SWITCHES\n");
    char running_s[16], blocked_s[16], ctxt_s[24], forks_s[24];
    snprintf(running_s, sizeof(running_s), "%lu", d->procs_running);
    snprintf(blocked_s, sizeof(blocked_s), "%lu", d->procs_blocked);
    snprintf(ctxt_s, sizeof(ctxt_s), "%.1f/s", d->ctxt_per_sec);
    snprintf(forks_s, sizeof(forks_s), "%.2f/s", d->forks_per_sec);
    log_two_col(f, "Running", running_s, "Blocked", blocked_s);
    log_two_col(f, "Ctxt switches", ctxt_s, "Forks", forks_s);
    fputc('\n', f);

    if (d->ntop > 0) {
        fprintf(f, "TOP PROCESSES\n");
        fprintf(f, "  %8s %7s  S  COMMAND\n", "PID", "%CPU");
        for (int i = 0; i < d->ntop; i++)
            fprintf(f, "  %8d %7.1f  %c  %s\n", d->top[i].pid, d->top[i].pct,
                    d->top[i].state, d->top[i].comm);
        fputc('\n', f);
    }

    if (d->have_uptime) {
        char up_s[64];
        format_duration(d->uptime, up_s, sizeof(up_s));
        fprintf(f, "UPTIME\n  %s\n\n", up_s);
    }

    fprintf(f, "PER-CORE USAGE\n");
    for (int i = 0; i < d->ncores; i += 2) {
        char c1[48], c2[48] = "";
        core_cell(d, i, c1, sizeof(c1));
        if (i + 1 < d->ncores) core_cell(d, i + 1, c2, sizeof(c2));
        fprintf(f, "  %-32s%s\n", c1, c2);
    }
    fputc('\n', f);

    fprintf(f, "AGGREGATE TIME BREAKDOWN\n");
    char nice_s[16], irq_s[16], soft_s[16], steal_s[16];
    snprintf(nice_s, sizeof(nice_s), "%.1f%%", d->agg.nice_pct);
    snprintf(irq_s, sizeof(irq_s), "%.1f%%", d->agg.irq_pct);
    snprintf(soft_s, sizeof(soft_s), "%.1f%%", d->agg.softirq_pct);
    snprintf(steal_s, sizeof(steal_s), "%.1f%%", d->agg.steal_pct);
    log_two_col(f, "Nice", nice_s, "IRQ", irq_s);
    log_two_col(f, "SoftIRQ", soft_s, "Steal", steal_s);
    fputc('\n', f);

    fflush(f);
}

static void write_json_snapshot(FILE *f, const Derived *d, const HostInfo *h)
{
    char iso[40];
    iso_time(d->ts, iso, sizeof(iso));
    fprintf(f, "{\"ts\":%lld,\"time\":", (long long)d->ts);
    json_str(f, iso);
    fputs(",\"host\":", f); json_str(f, h->hostname);
    fputs(",\"kernel\":", f); json_str(f, h->kernel);
    fputs(",\"model\":", f); json_str(f, h->model);
    fprintf(f, ",\"cores\":%d,\"interval_secs\":%.3f", d->ncores, d->elapsed_secs);

    const CoreDerived *a = &d->agg;
    fprintf(f, ",\"cpu\":{\"total\":%.2f,\"user\":%.2f,\"nice\":%.2f,\"system\":%.2f,"
               "\"idle\":%.2f,\"iowait\":%.2f,\"irq\":%.2f,\"softirq\":%.2f,\"steal\":%.2f}",
            a->total_pct, a->user_pct, a->nice_pct, a->system_pct, a->idle_pct,
            a->iowait_pct, a->irq_pct, a->softirq_pct, a->steal_pct);

    fputs(",\"per_core\":[", f);
    for (int i = 0; i < d->ncores; i++) {
        fprintf(f, "%s{\"id\":%d,\"total\":%.2f,\"user\":%.2f,\"system\":%.2f,\"iowait\":%.2f,\"mhz\":",
                i ? "," : "", d->core_id[i], d->core[i].total_pct, d->core[i].user_pct,
                d->core[i].system_pct, d->core[i].iowait_pct);
        json_num(f, d->core_mhz[i], 0);
        fputc('}', f);
    }
    fputc(']', f);

    if (d->have_load)
        fprintf(f, ",\"load\":[%.2f,%.2f,%.2f]", d->load[0], d->load[1], d->load[2]);
    else
        fputs(",\"load\":null", f);
    fputs(",\"uptime_secs\":", f);
    json_num(f, d->have_uptime ? d->uptime : NAN, 0);

    fprintf(f, ",\"procs\":{\"running\":%lu,\"blocked\":%lu,\"ctxt_per_sec\":%.1f,\"forks_per_sec\":%.2f}",
            d->procs_running, d->procs_blocked, d->ctxt_per_sec, d->forks_per_sec);

    if (d->psi.have_some) {
        fprintf(f, ",\"psi\":{\"some\":{\"avg10\":%.2f,\"avg60\":%.2f,\"avg300\":%.2f}",
                d->psi.some[0], d->psi.some[1], d->psi.some[2]);
        if (d->psi.have_full)
            fprintf(f, ",\"full\":{\"avg10\":%.2f,\"avg60\":%.2f,\"avg300\":%.2f}",
                    d->psi.full[0], d->psi.full[1], d->psi.full[2]);
        fputc('}', f);
    } else {
        fputs(",\"psi\":null", f);
    }

    if (d->have_freq)
        fprintf(f, ",\"freq_mhz\":{\"avg\":%.0f,\"min\":%.0f,\"max\":%.0f}",
                d->freq_avg, d->freq_min, d->freq_max);
    else
        fputs(",\"freq_mhz\":null", f);

    fputs(",\"temps\":[", f);
    for (int i = 0; i < d->ntemps; i++) {
        fputs(i ? ",{\"label\":" : "{\"label\":", f);
        json_str(f, d->temps[i].label);
        fprintf(f, ",\"celsius\":%.1f}", d->temps[i].celsius);
    }
    fputs("],\"temp_max_c\":", f);
    json_num(f, d->temp_max, 1);

    fputs(",\"top\":[", f);
    for (int i = 0; i < d->ntop; i++) {
        fprintf(f, "%s{\"pid\":%d,\"comm\":", i ? "," : "", d->top[i].pid);
        json_str(f, d->top[i].comm);
        fprintf(f, ",\"state\":\"%c\",\"cpu\":%.1f}",
                isprint((unsigned char)d->top[i].state) && d->top[i].state != '"' &&
                d->top[i].state != '\\' ? d->top[i].state : '?', d->top[i].pct);
    }
    fputc(']', f);

    fprintf(f, ",\"alert\":{\"active\":%s,\"threshold\":",
            d->alert_active ? "true" : "false");
    json_num(f, d->alert_threshold > 0 ? d->alert_threshold : NAN, 1);
    fputs("}}\n", f);
    fflush(f);
}

#define CSV_FIXED_COLS 27

static void write_csv_header(FILE *f, const Derived *d)
{
    fputs("ts,time,host,total,user,nice,system,idle,iowait,irq,softirq,steal,"
          "load1,load5,load15,running,blocked,ctxt_per_sec,forks_per_sec,"
          "psi_some_avg10,psi_full_avg10,freq_avg_mhz,temp_max_c,"
          "top_pid,top_comm,top_cpu,alert", f);
    for (int i = 0; i < d->ncores; i++)
        fprintf(f, ",cpu%d", d->core_id[i]);
    fputc('\n', f);
}

static void write_csv_row(FILE *f, const Derived *d, const HostInfo *h)
{
    char iso[40];
    iso_time(d->ts, iso, sizeof(iso));
    const CoreDerived *a = &d->agg;
    fprintf(f, "%lld,%s,", (long long)d->ts, iso);
    csv_str(f, h->hostname);
    fprintf(f, ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,",
            a->total_pct, a->user_pct, a->nice_pct, a->system_pct, a->idle_pct,
            a->iowait_pct, a->irq_pct, a->softirq_pct, a->steal_pct);
    if (d->have_load) fprintf(f, "%.2f,%.2f,%.2f,", d->load[0], d->load[1], d->load[2]);
    else fputs(",,,", f);
    fprintf(f, "%lu,%lu,%.1f,%.2f,", d->procs_running, d->procs_blocked,
            d->ctxt_per_sec, d->forks_per_sec);
    csv_num(f, d->psi.have_some ? d->psi.some[0] : NAN, 2); fputc(',', f);
    csv_num(f, d->psi.have_full ? d->psi.full[0] : NAN, 2); fputc(',', f);
    csv_num(f, d->have_freq ? d->freq_avg : NAN, 0); fputc(',', f);
    csv_num(f, d->temp_max, 1); fputc(',', f);
    if (d->ntop > 0) {
        fprintf(f, "%d,", d->top[0].pid);
        csv_str(f, d->top[0].comm);
        fprintf(f, ",%.1f,", d->top[0].pct);
    } else {
        fputs(",,,", f);
    }
    fprintf(f, "%d", d->alert_active);
    for (int i = 0; i < d->ncores; i++)
        fprintf(f, ",%.2f", d->core[i].total_pct);
    fputc('\n', f);
    fflush(f);
}

/* ---------------------------------------------------------------------
 * Logging - snapshots, one file per calendar day, plus optional
 * systemd journal output
 * ------------------------------------------------------------------- */

static void today_str(char *buf, size_t buflen)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, buflen, "%Y-%m-%d", &tmv);
}

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }
    /* Try to create it (and parents, best-effort single level) */
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void resolve_logdir(const char *requested, char *out, size_t outlen)
{
    if (requested && *requested) {
        strncpy(out, requested, outlen - 1);
        out[outlen - 1] = '\0';
        return;
    }
    /* Prefer /var/log/cpumon if it exists or we can create/write to it */
    if (ensure_dir(DEFAULT_LOGDIR) == 0 && access(DEFAULT_LOGDIR, W_OK) == 0) {
        strncpy(out, DEFAULT_LOGDIR, outlen - 1);
        out[outlen - 1] = '\0';
        return;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(out, outlen, "%s/%s", home, FALLBACK_LOGDIR);
        /* create parent-ish best effort */
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.local", home);
        ensure_dir(parent);
        snprintf(parent, sizeof(parent), "%s/.local/share", home);
        ensure_dir(parent);
        ensure_dir(out);
        return;
    }
    strncpy(out, "/tmp/cpumon", outlen - 1);
    out[outlen - 1] = '\0';
    ensure_dir(out);
}

static const char *const kLogExt[] = { "log", "jsonl", "csv" };

/* Recognises the file names cpumon itself writes: "YYYY-MM-DD.<ext>"
 * where ext is log/jsonl/csv, optionally followed by ".gz". Anything else
 * returns -1, so a log dir shared with other files is safe to sweep. */
static int parse_log_filename_date(const char *name, struct tm *out, int *fmt, int *gz)
{
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (name[i] != '-') return -1; }
        else if (!isdigit((unsigned char)name[i])) return -1;
    }
    if (name[10] != '.') return -1;
    const char *ext = name + 11;
    int f = -1, z = 0;
    for (int i = 0; i < 3; i++) {
        size_t el = strlen(kLogExt[i]);
        if (strncmp(ext, kLogExt[i], el) == 0) {
            if (ext[el] == '\0') { f = i; break; }
            if (strcmp(ext + el, ".gz") == 0) { f = i; z = 1; break; }
        }
    }
    if (f < 0) return -1;
    int y = atoi(name), mo = atoi(name + 5), d = atoi(name + 8);
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
    memset(out, 0, sizeof(*out));
    out->tm_year = y - 1900;
    out->tm_mon  = mo - 1;
    out->tm_mday = d;
    out->tm_hour = 12; /* noon, to sidestep any DST edge effects in mktime */
    out->tm_isdst = -1;
    if (fmt) *fmt = f;
    if (gz) *gz = z;
    return 0;
}

/* Whole calendar days between the file's date and today (0 = today). */
static int log_age_days(struct tm *file_tm, time_t now)
{
    struct tm today;
    localtime_r(&now, &today);
    today.tm_hour = 12; today.tm_min = 0; today.tm_sec = 0; today.tm_isdst = -1;
    time_t t_today = mktime(&today);
    time_t t_file = mktime(file_tm);
    double days = difftime(t_today, t_file) / 86400.0;
    return (int)(days + (days >= 0 ? 0.5 : -0.5));
}

static int gzip_file(const char *src)
{
    char dst[1024];
    snprintf(dst, sizeof(dst), "%s.gz", src);
    struct stat st;
    int existed = stat(dst, &st) == 0;
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    /* "a": if a .gz for this day already exists (e.g. the day's log was
     * reopened after being compressed), append a new gzip member -
     * multi-member files decompress as one stream. */
    gzFile out = gzopen(dst, "ab9");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int err = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (gzwrite(out, buf, (unsigned)n) != (int)n) { err = 1; break; }
    }
    if (ferror(in)) err = 1;
    fclose(in);
    if (gzclose(out) != Z_OK) err = 1;
    if (err) {
        if (!existed) unlink(dst);
        return -1;
    }
    return unlink(src);
}

/* ---------------------------------------------------------------------
 * Log maintenance - delete daily log files older than retention_days
 * (<= 0 disables) and gzip ones at least compress_after_days old
 * (<= 0 disables). Only ever touches files matching the exact name
 * patterns cpumon itself writes.
 * ------------------------------------------------------------------- */

static void logs_maintain(const char *logdir, int retention_days, int compress_after_days)
{
    if (retention_days <= 0 && compress_after_days <= 0) return;

    DIR *dir = opendir(logdir);
    if (!dir) return;

    time_t now = time(NULL);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        struct tm file_tm;
        int gz;
        if (parse_log_filename_date(ent->d_name, &file_tm, NULL, &gz) != 0)
            continue;
        int age = log_age_days(&file_tm, now);

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", logdir, ent->d_name);
        if (retention_days > 0 && age > retention_days) {
            if (unlink(path) != 0)
                fprintf(stderr, "%s: warning: could not delete expired log '%s': %s\n",
                        PROGNAME, path, strerror(errno));
        } else if (compress_after_days > 0 && !gz && age >= compress_after_days) {
            if (gzip_file(path) != 0)
                fprintf(stderr, "%s: warning: could not compress log '%s': %s\n",
                        PROGNAME, path, strerror(errno));
        }
    }
    closedir(dir);
}

static FILE *g_logfile = NULL;
static char g_log_date[16] = "";
static int g_log_format = -1;
static int g_csv_cores = -1;          /* core count of the last CSV header written */
static char g_logdir[512] = "";

static void log_close(void)
{
    if (g_logfile) fclose(g_logfile);
    g_logfile = NULL;
    g_log_date[0] = '\0';
    g_log_format = -1;
}

/* Core count declared by the last header line in an existing CSV file,
 * so reopening it doesn't repeat an identical header. */
static int csv_file_header_cores(FILE *f)
{
    char line[8192];
    int cores = -1;
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ts,time,", 8) != 0) continue;
        int cols = 1;
        for (const char *p = line; *p; p++) if (*p == ',') cols++;
        cores = cols - CSV_FIXED_COLS;
    }
    fseek(f, 0, SEEK_END);
    return cores;
}

static int log_open_for_today(const Config *c)
{
    char date[16];
    today_str(date, sizeof(date));
    if (g_logfile && strcmp(date, g_log_date) == 0 && g_log_format == c->format)
        return 0; /* already open for today */

    log_close();
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.%s", g_logdir, date, kLogExt[c->format]);
    g_logfile = fopen(path, c->format == FMT_CSV ? "a+" : "a");
    if (!g_logfile) {
        fprintf(stderr, "%s: cannot open log file '%s': %s\n",
                PROGNAME, path, strerror(errno));
        return -1;
    }
    snprintf(g_log_date, sizeof(g_log_date), "%s", date);
    g_log_format = c->format;
    g_csv_cores = c->format == FMT_CSV ? csv_file_header_cores(g_logfile) : -1;
    /* Rolling to a new day's file is a natural, cheap point to sweep
     * out anything older than the retention window. */
    logs_maintain(g_logdir, c->log_retention_days, c->compress_after_days);
    return 0;
}

/* systemd journal native protocol: KEY=value lines in one datagram;
 * values containing newlines use the KEY\n<le64 length><data>\n form.
 * Talking to the socket directly avoids a libsystemd dependency. */
static void journal_field(FILE *m, const char *key, const char *val)
{
    if (!strchr(val, '\n')) {
        fprintf(m, "%s=%s\n", key, val);
        return;
    }
    uint64_t len = strlen(val);
    unsigned char le[8];
    for (int i = 0; i < 8; i++) le[i] = (unsigned char)(len >> (8 * i));
    fprintf(m, "%s\n", key);
    fwrite(le, 1, 8, m);
    fwrite(val, 1, (size_t)len, m);
    fputc('\n', m);
}

static int journal_send(const char *buf, size_t len)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", JOURNAL_SOCKET);
    ssize_t n = sendto(fd, buf, len, MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static void summary_line(const Derived *d, char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen, "cpu %.1f%% (user %.1f%% sys %.1f%% iowait %.1f%% steal %.1f%%)",
                     d->agg.total_pct, d->agg.user_pct, d->agg.system_pct,
                     d->agg.iowait_pct, d->agg.steal_pct);
    if (d->have_load && n > 0 && (size_t)n < buflen)
        n += snprintf(buf + n, buflen - (size_t)n, " load %.2f/%.2f/%.2f",
                      d->load[0], d->load[1], d->load[2]);
    if (d->ntop > 0 && n > 0 && (size_t)n < buflen)
        snprintf(buf + n, buflen - (size_t)n, " top %s[%d] %.1f%%",
                 d->top[0].comm, d->top[0].pid, d->top[0].pct);
}

static void journal_emit(const Derived *d, const HostInfo *h)
{
    static int warned = 0;
    char summary[512], v[64];
    summary_line(d, summary, sizeof(summary));

    char *json = NULL, *msg = NULL;
    size_t json_len = 0, msg_len = 0;
    FILE *jm = open_memstream(&json, &json_len);
    if (jm) {
        write_json_snapshot(jm, d, h);
        fclose(jm);
        if (json_len > 0 && json[json_len - 1] == '\n') json[--json_len] = '\0';
    }

    FILE *m = open_memstream(&msg, &msg_len);
    if (!m) { free(json); return; }
    journal_field(m, "MESSAGE", summary);
    journal_field(m, "PRIORITY", "6");
    journal_field(m, "SYSLOG_IDENTIFIER", PROGNAME);
#define JNUM(key, val, prec) do { snprintf(v, sizeof(v), "%.*f", prec, (double)(val)); journal_field(m, key, v); } while (0)
    JNUM("CPUMON_TOTAL", d->agg.total_pct, 2);
    JNUM("CPUMON_USER", d->agg.user_pct, 2);
    JNUM("CPUMON_SYSTEM", d->agg.system_pct, 2);
    JNUM("CPUMON_IOWAIT", d->agg.iowait_pct, 2);
    JNUM("CPUMON_STEAL", d->agg.steal_pct, 2);
    if (d->have_load) {
        JNUM("CPUMON_LOAD1", d->load[0], 2);
        JNUM("CPUMON_LOAD5", d->load[1], 2);
        JNUM("CPUMON_LOAD15", d->load[2], 2);
    }
    if (d->psi.have_some) JNUM("CPUMON_PSI_SOME_AVG10", d->psi.some[0], 2);
    if (d->have_freq) JNUM("CPUMON_FREQ_AVG_MHZ", d->freq_avg, 0);
    if (!isnan(d->temp_max)) JNUM("CPUMON_TEMP_MAX_C", d->temp_max, 1);
    if (d->ntop > 0) {
        JNUM("CPUMON_TOP_PID", d->top[0].pid, 0);
        journal_field(m, "CPUMON_TOP_COMM", d->top[0].comm);
        JNUM("CPUMON_TOP_CPU", d->top[0].pct, 1);
    }
#undef JNUM
    if (json) journal_field(m, "CPUMON_JSON", json);
    fclose(m);

    if (journal_send(msg, msg_len) != 0) {
        if (!warned) {
            fprintf(stderr, "%s: warning: systemd journal unavailable (%s); "
                    "writing summaries to stderr instead\n", PROGNAME, strerror(errno));
            warned = 1;
        }
        fprintf(stderr, "%s\n", summary);
    }
    free(json);
    free(msg);
}

/* Writes one frame to every configured log target. */
static void log_frame(const Config *c, const Derived *d, const HostInfo *h)
{
    if ((c->log_target & TARGET_FILE) && log_open_for_today(c) == 0) {
        switch (c->format) {
            case FMT_JSON:
                write_json_snapshot(g_logfile, d, h);
                break;
            case FMT_CSV:
                if (g_csv_cores != d->ncores) {
                    write_csv_header(g_logfile, d);
                    g_csv_cores = d->ncores;
                }
                write_csv_row(g_logfile, d, h);
                break;
            default:
                write_text_snapshot(g_logfile, d, h);
        }
    }
    if (c->log_target & TARGET_JOURNAL)
        journal_emit(d, h);
}

/* Updates the alert state for a frame and reports transitions to syslog
 * (and so the journal) and the log file. Returns the ALERT_* event. */
static int alert_process(const Config *c, Derived *d, const HostInfo *h, int file_logging)
{
    int ev = alert_update(&g_alert, c->alert_pct, c->alert_secs, d->agg.total_pct,
                          d->mono_start, d->mono_end);
    d->alert_active = g_alert.active;
    d->alert_threshold = c->alert_pct;
    if (ev == ALERT_NONE) return ev;

    char msg[256];
    alert_message(ev, &g_alert, c, d, msg, sizeof(msg));
    syslog(ev == ALERT_RAISED ? LOG_WARNING : LOG_NOTICE, "%s", msg);
    snprintf(g_alert_msg, sizeof(g_alert_msg), "%s", ev == ALERT_RAISED ? msg : "");

    if (file_logging && (c->log_target & TARGET_FILE) && log_open_for_today(c) == 0) {
        char iso[40];
        iso_time(d->ts, iso, sizeof(iso));
        if (c->format == FMT_JSON) {
            fprintf(g_logfile, "{\"ts\":%lld,\"time\":\"%s\",\"host\":", (long long)d->ts, iso);
            json_str(g_logfile, h->hostname);
            fprintf(g_logfile, ",\"event\":\"alert\",\"state\":\"%s\",\"message\":",
                    ev == ALERT_RAISED ? "raised" : "cleared");
            json_str(g_logfile, msg);
            fputs("}\n", g_logfile);
        } else if (c->format == FMT_TEXT) {
            char ts[64];
            struct tm tmv;
            localtime_r(&d->ts, &tmv);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
            fprintf(g_logfile, "!!! %s %s: %s\n\n", ev == ALERT_RAISED ? "ALERT" : "RECOVERED", ts, msg);
        }
        /* CSV has no room for free-form events; the alert column carries it. */
        fflush(g_logfile);
    }
    return ev;
}

/* ---------------------------------------------------------------------
 * Replay - print logged snapshots back out, filtered by time
 * ------------------------------------------------------------------- */

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int text_header_time(const char *line, time_t *out)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    const char *end = strptime(line, "%Y-%m-%d %H:%M:%S", &tm);
    if (!end) return -1;
    tm.tm_isdst = -1;
    *out = mktime(&tm);
    return 0;
}

typedef struct {
    time_t since, until;
    char last_csv_header[8192];
} ReplayState;

static int in_range(const ReplayState *r, time_t t)
{
    return t >= r->since && t <= r->until;
}

/* Filters one (possibly gzip-compressed) log file to stdout. Returns the
 * number of snapshots printed. */
static int replay_file(ReplayState *r, const char *path, int fmt)
{
    gzFile g = gzopen(path, "rb"); /* reads plain files transparently too */
    if (!g) {
        fprintf(stderr, "%s: cannot read '%s': %s\n", PROGNAME, path, strerror(errno));
        return 0;
    }
    size_t cap = 256 * 1024;
    char *line = malloc(cap), *held = malloc(cap);
    if (!line || !held) { free(line); free(held); gzclose(g); return 0; }
    held[0] = '\0';
    int include = 0, printed = 0, last_was_ts = 0, continuing = 0;

    while (gzgets(g, line, (int)cap)) {
        size_t len = strlen(line);
        int complete = len > 0 && line[len - 1] == '\n';
        if (continuing) {             /* tail of an over-long line */
            if (include) fputs(line, stdout);
            continuing = !complete;
            continue;
        }
        continuing = !complete;

        if (fmt == FMT_JSON) {
            const char *p = strstr(line, "\"ts\":");
            include = p && in_range(r, (time_t)atoll(p + 5));
            if (include) { fputs(line, stdout); printed++; }
        } else if (fmt == FMT_CSV) {
            if (strncmp(line, "ts,", 3) == 0) {
                snprintf(held, cap, "%s", line);
                include = 0;
                continue;
            }
            include = in_range(r, (time_t)atoll(line));
            if (include) {
                if (held[0] && strcmp(held, r->last_csv_header) != 0) {
                    fputs(held, stdout);
                    snprintf(r->last_csv_header, sizeof(r->last_csv_header), "%s", held);
                }
                fputs(line, stdout);
                printed++;
            }
        } else {
            int is_sep = strncmp(line, "=====", 5) == 0;
            time_t t;
            if (is_sep && !last_was_ts) {
                /* Opening separator: hold it until the timestamp line
                 * after it says whether this snapshot is in range. */
                snprintf(held, cap, "%s", line);
                continue;
            }
            if (held[0]) {
                if (text_header_time(line, &t) == 0) {
                    include = in_range(r, t);
                    if (include) printed++;
                    last_was_ts = 1;
                } else {
                    last_was_ts = 0;
                }
                if (include) { fputs(held, stdout); fputs(line, stdout); }
                held[0] = '\0';
                continue;
            }
            last_was_ts = 0;
            if (strncmp(line, "!!! ", 4) == 0) {
                const char *sp = strchr(line + 4, ' ');
                if (sp && text_header_time(sp + 1, &t) == 0) {
                    if (in_range(r, t)) fputs(line, stdout);
                    continue;
                }
            }
            if (include) fputs(line, stdout);
        }
    }
    if (fmt == FMT_TEXT && held[0] && include) fputs(held, stdout);
    free(line);
    free(held);
    gzclose(g);
    return printed;
}

static int run_replay(const Config *c, const char *logdir, time_t since, time_t until)
{
    DIR *dir = opendir(logdir);
    if (!dir) {
        fprintf(stderr, "%s: cannot open log dir '%s': %s\n", PROGNAME, logdir, strerror(errno));
        return 1;
    }
    char since_day[16], until_day[16];
    struct tm tmv;
    localtime_r(&since, &tmv);
    strftime(since_day, sizeof(since_day), "%Y-%m-%d", &tmv);
    localtime_r(&until, &tmv);
    strftime(until_day, sizeof(until_day), "%Y-%m-%d", &tmv);

    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        struct tm ftm;
        if (parse_log_filename_date(ent->d_name, &ftm, NULL, NULL) != 0) continue;
        if (strncmp(ent->d_name, since_day, 10) < 0 || strncmp(ent->d_name, until_day, 10) > 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            char **nn = realloc(names, cap * sizeof(*nn));
            if (!nn) break;
            names = nn;
        }
        names[n++] = strdup(ent->d_name);
    }
    closedir(dir);
    qsort(names, n, sizeof(*names), name_cmp);

    ReplayState *r = calloc(1, sizeof(*r));
    if (!r) return 1;
    r->since = since;
    r->until = until;
    int printed = 0;
    for (size_t i = 0; i < n; i++) {
        struct tm ftm;
        int fmt, gz;
        char path[1024];
        parse_log_filename_date(names[i], &ftm, &fmt, &gz);
        snprintf(path, sizeof(path), "%s/%s", logdir, names[i]);
        printed += replay_file(r, path, fmt);
        free(names[i]);
    }
    free(names);
    free(r);
    (void)c;
    if (printed == 0) {
        fprintf(stderr, "%s: no logged snapshots in '%s' for that time range\n", PROGNAME, logdir);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * Plain-text (non-ncurses) single snapshot, for --once and as a
 * fallback if ncurses isn't compiled in.
 * ------------------------------------------------------------------- */

static void print_once(const Derived *d, const HostInfo *h)
{
    char ts[64];
    struct tm tmv;
    localtime_r(&d->ts, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    printf("%s(1)                 CPU Monitor                 %s(1)\n\n", PROGNAME, PROGNAME);
    printf("HOST\n    %s   kernel %s   %s\n", h->hostname, h->kernel, ts);
    printf("    %d core%s - %s\n\n", d->ncores, d->ncores == 1 ? "" : "s", h->model);

    printf("CPU OVERVIEW\n");
    printf("    ");
    print_bar_plain(stdout, d->agg.total_pct, 40);
    printf("\n    user %-8.1f%% system %-8.1f%% idle %-8.1f%% iowait %-8.1f%%\n\n",
           d->agg.user_pct, d->agg.system_pct, d->agg.idle_pct, d->agg.iowait_pct);

    if (d->have_freq)
        printf("FREQUENCY\n    avg %.0f MHz   min %.0f MHz   max %.0f MHz\n\n",
               d->freq_avg, d->freq_min, d->freq_max);

    if (d->ntemps > 0) {
        printf("TEMPERATURE\n");
        for (int i = 0; i < d->ntemps; i++)
            printf("    %-32s%.1f C\n", d->temps[i].label, d->temps[i].celsius);
        putchar('\n');
    }

    if (d->have_load)
        printf("LOAD AVERAGE\n    1m: %.2f   5m: %.2f   15m: %.2f\n\n", d->load[0], d->load[1], d->load[2]);

    if (d->psi.have_some) {
        printf("PRESSURE (PSI)\n    some avg10 %.2f%%  avg60 %.2f%%  avg300 %.2f%%\n",
               d->psi.some[0], d->psi.some[1], d->psi.some[2]);
        if (d->psi.have_full)
            printf("    full avg10 %.2f%%  avg60 %.2f%%  avg300 %.2f%%\n",
                   d->psi.full[0], d->psi.full[1], d->psi.full[2]);
        putchar('\n');
    }

    printf("PROCESSES\n    running %-6lu blocked %-6lu ctxt/s %-10.1f forks/s %.2f\n\n",
           d->procs_running, d->procs_blocked, d->ctxt_per_sec, d->forks_per_sec);

    if (d->ntop > 0) {
        printf("TOP PROCESSES\n    %8s %7s  S  COMMAND\n", "PID", "%CPU");
        for (int i = 0; i < d->ntop; i++)
            printf("    %8d %7.1f  %c  %s\n", d->top[i].pid, d->top[i].pct,
                   d->top[i].state, d->top[i].comm);
        putchar('\n');
    }

    if (d->have_uptime) {
        char up_s[64];
        format_duration(d->uptime, up_s, sizeof(up_s));
        printf("UPTIME\n    %s\n\n", up_s);
    }

    printf("PER-CORE\n");
    for (int i = 0; i < d->ncores; i++) {
        printf("    cpu%-4d ", d->core_id[i]);
        print_bar_plain(stdout, d->core[i].total_pct, 30);
        if (!isnan(d->core_mhz[i])) printf("  %5.0f MHz", d->core_mhz[i]);
        putchar('\n');
    }
}

static int run_once(const Config *c, const HostInfo *h)
{
    Sampler *s = malloc(sizeof(*s));
    Derived *d = malloc(sizeof(*d));
    if (!s || !d) { free(s); free(d); return 1; }
    sampler_start(s, c->top_n > 0);
    sampler_frame(s, c, d);
    switch (c->format) {
        case FMT_JSON: write_json_snapshot(stdout, d, h); break;
        case FMT_CSV:  write_csv_header(stdout, d); write_csv_row(stdout, d, h); break;
        default:       print_once(d, h);
    }
    sampler_free(s);
    free(s);
    free(d);
    return 0;
}

/* ---------------------------------------------------------------------
 * Plain ANSI watch mode - no ncurses at all. Uses only "clear screen /
 * cursor home" (\x1b[H\x1b[2J) and SGR color codes, which even quirky
 * or non-fully-VT100 terminals (some Windows/WSL front-ends, serial
 * consoles, etc.) tend to support when full curses cursor-addressing
 * does not render correctly. This is the recommended fallback if the
 * ncurses TUI (the default mode) looks garbled or doesn't redraw in
 * place on your terminal.
 * ------------------------------------------------------------------- */

static volatile sig_atomic_t g_plain_stop = 0;
static void handle_plain_signal(int sig) { (void)sig; g_plain_stop = 1; }

static void print_bar_ansi(double pct, int width)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)((pct / 100.0) * width + 0.5);
    const char *color = pct < 60.0 ? "\x1b[32m" : (pct < 85.0 ? "\x1b[33m" : "\x1b[31m");
    printf("[%s", color);
    for (int i = 0; i < width; i++)
        putchar(i < filled ? '#' : '-');
    printf("\x1b[0m] %5.1f%%", pct);
}

static int run_plain_watch(const Config *c, const HostInfo *h)
{
    signal(SIGINT, handle_plain_signal);
    signal(SIGTERM, handle_plain_signal);
    signal(SIGHUP, handle_plain_signal);

    int logging = !c->no_log;
    Sampler *s = malloc(sizeof(*s));
    Derived *d = malloc(sizeof(*d));
    if (!s || !d) { free(s); free(d); return 1; }
    sampler_start(s, c->top_n > 0);

    while (!g_plain_stop) {
        sampler_frame(s, c, d);
        history_push(&g_hist, d);
        alert_process(c, d, h, logging);

        char ibuf[32];
        format_interval(c->interval_secs, ibuf, sizeof(ibuf));
        char ts[64];
        struct tm tmv;
        localtime_r(&d->ts, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

        fputs("\x1b[H\x1b[2J", stdout); /* cursor home + clear screen */
        printf("\x1b[1;7m cpumon \x1b[0m  System CPU Monitor - %s   refresh: %s   %s\n\n",
               h->hostname, ibuf, ts);
        if (d->alert_active)
            printf("\x1b[1;41;37m ALERT \x1b[0m \x1b[1;31m%s\x1b[0m\n\n", g_alert_msg);

        printf("\x1b[1;36mCPU OVERVIEW\x1b[0m  (%d core%s - %s)\n  ",
               d->ncores, d->ncores == 1 ? "" : "s", h->model);
        print_bar_ansi(d->agg.total_pct, 40);
        printf("\n  user %-8.1f%% system %-8.1f%% idle %-8.1f%% iowait %-8.1f%%\n",
               d->agg.user_pct, d->agg.system_pct, d->agg.idle_pct, d->agg.iowait_pct);
        unsigned char series[HIST_LEN];
        char spark[HIST_LEN * 3 + 1];
        int ns = history_series(&g_hist, -1, 60, series);
        sparkline(spark, sizeof(spark), series, ns, g_utf8);
        printf("  history %s\n\n", spark);

        if (d->have_freq || !isnan(d->temp_max)) {
            printf("\x1b[1;36mFREQUENCY / TEMPERATURE\x1b[0m\n ");
            if (d->have_freq)
                printf(" avg %.0f MHz (%.0f-%.0f)", d->freq_avg, d->freq_min, d->freq_max);
            if (!isnan(d->temp_max))
                printf("   max temp %.1f C", d->temp_max);
            printf("\n\n");
        }

        if (d->have_load)
            printf("\x1b[1;36mLOAD AVERAGE\x1b[0m\n  1m: %.2f   5m: %.2f   15m: %.2f\n\n",
                   d->load[0], d->load[1], d->load[2]);

        if (d->psi.have_some)
            printf("\x1b[1;36mPRESSURE (PSI)\x1b[0m\n  some avg10 %.2f%%  avg60 %.2f%%  avg300 %.2f%%\n\n",
                   d->psi.some[0], d->psi.some[1], d->psi.some[2]);

        printf("\x1b[1;36mPROCESSES\x1b[0m\n  running %-6lu blocked %-6lu ctxt/s %-10.1f forks/s %.2f\n\n",
               d->procs_running, d->procs_blocked, d->ctxt_per_sec, d->forks_per_sec);

        if (d->ntop > 0) {
            printf("\x1b[1;36mTOP PROCESSES\x1b[0m\n  %8s %7s  S  COMMAND\n", "PID", "%CPU");
            for (int i = 0; i < d->ntop; i++)
                printf("  %8d %7.1f  %c  %s\n", d->top[i].pid, d->top[i].pct,
                       d->top[i].state, d->top[i].comm);
            putchar('\n');
        }

        printf("\x1b[1;36mPER-CORE\x1b[0m\n");
        int col = 0;
        for (int i = 0; i < d->ncores; i++) {
            printf("  cpu%-4d ", d->core_id[i]);
            print_bar_ansi(d->core[i].total_pct, 20);
            printf("  ");
            col++;
            if (col == 2) { col = 0; putchar('\n'); }
        }
        if (col != 0) putchar('\n');

        printf("\n(Ctrl-C to quit -- plain mode, refresh every %s)\n", ibuf);
        fflush(stdout);

        if (logging) log_frame(c, d, h);

        long remaining = c->interval_secs;
        while (remaining > 0 && !g_plain_stop) {
            long chunk = remaining > 1 ? 1 : remaining;
            sleep((unsigned int)chunk);
            remaining -= chunk;
        }
        if (g_plain_stop) break;
        sampler_tick(s);
    }
    log_close();
    sampler_free(s);
    free(s);
    free(d);
    printf("\n");
    return 0;
}

/* ---------------------------------------------------------------------
 * ncurses TUI
 * ------------------------------------------------------------------- */

#ifdef USE_NCURSES

enum { CP_HEADER = 1, CP_TITLE, CP_LABEL, CP_BAR_OK, CP_BAR_WARN, CP_BAR_CRIT, CP_VALUE, CP_DIM, CP_ALERT };

static void init_colors(void)
{
    start_color();
    use_default_colors();
    init_pair(CP_HEADER,  COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_TITLE,   COLOR_CYAN,  -1);
    init_pair(CP_LABEL,   COLOR_WHITE, -1);
    init_pair(CP_BAR_OK,  COLOR_GREEN, -1);
    init_pair(CP_BAR_WARN,COLOR_YELLOW,-1);
    init_pair(CP_BAR_CRIT,COLOR_RED,   -1);
    init_pair(CP_VALUE,   COLOR_WHITE, -1);
    init_pair(CP_DIM,     COLOR_BLUE,  -1);
    init_pair(CP_ALERT,   COLOR_WHITE, COLOR_RED);
}

static int bar_color_for(double pct)
{
    if (pct < 60.0) return CP_BAR_OK;
    if (pct < 85.0) return CP_BAR_WARN;
    return CP_BAR_CRIT;
}

static void draw_bar(WINDOW *w, int y, int x, int width, double pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)((pct / 100.0) * width + 0.5);
    int cp = bar_color_for(pct);
    mvwaddch(w, y, x, '[');
    wattron(w, COLOR_PAIR(cp) | A_BOLD);
    for (int i = 0; i < width; i++)
        mvwaddch(w, y, x + 1 + i, i < filled ? ACS_CKBOARD : ' ');
    wattroff(w, COLOR_PAIR(cp) | A_BOLD);
    mvwaddch(w, y, x + 1 + width, ']');
    wattron(w, COLOR_PAIR(cp) | A_BOLD);
    mvwprintw(w, y, x + width + 3, "%5.1f%%", pct);
    wattroff(w, COLOR_PAIR(cp) | A_BOLD);
}

static void section_title(WINDOW *w, int *y, const char *title)
{
    wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD | A_UNDERLINE);
    mvwprintw(w, *y, 0, "%s", title);
    wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD | A_UNDERLINE);
    (*y)++;
}

static void kv_row(WINDOW *w, int y, int x, const char *label, const char *value, int width)
{
    wattron(w, COLOR_PAIR(CP_LABEL));
    mvwprintw(w, y, x, "%-16s", label);
    wattroff(w, COLOR_PAIR(CP_LABEL));
    wattron(w, COLOR_PAIR(CP_VALUE) | A_BOLD);
    mvwprintw(w, y, x + 16, "%-*s", width, value);
    wattroff(w, COLOR_PAIR(CP_VALUE) | A_BOLD);
}

static void draw_spark(WINDOW *w, int y, int x, int core, int width)
{
    unsigned char series[HIST_LEN];
    char spark[HIST_LEN * 3 + 1];
    int n = history_series(&g_hist, core, width, series);
    sparkline(spark, sizeof(spark), series, n, g_utf8);
    wattron(w, COLOR_PAIR(CP_DIM));
    mvwaddstr(w, y, x, spark);
    wattroff(w, COLOR_PAIR(CP_DIM));
}

static int build_pad(WINDOW *pad, const Derived *d, const HostInfo *h, long interval_secs)
{
    werase(pad);
    int y = 1;

    char ts[64];
    struct tm tmv;
    localtime_r(&d->ts, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    wattron(pad, A_BOLD);
    mvwprintw(pad, y, 0, "System CPU Report");
    wattroff(pad, A_BOLD);
    wattron(pad, COLOR_PAIR(CP_DIM));
    mvwprintw(pad, y, 20, "host: %s   kernel: %s   sampled: %s", h->hostname, h->kernel, ts);
    wattroff(pad, COLOR_PAIR(CP_DIM));
    y += 2;

    if (d->alert_active) {
        wattron(pad, COLOR_PAIR(CP_ALERT) | A_BOLD);
        mvwprintw(pad, y, 0, " ALERT: %s ", g_alert_msg);
        wattroff(pad, COLOR_PAIR(CP_ALERT) | A_BOLD);
        y += 2;
    }

    /* ---- Overview ---- */
    section_title(pad, &y, "CPU OVERVIEW");
    mvwprintw(pad, y, 4, "%d core%s - %s", d->ncores, d->ncores == 1 ? "" : "s", h->model);
    y++;
    draw_bar(pad, y, 4, 40, d->agg.total_pct); y += 2;
    char user_h[16], sys_h[16], idle_h[16], iow_h[16];
    snprintf(user_h, sizeof(user_h), "%.1f%%", d->agg.user_pct);
    snprintf(sys_h, sizeof(sys_h), "%.1f%%", d->agg.system_pct);
    snprintf(idle_h, sizeof(idle_h), "%.1f%%", d->agg.idle_pct);
    snprintf(iow_h, sizeof(iow_h), "%.1f%%", d->agg.iowait_pct);
    kv_row(pad, y, 4, "User:", user_h, 14);
    kv_row(pad, y, 34, "System:", sys_h, 14); y++;
    kv_row(pad, y, 4, "Idle:", idle_h, 14);
    kv_row(pad, y, 34, "IOWait:", iow_h, 14); y += 2;

    /* ---- History ---- */
    {
        unsigned char series[HIST_LEN];
        int n = history_series(&g_hist, -1, 60, series);
        int mn, avg, mx;
        history_stats(series, n, &mn, &avg, &mx);
        char ibuf[32], span[64];
        format_interval(interval_secs, ibuf, sizeof(ibuf));
        format_duration((double)interval_secs * (n > 0 ? n - 1 : 0), span, sizeof(span));
        section_title(pad, &y, "HISTORY");
        wattron(pad, COLOR_PAIR(CP_LABEL));
        mvwprintw(pad, y, 4, "Total");
        wattroff(pad, COLOR_PAIR(CP_LABEL));
        draw_spark(pad, y, 11, -1, 60);
        y++;
        wattron(pad, COLOR_PAIR(CP_DIM));
        mvwprintw(pad, y, 11, "%d samples every %s (~%s)   min %d%%  avg %d%%  max %d%%",
                  n, ibuf, span, mn, avg, mx);
        wattroff(pad, COLOR_PAIR(CP_DIM));
        y += 2;
    }

    /* ---- Frequency / temperature ---- */
    if (d->have_freq) {
        section_title(pad, &y, "FREQUENCY");
        char avg_h[24], min_h[24], max_h[24];
        snprintf(avg_h, sizeof(avg_h), "%.0f MHz", d->freq_avg);
        snprintf(min_h, sizeof(min_h), "%.0f MHz", d->freq_min);
        snprintf(max_h, sizeof(max_h), "%.0f MHz", d->freq_max);
        kv_row(pad, y, 4, "Average:", avg_h, 14); y++;
        kv_row(pad, y, 4, "Min:", min_h, 14);
        kv_row(pad, y, 34, "Max:", max_h, 14); y += 2;
    }
    if (d->ntemps > 0) {
        section_title(pad, &y, "TEMPERATURE");
        for (int i = 0; i < d->ntemps; i++) {
            char val[16];
            snprintf(val, sizeof(val), "%.1f C", d->temps[i].celsius);
            wattron(pad, COLOR_PAIR(CP_LABEL));
            mvwprintw(pad, y, 4, "%-32s", d->temps[i].label);
            wattroff(pad, COLOR_PAIR(CP_LABEL));
            int cp = d->temps[i].celsius < 70 ? CP_BAR_OK : (d->temps[i].celsius < 85 ? CP_BAR_WARN : CP_BAR_CRIT);
            wattron(pad, COLOR_PAIR(cp) | A_BOLD);
            mvwprintw(pad, y, 36, "%s", val);
            wattroff(pad, COLOR_PAIR(cp) | A_BOLD);
            y++;
        }
        y++;
    }

    /* ---- Load average ---- */
    if (d->have_load) {
        section_title(pad, &y, "LOAD AVERAGE");
        mvwprintw(pad, y, 4, "1m: %.2f   5m: %.2f   15m: %.2f", d->load[0], d->load[1], d->load[2]);
        y += 2;
    }

    /* ---- Pressure ---- */
    if (d->psi.have_some) {
        section_title(pad, &y, "PRESSURE (PSI)");
        mvwprintw(pad, y, 4, "some   avg10 %6.2f%%   avg60 %6.2f%%   avg300 %6.2f%%",
                  d->psi.some[0], d->psi.some[1], d->psi.some[2]);
        y++;
        if (d->psi.have_full) {
            mvwprintw(pad, y, 4, "full   avg10 %6.2f%%   avg60 %6.2f%%   avg300 %6.2f%%",
                      d->psi.full[0], d->psi.full[1], d->psi.full[2]);
            y++;
        }
        y++;
    }

    /* ---- Processes & context switches ---- */
    section_title(pad, &y, "PROCESSES & CONTEXT SWITCHES");
    char running_h[16], blocked_h[16], ctxt_h[24], forks_h[24];
    snprintf(running_h, sizeof(running_h), "%lu", d->procs_running);
    snprintf(blocked_h, sizeof(blocked_h), "%lu", d->procs_blocked);
    snprintf(ctxt_h, sizeof(ctxt_h), "%.1f/s", d->ctxt_per_sec);
    snprintf(forks_h, sizeof(forks_h), "%.2f/s", d->forks_per_sec);
    kv_row(pad, y, 4, "Running:", running_h, 14);
    kv_row(pad, y, 34, "Blocked:", blocked_h, 14); y++;
    kv_row(pad, y, 4, "Ctxt switches:", ctxt_h, 14);
    kv_row(pad, y, 34, "Forks:", forks_h, 14); y += 2;

    /* ---- Top processes ---- */
    if (d->ntop > 0) {
        section_title(pad, &y, "TOP PROCESSES");
        wattron(pad, COLOR_PAIR(CP_DIM));
        mvwprintw(pad, y, 4, "%8s %7s  S  COMMAND", "PID", "%CPU");
        wattroff(pad, COLOR_PAIR(CP_DIM));
        y++;
        for (int i = 0; i < d->ntop; i++) {
            mvwprintw(pad, y, 4, "%8d ", d->top[i].pid);
            int cp = bar_color_for(d->top[i].pct);
            wattron(pad, COLOR_PAIR(cp) | A_BOLD);
            wprintw(pad, "%7.1f", d->top[i].pct);
            wattroff(pad, COLOR_PAIR(cp) | A_BOLD);
            wprintw(pad, "  %c  %s", d->top[i].state, d->top[i].comm);
            y++;
        }
        y++;
    }

    /* ---- Uptime ---- */
    if (d->have_uptime) {
        section_title(pad, &y, "UPTIME");
        char up_h[64];
        format_duration(d->uptime, up_h, sizeof(up_h));
        mvwprintw(pad, y, 4, "%s", up_h);
        y += 2;
    }

    /* ---- Aggregate time breakdown ---- */
    section_title(pad, &y, "AGGREGATE TIME BREAKDOWN");
    char nice_h[16], irq_h[16], soft_h[16], steal_h[16];
    snprintf(nice_h, sizeof(nice_h), "%.1f%%", d->agg.nice_pct);
    snprintf(irq_h, sizeof(irq_h), "%.1f%%", d->agg.irq_pct);
    snprintf(soft_h, sizeof(soft_h), "%.1f%%", d->agg.softirq_pct);
    snprintf(steal_h, sizeof(steal_h), "%.1f%%", d->agg.steal_pct);
    kv_row(pad, y, 4, "Nice:", nice_h, 14);
    kv_row(pad, y, 34, "IRQ:", irq_h, 14); y++;
    kv_row(pad, y, 4, "SoftIRQ:", soft_h, 14);
    kv_row(pad, y, 34, "Steal:", steal_h, 14); y += 2;

    /* ---- Per-core bars ---- */
    section_title(pad, &y, "PER-CORE USAGE");
    for (int i = 0; i < d->ncores && y < PAD_LINES - 2; i++) {
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "cpu%d", d->core_id[i]);
        wattron(pad, COLOR_PAIR(CP_LABEL));
        mvwprintw(pad, y, 4, "%-7s", lbl);
        wattroff(pad, COLOR_PAIR(CP_LABEL));
        draw_bar(pad, y, 11, 30, d->core[i].total_pct);
        if (!isnan(d->core_mhz[i]))
            mvwprintw(pad, y, 52, "%5.0f MHz", d->core_mhz[i]);
        draw_spark(pad, y, 64, i, 40);
        y++;
    }
    y++;

    return y; /* total content height used */
}

/* Header/footer are drawn directly onto stdscr (row 0 and row LINES-1)
 * rather than via per-frame subwin()/delwin(), which is unnecessary churn
 * and was one more thing that could go sideways on quirky terminals. */
static void draw_header(int cols, const char *hostname, long interval_secs)
{
    int cp = has_colors() ? CP_HEADER : 0;
    attr_t extra = has_colors() ? A_BOLD : (A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(cp) | extra);
    move(0, 0);
    for (int i = 0; i < cols; i++) addch(' ');

    char ibuf[32];
    format_interval(interval_secs, ibuf, sizeof(ibuf));
    mvprintw(0, 0, " %s(1)", PROGNAME);
    char mid[300];
    snprintf(mid, sizeof(mid), "System CPU Monitor - %s", hostname);
    int midlen = (int)strlen(mid);
    int midpos = (cols - midlen) / 2;
    if (midpos > 0 && midpos + midlen < cols)
        mvprintw(0, midpos, "%s", mid);
    char right[48];
    snprintf(right, sizeof(right), "refresh: %s ", ibuf);
    int rightlen = (int)strlen(right);
    if (cols - rightlen - 1 > 0)
        mvprintw(0, cols - rightlen - 1, "%s", right);
    attroff(COLOR_PAIR(cp) | extra);
}

static void draw_footer(int rows, int cols, int logging_enabled, int secs_to_refresh)
{
    int cp = has_colors() ? CP_HEADER : 0;
    attr_t extra = has_colors() ? A_BOLD : (A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(cp) | extra);
    char msg[256];
    snprintf(msg, sizeof(msg),
             " q:quit  Up/Dn or j/k:scroll  PgUp/PgDn  g/G:top/bottom  r:refresh now   log:%s   next refresh in %ds ",
             logging_enabled ? "on" : "off", secs_to_refresh);
    mvprintw(rows - 1, 0, "%-*.*s", cols, cols, msg);
    attroff(COLOR_PAIR(cp) | extra);
}

static int run_interactive(const Config *c, const HostInfo *h)
{
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr,
            "%s: stdout is not a terminal; the interactive display needs a real tty.\n"
            "Use --once for a single snapshot or --daemon for headless logging instead.\n",
            PROGNAME);
        return 1;
    }

    int logging = !c->no_log;
    long interval_secs = c->interval_secs;
    Sampler *s = malloc(sizeof(*s));
    Derived *d = malloc(sizeof(*d));
    if (!s || !d) { free(s); free(d); return 1; }
    sampler_start(s, c->top_n > 0);
    sampler_frame(s, c, d);
    history_push(&g_hist, d);
    alert_process(c, d, h, logging);

    initscr();
    if (has_colors()) init_colors();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(200); /* ms poll interval so we can update countdown + handle keys */
    /* Some terminals (notably several found under Windows/WSL front-ends)
     * misreport or only partially support cursor-addressing capabilities,
     * which makes ncurses' normal incremental-diff redraw either leave
     * stale content on screen or scroll instead of repainting in place.
     * Forcing a full repaint every cycle costs a little more output but
     * is far more likely to render correctly everywhere. */
    clearok(stdscr, TRUE);

    int scroll_y = 0;
    WINDOW *pad = newpad(PAD_LINES, PAD_COLS);
    if (!pad) {
        endwin();
        fprintf(stderr, "%s: failed to allocate display pad\n", PROGNAME);
        sampler_free(s); free(s); free(d);
        return 1;
    }

    int content_h = build_pad(pad, d, h, interval_secs);
    if (logging) log_frame(c, d, h);

    struct timespec last_refresh, now;
    clock_gettime(CLOCK_MONOTONIC, &last_refresh);

    int running = 1;
    while (running) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int content_rows = rows - 2;
        if (content_rows < 1) content_rows = 1;

        int max_scroll = content_h - content_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_y > max_scroll) scroll_y = max_scroll;
        if (scroll_y < 0) scroll_y = 0;

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - last_refresh.tv_sec) +
                          (now.tv_nsec - last_refresh.tv_nsec) / 1e9;
        int secs_to_refresh = (int)(interval_secs - elapsed);
        if (secs_to_refresh < 0) secs_to_refresh = 0;

        erase();
        draw_header(cols, h->hostname, interval_secs);
        draw_footer(rows, cols, logging, secs_to_refresh);
        wnoutrefresh(stdscr);
        pnoutrefresh(pad, scroll_y, 0, 1, 0, rows - 2, cols - 1);
        doupdate();

        int ch = getch();
        switch (ch) {
            case 'q': case 'Q': running = 0; break;
            case KEY_UP: case 'k': if (scroll_y > 0) scroll_y--; break;
            case KEY_DOWN: case 'j': if (scroll_y < max_scroll) scroll_y++; break;
            case KEY_NPAGE: scroll_y += content_rows; break;
            case KEY_PPAGE: scroll_y -= content_rows; break;
            case 'g': scroll_y = 0; break;
            case 'G': scroll_y = max_scroll; break;
            case KEY_RESIZE: resizeterm(0, 0); clearok(stdscr, TRUE); break;
            case 'r': case 'R': elapsed = interval_secs + 1; break;
            default: break;
        }

        if (running && elapsed >= interval_secs) {
            if (sampler_tick(s) == 0) {
                sampler_frame(s, c, d);
                history_push(&g_hist, d);
                alert_process(c, d, h, logging);
                content_h = build_pad(pad, d, h, interval_secs);
                if (logging) log_frame(c, d, h);
            }
            clock_gettime(CLOCK_MONOTONIC, &last_refresh);
        }
    }

    delwin(pad);
    endwin();
    log_close();
    sampler_free(s);
    free(s);
    free(d);
    return 0;
}

#endif /* USE_NCURSES */

/* ---------------------------------------------------------------------
 * CLI + config assembly
 * ------------------------------------------------------------------- */

typedef struct {
    int daemon, once, plain, replay;
    const char *config_path;
    const char *since, *until;
} Cli;

typedef struct {
    const char *shortf, *longf, *key;  /* key NULL = handled specially */
} ValueOpt;

static const ValueOpt kValueOpts[] = {
    { "-i", "--interval",            "CPUMON_INTERVAL" },
    { "-l", "--log-dir",             "CPUMON_LOG_DIR" },
    { "-r", "--log-retention-days",  "CPUMON_LOG_RETENTION_DAYS" },
    { "-z", "--compress-after-days", "CPUMON_COMPRESS_AFTER_DAYS" },
    { "-f", "--format",              "CPUMON_FORMAT" },
    { "-T", "--log-target",          "CPUMON_LOG_TARGET" },
    { "-t", "--top",                 "CPUMON_TOP" },
    { "-a", "--alert",               "CPUMON_ALERT_THRESHOLD" },
    { "-A", "--alert-duration",      "CPUMON_ALERT_DURATION" },
    { "-c", "--config",              NULL },
    { "-s", "--since",               NULL },
    { "-u", "--until",               NULL },
};

static void usage(void)
{
    printf(
"%s %s - Linux CPU usage monitor\n\n"
"Usage: %s [options]\n\n"
"Modes (default: interactive ncurses display):\n"
"  -d, --daemon               Headless mode: no TUI, just logs snapshots on\n"
"                             each interval. Intended for systemd. Handles\n"
"                             SIGTERM/SIGINT for clean shutdown; SIGHUP\n"
"                             re-reads the config file and reopens the log.\n"
"  -o, --once                 Print a single snapshot to stdout and exit\n"
"                             (no ncurses, no loop, no log file written).\n"
"  -p, --plain                Interactive watch mode without ncurses: plain\n"
"                             ANSI clear-screen/redraw instead of full cursor\n"
"                             addressing. Try this if the default ncurses\n"
"                             display looks garbled or doesn't redraw in\n"
"                             place on your terminal (common under some\n"
"                             Windows/WSL terminal front-ends).\n"
"  -R, --replay               Print logged snapshots from the log dir (plain\n"
"                             or gzip-compressed) and exit.\n"
"  -s, --since WHEN           With --replay: only snapshots at/after WHEN.\n"
"  -u, --until WHEN           With --replay: only snapshots at/before WHEN.\n"
"                             WHEN: YYYY-MM-DD[ HH:MM[:SS]], today, yesterday,\n"
"                             now, or an interval ago (30m, 2h, 7d).\n\n"
"Options:\n"
"  -i, --interval INTERVAL    Refresh/log interval. Accepts plain seconds or a\n"
"                             suffix: s (sec), m (min), h (hour), d (day).\n"
"                             Examples: -i 5, -i 5s, -i 5m, -i 1h. Default: 5s\n"
"  -f, --format FMT           text, json or csv. Applies to --once output and\n"
"                             to log files (YYYY-MM-DD.log/.jsonl/.csv).\n"
"                             Default: text\n"
"  -t, --top N                Show the N processes using the most CPU\n"
"                             (0 disables). Default: %d\n"
"  -a, --alert PCT            Alert (syslog/journal + log) when total CPU\n"
"                             usage stays at or above PCT percent. 0 = off.\n"
"  -A, --alert-duration DUR   How long usage must stay high before alerting\n"
"                             (interval syntax, e.g. 5m). Default: 0\n"
"  -l, --log-dir DIR          Directory for daily log files. Default: %s\n"
"                             (falls back to ~/%s if not writable).\n"
"  -r, --log-retention-days N Auto-delete log files older than N days.\n"
"                             N=0 disables auto-delete. Default: %d\n"
"  -z, --compress-after-days N  gzip log files at least N days old.\n"
"                             N=0 disables compression. Default: 0\n"
"  -T, --log-target TARGET    file, journal (systemd journal) or both.\n"
"                             Default: file\n"
"  -n, --no-log               Disable logging in interactive mode.\n"
"  -c, --config FILE          Read settings from FILE. Default: %s\n"
"                             for --daemon, ~/.config/%s otherwise.\n"
"  -h, --help                 Show this help and exit.\n"
"  -v, --version              Show version and exit.\n\n"
"Settings are applied as: built-in defaults, then the config file, then\n"
"CPUMON_* environment variables, then command-line options.\n\n"
"Interactive keys:  q quit | up/down or j/k scroll | PgUp/PgDn | g/G top/bottom\n"
"                   r refresh now\n\n"
"Examples:\n"
"  %s                          Interactive TUI, refresh every 5 seconds\n"
"  %s -i 5m                    Interactive TUI, refresh every 5 minutes\n"
"  %s --once                   One-shot plain text report\n"
"  %s --once -f json           One-shot JSON report\n"
"  %s --daemon -i 5m -a 90 -A 10m\n"
"                               Headless logger, alert after 10m above 90%%\n"
"  %s --replay --since 2h      Logged snapshots from the last two hours\n",
    PROGNAME, VERSION, PROGNAME, DEFAULT_TOP, DEFAULT_LOGDIR, FALLBACK_LOGDIR,
    DEFAULT_LOG_RETENTION_DAYS, SYSTEM_CONFIG, USER_CONFIG_REL,
    PROGNAME, PROGNAME, PROGNAME, PROGNAME, PROGNAME, PROGNAME);
}

/* Parses argv, applying setting options to cfg. Returns 0 to continue,
 * 1 if --help/--version was handled, -1 on error (already reported). */
static int parse_cli(int argc, char **argv, Config *cfg, Cli *cli)
{
    memset(cli, 0, sizeof(*cli));
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const ValueOpt *opt = NULL;
        const char *val = NULL;
        for (size_t k = 0; k < sizeof(kValueOpts) / sizeof(kValueOpts[0]); k++) {
            const ValueOpt *o = &kValueOpts[k];
            size_t ll = strlen(o->longf);
            if (strcmp(a, o->shortf) == 0 || strcmp(a, o->longf) == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "%s: option '%s' needs a value (see --help)\n", PROGNAME, a);
                    return -1;
                }
                opt = o;
                val = argv[++i];
                break;
            }
            if (strncmp(a, o->longf, ll) == 0 && a[ll] == '=') {
                opt = o;
                val = a + ll + 1;
                break;
            }
        }
        if (opt) {
            if (opt->key) {
                char err[160];
                if (config_set(cfg, opt->key, val, err, sizeof(err)) != 0) {
                    fprintf(stderr, "%s: %s: %s\n", PROGNAME, opt->longf, err);
                    return -1;
                }
            } else if (strcmp(opt->longf, "--config") == 0) {
                cli->config_path = val;
            } else if (strcmp(opt->longf, "--since") == 0) {
                cli->since = val;
            } else {
                cli->until = val;
            }
            continue;
        }

        if (strcmp(a, "-d") == 0 || strcmp(a, "--daemon") == 0) {
            cli->daemon = 1;
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--once") == 0) {
            cli->once = 1;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--plain") == 0) {
            cli->plain = 1;
        } else if (strcmp(a, "-R") == 0 || strcmp(a, "--replay") == 0) {
            cli->replay = 1;
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--no-log") == 0) {
            cfg->no_log = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s %s\n", PROGNAME, VERSION);
            return 1;
        } else {
            fprintf(stderr, "%s: unknown option '%s' (see --help)\n", PROGNAME, a);
            return -1;
        }
    }
    return 0;
}

/* Builds the effective config: defaults < config file < environment <
 * command line. The daemon reads the system-wide file, the interactive
 * modes a per-user one, and --replay both (so it finds the daemon's log
 * dir); --config overrides the choice. Returns -1 if an explicitly named
 * config file can't be read or argv is invalid. */
static int build_config(Config *cfg, const Cli *cli, int argc, char **argv)
{
    config_defaults(cfg);
    if (cli->config_path) {
        if (config_load_file(cfg, cli->config_path) != 0) {
            fprintf(stderr, "%s: cannot read config file '%s': %s\n",
                    PROGNAME, cli->config_path, strerror(errno));
            return -1;
        }
    } else {
        if (cli->daemon || cli->replay)
            config_load_file(cfg, SYSTEM_CONFIG);
        char upath[600];
        if (!cli->daemon && user_config_path(upath, sizeof(upath)) == 0)
            config_load_file(cfg, upath);
    }
    config_load_env(cfg);
    Cli scratch;
    return parse_cli(argc, argv, cfg, &scratch) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------------
 * Daemon mode (no ncurses) - for systemd
 * ------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;
static void handle_signal(int sig) { (void)sig; g_stop = 1; }
static void handle_reload(int sig) { (void)sig; g_reload = 1; }

static void announce_config(const Config *c)
{
    char ibuf[32], ret[32], cmp[32], alert[64];
    format_interval(c->interval_secs, ibuf, sizeof(ibuf));
    if (c->log_retention_days > 0) snprintf(ret, sizeof(ret), "%dd", c->log_retention_days);
    else snprintf(ret, sizeof(ret), "disabled");
    if (c->compress_after_days > 0) snprintf(cmp, sizeof(cmp), "%dd", c->compress_after_days);
    else snprintf(cmp, sizeof(cmp), "disabled");
    if (c->alert_pct > 0) {
        char dur[32];
        format_interval(c->alert_secs > 0 ? c->alert_secs : 1, dur, sizeof(dur));
        snprintf(alert, sizeof(alert), ">=%.0f%% for %s", c->alert_pct, c->alert_secs > 0 ? dur : "0s");
    } else {
        snprintf(alert, sizeof(alert), "disabled");
    }
    fprintf(stderr, "%s: daemon config: interval=%s, logdir=%s, target=%s, format=%s, "
            "log-retention=%s, compress-after=%s, top=%d, alert=%s\n",
            PROGNAME, ibuf, g_logdir,
            c->log_target == (TARGET_FILE | TARGET_JOURNAL) ? "both" :
                (c->log_target == TARGET_JOURNAL ? "journal" : "file"),
            kFormatNames[c->format], ret, cmp, c->top_n, alert);
}

static void daemon_prepare_logdir(const Config *c)
{
    resolve_logdir(c->logdir, g_logdir, sizeof(g_logdir));
    if ((c->log_target & TARGET_FILE) && ensure_dir(g_logdir) != 0)
        fprintf(stderr, "%s: warning: could not create log dir '%s': %s\n",
                PROGNAME, g_logdir, strerror(errno));
}

static int run_daemon(Config *cfg, const Cli *cli, int argc, char **argv, const HostInfo *h)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = handle_reload;
    sigaction(SIGHUP, &sa, NULL);

    fprintf(stderr, "%s: starting daemon mode\n", PROGNAME);
    announce_config(cfg);
    if (cfg->log_target & TARGET_FILE)
        logs_maintain(g_logdir, cfg->log_retention_days, cfg->compress_after_days);

    Sampler *s = malloc(sizeof(*s));
    Derived *d = malloc(sizeof(*d));
    if (!s || !d) { free(s); free(d); return 1; }
    sampler_start(s, cfg->top_n > 0);
    sampler_frame(s, cfg, d);
    alert_process(cfg, d, h, 1);
    log_frame(cfg, d, h);

    while (!g_stop) {
        /* Sleep in small chunks so SIGTERM/SIGHUP are handled promptly */
        long remaining = cfg->interval_secs;
        while (remaining > 0 && !g_stop && !g_reload) {
            long chunk = remaining > 1 ? 1 : remaining;
            sleep((unsigned int)chunk);
            remaining -= chunk;
        }
        if (g_stop) break;

        if (g_reload) {
            g_reload = 0;
            Config next;
            if (build_config(&next, cli, argc, argv) == 0) {
                *cfg = next;
                log_close();           /* reopen: picks up format/dir changes, logrotate-friendly */
                daemon_prepare_logdir(cfg);
                sampler_set_procs(s, cfg->top_n > 0);
                fprintf(stderr, "%s: reloaded configuration (SIGHUP)\n", PROGNAME);
                announce_config(cfg);
                syslog(LOG_NOTICE, "reloaded configuration");
                if (cfg->log_target & TARGET_FILE)
                    logs_maintain(g_logdir, cfg->log_retention_days, cfg->compress_after_days);
            } else {
                fprintf(stderr, "%s: reload failed, keeping previous configuration\n", PROGNAME);
            }
            continue; /* restart the wait with the (possibly new) interval */
        }

        if (sampler_tick(s) == 0) {
            sampler_frame(s, cfg, d);
            alert_process(cfg, d, h, 1);
            log_frame(cfg, d, h);
        }
    }
    log_close();
    sampler_free(s);
    free(s);
    free(d);
    fprintf(stderr, "%s: stopping (signal received)\n", PROGNAME);
    return 0;
}

/* ---------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */

#ifndef CPUMON_NO_MAIN
int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;

    Cli cli;
    Config cfg;
    config_defaults(&cfg);
    int rc = parse_cli(argc, argv, &cfg, &cli);
    if (rc < 0) return 2;
    if (rc > 0) return 0;
    if (!cli.replay && (cli.since || cli.until)) {
        fprintf(stderr, "%s: --since/--until only apply to --replay\n", PROGNAME);
        return 2;
    }
    if (build_config(&cfg, &cli, argc, argv) != 0) return 2;

    if (cli.replay) {
        time_t now = time(NULL), since = 0, until = (time_t)INT64_MAX;
        char err[200];
        if (cli.since && parse_when(cli.since, now, 0, &since, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: --since: %s\n", PROGNAME, err);
            return 2;
        }
        if (cli.until && parse_when(cli.until, now, 1, &until, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: --until: %s\n", PROGNAME, err);
            return 2;
        }
        if (until > (time_t)253402300799LL) until = (time_t)253402300799LL; /* 9999-12-31 */
        char logdir[512];
        const char *home = getenv("HOME");
        struct stat st;
        if (cfg.logdir[0])
            snprintf(logdir, sizeof(logdir), "%s", cfg.logdir);
        else if (stat(DEFAULT_LOGDIR, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(logdir, sizeof(logdir), "%s", DEFAULT_LOGDIR);
        else
            snprintf(logdir, sizeof(logdir), "%s/%s", home ? home : "", FALLBACK_LOGDIR);
        return run_replay(&cfg, logdir, since, until);
    }

    HostInfo host;
    host_info_read(&host);

    if (cli.once)
        return run_once(&cfg, &host);

    openlog(PROGNAME, LOG_PID, cli.daemon ? LOG_DAEMON : LOG_USER);

    if (cli.daemon) {
        daemon_prepare_logdir(&cfg);
        return run_daemon(&cfg, &cli, argc, argv, &host);
    }

    resolve_logdir(cfg.logdir, g_logdir, sizeof(g_logdir));

    if (cli.plain)
        return run_plain_watch(&cfg, &host);

#ifdef USE_NCURSES
    return run_interactive(&cfg, &host);
#else
    fprintf(stderr,
        "%s: built without ncurses support; use --plain, --once, or --daemon.\n",
        PROGNAME);
    return run_plain_watch(&cfg, &host);
#endif
}
#endif /* CPUMON_NO_MAIN */
