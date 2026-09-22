# cpumon

A Linux CPU usage monitor for the terminal — sibling project to
[`memmon`](https://github.com/shabbiryusufali/memmon-apt) (memory usage), built
the same way. It reads `/proc/stat` (aggregate and per-core
user/nice/system/idle/iowait/irq/softirq/steal breakdown), `/proc/loadavg`,
`/proc/uptime`, `/proc/cpuinfo`, CPU pressure (`/proc/pressure/cpu`),
per-core frequency (cpufreq), CPU temperature (hwmon / thermal zones) and
per-process CPU time (`/proc/[pid]/stat`), and shows it either:

- as a **scrollable, man-page-styled interactive display** (ncurses) that
  auto-refreshes on a "watch"-style interval, with usage-history sparklines
  and a top-processes list, or
- as a **headless logger** (`--daemon`) that appends a snapshot to a daily log
  file — as text, JSON Lines or CSV — and/or sends it to the systemd journal
  on the same interval, meant to run under systemd the same way as `memmon`.
  It can raise **alerts** when CPU usage stays high, and
  `cpumon --replay` prints logged history back out.

CPU usage is a rate, not a snapshot, so every measurement is the delta
between two `/proc/stat` reads: a 200ms bootstrap sample pair produces the
first frame of any mode, and every refresh after that is the delta between
consecutive interval ticks.

Log files older than a configurable retention window (**30 days by
default**) are deleted automatically, and older files can be gzip-compressed
— see [Log retention and compression](#log-retention-and-compression).

## Project layout

```
VERSION               The version. Bump it (and debian/changelog + the man page)
                       in one step with scripts/bump-version.sh
src/cpumon.c        Source (single-file C program)
man/cpumon.1         Man page
tests/                Unit tests (make test) and end-to-end smoke tests (make smoke)
.github/workflows/    CI: gcc/clang -Werror builds, tests, sanitizers, .deb build
systemd/cpumon.service   systemd unit (installed by both `make install` and the .deb)
config/cpumon.conf   Default daemon config, installed to /etc/cpumon/cpumon.conf
Makefile             Plain `make` / `make install` build, for a non-packaged install
debian/               Debian packaging (dpkg-buildpackage / debhelper)
scripts/build-deb.sh  Builds the .deb and publishes it into pool/ + Packages(.gz)
pool/, Packages, Packages.gz   The flat apt repository served from this repo
```

## Install via apt (recommended)

This repo doubles as a flat apt repository (`pool/`, `Packages`, `Packages.gz`
at the root). Once it's hosted somewhere apt can reach over HTTP(S) — see
[PACKAGING.md](PACKAGING.md) for hosting options — point apt at it and
install/upgrade normally:

```bash
echo "deb [trusted=yes] https://<wherever-you-host-this-repo>/ ./" | \
    sudo tee /etc/apt/sources.list.d/cpumon.list
sudo apt update
sudo apt install cpumon
```

Later releases just need:

```bash
sudo apt update
sudo apt upgrade cpumon
```

Installing the package:
- puts the binary at `/usr/bin/cpumon`
- installs `/etc/cpumon/cpumon.conf` (edit this to change the interval, log
  directory/format/target, retention, compression or alerts — see
  [Configuration](#configuration))
- installs and **enables + starts** `cpumon.service` automatically, no manual
  `systemctl enable` step needed
- installs the man page and docs

See [PACKAGING.md](PACKAGING.md) for how the repo is built/published, or to
install a single `.deb` directly without adding a repo.

## Build from source

Requires a C compiler plus ncurses and zlib development headers.

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libncurses-dev zlib1g-dev pkg-config

make
make check            # unit tests + end-to-end smoke tests (optional)
```

This produces a single `cpumon` binary at the repo root. `make` auto-detects
ncurses and zlib via `pkg-config`; if that's unavailable it falls back to
linking `-lncursesw` / `-lz`.

```bash
sudo make install     # installs to /usr/local/bin, plus the man page,
                       # systemd unit, and /etc/cpumon/cpumon.conf
sudo make uninstall   # stops/disables the service and removes all of it
                       # (log files are left in place)
```

`make install` points the systemd unit at wherever the binary was installed
(`PREFIX`/`BINDIR`), so the service works for a source install too. An
existing `/etc/cpumon/cpumon.conf` is never overwritten.

## Usage

```
cpumon                         Interactive TUI, refresh every 5 seconds (default)
cpumon -i 5m                   Interactive TUI, refresh every 5 minutes
cpumon --once                  One-shot plain-text report, no loop, no ncurses
cpumon --once -f json          One-shot report as JSON (or -f csv)
cpumon --plain                 Interactive watch, plain ANSI redraw (no ncurses)
cpumon --daemon -i 5m          Headless logger (for systemd), 5 minute interval
cpumon --daemon -a 90 -A 10m   ...and alert when usage stays >= 90% for 10 minutes
cpumon --replay --since 2h     Print the last two hours of logged snapshots
```

Interval syntax: a number with an optional suffix — `s` seconds (default if
omitted), `m` minutes, `h` hours, `d` days. `5`, `5s`, `5m`, `1h`, `2d` are all
valid.

Full option list:

| Flag | Config key | Meaning |
|---|---|---|
| `-d, --daemon` | | Headless mode: no TUI, just logs on each interval. Handles SIGTERM/SIGINT cleanly; SIGHUP reloads the config file. For systemd. |
| `-o, --once` | | Print a single snapshot to stdout and exit (no log file written) |
| `-p, --plain` | | Interactive watch without ncurses — plain ANSI clear/redraw. Use this if the default display looks garbled or doesn't redraw in place on your terminal |
| `-R, --replay` | | Print logged snapshots (plain or `.gz`) from the log dir and exit |
| `-s, --since WHEN` / `-u, --until WHEN` | | Limit `--replay`. `WHEN` is `YYYY-MM-DD[ HH:MM[:SS]]`, `today`, `yesterday`, `now`, or an interval ago (`30m`, `2h`, `7d`). A bare date for `--until` means the end of that day |
| `-i, --interval INTERVAL` | `CPUMON_INTERVAL` | Refresh/log interval (default `5s`) |
| `-f, --format FMT` | `CPUMON_FORMAT` | `text`, `json` or `csv`, for `--once` output and log files (default `text`) |
| `-t, --top N` | `CPUMON_TOP` | Show the `N` busiest processes; `0` disables process scanning (default `5`) |
| `-a, --alert PCT` | `CPUMON_ALERT_THRESHOLD` | Alert when total CPU usage stays at or above `PCT`%; `0` disables (default) |
| `-A, --alert-duration DUR` | `CPUMON_ALERT_DURATION` | How long usage must stay high before alerting (default `0`: first high sample) |
| `-l, --log-dir DIR` | `CPUMON_LOG_DIR` | Where daily log files go (default `/var/log/cpumon`, falling back to `~/.local/share/cpumon` if that's not writable) |
| `-r, --log-retention-days N` | `CPUMON_LOG_RETENTION_DAYS` | Auto-delete log files older than `N` days. `0` disables. Default `30` |
| `-z, --compress-after-days N` | `CPUMON_COMPRESS_AFTER_DAYS` | gzip log files at least `N` days old. `0` disables (the default; the shipped config sets `1`) |
| `-T, --log-target TARGET` | `CPUMON_LOG_TARGET` | `file`, `journal` or `both` (default `file`) |
| `-n, --no-log` | `CPUMON_NO_LOG` | Disable logging while in interactive mode |
| `-c, --config FILE` | | Read settings from `FILE` instead of the default (see [Configuration](#configuration)) |
| `-h, --help` | | Usage help |
| `-v, --version` | | Version |

### Interactive keys

| Key | Action |
|---|---|
| `q` | Quit |
| `↑`/`↓` or `j`/`k` | Scroll one line |
| `PgUp`/`PgDn` | Scroll one page |
| `g` / `G` | Jump to top / bottom |
| `r` | Refresh immediately (resets the countdown) |

The interactive screen is a scrollable pad, like `man` or `less` — the
content (overview, a usage-history sparkline, frequency, temperature, load
average, CPU pressure, processes & context switches, the top processes,
uptime, aggregate time breakdown, and a per-core usage bar, clock speed and
history sparkline for every core) is usually taller than one screen on
multi-core machines, so scroll down to see everything. The header/footer
bars stay pinned, and an alert banner appears at the top while an alert is
active. Sparklines use Unicode block characters in a UTF-8 locale and fall
back to ASCII otherwise.

## Configuration

Settings are applied in layers, each overriding the last:

1. built-in defaults
2. a config file of `KEY=VALUE` lines (the keys in the table above;
   `#` comments and quoted values are fine)
3. `CPUMON_*` environment variables with the same names
4. command-line options

The config file read depends on the mode: `--daemon` reads
`/etc/cpumon/cpumon.conf`, the interactive modes and `--once` read
`~/.config/cpumon/cpumon.conf` (or `$XDG_CONFIG_HOME/cpumon/cpumon.conf`),
and `--replay` reads both so it finds the daemon's log directory. `--config
FILE` picks a specific file instead. Missing default files are fine;
invalid lines are reported and skipped.

A personal config for the interactive display might look like:

```bash
# ~/.config/cpumon/cpumon.conf
CPUMON_INTERVAL=2
CPUMON_TOP=10
CPUMON_NO_LOG=yes
CPUMON_ALERT_THRESHOLD=95
```

## Logging

Every logged snapshot is appended to a daily file in the log directory. The
format is chosen with `--format` / `CPUMON_FORMAT`:

| Format | File | Contents |
|---|---|---|
| `text` (default) | `YYYY-MM-DD.log` | Section-based plain text, shown below |
| `json` | `YYYY-MM-DD.jsonl` | One JSON object per line: every metric, per-core usage and frequency, temperatures, PSI, top processes, alert state |
| `csv` | `YYYY-MM-DD.csv` | One row per snapshot with the aggregate metrics, top process and one column per core; a header row starts the file |

A `text` snapshot looks like this (sections for frequency, temperature,
pressure and top processes appear when that data is available):

```
======================================================================
2026-09-18 04:47:54 UTC   host=myhost   kernel=6.8.0-generic
======================================================================
CPU OVERVIEW  (4 cores - Intel(R) Xeon(R) Processor @ 2.80GHz)
  [----------------------------------------]   0.2%
  User:             0.2%        System:           0.0%
  Idle:             99.8%       IOWait:           0.0%

LOAD AVERAGE
  1m: 0.07   5m: 0.05   15m: 0.01

PROCESSES & CONTEXT SWITCHES
  Running:          1           Blocked:          0
  Ctxt switches:    237.0/s     Forks:            0.00/s

UPTIME
  6m 15s

PER-CORE USAGE
  cpu0:             0.0%        cpu1:             0.0%
  cpu2:             0.0%        cpu3:             0.0%

AGGREGATE TIME BREAKDOWN
  Nice:             0.0%        IRQ:              0.0%
  SoftIRQ:          0.0%        Steal:            0.0%
```

The log file name rolls over automatically at local midnight — no restart
needed, whether running interactively or as the systemd daemon.

### The systemd journal

With `--log-target journal` (or `both`), each snapshot is also sent to the
systemd journal: a one-line summary as the message, plus structured fields
such as `CPUMON_TOTAL`, `CPUMON_IOWAIT`, `CPUMON_LOAD1`, `CPUMON_TEMP_MAX_C`,
`CPUMON_TOP_COMM` and the full snapshot as `CPUMON_JSON`:

```bash
journalctl -u cpumon -f
journalctl -u cpumon -o json | jq -r '.CPUMON_TOTAL'
```

cpumon talks to the journal socket directly, so there's no libsystemd
dependency. If no journal is available it prints the summaries to stderr.

### Alerts

`--alert PCT` (`CPUMON_ALERT_THRESHOLD`) raises an alert once total CPU
usage has stayed at or above `PCT`% for `--alert-duration`
(`CPUMON_ALERT_DURATION`), and clears it once usage drops back below. Both
transitions go to syslog (and so the journal) as warning/notice messages,
and to the log file (`!!! ALERT ...` / `!!! RECOVERED ...` lines in text
logs, `"event":"alert"` objects in JSON logs, the `alert` column in CSV).
The interactive displays show an alert banner. Messages name the busiest
process:

```
CPU usage 97.3% has been >= 90% for 10m 4s (top: ffmpeg[4121] 385.2%)
```

### Replaying logs

`cpumon --replay` prints logged snapshots back out, reading compressed and
uncompressed files in every format:

```bash
cpumon --replay --since today
cpumon --replay --since "2026-09-18 09:00" --until "2026-09-18 17:00"
cpumon --replay --since 7d | grep '^!!! ALERT'
```

### Log retention and compression

Log files are named `YYYY-MM-DD.<ext>`, one per calendar day. Whenever the
log rolls over to a new day (and when the daemon starts or reloads), cpumon:

- deletes any of its own log files older than `--log-retention-days`
  (`CPUMON_LOG_RETENTION_DAYS`, default `30`; `0` keeps logs forever), and
- gzip-compresses files at least `--compress-after-days` old
  (`CPUMON_COMPRESS_AFTER_DAYS`; `0` disables, the shipped config uses `1`)
  to `YYYY-MM-DD.<ext>.gz`.

Only files matching cpumon's own `YYYY-MM-DD.{log,jsonl,csv}[.gz]` pattern
are ever touched.

## Running as a systemd service (5 minute interval, 30 day retention)

```bash
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now cpumon.service
```

Check it's running and look at recent snapshots:

```bash
systemctl status cpumon.service
tail -f /var/log/cpumon/$(date +%Y-%m-%d).log
```

Stop/disable:

```bash
sudo systemctl disable --now cpumon.service
```

To change any setting, edit `/etc/cpumon/cpumon.conf` (installed by `make
install` and the package) then apply it without a restart:

```bash
sudo systemctl reload cpumon.service
```

The reload sends SIGHUP, which makes cpumon re-read the file and reopen its
log file (so it also plays nicely with external log rotation).

## If the display looks garbled

A few terminal front-ends (some Windows/WSL setups in particular, or any
non-fully-interactive tty) don't fully support the cursor-addressing ncurses
relies on for in-place redraws — you might see mis-rendered symbols or each
refresh printing new lines instead of overwriting the old ones. If that
happens, run:

```bash
cpumon --plain
```

This drives the same watch loop with plain ANSI clear-screen/redraw codes
instead of full ncurses cursor addressing, which works correctly on a much
wider range of terminals at the cost of a little visual polish (no
scrolling — it always shows the current snapshot in full).

## Notes

- CPU usage is always a delta between two `/proc/stat` samples, never a
  single instantaneous reading — the first frame of any mode (including
  `--once`) comes from a 200ms bootstrap sample pair.
- CPU bars are colored green under 60%, yellow 60–85%, red above 85%, same
  thresholds as `memmon`'s memory/swap bars.
- "Steal" time (time stolen by the hypervisor on virtualized hosts) and
  "IOWait" are broken out separately from idle so a busy-but-not-CPU-bound
  host is easy to spot.
- Per-core bars degrade gracefully as core counts scale — the interactive
  pad simply grows taller and scrolls. Cores are matched by id between
  samples, so CPU hotplug doesn't misattribute usage.
- Process CPU is shown like `top(1)`: percent of one CPU, so a process using
  four cores fully shows 400%.
- CPU pressure (PSI) `some` is the share of time at least one runnable task
  was waiting for a CPU — a more direct saturation signal than load average.
  It needs kernel 4.20+ with PSI enabled and is omitted otherwise.
- Frequency comes from cpufreq (`scaling_cur_freq`), falling back to
  `/proc/cpuinfo`. Temperatures come from CPU hwmon drivers (coretemp,
  k10temp, zenpower, ...) or CPU thermal zones; both are omitted when the
  machine doesn't expose them (common in VMs).

## Development

```bash
make test     # unit tests (tests/test_cpumon.c)
make smoke    # end-to-end checks against this machine's /proc (tests/smoke.sh)
make check    # both
```

CI (`.github/workflows/ci.yml`) builds with gcc and clang with `-Werror`,
runs both test suites, reruns the unit tests under ASan/UBSan, and builds,
installs and runs the Debian package.

## License

Do whatever you want with it — see [LICENSE](LICENSE).
