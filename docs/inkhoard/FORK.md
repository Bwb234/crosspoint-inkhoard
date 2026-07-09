# InkHoard CrossPoint Fork Policy

Maintained fork of [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
for the native InkHoard client on the Xteink X4.

**Fork:** https://github.com/Bwb234/crosspoint-inkhoard  
**Upstream commit tracked by `inkhoard/main`:** `a43061305a8e9082cd8421d881c6c4adea37c1d3` (`develop`)  
**Pristine `master` mirrors:** `upstream/master` (`2754a5ff01644d36cf0a17db98f28408666ba518`)  
**Recorded:** 2026-07-09 (plan 005)

## Branch Model

| Branch | Role |
| --- | --- |
| `master` | Pristine upstream `master` mirror. Never commit fork-local work here. |
| `develop` | GitHub default branch inherited from the fork; treat as upstream tracking, not the integration line. |
| `inkhoard/main` | Integration branch for all InkHoard suite plans. Based on upstream `develop` HEAD at the recorded SHA above. |
| `inkhoard/<plan>-<slug>` | Feature branches (e.g. `inkhoard/007-credentials`). Merge into `inkhoard/main` only. |

Remotes:

- `origin` → `Bwb234/crosspoint-inkhoard`
- `upstream` → `crosspoint-reader/crosspoint-reader`

## Rebase Policy

- Rebase `inkhoard/main` onto upstream `develop` at most monthly, or when upstream ships a release, whichever is later.
- After every rebase: CI must be green; once hardware exists, also run plan 006's smoke tests.
- Prefer rebase over merge commits so the fork-local history stays a short stack on top of upstream.
- Record the new upstream SHA in this file's header when the integration tip moves.

## Isolation Rule

InkHoard code lives in its own directory (plans 007–013 create
`src/activities/inkhoard/` and/or `lib/InkHoardClient/` — reconcile against the
tree during those plans' reconnaissance steps).

Edits to shared CrossPoint files must be:

1. Minimal
2. Commented with `// INKHOARD:`
3. Listed in the shared-file touches table below so rebase conflicts are predictable

Generic improvements (bearer-header support, streaming hooks) should be offered
upstream instead of accumulating in the fork.

### Shared-file touches

| File | Why touched | Plan |
| --- | --- | --- |
| *(none yet — plan 005 adds no source changes)* | | |

## Version Scheme

`<upstream-version>+inkhoard.<n>`

Example: upstream `1.4.1` → first InkHoard release `1.4.1+inkhoard.1`.
Plan 014 implements packaging and OTA identity markers.

## Build Baseline (plan 005)

Corrections vs the InkHoard plan's assumptions (written from docs, not a tree):

| Assumption in plan 005 | Reality upstream |
| --- | --- |
| Default branch `master` | Default branch is **`develop`**; `master` is the release/stable line |
| Env name like `x4` | Single board config; build env is **`default`** (`FREEINK_DEVICE_X4=1` in shared flags) |
| `pio test -e native` | **No** native PlatformIO env; host tests are CMake/GTest via `pio run -t unit-tests` |
| Optional submodule | **`freeink-sdk` is required** (`git clone --recursive`) |
| Stock PlatformIO | Upstream CI uses **pioarduino** PlatformIO Core `v6.1.19` |

### Toolchain

| Item | Value |
| --- | --- |
| PlatformIO Core | pioarduino `6.1.19` |
| Platform | `pioarduino/platform-espressif32` `55.03.37` |
| Board | `esp32-c3-devkitm-1` |
| Build env | `default` |
| Host tests | `pio run -t unit-tests` or CMake under `test/` |
| App partition (`app0`/`app1`) | `0x640000` (6,553,600 bytes / ~6.25 MiB) — `partitions.csv` |
| Flash offset | `0x10000` |

### Reproduce

```bash
git clone --recursive https://github.com/Bwb234/crosspoint-inkhoard.git
cd crosspoint-inkhoard
git checkout inkhoard/main
pip install -U https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip
pio run -e default
pio run -t unit-tests
```

### Baseline firmware size

From green CI on `inkhoard/main` (run
[29057987363](https://github.com/Bwb234/crosspoint-inkhoard/actions/runs/29057987363),
2026-07-09), `pio run -e default`:

| Metric | Value |
| --- | --- |
| `firmware.bin` size (bytes) | 5248320 |
| Flash usage (linker) | 5234477 / 6553600 (~79.9%) |
| RAM usage (linker) | 101348 / 327680 (~30.9%) |
| App partition limit | 6553600 |
| Size check | CI fails if `firmware.bin` exceeds the app partition limit |

Local Windows toolchain install hit a disk-space STOP on first attempt
(`WinError 112` while unpacking `toolchain-riscv32-esp`). Host unit tests
still pass locally via CMake (`85/85`). Clean-clone firmware proof is CI.

## License

Upstream is MIT (`LICENSE`). A public fork with modifications is permitted;
preserve copyright and license notices.
