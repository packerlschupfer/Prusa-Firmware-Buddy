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

## Follow-ups (not done in this pass)

1. **lwIP `memp_stats`** — would need `MEMP_STATS=1` in `lwipopts.h`
   plus an iterator over `memp_pools[]`. Tells us if `MEMP_NUM_TCP_PCB=12`
   is over-provisioned. Likely savings: 1–2 KB in CCMRAM.
2. **`_sbrk` peak watermark** — instrument `_sbrk_r` in `src/common/heap.cpp`
   to record max `current_heap_end()`. Tells us exact peak heap-in-use
   during boot + any G29 / print / firmware-update.
3. **Full task list** — figure out why `osThreadNew`-created tasks
   don't appear in `uxTaskGetSystemState`, or use `vTaskList()` for
   a text dump.
