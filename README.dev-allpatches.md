# dev-allpatches — Core One+ extras for Prusa-Firmware-Buddy

> **⚠️ ARCHIVED 2026-07-26 — no longer actively maintained.**
>
> I've moved my Core One+ to **[Klipper (coreone-firmware
> fork)](https://github.com/packerlschupfer/coreone-firmware)** as the
> daily-driver firmware and stopped further Buddy-side development.
> This branch was rebuilt clean on top of current `upstream/master`
> (11 upstream-open PRs cherry-picked + audit tooling + docs), tagged
> `dev-allpatches-archived-2026-07-26`, and left as-is.
>
> **What still works:**
> - The 11 open PRs against upstream Prusa (#5228, #5229, #5230,
>   #5302, #5303, #5304, #5305, #5307, #5308, #5309, #5400) each live
>   on their own `pr/*` branch and are reviewable independently.
> - The audit toolkit + `docs/` directory (RAM map, feature flags,
>   runtime diagnostics, PuppyBus protocol, chamber-fan control).
>
> **What is NOT maintained:**
> - Rebases against future upstream drift
> - Feature follow-ups (M0 message text, `gcode` variable exposure,
>   BBF install path for `_noboot` builds, etc.)
> - The Moonraker WS/HTTP shim (was in the previous tangled version;
>   was removed from this rebuild). Not coming back — see the Klipper
>   fork instead for a Moonraker-native experience.
>
> Anyone forking to continue can pick up from
> `dev-allpatches-archived-2026-07-26` and rebase or cherry-pick as
> needed.

---

This branch (`dev-allpatches` on `packerlschupfer/Prusa-Firmware-Buddy`)
was a personal-build cumulative branch carrying every Core One+
modification I made on top of the upstream Prusa Buddy firmware.
It is **not stock Prusa** and **not intended for upstreaming as a
single PR** — the individual patches it bundles either already have
open upstream PRs (#5228-30, #5302-5309, #5400) or are too opinionated for
upstream's taste.

## Who this branch is for

- **Core One+ owners** who want Fluidd / Mainsail / OrcaSlicer running
  against their printer **without** adding a Raspberry Pi or
  installing Klipper.
- People debugging firmware RAM/heap usage who want a worked example
  of the analysis (see `docs/`).

## What it adds on top of stock

| Area | Change |
|---|---|
| Web API | 17+ extra Prusa-Link `/api/v1/*` endpoints (control, log, settings, extended status, dialog state, BBF upload, ...) |
| Moonraker shim | Full HTTP + WebSocket API surface — Fluidd loads end-to-end, OrcaSlicer's embedded webview works, no Pi needed |
| Klipper-command translation | `SET_PIN PIN=chamber_led`, `SET_HEATER_TEMPERATURE`, `SET_VELOCITY_LIMIT`, `LOAD_FILAMENT`, `UNLOAD_FILAMENT`, `CHANGE_FILAMENT`, `HOME_X/Y/Z`, `LIGHTS_ON/OFF`, `BED_MESH_CALIBRATE`, `PAUSE_AT_HEIGHT Z=<mm>`, `SOUND_BEEP`, `MANUAL_PROBE`, `EMERGENCY_STOP`, `FIRMWARE_RESTART`, ... all translate to their Marlin equivalents |
| Firmware-audit toolkit | `printer.system.diagnostics` WS RPC returns heap stats (`total`/`free`/`in_use`/`peak`), per-task stack high-water marks, and (when `LWIP_STATS=1`) all 19 lwIP memp pool watermarks |
| nhttp infrastructure | `BUFF_SIZE = 2 × TCP_MSS`, `BUFF_CNT = 6`, CORS preflight, `OPTIONS` method, `/server/files/{roots,job_queue,history,announcements,webcams}` stubs |
| SWD flashing fixes | Unattended reset, backup-SRAM clearing, signature appending, M997 wireless flash, reliable `--reset` |
| Web UI polish | Filament controls, dialog state, chamber temp/LED/preheat, 3-column readout, speed buttons, BBF upload |
| Build defaults | `BUDDY_ENABLE_CONNECT = NO` for COREONE (Connect cloud disabled; saves 99 KB flash + 9.4 KB RAM) |

## Build

```bash
git clone --recurse-submodules https://github.com/packerlschupfer/Prusa-Firmware-Buddy.git
cd Prusa-Firmware-Buddy
git switch dev-allpatches
python3 utils/build.py --preset coreone --bootloader no --skip-bootstrap
# output: build/coreone_release_noboot/firmware.{bin,bbf}
```

## Flash

The firmware is built with `--bootloader no` → self-contained binary,
flashes to `0x08000000` (replaces the bootloader sector). **Keep the
backup at `/home/mrnice/git/backup/full_flash_stock.bin` safe.** See
[`feedback_flashing.md`](https://github.com/packerlschupfer/Prusa-Firmware-Buddy/issues)
or `doc/swd_flashing.md` for the SWD recipe.

If you want to keep the stock bootloader intact, build with
`--bootloader yes`, drop the resulting `.bbf` on a USB stick, and let
the printer's LCD flash it normally.

## Hardware compatibility

| Printer | Compatibility |
|---|---|
| Prusa Core One / Core One+ | ✅ Built and tested on this branch (`--preset coreone`) |
| Prusa XL | ⚠️ `--preset xl` should build (xbuddy_extension features are `#ifdef`-gated). Untested. |
| Prusa MK4 / MK3.5 | ⚠️ Same Buddy lineage; should build. Untested. |
| Prusa MK3 / MK3S | ❌ Different MCU (AVR), different repo (`prusa3d/Prusa-Firmware`). Not compatible. |

## Run

- Connect Fluidd to `http://<printer-ip>/` (the printer serves Moonraker
  directly via the shim; no Pi needed). API token = your printer's
  web-UI password.
- OrcaSlicer: add a Klipper-type printer pointing at
  `http://<printer-ip>`, same token.

## Documentation

Audit artifacts in `docs/`:

- [`audit_summary.md`](docs/audit_summary.md) — executive summary of the
  firmware audit (10-step plan)
- [`ram_map.md`](docs/ram_map.md) — RAM region layout, top static
  consumers, headroom math
- [`feature_flags.md`](docs/feature_flags.md) — every `BUDDY_ENABLE_*` /
  `HAS_*` flag for the COREONE preset, with shrinkability notes
- [`code_size.md`](docs/code_size.md) — `.text` breakdown by module
- [`runtime_diagnostics.md`](docs/runtime_diagnostics.md) — heap and
  per-task stack measurements at idle and under print load; lwIP pool
  watermarks

## Status

This branch is **parked, not actively developed**. The author has
moved their Core One+ to Klipper (`feature/klipper-port`). The
dev-allpatches firmware works and is useful for anyone who wants the
Buddy + Moonraker-shim story; it just won't be getting new features
from me.

If you find bugs or want to extend it, open an issue or send a PR
against this branch.

## Upstream PRs

The following changes from this branch have open upstream PRs against
`prusa3d/Prusa-Firmware-Buddy`:

- #5228 control endpoint
- #5229 extended `/api/v1/status` fields
- #5230 dialog state in `/api/v1/status`
- #5302 `GET /api/v1/settings`
- #5303 `GET /api/v1/log`
- #5304 null-terminate JSON parser string_views
- #5305 M500 explicit-noop
- #5306 M303 reject `C < 3`
- #5307 USBSerial per-line buffer 128 → 512 bytes
- #5308 JSONIFY_STR VLA bounded
- #5309 SWD flashing RAM-clear contract documentation

The Moonraker shim (Phases 3+4) and the BBF-upload endpoint are
intentionally *not* upstreamed — too controversial.
