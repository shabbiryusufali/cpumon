/*
 * cpumon - Linux CPU usage monitor
 *
 * Reads /proc/stat (and hostname/uname/loadavg/uptime/cpuinfo) and renders
 * a scrollable, man-page-styled report either as an interactive ncurses
 * TUI (with a "watch"-style auto refresh) or, in --daemon mode, as
 * periodic plain-text snapshots appended to a daily log file. The
 * daemon mode is intended to be run under systemd. Daily log files
 * older than --log-retention-days (default 30, 0 disables) are
 * deleted automatically whenever the log rolls over to a new day.
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
 *          ./cpumon --daemon -i 5m       (headless logger, for systemd)
 *
 * License: do whatever you want with it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <ctype.h>
#include <locale.h>
#include <dirent.h>

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
#define STAT_PATH       "/proc/stat"
#define LOADAVG_PATH    "/proc/loadavg"
#define UPTIME_PATH     "/proc/uptime"
#define CPUINFO_PATH    "/proc/cpuinfo"
#define MAX_CORES       512
#define DEFAULT_LOGDIR  "/var/log/cpumon"
#define FALLBACK_LOGDIR ".local/share/cpumon"   /* under $HOME */
#define DEFAULT_LOG_RETENTION_DAYS 30            /* 0 disables auto-delete */
#define PAD_LINES        2200
#define PAD_COLS         220
#define BOOTSTRAP_USEC    200000                 /* 200ms warm-up sample pair */

/* ---------------------------------------------------------------------
 * /proc/stat parsing
 * ------------------------------------------------------------------- */

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
} CpuJiffies;

typedef struct {
    CpuJiffies total;                 /* aggregate "cpu" line */
    CpuJiffies cores[MAX_CORES];
    int ncores;                       /* number of per-core lines parsed */
    unsigned long long ctxt;
    unsigned long long processes;     /* forks since boot */
    unsigned long procs_running;
    unsigned long procs_blocked;
    time_t sampled_at;
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

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) == 0) {
            if (isdigit((unsigned char)line[3])) {
                if (cs->ncores < MAX_CORES) {
                    parse_cpu_line(line, &cs->cores[cs->ncores]);
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
                size_t len = strlen(colon);
                while (len > 0 && (colon[len - 1] == '\n' || colon[len - 1] == '\r')) colon[--len] = '\0';
                snprintf(buf, buflen, "%s", colon);
            }
            break;
        }
    }
    fclose(f);
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

/* ---------------------------------------------------------------------
 * Derived stats - percentages are always a delta between two CpuStat
 * samples, never a single reading.
 * ------------------------------------------------------------------- */

typedef struct {
    double total_pct, user_pct, nice_pct, system_pct, idle_pct;
    double iowait_pct, irq_pct, softirq_pct, steal_pct;
} CoreDerived;

typedef struct {
    CoreDerived agg;
    CoreDerived core[MAX_CORES];
    int ncores;
    double ctxt_per_sec;
    double forks_per_sec;
    unsigned long procs_running, procs_blocked;
    double elapsed_secs;
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

    d->ncores = (prev->ncores < cur->ncores) ? prev->ncores : cur->ncores;
    for (int i = 0; i < d->ncores; i++)
        derive_core(&prev->cores[i], &cur->cores[i], &d->core[i]);

    d->elapsed_secs = difftime(cur->sampled_at, prev->sampled_at);
    if (d->elapsed_secs <= 0) d->elapsed_secs = 1;

    unsigned long long dctxt = (cur->ctxt >= prev->ctxt) ? cur->ctxt - prev->ctxt : 0;
    unsigned long long dproc = (cur->processes >= prev->processes) ? cur->processes - prev->processes : 0;
    d->ctxt_per_sec  = (double)dctxt / d->elapsed_secs;
    d->forks_per_sec = (double)dproc / d->elapsed_secs;
    d->procs_running = cur->procs_running;
    d->procs_blocked = cur->procs_blocked;
}

/* Take a short (BOOTSTRAP_USEC) bootstrap pair so the very first frame
 * of any mode has a real delta to show instead of a placeholder. */
static void stat_sample_delta(CpuStat *prev_out, CpuStat *cur_out)
{
    stat_read(prev_out);
    usleep(BOOTSTRAP_USEC);
    stat_read(cur_out);
}

/* ---------------------------------------------------------------------
 * Logging - plain text snapshots, one file per calendar day
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

static void print_bar_plain(FILE *out, double pct, int width);

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

static void write_snapshot(FILE *f, const CpuStat *cs, const Derived *d,
                            const char *hostname, const char *kernel,
                            const char *model, long interval_secs)
{
    (void)interval_secs;
    char ts[64];
    struct tm tmv;
    localtime_r(&cs->sampled_at, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %Z", &tmv);

    fprintf(f, "======================================================================\n");
    fprintf(f, "%s   host=%s   kernel=%s\n", ts, hostname, kernel);
    fprintf(f, "======================================================================\n");

    fprintf(f, "CPU OVERVIEW  (%d core%s - %s)\n  ", d->ncores, d->ncores == 1 ? "" : "s", model);
    print_bar_plain(f, d->agg.total_pct, 40);
    fputc('\n', f);
    char user_s[16], sys_s[16], idle_s[16], iow_s[16];
    snprintf(user_s, sizeof(user_s), "%.1f%%", d->agg.user_pct);
    snprintf(sys_s, sizeof(sys_s), "%.1f%%", d->agg.system_pct);
    snprintf(idle_s, sizeof(idle_s), "%.1f%%", d->agg.idle_pct);
    snprintf(iow_s, sizeof(iow_s), "%.1f%%", d->agg.iowait_pct);
    log_two_col(f, "User", user_s, "System", sys_s);
    log_two_col(f, "Idle", idle_s, "IOWait", iow_s);
    fputc('\n', f);

    double l1 = -1, l5 = -1, l15 = -1;
    if (loadavg_read(&l1, &l5, &l15) == 0) {
        fprintf(f, "LOAD AVERAGE\n");
        fprintf(f, "  1m: %.2f   5m: %.2f   15m: %.2f\n\n", l1, l5, l15);
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

    double uptime_secs;
    if (uptime_read(&uptime_secs) == 0) {
        char up_s[64];
        format_duration(uptime_secs, up_s, sizeof(up_s));
        fprintf(f, "UPTIME\n  %s\n\n", up_s);
    }

    fprintf(f, "PER-CORE USAGE\n");
    for (int i = 0; i < d->ncores; i += 2) {
        char lbl1[16], val1[16];
        snprintf(lbl1, sizeof(lbl1), "cpu%d", i);
        snprintf(val1, sizeof(val1), "%.1f%%", d->core[i].total_pct);
        if (i + 1 < d->ncores) {
            char lbl2[16], val2[16];
            snprintf(lbl2, sizeof(lbl2), "cpu%d", i + 1);
            snprintf(val2, sizeof(val2), "%.1f%%", d->core[i + 1].total_pct);
            log_two_col(f, lbl1, val1, lbl2, val2);
        } else {
            log_two_col(f, lbl1, val1, NULL, NULL);
        }
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

static FILE *g_logfile = NULL;
static char g_log_date[16] = "";
static char g_logdir[512] = "";
static int g_log_retention_days = DEFAULT_LOG_RETENTION_DAYS;

static void prune_old_logs(const char *logdir, int retention_days);

static int log_open_for_today(void)
{
    char date[16];
    today_str(date, sizeof(date));
    if (g_logfile && strcmp(date, g_log_date) == 0)
        return 0; /* already open for today */

    if (g_logfile) {
        fclose(g_logfile);
        g_logfile = NULL;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.log", g_logdir, date);
    g_logfile = fopen(path, "a");
    if (!g_logfile) {
        fprintf(stderr, "%s: cannot open log file '%s': %s\n",
                PROGNAME, path, strerror(errno));
        return -1;
    }
    snprintf(g_log_date, sizeof(g_log_date), "%s", date);
    /* Rolling to a new day's file is a natural, cheap point to sweep
     * out anything older than the retention window. */
    prune_old_logs(g_logdir, g_log_retention_days);
    return 0;
}

/* ---------------------------------------------------------------------
 * Log retention - delete daily log files (YYYY-MM-DD.log) older than
 * retention_days. A retention_days value <= 0 disables pruning
 * entirely. Only ever touches files matching the exact name pattern
 * cpumon itself writes, so a log-dir shared with other files is safe.
 * ------------------------------------------------------------------- */

static int parse_log_filename_date(const char *name, struct tm *out)
{
    /* Expect exactly "YYYY-MM-DD.log" */
    int y, mo, d;
    char suffix[8];
    if (sscanf(name, "%4d-%2d-%2d.log%7s", &y, &mo, &d, suffix) != 3)
        return -1;
    if (strlen(name) != 14) /* "YYYY-MM-DD.log" == 14 chars, rejects trailing junk */
        return -1;
    memset(out, 0, sizeof(*out));
    out->tm_year = y - 1900;
    out->tm_mon  = mo - 1;
    out->tm_mday = d;
    out->tm_hour = 12; /* noon, to sidestep any DST edge effects in mktime */
    return 0;
}

static void prune_old_logs(const char *logdir, int retention_days)
{
    if (retention_days <= 0) return;

    DIR *dir = opendir(logdir);
    if (!dir) return;

    time_t now = time(NULL);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        struct tm file_tm;
        if (parse_log_filename_date(ent->d_name, &file_tm) != 0)
            continue;

        time_t file_time = mktime(&file_tm);
        if (file_time == (time_t)-1) continue;

        double age_days = difftime(now, file_time) / 86400.0;
        if (age_days <= (double)retention_days) continue;

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", logdir, ent->d_name);
        if (unlink(path) != 0) {
            fprintf(stderr, "%s: warning: could not delete expired log '%s': %s\n",
                    PROGNAME, path, strerror(errno));
        }
    }
    closedir(dir);
}

/* ---------------------------------------------------------------------
 * Plain-text (non-ncurses) single snapshot, for --once and as a
 * fallback if ncurses isn't compiled in.
 * ------------------------------------------------------------------- */

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

static void print_once(const CpuStat *cs, const Derived *d,
                        const char *hostname, const char *kernel, const char *model)
{
    char ts[64];
    struct tm tmv;
    localtime_r(&cs->sampled_at, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    printf("%s(1)                 CPU Monitor                 %s(1)\n\n", PROGNAME, PROGNAME);
    printf("HOST\n    %s   kernel %s   %s\n", hostname, kernel, ts);
    printf("    %d core%s - %s\n\n", d->ncores, d->ncores == 1 ? "" : "s", model);

    printf("CPU OVERVIEW\n");
    printf("    ");
    print_bar_plain(stdout, d->agg.total_pct, 40);
    printf("\n    user %-8.1f%% system %-8.1f%% idle %-8.1f%% iowait %-8.1f%%\n\n",
           d->agg.user_pct, d->agg.system_pct, d->agg.idle_pct, d->agg.iowait_pct);

    double l1, l5, l15;
    if (loadavg_read(&l1, &l5, &l15) == 0)
        printf("LOAD AVERAGE\n    1m: %.2f   5m: %.2f   15m: %.2f\n\n", l1, l5, l15);

    printf("PROCESSES\n    running %-6lu blocked %-6lu ctxt/s %-10.1f forks/s %.2f\n\n",
           d->procs_running, d->procs_blocked, d->ctxt_per_sec, d->forks_per_sec);

    double uptime_secs;
    if (uptime_read(&uptime_secs) == 0) {
        char up_s[64];
        format_duration(uptime_secs, up_s, sizeof(up_s));
        printf("UPTIME\n    %s\n\n", up_s);
    }

    printf("PER-CORE\n");
    for (int i = 0; i < d->ncores; i++) {
        printf("    cpu%-4d ", i);
        print_bar_plain(stdout, d->core[i].total_pct, 30);
        putchar('\n');
    }
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

static int run_plain_watch(long interval_secs, int logging_enabled,
                            const char *hostname, const char *kernel, const char *model)
{
    signal(SIGINT, handle_plain_signal);
    signal(SIGTERM, handle_plain_signal);

    CpuStat prev, cur;
    stat_sample_delta(&prev, &cur);

    while (!g_plain_stop) {
        Derived d;
        derive(&prev, &cur, &d);

        char ibuf[32];
        format_interval(interval_secs, ibuf, sizeof(ibuf));
        char ts[64];
        struct tm tmv;
        localtime_r(&cur.sampled_at, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

        fputs("\x1b[H\x1b[2J", stdout); /* cursor home + clear screen */
        printf("\x1b[1;7m cpumon \x1b[0m  System CPU Monitor - %s   refresh: %s   %s\n\n",
               hostname, ibuf, ts);

        printf("\x1b[1;36mCPU OVERVIEW\x1b[0m  (%d core%s - %s)\n  ",
               d.ncores, d.ncores == 1 ? "" : "s", model);
        print_bar_ansi(d.agg.total_pct, 40);
        printf("\n  user %-8.1f%% system %-8.1f%% idle %-8.1f%% iowait %-8.1f%%\n\n",
               d.agg.user_pct, d.agg.system_pct, d.agg.idle_pct, d.agg.iowait_pct);

        double l1, l5, l15;
        if (loadavg_read(&l1, &l5, &l15) == 0)
            printf("\x1b[1;36mLOAD AVERAGE\x1b[0m\n  1m: %.2f   5m: %.2f   15m: %.2f\n\n", l1, l5, l15);

        printf("\x1b[1;36mPROCESSES\x1b[0m\n  running %-6lu blocked %-6lu ctxt/s %-10.1f forks/s %.2f\n\n",
               d.procs_running, d.procs_blocked, d.ctxt_per_sec, d.forks_per_sec);

        printf("\x1b[1;36mPER-CORE\x1b[0m\n");
        int col = 0;
        for (int i = 0; i < d.ncores; i++) {
            printf("  cpu%-4d ", i);
            print_bar_ansi(d.core[i].total_pct, 20);
            printf("  ");
            col++;
            if (col == 2) { col = 0; putchar('\n'); }
        }
        if (col != 0) putchar('\n');

        printf("\n(Ctrl-C to quit -- plain mode, refresh every %s)\n", ibuf);
        fflush(stdout);

        if (logging_enabled) {
            if (log_open_for_today() == 0)
                write_snapshot(g_logfile, &cur, &d, hostname, kernel, model, interval_secs);
        }

        long remaining = interval_secs;
        while (remaining > 0 && !g_plain_stop) {
            long chunk = remaining > 1 ? 1 : remaining;
            sleep((unsigned int)chunk);
            remaining -= chunk;
        }
        prev = cur;
        stat_read(&cur);
    }
    if (g_logfile) fclose(g_logfile);
    printf("\n");
    return 0;
}

/* ---------------------------------------------------------------------
 * ncurses TUI
 * ------------------------------------------------------------------- */

#ifdef USE_NCURSES

enum { CP_HEADER = 1, CP_TITLE, CP_LABEL, CP_BAR_OK, CP_BAR_WARN, CP_BAR_CRIT, CP_VALUE, CP_DIM };

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

static int build_pad(WINDOW *pad, const CpuStat *cs, const Derived *d,
                      const char *hostname, const char *kernel, const char *model)
{
    werase(pad);
    int y = 1;

    char ts[64];
    struct tm tmv;
    localtime_r(&cs->sampled_at, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    wattron(pad, A_BOLD);
    mvwprintw(pad, y, 0, "System CPU Report");
    wattroff(pad, A_BOLD);
    wattron(pad, COLOR_PAIR(CP_DIM));
    mvwprintw(pad, y, 20, "host: %s   kernel: %s   sampled: %s", hostname, kernel, ts);
    wattroff(pad, COLOR_PAIR(CP_DIM));
    y += 2;

    /* ---- Overview ---- */
    section_title(pad, &y, "CPU OVERVIEW");
    mvwprintw(pad, y, 4, "%d core%s - %s", d->ncores, d->ncores == 1 ? "" : "s", model);
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

    /* ---- Load average ---- */
    double l1, l5, l15;
    if (loadavg_read(&l1, &l5, &l15) == 0) {
        section_title(pad, &y, "LOAD AVERAGE");
        mvwprintw(pad, y, 4, "1m: %.2f   5m: %.2f   15m: %.2f", l1, l5, l15);
        y += 2;
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

    /* ---- Uptime ---- */
    double uptime_secs;
    if (uptime_read(&uptime_secs) == 0) {
        section_title(pad, &y, "UPTIME");
        char up_h[64];
        format_duration(uptime_secs, up_h, sizeof(up_h));
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
    for (int i = 0; i < d->ncores; i++) {
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "cpu%d", i);
        wattron(pad, COLOR_PAIR(CP_LABEL));
        mvwprintw(pad, y, 4, "%-6s", lbl);
        wattroff(pad, COLOR_PAIR(CP_LABEL));
        draw_bar(pad, y, 11, 30, d->core[i].total_pct);
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
    char mid[128];
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

static int run_interactive(long interval_secs, int logging_enabled, const char *hostname,
                            const char *kernel, const char *model)
{
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr,
            "%s: stdout is not a terminal; the interactive display needs a real tty.\n"
            "Use --once for a single snapshot or --daemon for headless logging instead.\n",
            PROGNAME);
        return 1;
    }

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
    if (!pad) { endwin(); fprintf(stderr, "%s: failed to allocate display pad\n", PROGNAME); return 1; }

    CpuStat prev, cur;
    stat_sample_delta(&prev, &cur);
    Derived d;
    derive(&prev, &cur, &d);
    int content_h = build_pad(pad, &cur, &d, hostname, kernel, model);

    if (logging_enabled) {
        if (log_open_for_today() == 0)
            write_snapshot(g_logfile, &cur, &d, hostname, kernel, model, interval_secs);
    }

    struct timespec last_refresh, now;
    clock_gettime(CLOCK_MONOTONIC, &last_refresh);

    int running = 1;
    while (running) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        (void)cols;
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
        draw_header(cols, hostname, interval_secs);
        draw_footer(rows, cols, logging_enabled, secs_to_refresh);
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

        if (elapsed >= interval_secs) {
            prev = cur;
            stat_read(&cur);
            derive(&prev, &cur, &d);
            content_h = build_pad(pad, &cur, &d, hostname, kernel, model);
            if (logging_enabled) {
                if (log_open_for_today() == 0)
                    write_snapshot(g_logfile, &cur, &d, hostname, kernel, model, interval_secs);
            }
            clock_gettime(CLOCK_MONOTONIC, &last_refresh);
        }
    }

    delwin(pad);
    endwin();
    return 0;
}

#endif /* USE_NCURSES */

/* ---------------------------------------------------------------------
 * Daemon mode (no ncurses) - for systemd
 * ------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;
static void handle_signal(int sig) { (void)sig; g_stop = 1; }

static int run_daemon(long interval_secs, const char *hostname, const char *kernel, const char *model)
{
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    if (g_log_retention_days > 0)
        fprintf(stderr, "%s: starting daemon mode, interval=%lds, logdir=%s, log-retention=%dd\n",
                PROGNAME, interval_secs, g_logdir, g_log_retention_days);
    else
        fprintf(stderr, "%s: starting daemon mode, interval=%lds, logdir=%s, log-retention=disabled\n",
                PROGNAME, interval_secs, g_logdir);
    prune_old_logs(g_logdir, g_log_retention_days);

    CpuStat prev, cur;
    stat_sample_delta(&prev, &cur);
    Derived d;
    derive(&prev, &cur, &d);
    if (log_open_for_today() == 0)
        write_snapshot(g_logfile, &cur, &d, hostname, kernel, model, interval_secs);

    while (!g_stop) {
        /* Sleep in small chunks so SIGTERM is handled promptly */
        long remaining = interval_secs;
        while (remaining > 0 && !g_stop) {
            long chunk = remaining > 1 ? 1 : remaining;
            sleep((unsigned int)chunk);
            remaining -= chunk;
        }
        if (g_stop) break;

        prev = cur;
        if (stat_read(&cur) == 0) {
            derive(&prev, &cur, &d);
            if (log_open_for_today() == 0)
                write_snapshot(g_logfile, &cur, &d, hostname, kernel, model, interval_secs);
        }
    }
    if (g_logfile) fclose(g_logfile);
    fprintf(stderr, "%s: stopping (signal received)\n", PROGNAME);
    return 0;
}

/* ---------------------------------------------------------------------
 * CLI
 * ------------------------------------------------------------------- */

static void usage(void)
{
    printf(
"%s %s - Linux CPU usage monitor\n\n"
"Usage: %s [options]\n\n"
"Options:\n"
"  -i, --interval INTERVAL   Refresh/log interval. Accepts plain seconds or a\n"
"                             suffix: s (sec), m (min), h (hour), d (day).\n"
"                             Examples: -i 5, -i 5s, -i 5m, -i 1h. Default: 5s\n"
"  -d, --daemon               Headless mode: no TUI, just logs snapshots on\n"
"                             each interval. Intended for systemd. Handles\n"
"                             SIGTERM/SIGINT for clean shutdown.\n"
"  -o, --once                 Print a single snapshot to stdout and exit\n"
"                             (no ncurses, no loop, no log file written).\n"
"  -p, --plain                 Interactive watch mode without ncurses: plain\n"
"                             ANSI clear-screen/redraw instead of full cursor\n"
"                             addressing. Try this if the default ncurses\n"
"                             display looks garbled or doesn't redraw in\n"
"                             place on your terminal (common under some\n"
"                             Windows/WSL terminal front-ends).\n"
"  -l, --log-dir DIR          Directory for daily log files (named\n"
"                             YYYY-MM-DD.log). Default: %s\n"
"                             (falls back to ~/%s if not writable).\n"
"  -r, --log-retention-days N  Auto-delete log files older than N days.\n"
"                             N=0 disables auto-delete. Default: %d\n"
"                             (also settable via CPUMON_LOG_RETENTION_DAYS).\n"
"  -n, --no-log                Disable logging in interactive mode.\n"
"  -h, --help                  Show this help and exit.\n"
"  -v, --version                Show version and exit.\n\n"
"Interactive keys:  q quit | up/down or j/k scroll | PgUp/PgDn | g/G top/bottom\n"
"                    r refresh now\n\n"
"Examples:\n"
"  %s                          Interactive TUI, refresh every 5 seconds\n"
"  %s -i 5m                    Interactive TUI, refresh every 5 minutes\n"
"  %s --once                   One-shot plain text report\n"
"  %s --daemon -i 5m           Headless logger for systemd (5 minute interval)\n"
"  %s --daemon -i 5m -r 14     Same, but only keep 14 days of logs\n",
    PROGNAME, VERSION, PROGNAME, DEFAULT_LOGDIR, FALLBACK_LOGDIR,
    DEFAULT_LOG_RETENTION_DAYS,
    PROGNAME, PROGNAME, PROGNAME, PROGNAME, PROGNAME);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    long interval_secs = 5;
    int daemon_mode = 0;
    int once_mode = 0;
    int plain_mode = 0;
    int no_log = 0;
    const char *logdir_arg = NULL;
    char errbuf[128];

    /* CPUMON_LOG_RETENTION_DAYS lets systemd's EnvironmentFile=/etc/cpumon/cpumon.conf
     * configure retention without editing the unit or passing -r explicitly;
     * -r/--log-retention-days on the command line still wins over it. */
    const char *retention_env = getenv("CPUMON_LOG_RETENTION_DAYS");
    if (retention_env && *retention_env) {
        char *end;
        long v = strtol(retention_env, &end, 10);
        if (end != retention_env && *end == '\0' && v >= 0)
            g_log_retention_days = (int)v;
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((strcmp(a, "-i") == 0 || strcmp(a, "--interval") == 0) && i + 1 < argc) {
            long v = parse_interval(argv[++i], errbuf, sizeof(errbuf));
            if (v < 0) { fprintf(stderr, "%s: %s\n", PROGNAME, errbuf); return 2; }
            interval_secs = v;
        } else if (strncmp(a, "--interval=", 11) == 0) {
            long v = parse_interval(a + 11, errbuf, sizeof(errbuf));
            if (v < 0) { fprintf(stderr, "%s: %s\n", PROGNAME, errbuf); return 2; }
            interval_secs = v;
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--once") == 0) {
            once_mode = 1;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--plain") == 0) {
            plain_mode = 1;
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--no-log") == 0) {
            no_log = 1;
        } else if ((strcmp(a, "-l") == 0 || strcmp(a, "--log-dir") == 0) && i + 1 < argc) {
            logdir_arg = argv[++i];
        } else if (strncmp(a, "--log-dir=", 10) == 0) {
            logdir_arg = a + 10;
        } else if ((strcmp(a, "-r") == 0 || strcmp(a, "--log-retention-days") == 0) && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 0) {
                fprintf(stderr, "%s: invalid log-retention-days '%s'\n", PROGNAME, argv[i]);
                return 2;
            }
            g_log_retention_days = (int)v;
        } else if (strncmp(a, "--log-retention-days=", 21) == 0) {
            char *end;
            long v = strtol(a + 21, &end, 10);
            if (end == a + 21 || *end != '\0' || v < 0) {
                fprintf(stderr, "%s: invalid log-retention-days '%s'\n", PROGNAME, a + 21);
                return 2;
            }
            g_log_retention_days = (int)v;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s %s\n", PROGNAME, VERSION);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option '%s' (see --help)\n", PROGNAME, a);
            return 2;
        }
    }

    char hostname[256] = "unknown";
    gethostname(hostname, sizeof(hostname) - 1);

    struct utsname uts;
    char kernel[128] = "unknown";
    if (uname(&uts) == 0)
        snprintf(kernel, sizeof(kernel), "%s", uts.release);

    char model[192];
    cpu_model_read(model, sizeof(model));

    resolve_logdir(logdir_arg, g_logdir, sizeof(g_logdir));
    if (ensure_dir(g_logdir) != 0 && daemon_mode) {
        fprintf(stderr, "%s: warning: could not create log dir '%s': %s\n",
                PROGNAME, g_logdir, strerror(errno));
    }

    if (once_mode) {
        CpuStat prev, cur;
        stat_sample_delta(&prev, &cur);
        Derived d;
        derive(&prev, &cur, &d);
        print_once(&cur, &d, hostname, kernel, model);
        return 0;
    }

    if (daemon_mode) {
        return run_daemon(interval_secs, hostname, kernel, model);
    }

    if (plain_mode) {
        return run_plain_watch(interval_secs, !no_log, hostname, kernel, model);
    }

#ifdef USE_NCURSES
    return run_interactive(interval_secs, !no_log, hostname, kernel, model);
#else
    fprintf(stderr,
        "%s: built without ncurses support; use --plain, --once, or --daemon.\n",
        PROGNAME);
    return run_plain_watch(interval_secs, !no_log, hostname, kernel, model);
#endif
}
