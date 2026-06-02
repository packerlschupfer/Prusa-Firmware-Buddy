# Runtime memory diagnostics — first measurements

Captured 2026-06-02 via the new `printer.system.diagnostics` WS RPC,
on commit with the diagnostics RPC added (dev-allpatches HEAD + WIP).

## Heap (FreeRTOS + newlib unified)

Buddy maps `pvPortMalloc` directly to newlib `malloc` (see
`src/common/heap.cpp`). The "FreeRTOS heap" and "newlib heap" are the
same pool. `configTOTAL_HEAP_SIZE = 40 KB` is unused; the real heap is
defined by the linker as the span between `_end` (top of `.bss`) and
`_estack - ISR_STACK_LENGTH_BYTES` (1640 B reserved for ISR stack).

| Metric | Bytes | KB |
|---|---:|---:|
| `heap.total` (span size) | 35,396 | 34.6 |
| `heap.in_use` (idle, fresh boot) | 14,412 | 14.1 |
| `heap.free` (idle) | 20,984 | 20.5 |

**Idle steady-state usage is ~14 KB.** Peak-during-boot is what bites
us — anecdotally, 8 KB of new static (= 27 KB heap total left) BSODs
on boot, so something allocates 13–14 KB at startup. To get the
high-water mark precisely, we'd need to instrument `_sbrk_r` to track
max `current_heap_end()` during boot. Deferred — the current rough
ceiling (~4 KB headroom for new static) matches empirical observations.

## Tasks visible to `uxTaskGetSystemState`

| Task | Stack high-water mark (words) | Slack (bytes) | Allocated stack | Utilization |
|---|---:|---:|---:|---:|
| `defaultTask` | 628 | 2,512 | 4,640 B | 46% used |
| `tcpip_thread` | 193 | 772 | 1,248 B | 38% used |
| `IDLE` | 103 | 412 | 512 B | 80% used |

**Caveat:** only 3 tasks appear, even though the .map shows 8+ named
task stack buffers (network, puppies, USB host, USB device, measurement,
metric_system, USBH_MSC_WorkerTask, USBH_Thread). Either:
- Buddy creates them via a path that bypasses `uxTaskGetSystemState` (e.g.
  some `osThreadNew` impls or a custom scheduler tweak), OR
- the missing tasks weren't yet scheduled at our query window.

This is a follow-up to investigate. For now, the visible data still gives
two cuts:
- **`defaultTask`**: allocated 4,640 B, peak ever uses ~2,128 B → could
  safely shrink to 3,072 B. **Saves ~1.5 KB.**
- **`tcpip_thread`**: allocated 1,248 B, peak ever ~476 B → could
  shrink to 800 B. **Saves ~450 B.**
- `IDLE`: 80% utilized, do not shrink.

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
