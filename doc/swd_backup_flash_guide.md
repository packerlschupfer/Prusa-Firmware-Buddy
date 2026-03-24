# SWD Backup & Flash Guide — Prusa Core One (xBuddy Board)

## Hardware

- **MCU**: STM32F427ZIT6 (Cortex-M4, 2 MB flash, 192 KB SRAM + 64 KB CCMRAM)
- **Board**: xBuddy (Rev 44, PCB ID-10589)
- **Debug header**: J21 — unpopulated 2x5 pin 1.27mm pitch (ARM 10-pin Cortex Debug standard)
- **Location**: rear-right of the Core One, inside the electronics box (T10 Torx, six M3x4bT bolts)

## J21 SWD Header Pinout

Standard ARM 10-pin SWD connector (DS1031-08-2-SPBRS-6-1):

```
        +---------+
  +3V3  | 1     2 | SWDIO (PA13)
  +3V3  | 3     4 | SWCLK (PA14)
  +3V3  | 5     6 | SWO
   KEY  | 7     8 | NC
   GND  | 9    10 | NRST
        +---------+
```

> **WARNING**: Pin 1 orientation on the PCB may be opposite to what the schematic shows.
> Always verify pin 1 with a multimeter (check 3.3V) before connecting a debugger.

The header is **unpopulated** on production boards. Solder a 2x5 1.27mm pin header,
or use pogo pins / test clips.

## Required Tools

- ST-Link v2/v3 or J-Link debug probe
- OpenOCD (`sudo apt install openocd`) or STM32CubeProgrammer
- 1.27mm pitch SWD cable or dupont wires

## Flash Memory Layout

| Address        | Size   | Sector(s) | Contents                       |
|----------------|--------|-----------|--------------------------------|
| `0x08000000`   | 16 KB  | 0         | Preboot                        |
| `0x08004000`   | 16 KB  | 1         | Main bootloader                |
| `0x08008000`   | 16 KB  | 2         | Main bootloader                |
| `0x0800C000`   | 16 KB  | 3         | Main bootloader                |
| `0x08010000`   | 64 KB  | 4         | Main bootloader                |
| `0x08020000`   | 512 B  | 5 (start) | BBF header / firmware descriptor |
| `0x08020200`   | ~1919 KB | 5-11    | Application firmware           |
| **Bootloader total** | **128 KB** | 0-4 |                          |
| **Flash total**      | **2 MB**   | 0-11+12-23 |                    |

## Step 1: Backup Stock Firmware

```bash
# Full 2 MB flash dump (bootloader + firmware + everything)
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "init; halt; flash read_image backup_full_flash.bin 0x08000000 0x200000; shutdown"

# Bootloader only (128 KB)
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "init; halt; flash read_image backup_bootloader.bin 0x08000000 0x20000; shutdown"

# Application firmware only
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "init; halt; flash read_image backup_firmware.bin 0x08020000 0x1E0000; shutdown"

# Option bytes (check RDP level, write protection)
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "init; halt; mdw 0x1FFFC000 4; shutdown"
```

**Store these backups safely.** The bootloader is closed-source and not publicly downloadable
as a standalone binary.

## Step 2: Build Custom Firmware

```bash
cd /home/mrnice/git/Prusa-Firmware-Buddy

# Build for Core One (unsigned BBF)
python utils/build.py --preset coreone --generate-bbf

# Output will be in build/coreone/
```

## Step 3: Flash Custom Firmware

### Option A: Application only (preserves stock bootloader)
```bash
# Flash just the firmware region — safe, bootloader untouched
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "init; halt; flash write_image erase build/coreone/firmware.bin 0x08020000; reset; shutdown"
```

### Option B: Full image (custom bootloader + firmware)
```bash
# Flash everything — replaces bootloader too
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "init; halt; flash write_image erase full_image.bin 0x08000000; reset; shutdown"
```

## Step 4: Restore Stock Firmware

```bash
# Restore from full backup
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "init; halt; flash write_image erase backup_full_flash.bin 0x08000000; reset; shutdown"
```

## Using the Repo's OpenOCD Configs

The firmware repo includes ready-made debug configs:

```bash
# Connect with the built-in config (supports FreeRTOS thread awareness)
openocd -f utils/debug/buddy.cfg
```

See `doc/debugging_profiling.md` for GDB setup and live debugging instructions.

## Notes

- **No appendix break required** when using SWD — you bypass the bootloader entirely
- **SWD is not disabled** by the firmware — PA13 is restored to SWD mode after the appendix check
- **RDP Level**: Unknown whether set in stock bootloader. If RDP Level 1, readback may
  require lowering RDP first (which triggers a mass erase). Check option bytes first.
  If RDP Level 0, no issues. If RDP Level 2, SWD is permanently disabled (unlikely).
- The appendix tab GPIO is PA13 (same pin as SWDIO). When SWD is connected, the debugger
  drives this pin, so `appendix_exist()` returns false — this is by design.

## References

- [xBuddy schematic PDF (Rev 44)](https://www.prusa3d.com/downloads/Electronics_drawings/FDM-xBUDDY-44.pdf)
- [Prusa KB: Flashing custom firmware](https://help.prusa3d.com/article/flashing-custom-firmware-core-one-l-core-one-mk4-s-mk3-9-s-mk3-5-s_814967)
- [Prusa KB: Core One electronics](https://help.prusa3d.com/article/prusa-core-one-electronics_857793)
- [Forum: xBuddy debug header pinout](https://forum.prusa3d.com/forum/user-mods-enclosures-nozzles/xbuddy-debug-header-pinout/)
- [Firmware repo: doc/debugging_profiling.md](doc/debugging_profiling.md)
- [Firmware repo: utils/debug/buddy.cfg](utils/debug/buddy.cfg)
