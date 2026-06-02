# Upstream PR plan for feature/moonraker-api

Inventory of work on `feature/moonraker-api` that's *generically useful
beyond dev-allpatches* and could be offered upstream to
`prusa3d/Prusa-Firmware-Buddy`. The user has 11 existing PRs already
live (#5228-30, #5302-9); this is what's outstanding.

Current tip: `fa8e660f7` (tagged as `moonraker-api-snapshot-2026-06-02`
in this repo).

## PR A — Audit toolkit (small, recommended)

**Title:** `feat(wui): runtime memory + task + lwIP diagnostics RPC`

**Scope:** Six tightly-coupled commits that add a single new WS RPC
(`printer.system.diagnostics`) plus a `heap_max_ever_used()` peak
watermark in the sbrk implementation. Useful for any Buddy developer
debugging RAM pressure, stack high-water marks, or lwIP pool sizing.

**Commits to cherry-pick (newest first):**

| SHA | Subject |
|---|---|
| `b9f50e879` | feat(audit): expose lwIP memp pool stats in diagnostics RPC |
| `0d270c11b` | feat(audit): heap_max_ever_used + full task list in diagnostics RPC |
| `c6eff0de0` | feat(wui): Moonraker WS — printer.system.diagnostics RPC |
| `80e74b6f7` | fix(wui): Moonraker WS — bump gcode-response ring entry 96 → 256 bytes |
| `27f013c7a` | doc(wui): record GCODE_LOG_LINES static-RAM ceiling on STM32F427 |

**Body draft:**

```
Adds a printer.system.diagnostics WS RPC returning:
- heap.{total, in_use, free, peak}   — bytes; peak is sbrk high-water
- task_count + tasks[]               — name + stack high-water mark
- lwip[]                             — when LWIP_STATS=1, per-pool stats

Plus heap_max_ever_used() in src/common/heap.cpp instrumenting _sbrk_r.
The lwIP block is gated #if LWIP_STATS && MEMP_STATS so it's a true
no-op when those build flags are off (the upstream default).

Used to size the GCODE_LOG_LINES ring and to confirm BUFF_SIZE=2*MSS
is sufficient under print load. Heap peak watermark surfaced an
empirical static-add ceiling of ~10 KB on the current firmware that
matches the boot allocation pattern.

Result format:
  {
    "heap": {"total":35396,"free":20984,"in_use":14412,"peak":24384},
    "task_count": 15,
    "tasks": [{"n":"defaultTask","hwm":628}, ...],
    "lwip": [{"n":"TCP_PCB","used":1,"max":2,"avail":12,"err":0}, ...]
  }
```

**Why upstream might like it:** small, well-isolated, opt-in via
existing build flag (`LWIP_STATS`). No behavior change for stock
builds.

**Why upstream might pushback:** adds a new RPC method to the
Moonraker shim path which Prusa hasn't upstreamed. Could be reframed
as a Prusa-Link `/api/v1/diagnostics` endpoint if upstream prefers.

**Status:** READY to PR. Recommended.

---

## PR B — Klipper-command compatibility shim (large, deferred)

**Title:** `feat(wui): Moonraker HTTP + WebSocket compatibility shim`

**Scope:** The big squashed Moonraker shim plus its incremental
improvements (Klipper command translation, HTTP stubs, additional
macros). ~5,500 lines added.

**Commits to cherry-pick:**

| SHA | Subject |
|---|---|
| `fa8e660f7` | SOUND_BEEP + PAUSE_AT_HEIGHT trigger |
| `afe6e232f` | filament + per-axis home + lights macros |
| `fdb6cea84` | HTTP — stub job_queue / history / files-roots / announcements |
| `065011002` | Klipper-command shim — also wire HTTP /printer/gcode/script |
| `4a2687fb7` | Moonraker HTTP + WebSocket shim (the big squash) |

**Status:** NOT READY to PR. Reasoning:

1. Upstream Prusa has not signaled interest in hosting a third API
   surface (PrusaLink + Connect + Moonraker). Maintenance burden falls
   on Prusa.
2. The user is no longer running Buddy as their daily firmware (now on
   Klipper). Lower personal motivation to shepherd a large PR through
   review.
3. Most Core One+ users wanting Klipper-style UI will go to actual
   Klipper anyway.

If a Core One+ user community emerges around dev-allpatches and asks
for upstream, revisit. Otherwise leave as fork-only.

---

## PR C — USBSerial linebuf bump

**Status:** ALREADY UPSTREAM as #5307. The same commit lives on
`pr/usbserial-bigger-linebuf` and was extracted there. Don't duplicate.

---

## How to actually open PR A

Once you decide to push:

```bash
cd /home/mrnice/git/Prusa-Firmware-Buddy
git switch -c pr/diagnostics-rpc upstream/master
git cherry-pick 27f013c7a 80e74b6f7 c6eff0de0 0d270c11b b9f50e879
# resolve any conflicts with current upstream
git push -u origin pr/diagnostics-rpc
gh pr create --repo prusa3d/Prusa-Firmware-Buddy \
    --base master --head packerlschupfer:pr/diagnostics-rpc \
    --title "feat(wui): runtime memory + task + lwIP diagnostics RPC" \
    --body-file docs/upstream_pr_plan.md   # or write a proper body
```

The cherry-picks may need rebasing onto a current upstream — `feature/moonraker-api`'s
base is on the fork lineage (post-`3fc7b43a3`), not pristine
`upstream/master`. Resolve any conflicts as you go.
