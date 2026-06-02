# Feature-flag audit — coreone preset

Captured 2026-06-02 from `ProjectOptions.cmake`. For each ON-by-default flag
on the COREONE printer, the recommendation column reflects whether it
*could* be cut for a Fluidd-driven dev build without functional loss.

## Off by default (already saving RAM/flash)

| Flag | Reason off | Saved (approx) |
|---|---|---|
| `BUDDY_ENABLE_CONNECT` | Prusa Connect cloud, disabled on dev-allpatches per commit `929706674` | 9.4 KB RAM, 99 KB flash |

## On by default — load-bearing for Core One+ (do NOT touch)

| Flag | Reason load-bearing |
|---|---|
| `BUDDY_ENABLE_WUI` | Prusa-Link / web server. Required for HTTP + Moonraker shim. |
| `HAS_GUI` | LCD touchscreen UI. Disable bricks the user-visible interface. |
| `HAS_TOUCH` | Touchscreen input. Disable bricks LCD interaction. |
| `HAS_PLANNER` | Motion planner. Required for any movement. |
| `HAS_USB_DEVICE` | USB CDC serial — gcode upload, /api/v1/log etc. |
| `HAS_LOCAL_BED` | Bed thermal control. Required. |
| `HAS_CHAMBER_API` | Chamber temperature reporting. We use it in WS push. |
| `HAS_LOADCELL_HX717` | Loadcell driver — Core One+ homes via loadcell. Critical. |
| `HAS_PRECISE_HOMING_COREXY` | CoreXY kinematics. Required. |
| `HAS_PHASE_STEPPING` | Quieter stepping. Removing would degrade print quality. |
| `HAS_BURST_STEPPING` | Smooth motion at high feed rates. Removing would degrade print quality. |
| `HAS_DOOR_SENSOR` | Safety interlock. |
| `HAS_EMERGENCY_STOP` | Safety. |
| `HAS_XBUDDY_EXTENSION` | Chamber fans/LED hardware. We expose them via Moonraker. |
| `HAS_DWARF` | (despite the name, refers to the puppy MCU communication on Core One+'s mainboard). Required. |
| `HAS_PUPPIES` / `HAS_PUPPIES_BOOTLOADER` | Puppy MCU communication. Required. |
| `HAS_ADVANCED_POWER` | PSU monitoring. |
| `HAS_LOCAL_ACCELEROMETER` | Required for input shaper calibration. |
| `HAS_TRANSLATIONS` | LCD strings in multiple languages. Could be cut to English-only but very low-value cut. |

## On by default — POTENTIALLY shrinkable / disposable

| Flag | What it adds | Cut value? |
|---|---|---|
| `HAS_MMU2` | MMU3 protocol code + state machines. User has no MMU. | **Maybe — would save real RAM/flash** if user truly doesn't run MMU. Confirm with user. |
| `HAS_NFC` | NFC chip support (filament tags?). Unknown user usage. | Investigate. |
| `HAS_COLDPULL` | Cold-pull wizard for clogs. Used rarely. | Skip — small footprint, useful occasionally. |
| `HAS_GEARBOX_ALIGNMENT` | Gearbox alignment wizard. Used at calibration time. | Skip — same. |
| `HAS_AUTO_RETRACT` | Auto-retract on filament unload. Useful. | Keep. |
| `HAS_CEILING_CLEARANCE` | Z-max sanity check. Useful. | Keep. |
| `HAS_GCODE_COMPATIBILITY` | Klipper-style command parser (M862 etc). We rely on this in slicer-injected start gcodes. | Keep. |
| `HAS_SIDE_FSENSOR` | Side-mounted filament sensor. Standard Core One+ has it. | Keep. |
| `HAS_LOVE_BOARD` | EEPROM on the love board. | Keep. |
| `HAS_LEDS_MENU` | LCD menu for the side strip LEDs. Small. | Keep. |
| `HAS_TRANSLATIONS` | LCD localization strings (multi-language). | Cut to English-only saves ~tens of KB flash but no RAM. Low priority. |
| `HAS_MANUAL_BELT_TUNING` | Belt-tune wizard. Used during maintenance. | Skip. |
| `HAS_UNEVEN_BED_PROMPT` | Prompt before a print if bed is uneven. | Skip. |
| `HAS_DOOR_SENSOR_CALIBRATION` | Door sensor calibration wizard. Calibration-only. | Skip. |
| `HAS_CHAMBER_VENTS` | Chamber-vent control. May be relevant if vents are installed. | Keep. |
| `HAS_FILAMENT_SENSORS_MENU` | LCD menu for filament sensor config. | Keep. |
| `HAS_SWITCHED_FAN_TEST` | Fan-switching diagnostics. Self-test only. | Keep. |
| `HAS_ATTACHABLE_ACCELEROMETER` | External USB accelerometer for input shaper recal (user has the module per memory). | Keep (you'll need it). |
| `HAS_I2C_EXPANDER` | I2C expander on Core One+ mainboard. | Keep. |
| `HAS_CHAMBER_FILTRATION_API` | Filtration fan API. Already gated at runtime. | Keep. |

## Top candidates for further trim (in rough RAM/flash savings order)

1. **`HAS_MMU2`** — if the user does NOT use MMU3, disabling probably reclaims tens of KB flash + a few KB RAM (MMU2 state machine, command tables). Needs explicit user confirmation.
2. **`HAS_TRANSLATIONS`** — pure flash trim, possibly tens of KB. Cuts non-English LCD strings. RAM impact minimal.
3. **`HAS_NFC`** — unknown size, unknown usage. Audit first.
4. **`HAS_COLDPULL`** / **`HAS_GEARBOX_ALIGNMENT`** / **`HAS_MANUAL_BELT_TUNING`** / **`HAS_UNEVEN_BED_PROMPT`** / **`HAS_DOOR_SENSOR_CALIBRATION`** — wizards used only during calibration. Together likely a few KB of flash; questionable user-value to cut.

## How to disable a feature

Edit `ProjectOptions.cmake` to remove the printer from the
`set_feature_for_printers` list, then full clean rebuild. Example
already applied: `CONNECT` defaulted to `NO` for `COREONE` via the
board/printer check at line 191.
