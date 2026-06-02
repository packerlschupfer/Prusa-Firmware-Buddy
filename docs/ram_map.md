# Core One+ firmware RAM map — dev-allpatches baseline

Captured 2026-06-02 on commit `929706674` (`dev-allpatches`, Prusa Connect
disabled). Build preset: `coreone_release_noboot`. This is the reference
point for all RAM-budget analysis going forward. Re-run the commands at
the bottom of this doc to refresh against a new build.

## Region layout

| Region | Origin | Size | Notes |
|---|---|---|---|
| `FLASH` | `0x08000000` | 2 MB | Code + read-only data. Used: ~1.87 MB. |
| `DXRAM` | `0x20000000` | 100 B | Reserved (vector table redirect, etc.). |
| `RAM` | `0x20000064` | ~192 KB | Main SRAM. Hosts `.data`, `.bss`, FreeRTOS `ucHeap`, ETH DMA, task stacks, growable newlib heap. |
| `CCMRAM` | `0x10000000` | 64 KB | Core-Coupled Memory. Hosts lwIP `memp_memory_*` pools. Essentially full. |

`_estack = 0x20030000` (top of main RAM); `_Min_Heap_Size` and
`_Min_Stack_Size` are 2 KB each as linker assertions.

## Totals (current)

| Section | Bytes | KB |
|---|---|---|
| `.text` (flash) | 1,872,292 | 1828.4 |
| `.data` (RAM, initialized globals) | 16 | 0.0 |
| `.bss`  (RAM, zero-init globals + ucHeap + DMA + thread stacks) | 213,360 | 208.4 |

After `.bss` ends, the remainder of RAM up to `_estack - _Min_Stack_Size`
is the growable newlib heap. Total RAM available for new static data
before runtime heap allocations fail: **~13 KB.**

## Top RAM consumers (>= 1 KB)

| Bytes | Symbol |
|---:|---|
| 14,864 | `(anon)::server` — nhttp `Server` (HTTP send-buffer pool: `BUFF_SIZE × BUFF_CNT = 2048 × 6 = 12 KB`) |
| 10,980 | `(anon)::rx_allocator_buffer` — network RX allocator |
| 9,040 | `marlin_server::media_prefetch` — gcode prefetch ring |
| 8,193 | `nhttp::link_content::snapshot_serial_log_into_static::buf` — `/api/v1/log` snapshot |
| 8,192 | `nhttp::printer::(anon)::ring` — second nhttp ring (filelist/events) |
| 6,664 | `PreciseStepping::move_segment_queue` |
| 6,224 | `phase_stepping::axis_states` |
| 6,144 | `nhttp::printer::(anon)::g_mesh_buf` — UBL mesh JSON staging |
| 6,144 | `displayTask_buffer` — LCD task stack |
| 6,096 | `Tx_Buff` — ETH MAC TX DMA |
| 6,096 | `Rx_Buff` — ETH MAC RX DMA |
| 5,760 | `ili9488_buff` — display framebuffer |
| 5,742 | `nhttp::printer::(anon)::g_ws_frame_pool` — 3 × ~1916-byte WS frame slots |
| 4,640 | `os_thread_buffer_defaultTask` — main FreeRTOS task stack |
| 4,456 | `default_instance.lto_priv.0` (LTO-bundled; need step 2 to identify) |
| 4,192 | `ScreenFactory::storage` |
| 4,112 | `nhttp::printer::(anon)::g_gcode_log` — **OUR 16 × 256 WS gcode-response ring** |
| 4,100 | `PreciseStepping::step_event_queue` |
| 4,096 | `dma_buffer_rx` |
| 4,096 | `os_thread_buffer_network` |
| 3,620 | `loadcell` |
| 3,584 | `os_thread_buffer_puppies` |
| 2,560 | `os_thread_buffer_usb_device_task` |
| 2,480 | `os_thread_buffer_measurementTask` |
| 2,259 | `memp_memory_TCP_PCB_base` (CCMRAM) |

The `*.lto_priv.0` suffix on some symbols is just LTO's name decoration —
a no-LTO rebuild was done 2026-06-02 (`build-nolto-analyze/`) and
confirmed the same top consumers, only with the suffix stripped. LTO
isn't hiding anything significant in this codebase.

## Top 50 static consumers (full list — `.bss + .data ≥ 512 B`)

The top 50 own 171 KB of the 213 KB of .bss / .data. See raw table in
`/tmp/top50.txt` after running the regenerate command at the bottom of
this doc, or inspect by running:

```bash
arm-none-eabi-nm --size-sort -S --radix=d build/coreone_release_noboot/firmware \
  | awk '$3 ~ /^[bdBD]$/ && $2+0 >= 512' | sort -rn | head -50
```

**Tags** for the top 20 (handwritten judgment):

| # | Bytes | Symbol | Tag |
|---:|---:|---|---|
| 1 | 14,864 | `(anon)::server` (nhttp Server: 12 KB HTTP send buf pool) | shrinkable: BUFF_CNT 6→4 saves 4 KB |
| 2 | 10,980 | `rx_allocator_buffer` | load-bearing (network RX) |
| 3 | 9,040 | `marlin_server::media_prefetch` | load-bearing (smooth printing) |
| 4 | 8,193 | `snapshot_serial_log_into_static::buf` | shrinkable to 4K (lose /api/v1/log resolution) |
| 5 | 8,192 | `nhttp::printer::ring` (other ring) | investigate — what feeds this? |
| 6 | 6,664 | `move_segment_queue` (PreciseStepping) | load-bearing motion |
| 7 | 6,224 | `phase_stepping::axis_states` | load-bearing (HAS_PHASE_STEPPING) |
| 8 | 6,144 | `g_mesh_buf` (UBL JSON staging, ours) | load-bearing for bed_mesh fetch |
| 9 | 6,144 | `displayTask_buffer` | load-bearing (LCD task stack) |
| 10 | 6,096 | `Tx_Buff` (ETH MAC DMA) | load-bearing |
| 11 | 6,096 | `Rx_Buff` (ETH MAC DMA) | load-bearing |
| 12 | 5,760 | `ili9488_buff` (LCD framebuffer) | load-bearing |
| 13 | 5,742 | `g_ws_frame_pool` (3 × WS frames, ours) | already minimized |
| 14 | 4,640 | `defaultTask` stack | investigate watermark (step 4) |
| 15 | 4,456 | `default_instance` (LTO bundle — investigate) | unknown |
| 16 | 4,192 | `ScreenFactory::storage` | load-bearing GUI |
| 17 | 4,112 | **`g_gcode_log`** (our 16×256 WS ring) | the budget we're squeezing |
| 18 | 4,100 | `step_event_queue` | load-bearing motion |
| 19 | 4,096 | `dma_buffer_rx` | load-bearing |
| 20 | 4,096 | `network` task stack | investigate watermark (step 4) |

**Headline candidates to reclaim RAM** (in order of value):
- BUFF_CNT 6→4 → 4 KB (HTTP send buffer pool slack)
- defaultTask stack 4640→2048 if watermark allows → ~2.5 KB
- network task stack 4096→2048 if watermark allows → ~2 KB
- puppies task stack 3584→2048 if watermark allows → ~1.5 KB
- usb_device task stack 2560→1536 → ~1 KB
- measurement task stack 2480→1536 → ~1 KB
- FreeRTOS heap shrink (configTOTAL_HEAP_SIZE) if min-ever-free is high → potentially 5-10 KB
- snapshot_serial_log buffer 8K→4K → 4 KB (resolution loss)

Step 4 (task stack high-watermarks) will tell us which of these are real.

## Empirical static-add ceiling

Verified 2026-06-02 by bumping the gcode-response ring (with `GCODE_LOG_LINE_LEN = 256`):

| Ring entries | New static (vs 16 × 96 baseline) | Result |
|---|---|---|
| 16 × 256 (current) | +2.5 KB | works |
| 32 × 256 | +6.5 KB | BSOD "out of heap" on boot |
| 64 × 256 | +14.5 KB | network dead after boot |

After disabling Prusa Connect (commit `929706674`) the headroom grew
from ~3.8 KB to ~13 KB.

## How to regenerate the numbers

From the repo root:

```bash
arm-none-eabi-size build/coreone_release_noboot/firmware
arm-none-eabi-nm --size-sort -S --radix=d build/coreone_release_noboot/firmware \
  | awk '$3 ~ /^[bdBD]$/ && $2+0 > 1024 {print $2, $4}' \
  | sort -rn | head -25
grep -E "^\.(data|bss|ccmram)\s+0x" build/coreone_release_noboot/firmware.map
```
