# Packaging cpumon as a .deb

There are two different things people mean by "install via apt", and both
are covered below:

1. **Install a `.deb` file directly with apt** — works today, no server
   needed, apt still resolves and installs dependencies for you.
2. **Set up a real apt repository** — lets you (and any other machine) do
   `apt update && apt install cpumon` after adding one line to
   `/etc/apt/sources.list.d/`, and get upgrades the normal way.

This is a real Debian source package (`debian/` uses debhelper, compat 13),
built with the standard `dpkg-buildpackage` tooling — not a hand-rolled
`DEBIAN/control` tree. A pre-built package lives in `pool/main/`, with the
matching `Packages`/`Packages.gz`/`Release` index already generated at the
repo root.

**Before publishing a new build**, check `debian/control`'s `Maintainer:`
field is accurate for your setup.

## 1. Install the .deb directly (simplest)

```bash
sudo apt install ./pool/main/cpumon_1.1-1_amd64.deb
```

Using `apt install ./file.deb` (not `dpkg -i`) is what you want: apt reads
the package's `Depends:` line and pulls in `libncursesw6`/`libtinfo6`/`zlib1g` from
your normal repos automatically if they're missing, whereas plain `dpkg -i`
will fail on missing dependencies and leave you to fix it with
`apt-get install -f`.

This gets you:
- `/usr/bin/cpumon`
- `/etc/cpumon/cpumon.conf` — interval, log directory/format/target,
  retention, compression, top processes and alerts, read by the systemd
  service (edit + `systemctl reload cpumon` to apply; preserved across
  upgrades since it's a conffile)
- `/usr/lib/systemd/system/cpumon.service` — **enabled and started
  automatically** on install (via `dh_installsystemd`'s postinst), no manual
  `systemctl enable --now` needed
- `man cpumon`
- `/usr/share/doc/cpumon/README.md`, `PACKAGING.md`

Remove it the normal way: `sudo apt remove cpumon` (or `purge` to also drop
the systemd unit's enabled/disabled state and stop the service).

## 2. Building / re-versioning the package

Requires `debhelper`, `dpkg-dev`, a C compiler, and ncurses dev headers:

```bash
sudo apt-get install build-essential debhelper libncursesw5-dev zlib1g-dev pkg-config dpkg-dev
```

(On newer Debian/Ubuntu releases where `libncursesw5-dev` no longer exists,
`libncurses-dev` provides the same wide-char headers.)

### Bumping the version

The version lives in three places that all have to agree: the top-level
`VERSION` file (compiled into `cpumon --version`), `debian/changelog`
(drives the package's version), and the `.TH` line in `man/cpumon.1`.
Bump all three in one step:

```bash
./scripts/bump-version.sh 1.1 "Describe what changed."
```

This writes `1.1` to `VERSION`, prepends a `debian/changelog` entry for
`1.1-1` (edit it afterwards if the one-line summary isn't enough), and
updates the man page's version/date. Then build:

```bash
./scripts/build-deb.sh
```

This runs `dpkg-buildpackage -us -uc -b`, copies the resulting
`.deb` into `pool/main/`, and regenerates `Packages`/`Packages.gz`/`Release`
at the repo root so the flat repository (see below) is immediately up to
date. The build runs the unit tests (`make test`) first; set
`DEB_BUILD_OPTIONS=nocheck` to skip them.

Prefer to drive the tools yourself?

```bash
dpkg-buildpackage -us -uc -b     # builds ../cpumon_<version>_amd64.deb
```

## 3. Hosting a real apt repository

This is what lets `apt update && apt install cpumon` work without anyone
downloading a file by hand. This repo already is a **flat repository**: a
directory of `.deb` files (`pool/`) plus an index (`Packages`, `Packages.gz`,
`Release`), served over plain HTTP(S) — exactly what `scripts/build-deb.sh`
regenerates on every build. `Release` (unsigned) is required — without it
apt refuses the repo with "does not have a Release file" — and is what gets
signed below for anything beyond personal use.

Serve the checked-out repo directory (or the corresponding branch, e.g. via
GitHub Pages, an S3 bucket, or any static file host) over HTTPS, then on a
machine that should install from it:

```bash
echo "deb [trusted=yes] https://your-host/cpumon/ ./" | \
    sudo tee /etc/apt/sources.list.d/cpumon.list
sudo apt update
sudo apt install cpumon
```

`[trusted=yes]` skips GPG signature checking, which is fine for personal/
internal use but apt will nag about it and, more importantly, an
unsigned repo means anyone who can tamper with that URL or DNS can push
you a malicious package. For anything beyond personal use, sign the
repo instead of using `trusted=yes`:

```bash
gpg --full-generate-key                 # if you don't have a key yet

# Sign the Release file already published at the repo root by
# scripts/build-deb.sh.
gpg --default-key YOUR_KEY_ID -abs -o Release.gpg Release   # detached sig
# or, for the modern inline form apt also accepts:
gpg --default-key YOUR_KEY_ID --clearsign -o InRelease Release
```

Once signed, drop `[trusted=yes]` and instead have users import your public
key (`sudo apt-key add your-key.pub` on older apt, or the modern
`signed-by=/usr/share/keyrings/...gpg` form in the sources.list entry).

For a larger/public-facing repo, `reprepro` or `aptly` manage multiple
package versions, distributions (stable/testing), and signing for you
rather than the manual `dpkg-scanpackages` flow above — worth it once
you're publishing updates regularly rather than a one-off package.

## Log retention and configuration

The daemon prunes its own daily log files (`YYYY-MM-DD.log`) once they're
older than a configurable retention window (30 days by default, `0` disables
it). This is controlled by `/etc/cpumon/cpumon.conf`, which the package
installs as a conffile — it survives `apt upgrade` and any local edits are
preserved (dpkg will prompt if a future package version changes the
shipped default and you've also modified it locally).

## Project layout

| Path | Purpose |
|---|---|
| `VERSION` | The version. Bump it (and everything derived from it) via `scripts/bump-version.sh` |
| `src/cpumon.c` | Source |
| `man/cpumon.1` | Man page source (gzipped into the package automatically) |
| `systemd/cpumon.service` | systemd unit (symlinked from `debian/cpumon.service` for the packaging build) |
| `config/cpumon.conf` | Default config installed to `/etc/cpumon/cpumon.conf` |
| `Makefile` | Plain `make` / `make install`, for a non-packaged install |
| `debian/` | Debhelper packaging: `control`, `rules`, `changelog`, `copyright`, etc. |
| `scripts/build-deb.sh` | Builds the .deb and publishes it into `pool/main/` + regenerates `Packages`/`Packages.gz`/`Release` |
| `pool/`, `Packages`, `Packages.gz`, `Release` | The flat apt repository served from this repo |
