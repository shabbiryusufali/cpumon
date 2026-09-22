#!/bin/sh
# smoke.sh - end-to-end checks of a built cpumon binary against the live
# /proc of the machine running it. Used by `make smoke` and CI.
#
# Usage: sh tests/smoke.sh [path-to-cpumon]
set -eu

BIN="${1:-./cpumon}"
case "$BIN" in /*) ;; *) BIN="$(pwd)/$BIN" ;; esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# Keep any real user/system config out of the picture.
export HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/home/.config"
mkdir -p "$HOME"
unset CPUMON_INTERVAL CPUMON_LOG_DIR CPUMON_LOG_RETENTION_DAYS CPUMON_FORMAT \
      CPUMON_LOG_TARGET CPUMON_TOP CPUMON_ALERT_THRESHOLD CPUMON_ALERT_DURATION \
      CPUMON_COMPRESS_AFTER_DAYS CPUMON_NO_LOG 2>/dev/null || true

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

has_json() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import json,sys
for l in sys.stdin:
    l=l.strip()
    if l: json.loads(l)'
    else
        cat >/dev/null
    fi
}

# Keeps one CPU busy for $1 seconds in the background, so tests that
# need non-zero usage don't depend on how idle the machine happens to be.
busy_loop() {
    sh -c 'end=$(($(date +%s)+'"$1"')); while [ "$(date +%s)" -lt "$end" ]; do :; done' &
    busy=$!
}

# Runs the daemon for roughly $1 seconds with the remaining arguments.
run_daemon() {
    secs="$1"; shift
    "$BIN" --daemon --config /dev/null "$@" 2>>"$TMP/daemon.err" &
    pid=$!
    sleep "$secs"
    kill -TERM "$pid"
    wait "$pid" || fail "daemon exited non-zero ($*)"
}

"$BIN" --version | grep -q '^cpumon ' || fail "--version"
"$BIN" --help | grep -q -- '--replay' || fail "--help"
pass "version/help"

if "$BIN" --bogus >/dev/null 2>&1; then fail "unknown option accepted"; fi
if "$BIN" --once -f xml >/dev/null 2>&1; then fail "bad --format accepted"; fi
if "$BIN" --once --since 2h >/dev/null 2>&1; then fail "--since without --replay accepted"; fi
pass "argument validation"

"$BIN" --once > "$TMP/once.txt"
grep -q "CPU OVERVIEW" "$TMP/once.txt" || fail "--once text"
grep -q "PER-CORE" "$TMP/once.txt" || fail "--once per-core"
"$BIN" --once --format json | has_json || fail "--once json not valid JSON"
"$BIN" --once --format=json | grep -q '"per_core":\[' || fail "--once json per_core"
"$BIN" --once -f csv > "$TMP/once.csv"
head -1 "$TMP/once.csv" | grep -q '^ts,time,host,total,' || fail "--once csv header"
[ "$(wc -l < "$TMP/once.csv")" -eq 2 ] || fail "--once csv rows"
pass "--once text/json/csv"

# A busy process should show up in the top list.
busy_loop 3
sleep 0.3
"$BIN" --once --top 3 | grep -q "TOP PROCESSES" || fail "top processes missing under load"
wait "$busy" || true
pass "top processes"

# Daemon in each format.
for fmt in text json csv; do
    d="$TMP/logs-$fmt"
    mkdir -p "$d"
    run_daemon 2.5 -i 1 -l "$d" -f "$fmt"
    case "$fmt" in
        text) f="$d/$(date +%Y-%m-%d).log"
              [ "$(grep -c '^CPU OVERVIEW' "$f")" -ge 2 ] || fail "daemon text snapshots" ;;
        json) f="$d/$(date +%Y-%m-%d).jsonl"
              [ "$(wc -l < "$f")" -ge 2 ] || fail "daemon json snapshots"
              has_json < "$f" || fail "daemon json invalid" ;;
        csv)  f="$d/$(date +%Y-%m-%d).csv"
              [ "$(grep -c '^ts,' "$f")" -eq 1 ] || fail "csv header count"
              [ "$(wc -l < "$f")" -ge 3 ] || fail "daemon csv rows" ;;
    esac
done
# Restarting onto an existing CSV file must not repeat the header.
run_daemon 1.5 -i 1 -l "$TMP/logs-csv" -f csv
[ "$(grep -c '^ts,' "$TMP/logs-csv/$(date +%Y-%m-%d).csv")" -eq 1 ] || fail "csv header repeated on reopen"
pass "daemon text/json/csv logging"

# Alerts: one busy CPU puts total usage at >= 100/ncores percent (above the
# 0.5% threshold on machines with up to 200 cores), and with no hold time
# the alert fires on the first frame. An idle machine can read exactly 0%.
d="$TMP/logs-alert"; mkdir -p "$d"
busy_loop 3
sleep 0.3
run_daemon 1.5 -i 1 -l "$d" -a 0.5
wait "$busy" || true
grep -q '^!!! ALERT' "$d/$(date +%Y-%m-%d).log" || fail "alert not logged"
pass "alerts"

# SIGHUP reload: switch format text -> json via the config file.
d="$TMP/logs-hup"; mkdir -p "$d"
printf 'CPUMON_INTERVAL=1\nCPUMON_LOG_DIR=%s\nCPUMON_FORMAT=text\n' "$d" > "$TMP/hup.conf"
"$BIN" --daemon --config "$TMP/hup.conf" 2>>"$TMP/daemon.err" &
pid=$!
sleep 1.5
printf 'CPUMON_INTERVAL=1\nCPUMON_LOG_DIR=%s\nCPUMON_FORMAT=json\n' "$d" > "$TMP/hup.conf"
kill -HUP "$pid"
sleep 2.5
kill -TERM "$pid"
wait "$pid" || fail "daemon exit after HUP"
[ -s "$d/$(date +%Y-%m-%d).log" ] || fail "pre-reload text log missing"
[ -s "$d/$(date +%Y-%m-%d).jsonl" ] || fail "post-reload json log missing"
grep -q "reloaded configuration" "$TMP/daemon.err" || fail "reload not announced"
pass "SIGHUP reload"

# Retention + compression of old files, and replay across them.
d="$TMP/logs-maint"; mkdir -p "$d"
printf '======\n2000-01-01 12:00:00 UTC   host=x\n======\nOLD\n' > "$d/2000-01-01.log"
printf '======\n2020-01-02 12:00:00 UTC   host=x\n======\nCOMPRESS ME\n\n' > "$d/2020-01-02.log"
printf 'not a cpumon log\n' > "$d/notes.txt"
run_daemon 1.5 -i 1 -l "$d" -r 100000 -z 1
[ -e "$d/2020-01-02.log.gz" ] || fail "old log not compressed"
[ ! -e "$d/2020-01-02.log" ] || fail "compressed source not removed"
[ -e "$d/notes.txt" ] || fail "unrelated file touched"
run_daemon 1.5 -i 1 -l "$d" -r 5000   # 2000-01-01 expires, 2020-01-02 stays
[ ! -e "$d/2000-01-01.log" ] || fail "expired log not deleted"
[ -e "$d/notes.txt" ] || fail "unrelated file deleted"
[ -e "$d/2020-01-02.log.gz" ] || fail "log inside retention deleted"
pass "retention + compression"

"$BIN" --replay -l "$d" --since 2020-01-02 --until 2020-01-02 > "$TMP/replay.txt"
grep -q "COMPRESS ME" "$TMP/replay.txt" || fail "replay of .gz file"
if grep -q "CPU OVERVIEW" "$TMP/replay.txt"; then fail "replay ignored --until"; fi
"$BIN" --replay -l "$TMP/logs-json" --since today | has_json || fail "replay json"
"$BIN" --replay -l "$TMP/logs-csv" --since 1h | head -1 | grep -q '^ts,' || fail "replay csv header"
if "$BIN" --replay -l "$d" --since 1999-01-01 --until 1999-12-31 >/dev/null 2>&1; then
    fail "empty replay should exit non-zero"
fi
pass "replay"

# Journal target falls back to stderr when there's no journald socket,
# and must never crash either way.
run_daemon 1.5 -i 1 -T journal
pass "journal target"

echo "all smoke tests passed"
