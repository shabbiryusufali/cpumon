# cpumon

A Linux CPU usage monitor for the terminal — sibling project to
[`memmon`](https://github.com/shabbiryusufali/memmon-apt) (memory usage), built
the same way. It reads `/proc/stat` (aggregate and per-core
user/nice/system/idle/iowait/irq/softirq/steal breakdown), `/proc/loadavg`,
`/proc/uptime`, and `/proc/cpuinfo`, and shows it either:

- as a **scrollable, man-page-styled interactive display** (ncurses) that
  auto-refreshes on a "watch"-style interval, or
- as a **headless logger** (`--daemon`) that appends a formatted, structured
  snapshot to a daily log file on the same interval — meant to run under
  systemd, the same way as `memmon`.

CPU usage is a rate, not a snapshot, so every measurement is the delta
between two `/proc/stat` reads: a 200ms bootstrap sample pair produces the
first frame of any mode, and every refresh after that is the delta between
consecutive interval ticks.

Log files older than a configurable retention window (**30 days by
default**) are deleted automatically — see [Log retention](#log-retention).

## Project layout

```
VERSION               The version. Bump it (and debian/changelog + the man page)
                       in one step with scripts/bump-version.sh
src/cpumon.c        Source (single-file C program)
man/cpumon.1         Man page
systemd/cpumon.service   systemd unit (installed by both `make install` and the .deb)
config/cpumon.conf   Default runtime config, installed to /etc/cpumon/cpumon.conf
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
  directory, or log retention — see below)
- installs and **enables + starts** `cpumon.service` automatically, no manual
  `systemctl enable` step needed
- installs the man page and docs

See [PACKAGING.md](PACKAGING.md) for how the repo is built/published, or to
install a single `.deb` directly without adding a repo.

## Build from source

Requires a C compiler and ncurses development headers.

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libncursesw5-dev

make
```

This produces a single `cpumon` binary at the repo root. `make` auto-detects
ncurses via `pkg-config`; if that's unavailable it falls back to linking
`-lncursesw`.

```bash
sudo make install     # installs to /usr/local/bin, plus the man page,
                       # systemd unit, and /etc/cpumon/cpumon.conf
```

## Usage

```
cpumon                    Interactive TUI, refresh every 5 seconds (default)
cpumon -i 5m               Interactive TUI, refresh every 5 minutes
cpumon -i 1h               Interactive TUI, refresh every hour
cpumon --once               One-shot plain-text report, no loop, no ncurses
cpumon --plain               Interactive watch, plain ANSI redraw (no ncurses)
cpumon --daemon -i 5m       Headless logger (for systemd), 5 minute interval
```

Interval syntax: a number with an optional suffix — `s` seconds (default if
omitted), `m` minutes, `h` hours, `d` days. `5`, `5s`, `5m`, `1h`, `2d` are all
valid.

Full option list:

| Flag | Meaning |
|---|---|
| `-i, --interval INTERVAL` | Refresh/log interval (default `5s`) |
| `-d, --daemon` | Headless mode: no TUI, just logs on each interval, handles SIGTERM/SIGINT cleanly. For systemd. |
| `-o, --once` | Print a single snapshot to stdout and exit (no log file written) |
| `-p, --plain` | Interactive watch without ncurses — plain ANSI clear/redraw. Use this if the default display looks garbled or doesn't redraw in place on your terminal |
| `-l, --log-dir DIR` | Where daily log files go (default `/var/log/cpumon`, falling back to `~/.local/share/cpumon` if that's not writable) |
| `-r, --log-retention-days N` | Auto-delete log files older than `N` days. `0` disables. Default `30` (also settable via `CPUMON_LOG_RETENTION_DAYS`) |
| `-n, --no-log` | Disable logging while in interactive mode |
| `-h, --help` | Usage help |
| `-v, --version` | Version |

### Interactive keys

| Key | Action |
|---|---|
| `q` | Quit |
| `↑`/`↓` or `j`/`k` | Scroll one line |
| `PgUp`/`PgDn` | Scroll one page |
| `g` / `G` | Jump to top / bottom |
| `r` | Refresh immediately (resets the countdown) |

The interactive screen is a scrollable pad, like `man` or `less` — the
content (overview, load average, processes & context switches, uptime,
aggregate time breakdown, and a per-core usage bar for every core) is
usually taller than one screen on multi-core machines, so scroll down to
see everything. The header/footer bars stay pinned.

## Logging

Every logged snapshot is appended to `<log-dir>/YYYY-MM-DD.log` as a
structured, section-based plain text block:

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

### Log retention

Log files are named `YYYY-MM-DD.log`, one per calendar day. Whenever the log
rolls over to a new day, cpumon deletes any of its own log files older than
the configured retention window:

- `-r, --log-retention-days N` on the command line (default `30`)
- or the `CPUMON_LOG_RETENTION_DAYS` environment variable, which the systemd
  unit sets from `/etc/cpumon/cpumon.conf`

Set it to `0` to disable auto-deletion and keep logs forever. Only files
matching cpumon's own `YYYY-MM-DD.log` pattern are ever touched.

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

To change the interval, log directory, or log retention, edit
`/etc/cpumon/cpumon.conf` (installed by `make install`) then apply it:

```bash
sudo systemctl restart cpumon.service
```

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
  pad simply grows taller and scrolls.
