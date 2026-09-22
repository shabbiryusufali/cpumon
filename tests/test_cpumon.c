/*
 * Unit tests for cpumon's parsing, maths, config and output helpers.
 *
 * The program is a single C file of static functions, so the tests
 * compile it in directly (without its main()) rather than linking
 * against it. Build and run with `make test`.
 */

#define CPUMON_NO_MAIN
#define JOURNAL_SOCKET "/tmp/cpumon-test-journal.sock"
#include "../src/cpumon.c"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_STR(a, b) do { \
    g_checks++; \
    if (strcmp((a), (b)) != 0) { \
        g_failures++; \
        fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, (b), (a)); \
    } \
} while (0)

#define CHECK_NEAR(a, b) do { \
    g_checks++; \
    double _a = (a), _b = (b); \
    if (!(_a - _b < 0.01 && _b - _a < 0.01)) { \
        g_failures++; \
        fprintf(stderr, "%s:%d: expected %g, got %g\n", __FILE__, __LINE__, _b, _a); \
    } \
} while (0)

/* Captures everything a writer prints to a FILE* as a string. */
typedef struct { char *buf; size_t len; FILE *f; } Mem;
static void mem_open(Mem *m) { m->buf = NULL; m->len = 0; m->f = open_memstream(&m->buf, &m->len); }
static char *mem_close(Mem *m) { fclose(m->f); return m->buf; }

static void test_parse_interval(void)
{
    char err[128];
    CHECK(parse_interval("5", err, sizeof(err)) == 5);
    CHECK(parse_interval("5s", err, sizeof(err)) == 5);
    CHECK(parse_interval("5m", err, sizeof(err)) == 300);
    CHECK(parse_interval("1h", err, sizeof(err)) == 3600);
    CHECK(parse_interval("2D", err, sizeof(err)) == 172800);
    CHECK(parse_interval("0.5m", err, sizeof(err)) == 30);
    CHECK(parse_interval("0", err, sizeof(err)) == 1);       /* clamped */
    CHECK(parse_interval("", err, sizeof(err)) == -1);
    CHECK(parse_interval("abc", err, sizeof(err)) == -1);
    CHECK(parse_interval("-5", err, sizeof(err)) == -1);
    CHECK(parse_interval("5x", err, sizeof(err)) == -1);
    CHECK(parse_interval("5mm", err, sizeof(err)) == -1);
}

static void test_format_interval_duration(void)
{
    char b[64];
    format_interval(5, b, sizeof(b));      CHECK_STR(b, "5s");
    format_interval(90, b, sizeof(b));     CHECK_STR(b, "90s");
    format_interval(300, b, sizeof(b));    CHECK_STR(b, "5m");
    format_interval(7200, b, sizeof(b));   CHECK_STR(b, "2h");
    format_interval(86400, b, sizeof(b));  CHECK_STR(b, "1d");
    format_duration(59, b, sizeof(b));     CHECK_STR(b, "0m 59s");
    format_duration(3725, b, sizeof(b));   CHECK_STR(b, "1h 2m 5s");
    format_duration(90061, b, sizeof(b));  CHECK_STR(b, "1d 1h 1m");
}

static void test_parse_when(void)
{
    char err[200];
    time_t now = 1790000000, t;
    CHECK(parse_when("now", now, 0, &t, err, sizeof(err)) == 0 && t == now);
    CHECK(parse_when("2h", now, 0, &t, err, sizeof(err)) == 0 && t == now - 7200);
    CHECK(parse_when("7d", now, 0, &t, err, sizeof(err)) == 0 && t == now - 7 * 86400);

    struct tm tm;
    CHECK(parse_when("2026-03-04 05:06:07", now, 0, &t, err, sizeof(err)) == 0);
    localtime_r(&t, &tm);
    CHECK(tm.tm_year == 126 && tm.tm_mon == 2 && tm.tm_mday == 4);
    CHECK(tm.tm_hour == 5 && tm.tm_min == 6 && tm.tm_sec == 7);

    CHECK(parse_when("2026-03-04T05:06", now, 0, &t, err, sizeof(err)) == 0);
    localtime_r(&t, &tm);
    CHECK(tm.tm_hour == 5 && tm.tm_min == 6 && tm.tm_sec == 0);

    /* A bare date is the start of the day, or its end for --until. */
    CHECK(parse_when("2026-03-04", now, 0, &t, err, sizeof(err)) == 0);
    localtime_r(&t, &tm);
    CHECK(tm.tm_hour == 0 && tm.tm_min == 0);
    CHECK(parse_when("2026-03-04", now, 1, &t, err, sizeof(err)) == 0);
    localtime_r(&t, &tm);
    CHECK(tm.tm_mday == 4 && tm.tm_hour == 23 && tm.tm_min == 59 && tm.tm_sec == 59);

    time_t today, yesterday;
    CHECK(parse_when("today", now, 0, &today, err, sizeof(err)) == 0);
    CHECK(parse_when("yesterday", now, 0, &yesterday, err, sizeof(err)) == 0);
    CHECK(today <= now && now - today < 86400);
    CHECK(today - yesterday >= 82800 && today - yesterday <= 90000); /* DST-safe */

    CHECK(parse_when("next tuesday", now, 0, &t, err, sizeof(err)) == -1);
    CHECK(parse_when("2026-13-45", now, 0, &t, err, sizeof(err)) == -1);
}

static void test_derive_core(void)
{
    CpuJiffies a = { 100, 0, 50, 800, 50, 0, 0, 0, 0, 0 };
    CpuJiffies b = { 160, 10, 70, 830, 60, 5, 5, 10, 0, 0 };
    /* deltas: user 60 nice 10 sys 20 idle 30 iowait 10 irq 5 softirq 5 steal 10 = 150 */
    CoreDerived d;
    derive_core(&a, &b, &d);
    CHECK_NEAR(d.user_pct, 40.0);
    CHECK_NEAR(d.nice_pct, 100.0 * 10 / 150);
    CHECK_NEAR(d.system_pct, 100.0 * 20 / 150);
    CHECK_NEAR(d.idle_pct, 20.0);
    CHECK_NEAR(d.iowait_pct, 100.0 * 10 / 150);
    CHECK_NEAR(d.steal_pct, 100.0 * 10 / 150);
    CHECK_NEAR(d.total_pct, 80.0);

    derive_core(&a, &a, &d);                 /* no time passed */
    CHECK_NEAR(d.idle_pct, 100.0);
    CHECK_NEAR(d.total_pct, 0.0);

    derive_core(&b, &a, &d);                 /* counters went backwards */
    CHECK_NEAR(d.idle_pct, 100.0);
    CHECK_NEAR(d.total_pct, 0.0);
}

static void test_derive_matches_cores_by_id(void)
{
    static CpuStat prev, cur;
    static Derived d;
    memset(&prev, 0, sizeof(prev));
    memset(&cur, 0, sizeof(cur));
    prev.mono = 10.0; cur.mono = 12.0;
    prev.ncores = 3; cur.ncores = 2;
    prev.core_id[0] = 0; prev.core_id[1] = 1; prev.core_id[2] = 2;
    cur.core_id[0] = 0;  cur.core_id[1] = 2;  /* cpu1 went offline */
    CpuJiffies base = { 0, 0, 0, 100, 0, 0, 0, 0, 0, 0 };
    prev.cores[0] = base; prev.cores[1] = base; prev.cores[2] = base;
    cur.cores[0] = base;  cur.cores[0].idle += 100;           /* cpu0 idle */
    cur.cores[1] = base;  cur.cores[1].user += 100;           /* cpu2 busy */
    prev.ctxt = 100; cur.ctxt = 300;
    derive(&prev, &cur, &d);
    CHECK(d.ncores == 2);
    CHECK(d.core_id[1] == 2);
    CHECK_NEAR(d.core[0].total_pct, 0.0);
    CHECK_NEAR(d.core[1].total_pct, 100.0);
    CHECK_NEAR(d.elapsed_secs, 2.0);
    CHECK_NEAR(d.ctxt_per_sec, 100.0);       /* sub-second-accurate rate */
}

static void test_parse_cpu_line(void)
{
    CpuJiffies j;
    CHECK(parse_cpu_line("cpu3 1 2 3 4 5 6 7 8 9 10\n", &j) == 0);
    CHECK(j.user == 1 && j.idle == 4 && j.steal == 8 && j.guest_nice == 10);
    CHECK(parse_cpu_line("cpu 10 20 30 40\n", &j) == 0);     /* old kernels */
    CHECK(j.system == 30 && j.idle == 40 && j.iowait == 0);
    CHECK(parse_cpu_line("cpu 10 20\n", &j) == -1);
}

static void test_psi_parse(void)
{
    Psi p;
    CHECK(psi_parse("some avg10=1.50 avg60=2.25 avg300=0.10 total=12345\n"
                    "full avg10=0.00 avg60=0.50 avg300=0.00 total=99\n", &p) == 0);
    CHECK(p.have_some && p.have_full);
    CHECK_NEAR(p.some[0], 1.5);
    CHECK_NEAR(p.some[1], 2.25);
    CHECK_NEAR(p.full[1], 0.5);
    CHECK(psi_parse("some avg10=3.00 avg60=0.00 avg300=0.00 total=1\n", &p) == 0);
    CHECK(p.have_some && !p.have_full);
    CHECK(psi_parse("garbage\n", &p) == -1);
}

static void test_cpuinfo_mhz(void)
{
    const char *text =
        "processor\t: 0\nmodel name\t: X\ncpu MHz\t\t: 2100.123\n\n"
        "processor\t: 1\ncpu MHz\t\t: 3400.5\n\n"
        "processor\t: 3\ncpu MHz\t\t: 800\n\n";
    FILE *f = fmemopen((void *)text, strlen(text), "r");
    int ids[3] = { 0, 1, 2 };
    double mhz[3] = { NAN, NAN, NAN };
    CHECK(cpuinfo_mhz_parse(f, ids, 3, mhz) == 2);
    fclose(f);
    CHECK_NEAR(mhz[0], 2100.123);
    CHECK_NEAR(mhz[1], 3400.5);
    CHECK(isnan(mhz[2]));
}

static void test_proc_parse_stat(void)
{
    ProcEntry e;
    const char *line =
        "1234 (my (weird) proc) S 1 1234 1234 0 -1 4194560 100 0 0 0 "
        "250 75 0 0 20 0 3 0 99999 1000000 200 18446744073709551615\n";
    CHECK(proc_parse_stat(line, &e) == 0);
    CHECK(e.pid == 1234);
    CHECK_STR(e.comm, "my (weird) proc");
    CHECK(e.state == 'S');
    CHECK(e.ticks == 325);
    CHECK(e.start == 99999);
    CHECK(proc_parse_stat("1234 no parens", &e) == -1);
    CHECK(proc_parse_stat("1 (x)", &e) == -1);
}

static void test_top_compute(void)
{
    ProcEntry pv[] = {
        { 10, 'S', 100, 5, "idle" },
        { 20, 'R', 1000, 6, "busy" },
        { 30, 'R', 500, 7, "reused" },
    };
    ProcEntry cv[] = {
        { 10, 'S', 100, 5, "idle" },          /* no CPU used: excluded */
        { 20, 'R', 1200, 6, "busy" },         /* 200 ticks */
        { 30, 'R', 50, 99, "newpid" },        /* pid reused: all 50 ticks count */
        { 40, 'R', 100, 100, "fresh" },       /* started during the interval */
    };
    ProcTable prev = { pv, 3, 3 }, cur = { cv, 4, 4 };
    TopProc top[MAX_TOP];
    int n = top_compute(&prev, &cur, 2.0, 100, 10, top);
    CHECK(n == 3);
    CHECK(top[0].pid == 20);
    CHECK_NEAR(top[0].pct, 100.0);           /* 200 ticks / (100 Hz * 2 s) */
    CHECK(top[1].pid == 40);
    CHECK_NEAR(top[1].pct, 50.0);
    CHECK(top[2].pid == 30);
    CHECK_STR(top[2].comm, "newpid");
    CHECK_NEAR(top[2].pct, 25.0);
    CHECK(top_compute(&prev, &cur, 2.0, 100, 1, top) == 1);
    CHECK(top_compute(&prev, &cur, 2.0, 100, 0, top) == 0);
}

static void test_log_filenames(void)
{
    struct tm tm;
    int fmt, gz;
    CHECK(parse_log_filename_date("2026-09-18.log", &tm, &fmt, &gz) == 0);
    CHECK(tm.tm_year == 126 && tm.tm_mon == 8 && tm.tm_mday == 18 && fmt == FMT_TEXT && !gz);
    CHECK(parse_log_filename_date("2026-09-18.jsonl", &tm, &fmt, &gz) == 0 && fmt == FMT_JSON && !gz);
    CHECK(parse_log_filename_date("2026-09-18.csv.gz", &tm, &fmt, &gz) == 0 && fmt == FMT_CSV && gz);
    CHECK(parse_log_filename_date("2026-09-18.log.gz", &tm, &fmt, &gz) == 0 && fmt == FMT_TEXT && gz);
    CHECK(parse_log_filename_date("2026-09-18.log.bak", &tm, NULL, NULL) == -1);
    CHECK(parse_log_filename_date("2026-09-18.logx", &tm, NULL, NULL) == -1);
    CHECK(parse_log_filename_date("2026-9-18.log", &tm, NULL, NULL) == -1);
    CHECK(parse_log_filename_date("2026-13-01.log", &tm, NULL, NULL) == -1);
    CHECK(parse_log_filename_date("notes.txt", &tm, NULL, NULL) == -1);
    CHECK(parse_log_filename_date("2026-09-18.gz", &tm, NULL, NULL) == -1);
    CHECK(parse_log_filename_date("", &tm, NULL, NULL) == -1);
}

static void test_log_age_days(void)
{
    struct tm now_tm = { 0 };
    now_tm.tm_year = 126; now_tm.tm_mon = 8; now_tm.tm_mday = 18;
    now_tm.tm_hour = 0; now_tm.tm_min = 5; now_tm.tm_isdst = -1;
    time_t just_after_midnight = mktime(&now_tm);
    now_tm.tm_hour = 23; now_tm.tm_min = 55; now_tm.tm_isdst = -1;
    time_t just_before_midnight = mktime(&now_tm);

    struct tm f;
    parse_log_filename_date("2026-09-18.log", &f, NULL, NULL);
    CHECK(log_age_days(&f, just_after_midnight) == 0);
    CHECK(log_age_days(&f, just_before_midnight) == 0);
    parse_log_filename_date("2026-09-17.log", &f, NULL, NULL);
    CHECK(log_age_days(&f, just_after_midnight) == 1);
    CHECK(log_age_days(&f, just_before_midnight) == 1);
    parse_log_filename_date("2026-08-18.log", &f, NULL, NULL);
    CHECK(log_age_days(&f, just_after_midnight) == 31);
    parse_log_filename_date("2025-09-18.log", &f, NULL, NULL);
    CHECK(log_age_days(&f, just_after_midnight) == 365);
}

static void test_config(void)
{
    Config c;
    char err[160];
    config_defaults(&c);
    CHECK(c.interval_secs == 5 && c.log_retention_days == 30 && c.top_n == DEFAULT_TOP);
    CHECK(c.format == FMT_TEXT && c.log_target == TARGET_FILE && c.alert_pct == 0);

    CHECK(config_set(&c, "CPUMON_INTERVAL", "5m", err, sizeof(err)) == 0 && c.interval_secs == 300);
    CHECK(config_set(&c, "CPUMON_INTERVAL", "soon", err, sizeof(err)) == -1 && c.interval_secs == 300);
    CHECK(config_set(&c, "CPUMON_FORMAT", "JSON", err, sizeof(err)) == 0 && c.format == FMT_JSON);
    CHECK(config_set(&c, "CPUMON_FORMAT", "xml", err, sizeof(err)) == -1);
    CHECK(config_set(&c, "CPUMON_LOG_TARGET", "both", err, sizeof(err)) == 0 &&
          c.log_target == (TARGET_FILE | TARGET_JOURNAL));
    CHECK(config_set(&c, "CPUMON_LOG_TARGET", "journal", err, sizeof(err)) == 0 && c.log_target == TARGET_JOURNAL);
    CHECK(config_set(&c, "CPUMON_LOG_TARGET", "syslog", err, sizeof(err)) == -1);
    CHECK(config_set(&c, "CPUMON_TOP", "0", err, sizeof(err)) == 0 && c.top_n == 0);
    CHECK(config_set(&c, "CPUMON_TOP", "51", err, sizeof(err)) == -1);
    CHECK(config_set(&c, "CPUMON_ALERT_THRESHOLD", "90%", err, sizeof(err)) == 0 && c.alert_pct == 90);
    CHECK(config_set(&c, "CPUMON_ALERT_THRESHOLD", "101", err, sizeof(err)) == -1);
    CHECK(config_set(&c, "CPUMON_ALERT_DURATION", "10m", err, sizeof(err)) == 0 && c.alert_secs == 600);
    CHECK(config_set(&c, "CPUMON_ALERT_DURATION", "0", err, sizeof(err)) == 0 && c.alert_secs == 0);
    CHECK(config_set(&c, "CPUMON_LOG_RETENTION_DAYS", "-1", err, sizeof(err)) == -1);
    CHECK(config_set(&c, "CPUMON_COMPRESS_AFTER_DAYS", "2", err, sizeof(err)) == 0 && c.compress_after_days == 2);
    CHECK(config_set(&c, "CPUMON_NO_LOG", "yes", err, sizeof(err)) == 0 && c.no_log == 1);
    CHECK(config_set(&c, "CPUMON_NO_LOG", "maybe", err, sizeof(err)) == -1);
    CHECK(config_set(&c, "CPUMON_NOPE", "1", err, sizeof(err)) == -2);
}

static void test_config_file(void)
{
    char path[] = "/tmp/cpumon-test-conf-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const char *text =
        "# comment\n"
        "\n"
        "CPUMON_INTERVAL=2m\n"
        "  CPUMON_LOG_DIR = \"/var/tmp/cpu logs\"\n"
        "export CPUMON_TOP=7\n"
        "CPUMON_FORMAT='csv'\n"
        "CPUMON_ALERT_THRESHOLD=bogus\n"
        "not a setting\n";
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);

    Config c;
    config_defaults(&c);
    FILE *saved = stderr;
    stderr = fopen("/dev/null", "w");        /* bad lines warn; keep output clean */
    int rc = config_load_file(&c, path);
    fclose(stderr);
    stderr = saved;
    unlink(path);
    CHECK(rc == 0);
    CHECK(c.interval_secs == 120);
    CHECK_STR(c.logdir, "/var/tmp/cpu logs");
    CHECK(c.top_n == 7);
    CHECK(c.format == FMT_CSV);
    CHECK(c.alert_pct == 0);                 /* invalid value ignored */
    CHECK(config_load_file(&c, "/nonexistent/cpumon.conf") == -1);
}

static void test_cli(void)
{
    Config c;
    Cli cli;
    config_defaults(&c);
    char *argv1[] = { "cpumon", "--daemon", "-i", "1h", "--format=json", "-t", "3",
                      "--alert", "85", "-A", "5m", "-c", "/x.conf", "-n", NULL };
    CHECK(parse_cli(14, argv1, &c, &cli) == 0);
    CHECK(cli.daemon && !cli.once && !cli.replay);
    CHECK(c.interval_secs == 3600 && c.format == FMT_JSON && c.top_n == 3);
    CHECK(c.alert_pct == 85 && c.alert_secs == 300 && c.no_log);
    CHECK_STR(cli.config_path, "/x.conf");

    char *argv2[] = { "cpumon", "--replay", "--since", "2h", "--until=now", NULL };
    CHECK(parse_cli(5, argv2, &c, &cli) == 0);
    CHECK(cli.replay);
    CHECK_STR(cli.since, "2h");
    CHECK_STR(cli.until, "now");

    FILE *saved = stderr;
    stderr = fopen("/dev/null", "w");
    char *argv3[] = { "cpumon", "--interval", NULL };
    char *argv4[] = { "cpumon", "--wat", NULL };
    char *argv5[] = { "cpumon", "--top", "999", NULL };
    CHECK(parse_cli(2, argv3, &c, &cli) == -1);
    CHECK(parse_cli(2, argv4, &c, &cli) == -1);
    CHECK(parse_cli(3, argv5, &c, &cli) == -1);
    fclose(stderr);
    stderr = saved;
}

static void test_alerts(void)
{
    AlertState a = { 0, -1, 0 };
    /* 60s hold: two 30s frames above threshold are needed. */
    CHECK(alert_update(&a, 90, 60, 95, 0, 30) == ALERT_NONE);
    CHECK(alert_update(&a, 90, 60, 95, 30, 60) == ALERT_RAISED && a.active);
    CHECK(alert_update(&a, 90, 60, 99, 60, 90) == ALERT_NONE && a.active);
    CHECK(alert_update(&a, 90, 60, 50, 90, 120) == ALERT_CLEARED && !a.active);
    /* A dip resets the hold timer. */
    CHECK(alert_update(&a, 90, 60, 95, 120, 150) == ALERT_NONE);
    CHECK(alert_update(&a, 90, 60, 10, 150, 180) == ALERT_NONE);
    CHECK(alert_update(&a, 90, 60, 95, 180, 210) == ALERT_NONE);
    CHECK(alert_update(&a, 90, 60, 95, 210, 240) == ALERT_RAISED);
    /* Disabling alerts while one is active clears it. */
    CHECK(alert_update(&a, 0, 60, 95, 240, 270) == ALERT_CLEARED && !a.active);
    CHECK(alert_update(&a, 0, 60, 95, 270, 300) == ALERT_NONE);
    /* No hold time: fires on the first high frame. */
    CHECK(alert_update(&a, 50, 0, 60, 300, 305) == ALERT_RAISED);
}

static void test_history_sparkline(void)
{
    static History h;
    static Derived d;
    memset(&h, 0, sizeof(h));
    memset(&d, 0, sizeof(d));
    d.ncores = 1;
    for (int i = 0; i <= 10; i++) {
        d.agg.total_pct = i * 10;
        d.core[0].total_pct = 100 - i * 10;
        history_push(&h, &d);
    }
    unsigned char v[HIST_LEN];
    int n = history_series(&h, -1, 5, v);
    CHECK(n == 5);
    CHECK(v[0] == 60 && v[4] == 100);        /* oldest first */
    n = history_series(&h, 0, HIST_LEN, v);
    CHECK(n == 11 && v[0] == 100 && v[10] == 0);
    CHECK(history_series(&h, 5, 10, v) == 0);

    for (int i = 0; i < HIST_LEN + 7; i++) history_push(&h, &d); /* wraps */
    CHECK(history_series(&h, -1, HIST_LEN * 2, v) == HIST_LEN);

    unsigned char vals[] = { 0, 50, 100 };
    char buf[64];
    sparkline(buf, sizeof(buf), vals, 3, 0);
    CHECK_STR(buf, "_+@");
    sparkline(buf, sizeof(buf), vals, 3, 1);
    CHECK_STR(buf, "\xe2\x96\x81\xe2\x96\x85\xe2\x96\x88");
    sparkline(buf, 5, vals, 3, 1);           /* truncates on a whole glyph */
    CHECK_STR(buf, "\xe2\x96\x81");

    int mn, avg, mx;
    history_stats(vals, 3, &mn, &avg, &mx);
    CHECK(mn == 0 && avg == 50 && mx == 100);
}

static void fill_frame(Derived *d)
{
    memset(d, 0, sizeof(*d));
    d->ts = 1790000000;
    d->ncores = 2;
    d->core_id[0] = 0; d->core_id[1] = 1;
    d->core[0].total_pct = 12.5; d->core[1].total_pct = 87.5;
    d->core_mhz[0] = 2100; d->core_mhz[1] = NAN;
    d->agg.total_pct = 50; d->agg.user_pct = 40; d->agg.system_pct = 10; d->agg.idle_pct = 50;
    d->have_load = 1; d->load[0] = 1.5; d->load[1] = 1.25; d->load[2] = 1;
    d->psi.have_some = 1; d->psi.some[0] = 3.5;
    d->have_freq = 1; d->freq_avg = 2100; d->freq_min = 2100; d->freq_max = 2100;
    d->ntemps = 1; snprintf(d->temps[0].label, sizeof(d->temps[0].label), "coretemp Package id 0");
    d->temps[0].celsius = 55.5; d->temp_max = 55.5;
    d->ntop = 1; d->top[0].pid = 42; d->top[0].state = 'R'; d->top[0].pct = 99.5;
    snprintf(d->top[0].comm, sizeof(d->top[0].comm), "a,\"b\"\x01\xff");
    d->elapsed_secs = 5;
}

static void test_writers(void)
{
    static Derived d;
    HostInfo h = { "host1", "6.1.0", "Test CPU" };
    fill_frame(&d);
    Mem m;

    mem_open(&m);
    write_json_snapshot(m.f, &d, &h);
    char *json = mem_close(&m);
    CHECK(strncmp(json, "{\"ts\":1790000000,", 17) == 0);
    CHECK(strstr(json, "\"per_core\":[{\"id\":0,\"total\":12.50") != NULL);
    CHECK(strstr(json, "\"mhz\":2100}") != NULL);
    CHECK(strstr(json, "\"mhz\":null}") != NULL);
    CHECK(strstr(json, "\"load\":[1.50,1.25,1.00]") != NULL);
    CHECK(strstr(json, "\"comm\":\"a,\\\"b\\\"\\u0001?\"") != NULL);
    CHECK(strstr(json, "\"temp_max_c\":55.5") != NULL);
    CHECK(strstr(json, "\"alert\":{\"active\":false,\"threshold\":null}}\n") != NULL);
    free(json);

    mem_open(&m);
    write_csv_header(m.f, &d);
    write_csv_row(m.f, &d, &h);
    char *csv = mem_close(&m);
    char *row = strchr(csv, '\n') + 1;
    int header_cols = 1, row_cols = 1;
    for (char *p = csv; p < row; p++) if (*p == ',') header_cols++;
    CHECK(header_cols == CSV_FIXED_COLS + 2);
    CHECK(strstr(csv, ",cpu0,cpu1\n") != NULL);
    CHECK(strncmp(row, "1790000000,", 11) == 0);
    CHECK(strstr(row, ",42,\"a,\"\"b\"\"\x01\xff\",99.5,0,12.50,87.50\n") != NULL);
    /* the quoted comm contains a comma; count columns outside quotes */
    int inq = 0;
    for (char *p = row; *p && *p != '\n'; p++) {
        if (*p == '"') inq = !inq;
        else if (*p == ',' && !inq) row_cols++;
    }
    CHECK(row_cols == header_cols);
    free(csv);

    mem_open(&m);
    write_text_snapshot(m.f, &d, &h);
    char *text = mem_close(&m);
    CHECK(strstr(text, "host=host1   kernel=6.1.0") != NULL);
    CHECK(strstr(text, "CPU OVERVIEW  (2 cores - Test CPU)") != NULL);
    CHECK(strstr(text, "TOP PROCESSES") != NULL);
    CHECK(strstr(text, "coretemp Package id 0") != NULL);
    CHECK(strstr(text, "PRESSURE (PSI)") != NULL);
    CHECK(strstr(text, "cpu0     12.5%   2100 MHz") != NULL);
    time_t t;
    CHECK(text_header_time(strchr(text, '\n') + 1, &t) == 0 && t == d.ts);
    free(text);
}

static void test_journal(void)
{
    static Derived d;
    HostInfo h = { "host1", "6.1.0", "Test CPU" };
    fill_frame(&d);

    unlink(JOURNAL_SOCKET);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", JOURNAL_SOCKET);
    CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);

    journal_emit(&d, &h);

    static char buf[65536];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    close(fd);
    unlink(JOURNAL_SOCKET);
    CHECK(n > 0);
    if (n <= 0) return;
    buf[n] = '\0';
    CHECK(strncmp(buf, "MESSAGE=cpu 50.0% (user 40.0% ", 30) == 0);
    CHECK(strstr(buf, "\nPRIORITY=6\n") != NULL);
    CHECK(strstr(buf, "\nSYSLOG_IDENTIFIER=cpumon\n") != NULL);
    CHECK(strstr(buf, "\nCPUMON_TOTAL=50.00\n") != NULL);
    CHECK(strstr(buf, "\nCPUMON_TOP_PID=42\n") != NULL);
    char *j = strstr(buf, "\nCPUMON_JSON=");
    CHECK(j != NULL && strstr(j, "\"host\":\"host1\"") != NULL);
}

static void test_gzip_and_replay(void)
{
    char dir[] = "/tmp/cpumon-test-logs-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/2026-01-02.log", dir);
    FILE *f = fopen(path, "w");
    fputs("======\n2026-01-02 08:00:00 UTC   host=a\n======\nMORNING\n\n"
          "!!! ALERT 2026-01-02 09:00:00: busy\n\n"
          "======\n2026-01-02 20:00:00 UTC   host=a\n======\nEVENING\n\n", f);
    fclose(f);
    CHECK(gzip_file(path) == 0);
    CHECK(access(path, F_OK) != 0);
    snprintf(path, sizeof(path), "%s/2026-01-02.log.gz", dir);
    CHECK(access(path, F_OK) == 0);

    char err[200];
    time_t since, until;
    parse_when("2026-01-02 07:00", 0, 0, &since, err, sizeof(err));
    parse_when("2026-01-02 12:00", 0, 1, &until, err, sizeof(err));
    ReplayState *r = calloc(1, sizeof(*r));
    r->since = since;
    r->until = until;

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    char out[] = "/tmp/cpumon-test-replay-XXXXXX";
    int ofd = mkstemp(out);
    dup2(ofd, STDOUT_FILENO);
    int printed = replay_file(r, path, FMT_TEXT);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    char got[1024] = "";
    lseek(ofd, 0, SEEK_SET);
    ssize_t n = read(ofd, got, sizeof(got) - 1);
    if (n > 0) got[n] = '\0';
    close(ofd);
    unlink(out);
    CHECK(printed == 1);
    CHECK(strstr(got, "MORNING") != NULL);
    CHECK(strstr(got, "!!! ALERT 2026-01-02 09:00:00") != NULL);
    CHECK(strstr(got, "EVENING") == NULL);
    CHECK(strstr(got, "2026-01-02 20:00:00") == NULL);
    free(r);
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    setenv("TZ", "UTC", 1);
    tzset();
    test_parse_interval();
    test_format_interval_duration();
    test_parse_when();
    test_derive_core();
    test_derive_matches_cores_by_id();
    test_parse_cpu_line();
    test_psi_parse();
    test_cpuinfo_mhz();
    test_proc_parse_stat();
    test_top_compute();
    test_log_filenames();
    test_log_age_days();
    test_config();
    test_config_file();
    test_cli();
    test_alerts();
    test_history_sparkline();
    test_writers();
    test_journal();
    test_gzip_and_replay();

    if (g_failures) {
        fprintf(stderr, "%d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("all %d checks passed\n", g_checks);
    return 0;
}
