# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

This is a fork of [kanoi/cgminer](https://github.com/kanoi/cgminer) (version 4.13.6) maintained by bitshopper.de. The primary focus of this fork is the GekkoScience ASIC miner driver (`driver-gekko.c` / `driver-gekko.h`). All other drivers are present in the tree but largely unchanged from upstream.

## Build Commands

### Dependencies (Ubuntu/Debian)
```
sudo apt-get install build-essential autoconf automake libtool \
    pkg-config libcurl4-openssl-dev libudev-dev \
    libusb-1.0-0-dev libncurses5-dev zlib1g-dev git
```

### Build from git
```
./autogen.sh
CFLAGS="-O2 -Wall -march=native -fcommon" ./configure --enable-gekko
make
```

The `--enable-gekko` configure flag is the key one for this repo. Without it the GekkoScience driver is compiled out.

### macOS (Homebrew)
See `mac-build.txt` for the full Brew-based procedure. On older Macs without proper multitasking, add `--mac-yield` at runtime.

### Running cgminer with a GekkoScience device
```
./cgminer --gekko-compacf-detect -o stratum+tcp://pool:port -u user -p pass
```

Use the device-specific `--detect` flag matching your hardware (see Device Matrix below).

## Architecture

### Driver Registration Flow
All hardware drivers implement the `struct device_drv` interface defined in `miner.h`. The GekkoScience driver registers as `gekko_drv` at the bottom of `driver-gekko.c:7632`. cgminer's core (`cgminer.c`) calls into drivers via this vtable. Key vtable entries:
- `drv_detect` → `compac_detect` scans USB for known device IDs
- `thread_init` → `compac_init` sets up per-device threads
- `scanwork` → `compac_scanwork` is the main work dispatch loop

### GekkoScience Driver Internals (`driver-gekko.c`)

Each miner runs **four threads** per device stored in `COMPAC_INFO`:
- `thr` — the main scanwork/dispatch thread
- `rthr` (`compac_listen`) — USB receive thread; routes to `compac_listen2` (BM1397/BM1362/BM1370/BM1384/BM1387) or `compac_listengsk` (BFCLAR)
- `wthr` (`compac_mine2`) — long-term frequency tuning loop with plateau detection
- `tthr` (`compac_telemetry`) — polls the MCU telemetry interface on devices that have one (GSA1/GSA2)
- `nthr` (`compac_gsfak_nonce_que`) — nonce dispatch queue for GSF/GSA/GSK devices

Per-device state lives entirely in `struct COMPAC_INFO` (defined in `driver-gekko.h`).

### Device Identity → ASIC Mapping

| USB identity (ident) | Product string | ASIC | CLI detect flag |
|---|---|---|---|
| `IDENT_BSC/GSC` | Compac BM1384 | BM1384 | `--gekko-compac-detect` |
| `IDENT_BSD/GSD` | 2Pac BM1384 | BM1384 | `--gekko-2pac-detect` |
| `IDENT_BSE/GSE` | Terminus BM1384 | BM1384 | `--gekko-terminus-detect` |
| `IDENT_GSH` | NewPac | BM1387 (2 chips) | `--gekko-newpac-detect` |
| `IDENT_GSI` | R606 | BM1387 (12 chips) | `--gekko-r606-detect` |
| `IDENT_GSF` | CompacF | BM1397 | `--gekko-compacf-detect` |
| `IDENT_GSFM` | R909 | BM1397 | `--gekko-r909-detect` |
| `IDENT_GSA1` | Compac A1 / Terminus A1 | BM1362 | `--gekko-compaca1-detect` |
| `IDENT_GSA2` | Compac A2 / Terminus A2 | BM1370 | `--gekko-compaca2-detect` |
| `IDENT_GSK` | KBox (MCP2210 SPI) | BFCLAR | `--gekko-kbox-detect` |

`BSC/BSD/BSE` are bitshopper.de OEM variants of the GekkoScience BM1384 devices; they share identical code paths with their GSC/GSD/GSE counterparts (`iManufacturer = "bitshopperde"` vs `"GekkoScience"`).

### Frequency Tuning

`compac_mine2` implements the tuning algorithm. It uses two efficiency metrics:
- `eff_gs` — short-term ratio of observed hash rate to expected hash rate
- `wu` — work units (accepted nonces / time), compared against `wu_max`

Frequency is stepped up when `eff_gs > info->tune_up` and down when below `info->tune_down`. The `--gekko-tune-up` and `--gekko-tune-down` options set these thresholds. Plateau detection (`enum plateau_type`) catches stuck miners.

### Ticket / Difficulty Scaling (BM1397)

BM1397 devices use a hardware difficulty filter ("ticket mask"). `set_ticket()` selects the ticket from `ticket_1397[]` based on current hashrate, reducing USB nonce traffic. After `TICKET_DELAY` work items the driver validates that nonces are being produced at the expected rate.

### Telemetry (GSA1 / GSA2)

GSA1/GSA2 devices have a second USB interface (`info->telemetry`) connected to an MCU that reports voltage, current, temperatures, and fan speed. The telemetry protocol has three versions controlled by `info->telem_version`:
- V1 (`TELEM_IS_V1`) — basic Vin/Iin/Temp
- V2 (`TELEM_IS_V2`) — adds Vout/Iout/Temp2
- V3 (`TELEM_IS_V3`) — adds a capability mask (`telem_mask`) so the driver can query which fields are available

`enable_gsa1_telem()` initialises the MCU and sets core voltage (`telem_corev`). `get_gsa1_telem()` polls it. Cooldown logic (`info->cooldown`) reduces frequency and cuts the regulator if temperature exceeds threshold.

### Midstate Boosting

When `!opt_gekko_noboost` and the pool sends a `vmask`, BM1387 and BM1397 devices send work with 2/4 midstates (`info->midstates`), multiplying effective throughput without increasing USB bandwidth proportionally. The `--gekko-noboost` and `--gekko-lowboost` flags disable or limit this.

### apibridge Companion Process

`apibridge` is a second, separate binary (opt-in via `--enable-apibridge` / `--api-bridge`) that exposes cgminer's RPC API (`api.c`) as a modern HTTP/WebSocket JSON API for a future mobile dashboard. It is fork/exec'd and supervised by cgminer (`cgminer-apibridge.c`: crash/respawn with backoff, clean shutdown) so a bug in the web-facing code can never affect the mining threads, and vice versa. It only talks to cgminer over the existing local RPC socket — no shared address space, no direct access to `COMPAC_INFO`. Currently Phase 1 (read-only monitoring) only; see `APIBRIDGE-README` for the full protocol, endpoints, and the Phase 2/3 roadmap (control endpoints, TLS, discovery).

### Key Source Files

- `driver-gekko.c` — entire GekkoScience driver (~7600 lines); all device-specific logic
- `driver-gekko.h` — `COMPAC_INFO`, `ASIC_INFO`, telemetry macros, frequency macros
- `usbutils.c` — USB device table (lines ~1039–1224 contain the Gekko entries); USB I/O primitives
- `usbgekdev.h` — placeholder for extra dev-only USB device strings (empty in production)
- `cgminer.c` — CLI option definitions for all `--gekko-*` flags (around line 1966)
- `miner.h` — `opt_gekko_*` extern declarations; `device_drv` vtable definition
- `cgminer-apibridge.c` / `.h` — fork/exec + supervision glue for the apibridge companion process (see above)
- `apibridge/` — the apibridge binary's own sources (HTTP/WebSocket server, cgminer RPC client, poll cache); see `APIBRIDGE-README`

## Git Workflow

This repo is a fork of kanoi/cgminer (upstream). Remotes:
- `origin`   = my fork on GitHub
- `upstream` = https://github.com/kanoi/cgminer.git

### Branch model
- `master`: untouched mirror of `upstream/master`. NEVER commit to it directly. Only update it via fast-forward from upstream.
- `edition-macentyre`: my long-lived main branch with all custom changes. Releases and CI build from this branch.
- `feature/*`: short-lived branches, created off `edition-macentyre`.

### Sync master with upstream
```
git checkout master
git fetch upstream
git merge --ff-only upstream/master
git push origin master
```
If `--ff-only` fails, a commit accidentally landed on master — report this instead of creating a merge commit.

### Pull upstream updates into edition-macentyre
First sync master (see above), then:
```
git checkout edition-macentyre
git merge master      # Merge, NOT rebase (edition-macentyre is pushed/shared)
```

### Developing a feature
```
git checkout -b feature/xyz edition-macentyre
# ... work ...
git checkout edition-macentyre
git merge feature/xyz
```
Feature branches may be cleaned up via rebase onto `edition-macentyre` before merging, as long as they haven't been shared yet.

### Releases (independent of upstream)
Use your own tag scheme that won't collide with kanoi's tags, e.g. `v4.12.1-mac.1`:
```
git checkout edition-macentyre
git tag -a v4.12.1-mac.1 -m "Release description"
git push origin v4.12.1-mac.1
```

### Minimizing conflicts
- Keep changes small and locally scoped.
- Sync frequently (e.g. weekly); don't let branches diverge for long.
- Prefer putting custom functionality in NEW files instead of rewriting existing upstream files.
- Contribute generally useful fixes back to kanoi/cgminer as a pull request so they land in upstream.

**Never** suggest `git push --force` on `master` or `edition-macentyre`. Never commit directly to `master`.
