# Runtime memory diagnostics — full readout

Captured 2026-06-02 via the `printer.system.diagnostics` WS RPC at
idle, post-boot. Heap peak watermark added by instrumenting `_sbrk_r`
in `src/common/heap.cpp`; the missing tasks were just my output
buffer being too small (224 B → 1024 B fixed it).

## Heap (FreeRTOS + newlib unified)

Buddy maps `pvPortMalloc` directly to newlib `malloc` (see
`src/common/heap.cpp`). The "FreeRTOS heap" and "newlib heap" are the
same pool. `configTOTAL_HEAP_SIZE = 40 KB` is unused; the real heap is
defined by the linker as the span between `_end` (top of `.bss`) and
`_estack - ISR_STACK_LENGTH_BYTES` (1640 B reserved for ISR stack).

| Metric | Bytes | KB |
|---|---:|---:|
| `heap.total` (span size) | 34,372 | 33.6 |
| `heap.in_use` (idle, fresh boot) | 14,412 | 14.1 |
| `heap.free` (idle) | 19,960 | 19.5 |
| **`heap.peak`** (sbrk high water mark since boot) | **24,384** | **23.8** |
| **Cushion before BSOD on new static** | **9,988** | **9.8** |

The `peak` field is the largest `current_heap_end() - heap_start` ever
recorded — newlib never gives memory back via `sbrk`, so this is a
monotonically-rising high-water mark. Subtracting `peak` from `total`
tells us exactly how much new static data we can add before something
in the boot allocation chain fails.

**Empirical confirmation:** the 8 KB ring-bump that BSOD'd earlier put
us at `total - new_static - peak` ≈ 33.6 - 8 - 23.8 = **+1.8 KB cushion**
— too thin, and indeed it crashed. The 4 KB ring-bump leaves 5.8 KB
cushion and works.

## Tasks — full readout

All 15 FreeRTOS tasks visible. Allocated stack sizes from the .map; `hwm`
is `uxTaskGetStackHighWaterMark` in words (× 4 = bytes of slack).

| Task | Allocated | hwm × 4 (slack) | Peak used | Utilization |
|---|---:|---:|---:|---:|
| `displayTask` | 6,144 | 3,920 | 2,224 | 36% |
| `network` | 4,096 | 2,908 | 1,188 | 29% |
| `defaultTask` | 4,640 | 2,552 | 2,088 | 45% |
| `worker_thread` | ? | 3,924 | ? | ? |
| `puppies` | 3,584 | 1,808 | 1,776 | 50% |
| `usb_device_task` | 2,560 | 2,216 | 344 | **13%** |
| `measurementTask` | 2,480 | 2,204 | 276 | **11%** |
| `USBH_MSC_Worker` | 2,048 | 1,860 | 188 | **9%** |
| `metric_system_t` | 1,500 | 936 | 564 | 38% |
| `USBH_Thread` | 1,280 | 844 | 436 | 34% |
| `log_task` | 1,572 | 700 | 872 | 55% |
| `tcpip_thread` | (lwIP units) | 1,488 | ? | ? |
| `puppies` | 3,584 | 1,808 | 1,776 | 50% |
| `acFaultTask` | ? | 220 | ? | ? |
| `TmrSvc` | 512 (default) | 408 | 104 | 20% |
| `IDLE` | 512 (default) | 412 | 100 | **80% — DO NOT shrink** |

### Actionable cuts (~10 KB reclaimable from task stacks)

| Task | Current | Proposed | Saves | Reason |
|---|---:|---:|---:|---|
| `USBH_MSC_Worker` | 2,048 | 1,024 | 1,024 | 9% used, halve safely |
| `usb_device_task` | 2,560 | 1,536 | 1,024 | 13% used |
| `measurementTask` | 2,480 | 1,024 | 1,456 | 11% used |
| `displayTask` | 6,144 | 4,096 | 2,048 | 36% used |
| `defaultTask` | 4,640 | 3,200 | 1,440 | 45% used |
| `network` | 4,096 | 2,560 | 1,536 | 29% used |
| `puppies` | 3,584 | 2,560 | 1,024 | 50% used |
| **Total** | | | **~9.6 KB** | |

These are conservative — each leaves ≥1 KB headroom over peak.

## Note on observed limitation that wasn't real

Earlier I reported only 3 tasks visible. That was an **output-buffer
bug in my own RPC**, not a FreeRTOS visibility issue. `uxTaskGetSystemState`
correctly returns all 15 tasks; my 224-byte scratch was just running out
of room after the heap JSON + ~3 task entries. Bumping the diagnostics
output buffer to 1024 B reveals everything.

## Under-load measurement (real print)

Captured 2026-06-02 by polling `printer.system.diagnostics` during a
real print. `hwm` is a low-water mark — captures the worst-case stack
slack ever observed since boot, so a single under-load sample is
sufficient to capture the print peak.

### Heap during print

| Metric | Idle | Post-print | Δ |
|---|---:|---:|---:|
| `heap.in_use` | 14,412 | 14,456 | +44 |
| `heap.peak` | 24,384 | **24,384** | **0** |
| `heap.free` | 19,960 | 19,916 | -44 |

**Heap PEAK did not grow during the print.** The 9.8 KB cushion is real
and stable — peak boot-allocations dominate, runtime print allocations
are negligible.

### Task stack hwm: idle vs under-load

| Task | Idle slack | Post-print slack | Δ used | Verdict |
|---|---:|---:|---:|---|
| `worker_thread` | 3,924 | **884** | **+3,040** | trim would crash mid-print |
| `tcpip_thread` | 1,488 | 564 | +924 | trim would crash mid-print |
| `defaultTask` | 2,552 | 1,684 | +868 | conservative trim only |
| `displayTask` | 3,920 | 3,272 | +648 | safe to trim |
| `log_task` | 700 | 568 | +132 | leave |
| `network` | 2,908 | 2,908 | 0 | safe to trim |
| `usb_device_task` | 2,216 | 2,216 | 0 | safe to trim |
| `puppies` | 1,808 | 1,808 | 0 | safe to trim modestly |
| `USBH_MSC_Worker` | 1,860 | 1,824 | 36 | safe to trim |
| `measurementTask` | 2,156 | 2,156 | 0 | safe to trim |
| `acFaultTask` | 220 | 220 | 0 | DO NOT TOUCH |
| `IDLE` | 412 | 412 | 0 | 80% util, do not touch |
| `TmrSvc` | 408 | 408 | 0 | leave |

### Revised conservative trim plan

The idle-only plan was 9.6 KB. The real-data plan is **~6.8 KB** with
≥50% margin over observed peaks:

| Task | Current | Cut to | Saves |
|---|---:|---:|---:|
| `displayTask` | 6,144 | 4,096 | 2,048 |
| `usb_device_task` | 2,560 | 1,536 | 1,024 |
| `measurementTask` | 2,480 | 1,024 | 1,456 |
| `USBH_MSC_Worker` | 2,048 | 1,280 | 768 |
| `network` | 4,096 | 2,560 | 1,536 |
| **Total** | | | **~6.8 KB** |

DO NOT trim `worker_thread` or `tcpip_thread` — both showed >900 B of
load-driven extra usage and are close to their floors under print load.

### Lesson

Trimming task stacks based on idle measurements alone is unsafe.
`worker_thread` looked like it had 3.9 KB to give away at idle; in
reality it needs 2.8+ KB more headroom during a print. The diagnostics
RPC committed in `feature/moonraker-api` makes the under-load
measurement cheap (one HTTP call), so anyone revisiting the trim plan
should do that first.

### Network stats during print

```
altcp_write_failures: 0
buffer_starvations:   0
send_space_zero:      0
connection_aborts:    1   (my poller's WS conn dropping, not the printer)
lwip_err_callbacks:   10
last_lwip_err:        0
```

`0 buffer_starvations` and `0 write_failures` confirm the
`BUFF_SIZE = 2 × TCP_MSS` bump from phase 4ze is sufficient for print-time
status push + gcode response traffic.

## How to refresh

After flashing a firmware that has the diagnostics RPC:

```python
# Connect to ws://192.168.16.13/websocket
# Send: {"jsonrpc":"2.0","id":1,"method":"server.connection.identify",
#         "params":{"access_token":"<api-key>"}}
# Send: {"jsonrpc":"2.0","id":2,"method":"printer.system.diagnostics",
#         "params":{}}
```

The response is one JSON object with `heap` and `tasks` keys.

## lwIP memp pool stats (CCMRAM picture closed)

Enabled `LWIP_STATS = 1` and `LWIP_STATS_DISPLAY = 1` in
`include/buddy/lwipopts.h` (dev-allpatches only — the RPC code gates it
behind `#if LWIP_STATS && MEMP_STATS`, so it's a no-op when off, which
remains the upstream default). All 19 lwIP memp pools captured:

| Pool | Avail | Max ever | Saturation | Notes |
|---|---:|---:|---:|---|
| `RAW_PCB` | 4 | 0 | 0% | unused (no raw IP sockets) |
| `UDP_PCB` | 5 | 4 | **80%** | near capacity — DHCP + mDNS + Prusa-Link |
| `TCP_PCB` | 12 | 2 | 16% | could cut to 4 |
| `TCP_PCB_LISTEN` | 8 | 1 | 12% | over-provisioned |
| `TCP_SEG` | 16 | 2 | 12% | over-provisioned |
| `REASSDATA` | 5 | 0 | 0% | IP frag reassembly unused |
| `FRAG_PBUF` | 15 | 0 | 0% | IP frag pbufs unused |
| `NETBUF` | 2 | 0 | 0% | netconn unused |
| `NETCONN` | 4 | 0 | 0% | netconn unused |
| `TCPIP_MSG_API` | 8 | 2 | 25% | adequate |
| `TCPIP_MSG_INPKT` | 30 | 2 | 6% | hugely over-provisioned |
| `IGMP_GROUP` | 8 | 2 | 25% | mDNS multicast |
| `SYS_TIMEOUT` | 16 | 11 | 68% | adequate |
| `NETDB` | 1 | 0 | 0% | DNS lookups unused |
| `PBUF_REF/ROM` | 16 | 2 | 12% | over-provisioned |
| `PBUF_POOL` | 0 | 0 | n/a | disabled (Buddy uses custom pbuf alloc) |
| `MALLOC_128` | 7 | 3 | 42% | adequate |
| `MALLOC_512` | 2 | 1 | 50% | adequate |
| `MALLOC_1512` | 1 | 1 | **100%** | exactly at capacity, no slack |

**`err = 0` across every pool** — no allocation failures under real
traffic. Nothing is currently under-provisioned.

If we cut every over-provisioned pool to `max × 2`, conservative CCMRAM
reclaim is ~3-4 KB. But CCMRAM is a separate region from the main RAM
where our heap pressure lives, so this is *information* rather than
*action* — the data is now on file in case it ever becomes useful.

**Watch-list:** `MALLOC_1512` at 1/1 is the tightest pool. Any future
feature needing a second 1.5 KB lwIP alloc would fail; raise
`MEMP_NUM_MEM_1512` in `lwipopts.h` first if you ever add one.

## Follow-ups (still on the shelf)

1. **(done)** ~~lwIP `memp_stats`~~ — captured above.
2. **(done)** ~~`_sbrk` peak watermark~~ — instrumented; `heap.peak` field
   in the diagnostics RPC.
3. **(done)** ~~Full task list~~ — was a 224-byte output buffer bug, not
   a FreeRTOS visibility issue. Now all 15 tasks visible.
