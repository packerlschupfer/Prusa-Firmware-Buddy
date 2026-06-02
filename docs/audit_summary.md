# Firmware audit — 10-step summary

Done 2026-06-02 on `dev-allpatches` HEAD. The four `docs/*.md` files
in this directory capture the data; this doc is the executive summary.

## Findings

### 1. RAM is tight; the live ceiling is the unified heap

- Total RAM: 196 KB. `.bss + .data` = **213 KB** (`.bss` includes 8K
  ETH DMA, all task stacks, all named statics).
- The growable heap lives **between the top of `.bss` and the ISR-stack
  reserve at the top of RAM**. Currently **34.6 KB total, 14.1 KB used
  at idle, 20.5 KB free**.
- FreeRTOS `pvPortMalloc` is mapped to newlib `malloc` (`src/common/heap.cpp`)
  — there is **no separate FreeRTOS heap**. `configTOTAL_HEAP_SIZE = 40 KB`
  is ignored.
- **Empirical static-add ceiling: ~13 KB** with Connect disabled,
  was ~4 KB with Connect enabled. Beyond that the firmware BSODs at
  boot ("internal error out of heap") because peak boot allocations
  reach ~13–14 KB.

### 2. CCMRAM is 99% full; lwIP pools live there

`memp_memory_*` (pbuf, TCP_PCB, NETBUF, etc.) total 63 KB of the
64 KB CCMRAM region. Headroom for raising any `MEMP_NUM_*` in
`include/buddy/lwipopts.h` is near zero unless we cut another pool.

### 3. Biggest static consumers

`(anon)::server` (nhttp Server, 14.9 KB, mostly `BUFF_SIZE × BUFF_CNT`
HTTP send buffers) leads. Other big ones: `rx_allocator_buffer` (11 KB),
`media_prefetch` (9 KB), `serial_log` snapshot buf (8 KB), `move_segment_queue`
(6.5 KB). Our own `g_gcode_log` (16 × 256 = 4 KB) is in the second tier.

The single cheapest cut for headroom is **`BUFF_CNT 6 → 4` in nhttp's
`server.h`**, freeing 4 KB without any feature loss for typical usage
(printer rarely sees more than 3 simultaneous HTTP connections).

### 4. Connect was the easy 10-KB win

Disabling `BUDDY_ENABLE_CONNECT` (commit `929706674` on dev-allpatches)
reclaimed **9.4 KB RAM and 99 KB flash** — the connectTask stack, the
protocol code, the TLS state.

### 5. Task stack slack — partial data

Only 3 of 8+ task stacks appear in `uxTaskGetSystemState` (probably an
osThreadNew vs xTaskCreate visibility issue, follow-up). What we did see:

| Task | Allocated | Peak used | Could shrink to |
|---|---:|---:|---:|
| `defaultTask` | 4,640 B | 2,128 B (46%) | 3,072 B (saves 1.5 KB) |
| `tcpip_thread` | 1,248 B | 476 B (38%) | 800 B (saves 450 B) |
| `IDLE` | 512 B | 100 B (80%) | leave |

Full task-list inspection is a follow-up.

### 6. Flash is at 1.87 MB / 2 MB → 11% free

After Connect disable, ~225 KB headroom. Translations cut would free
~50-100 KB more if we ever needed it. Not pressed for flash now.

### 7. Step 10 — diff against stock

(In progress; will be appended to `docs/ram_map.md` once the stock
worktree build at `3fc7b43a3` finishes.)

## Headroom reclaim plan (cheapest first)

| # | Cut | Saved | Risk |
|---|---|---:|---|
| 1 | `BUDDY_ENABLE_CONNECT=NO` (already done) | 9.4 KB RAM, 99 KB flash | none (you don't use Connect) |
| 2 | `BUFF_CNT 6 → 4` in `nhttp/server.h` | 4 KB RAM | minimal (only matters under HTTP load) |
| 3 | `defaultTask` stack 4640 → 3072 | 1.5 KB RAM | low — but monitor for new stack overflow |
| 4 | `snapshot_serial_log` buf 8K → 4K | 4 KB RAM | `/api/v1/log` returns less per call |
| 5 | `HAS_MMU2=NO` (if you don't use MMU) | unknown — could be 5-10 KB | breaks MMU3 support |
| 6 | `HAS_TRANSLATIONS=NO` | 0 KB RAM, ~50-100 KB flash | LCD English-only |
| 7 | Full task-stack audit + targeted shrinks | 1-3 KB RAM | requires step 4-7 follow-up |

Total reachable RAM reclaim with #1–#4: **~19 KB**. That would allow
e.g. doubling `GCODE_LOG_LINES` to 64 (full M115 banner captures) AND
adding new features later.

## Follow-ups (not done in this pass)

- Find why most osThreadNew-created tasks don't appear in `uxTaskGetSystemState`,
  fix or use `vTaskList()` for full visibility.
- Instrument `_sbrk_r` to track peak heap usage during boot + during a
  full print. Tells us the true safety margin.
- Stock-baseline diff (step 10) — pending background build.
- Enable `MEMP_STATS=1` in `lwipopts.h`, expose lwIP pool watermarks.
  Confirm `MEMP_NUM_TCP_PCB=12` is right-sized for real traffic.
