# Flash / code-size profile — dev-allpatches

Captured 2026-06-02 on commit `929706674` (Prusa Connect disabled). Build
preset: `coreone_release_noboot`. Total `.text`: 1,872,292 B (1828 KB).

## .text symbol breakdown by category

`arm-none-eabi-nm --size-sort` covers symbol-sized text only; the
remaining ~1 MB is RODATA (translations, strings, font tables,
constants) bundled into `.text` by the linker.

| Symbol-sized text | KB | % | Category |
|---:|---:|---:|---|
| 620,226 | 605.7 | 69.2% | other (Marlin core, translations, RODATA, etc.) |
| 124,036 | 121.1 | 13.8% | GUI (screens, dialogs, sound) |
| 73,904 | 72.2 | 8.2% | nhttp / WUI |
| 37,398 | 36.5 | 4.2% | libc / libgcc |
| 19,702 | 19.2 | 2.2% | lwIP |
| 18,125 | 17.7 | 2.0% | marlin_server |
| 3,098 | 3.0 | 0.3% | FreeRTOS |
| **896,489 total symbol .text** | | | |

The `nhttp/WUI` category at 72 KB includes the entire HTTP server, the
Moonraker shim, and the WebSocket handler — comparable in size to the
top-level Marlin gcode dispatcher.

## Top 20 individual functions by .text size

| Bytes | Function |
|---:|---|
| 9,876 | `GcodeSuite::process_parsed_command(bool)` |
| 8,100 | `GcodeSuite::process_parsed_command_custom(bool)` |
| 7,140 | `marlin_server::_server_print_loop()` |
| 6,620 | `TC6_Handler()` (touch controller interrupt) |
| 6,184 | `phase_stepping::calibrate_axis(...)` |
| 5,756 | `startup_task` |
| 5,288 | `ip4_input` (lwIP) |
| 5,000 | `run_z_probe(...)` |
| 4,860 | `marlin_server::cycle()` |
| 4,752 | `WebSocketHandler::handle_text_frame_into_buf()` |
| 4,032 | `ScreenPrintPreview::Change(...)` |
| 3,898 | `_sub_I_65535_0.0` (C++ static initializers) |
| 3,754 | `init_config_store()` |
| 3,752 | `mbedtls_internal_sha1_process` (used for HTTP digest auth, WS handshake SHA1) |
| 3,740 | `render_objects_query(...)` (Moonraker /objects/query) |
| 3,676 | `PrusaLinkApiV1::accept(...)` |
| 3,656 | `WebSocketHandler::step(...)` |
| 3,620 | `StartDefaultTask` |
| 3,444 | `StatusRenderer::renderState(...)` |
| 3,144 | `auto visit_display_config<...>` (FSM state visitor) |

## Observations

- The two `GcodeSuite::process_parsed_command*` functions own 18 KB
  alone — that's the M-code/G-code dispatcher. Hard to shrink without
  ripping out features.
- The Moonraker shim functions (`WebSocketHandler::*`, `render_objects_query`,
  `StatusRenderer::renderState`) total ~15 KB — about 1% of total flash.
  Reasonable cost for what it provides.
- `_sub_I_65535_0.0` at 3.9 KB is auto-generated C++ static
  initializer dispatch — common in codebases with many global
  constructors. Could be shrunk by removing unused globals.
- `TC6_Handler` at 6.6 KB is the touch-controller IRQ handler. Big but
  load-bearing for LCD touch.

## Total flash budget

| | Bytes | Percentage |
|---|---:|---:|
| `.text` used | 1,872,292 | ~95% |
| `FLASH` region size | 2,097,152 | 100% |
| **Free flash** | ~225 KB | ~11% |

Connect disable freed ~99 KB of flash; that's the bulk of headroom we
have now. Translations cut would free another ~50-100 KB if needed.

## How to regenerate

```bash
arm-none-eabi-nm --size-sort -S --radix=d build/coreone_release_noboot/firmware \
  | awk '$3 ~ /^[tTwW]$/ && $2+0 > 2000 {print $2, $4}' \
  | sort -rn | head -25
```
