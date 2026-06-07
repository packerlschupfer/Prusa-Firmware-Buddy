# PuppyBus bootloader protocol — reference for Klipper-on-H503 plan

Captured 2026-06-06 from reading the Buddy-side implementation. Reference
for backlog item **9b — Klipper-on-H503-via-PuppyBus**
(see `~/.claude/projects/.../memory/project_followups.md`).

The headline finding is at the bottom: **delivery is already abstracted
behind a CMake variable**, so the modification to Buddy needed to
deliver Klipper-as-puppy-app is essentially "point that variable at our
Klipper binary." No `PuppyBootstrap.cpp` patching.

## Protocol overview

The Prusa-Bootloader-Puppy runs on every Prusa "puppy" MCU (Dwarf,
Modular Bed, xBuddy Extension) and speaks a custom command protocol
over **RS-485 Modbus-RTU** to the parent Buddy MCU. Buddy uses this
protocol on every boot to:

1. Power-cycle + reset each puppy via dedicated GPIOs
2. Discover what puppy is in each dock (address assignment + hw type)
3. Compute the expected SHA-256 fingerprint of the puppy app file on
   Buddy's internal storage
4. Verify the puppy's currently-installed app fingerprint matches;
   if not, upload the new app via `WRITE_FLASH`
5. Verify again, then `START_APPLICATION` to jump to the app

Source: `src/puppies/PuppyBootstrap.cpp`. Header:
`include/puppies/BootloaderProtocol.hpp`.

## Command set

12 commands, single-byte opcodes (`commands_t : uint8_t`):

| Opcode | Name | Direction | Payload |
|---|---|---|---|
| `0x00` | `GET_PROTOCOL_VERSION` | bidi | reply: `uint16_t` |
| `0x01` | `SET_ADDRESS` | host→puppy | req: `uint8_t new_addr`; no reply |
| `0x03` | `GET_HARDWARE_INFO` | bidi | reply: `HwInfo` struct (12 B) |
| `0x05` | `START_APPLICATION` | bidi | req: `uint32_t salt`, `uint8_t[32] expected_fp`; bootloader verifies + jumps |
| `0x06` | `WRITE_FLASH` | bidi | req: `uint32_t offset`, `uint8_t[≤247]` data |
| `0x07` | `FINALIZE_FLASH` | bidi | commit + auto-recompute fingerprint |
| `0x08` | `READ_FLASH` | bidi | req: `uint32_t offset`, `uint8_t len`; reply: bytes |
| `0x0c` | `GET_MAX_PACKET_LENGTH` | bidi | reply: `uint16_t` |
| `0x0e` | `GET_FINGERPRINT` | bidi | reply: `uint8_t[32]` (computed by 0x0f) |
| `0x0f` | `COMPUTE_FINGERPRINT` | host→puppy | req: `uint32_t salt`; async, takes ~330 ms (host polls) |
| `0x10` | `READ_OTP` | bidi | (protocol ≥ 0x0302) read raw OTP bytes |

**Removed opcodes (do not reuse):** `0x02`, `0x04`, `0x09`, `0x0a`,
`0x0b`, `0x0d`, `0x44`, `0x46`.

### Status / reply codes (`status_t`)

```
0x00  COMMAND_OK
0x01  COMMAND_FAILED
0x02  COMMAND_NOT_SUPPORTED
0x03  INVALID_TRANSFER
0x04  INVALID_CRC
0x05  INVALID_ARGUMENTS

(internal-only — never transmitted)
0x11  WRITE_ERROR
0x12  NO_RESPONSE
0x13  INCOMPLETE_RESPONSE
0x14  BAD_RESPONSE
0x15  READ_DATA_ERROR
```

### Sizes

```
MAX_PACKET_LENGTH       = 255 B
MAX_REQUEST_DATA_LEN    = 251 B  (= 255 - 4 framing)
MAX_RESPONSE_DATA_LEN   = 250 B  (= 255 - 4 framing - 1 status byte)
MAX_FLASH_BLOCK_LENGTH  = 247 B  (= MAX_REQUEST_DATA_LEN - 4 offset bytes)
MAX_FLASH_TOTAL_LENGTH  = 122,880 B  (= (128 KiB) - 8 KiB)
```

## Addresses (puppy bus)

```
0x00  DEFAULT_ADDRESS   ← assigned by bootloader after reset
0x0A  FIRST_ASSIGNED    ← buddy's first dynamic assignment
0x1A  MODBUS_OFFSET     ← address base AFTER jump-to-app (app uses modbus, not bootloader proto)
```

Each puppy boots into the bootloader at `0x00`. Buddy sends
`SET_ADDRESS` to one puppy at a time (using GPIO reset pins to keep
the other puppies in reset), then walks through `FIRST_ASSIGNED`,
`FIRST_ASSIGNED+1`, etc.

## Hardware info struct (`GET_HARDWARE_INFO`)

```cpp
struct HwInfo {
    uint8_t  hw_type;          // matches PuppyInfo.hw_info_hwtype
    uint16_t hw_revision;
    uint32_t bl_version;
    uint32_t application_size; // bootloader's view of how big the app slot is
};
```

`hw_type` values defined in `include/puppies/puppy_constants.hpp`:

| Puppy | `hw_type` | `fw_path` |
|---|---:|---|
| Dwarf (toolhead) | 42 | `/internal/res/puppies/fw-dwarf.bin` |
| Modular Bed (XL) | 43 | `/internal/res/puppies/fw-modularbed.bin` |
| **xBuddy Extension** | **44** | `/internal/res/puppies/fw-xbuddy-extension.bin` |

## H503 flash layout (`src/puppy/xbuddy_extension/stm32h503.ld`)

```
TOTAL_FLASH_SIZE = 128 KiB
BL_SIZE          = 8 KiB    in stm32h503_boot.ld
BL_SIZE          = 0        in stm32h503_noboot.ld
FLASH_ONE_PAGE   = 8 KiB    (so 16 pages)
DESCRIPTOR_SIZE  = 128 B
APP_SIZE         = TOTAL - BL_SIZE - DESCRIPTOR_SIZE = 122,880 B  (boot variant)

Memory layout (boot variant):
  0x08000000  ┌───────────────────────────────────┐
              │ Prusa-Bootloader-Puppy   (8 KiB)  │
  0x08002000  ├───────────────────────────────────┤  ← APP START — vector table here
              │ .isr_vector                       │
              │ .text, .rodata, .data init values │
              │  (up to 122,752 B usable)         │
  0x0801FF80  ├───────────────────────────────────┤
              │ FW_DESCRIPTOR  (128 B)            │  ← 48 B FWDescriptor + 80 B 0x00 pad
  0x08020000  └───────────────────────────────────┘

RAM   = 32 KiB at 0x20000000, last 1 KiB reserved for ISR_STACK
```

## FW descriptor (`include/puppies/crash_dump_shared.hpp`)

```cpp
struct __attribute__((aligned(8))) FWDescriptor {
    enum class StoredType : uint32_t {
        fw         = 12321,
        crash_dump = 0x71439503,
    };
    StoredType stored_type;   // 4 B  — written by bootloader after a successful flash
    uint32_t   dump_offset;   // 4 B
    uint8_t    fingerprint[32]; // 32 B — SHA-256
    uint32_t   dump_size;     // 4 B
    // total: 44 B used, 4 B alignment pad → 48 B
};
constexpr uint8_t APP_DESCRIPTOR_LENGTH = 128;   // reserved region
```

At build time the descriptor is **48 zero bytes** (a placeholder, see
the "magical incantation" in `src/puppy/xbuddy_extension/main.cpp:11`).
The linker pads the remaining 80 B to fill the 128 B region.

**The bootloader does NOT validate descriptor contents during the
WRITE_FLASH flow.** Fingerprint verification is done via runtime
`COMPUTE_FINGERPRINT`/`GET_FINGERPRINT` commands (computed over the
actual flashed bytes plus a host-provided salt). The descriptor is
used by the *crash dump* path post-runtime: if the puppy app crashes,
the bootloader overwrites the descriptor with
`StoredType::crash_dump` + offset/size of the dump, and Buddy reads
it back on next boot to extract the crash dump.

## Bootstrap orchestration flow (`PuppyBootstrap::run`)

```
1. Lock the puppy bus (PuppyBus::LockGuard)
2. reset_all_puppies() — pulse the dedicated reset GPIOs (PG8/PG2-style)
3. run_address_assignment() — walk docks, SET_ADDRESS each puppy
4. verify minimum config (else retry up to N times)
5. Pick a random salt per puppy
6. For each puppy:
     - COMPUTE_FINGERPRINT(salt)              ← bootloader starts hashing
     - meanwhile, host computes expected_fp from the file on disk
     - wait_for_fingerprint() (poll GET_PROTOCOL_VERSION until ready, ≤1 s)
     - GET_FINGERPRINT → compare against expected_fp
7. If mismatch:
     - attempt_crash_dump_download() — pull crash dump if present
     - flash_firmware():
         while bytes remaining:
             WRITE_FLASH(offset, chunk≤247B)
         FINALIZE_FLASH()
         restart_fingerprint_computation(new_salt)
         GET_FINGERPRINT → verify
8. START_APPLICATION(salt, fingerprint) for each puppy
   ↑ bootloader does its own SHA-256(salt ‖ app) verification, then jumps
```

### The fingerprint contract

- **Salt** is a per-boot `uint32_t` random value chosen by Buddy
- **Fingerprint** is `SHA-256(salt_bytes ‖ app_bytes_from_flash)`
- Host (Buddy) does the same hash over (salt ‖ on-disk file bytes)
- Both must match for the bootloader to allow `START_APPLICATION` to jump
- The salt mechanism prevents replay attacks where someone could feed
  a stale fingerprint to make the bootloader run unintended code

### Discovery time gates

- `MINIMAL_BOOTLOADER_VERSION` — minimum `bl_version` Buddy will tolerate
- Protocol **major version** must match `0x0302` (i.e. byte mask `0xFF00`)
- `MAX_FLASH_TOTAL_LENGTH` (122,880 B) is the absolute upper bound for an
  app binary; Buddy's `flash_firmware` calls `fatal_error` if the file
  exceeds this

## The big finding — delivery is a CMake variable

From `src/resources/CMakeLists.txt:115`:
```cmake
add_resource("${XBUDDY_EXTENSION_BINARY_PATH}" "/puppies/fw-xbuddy-extension.bin")
```

And from `CMakeLists.txt` lines 411-430 + `ProjectOptions.cmake:824`:
```cmake
if(NOT XBUDDY_EXTENSION_BINARY_PATH AND HAS_XBUDDY_EXTENSION)
  # default: the in-tree xbuddy_extension build target
  ...
endif()
```

**`XBUDDY_EXTENSION_BINARY_PATH`** can be overridden from the cmake
command line to point at *any* `firmware.bin`. The resources system
bundles it into Buddy's internal filesystem under
`/puppies/fw-xbuddy-extension.bin`. `PuppyBootstrap` reads that file
at boot and uploads it via `WRITE_FLASH`.

Implication for backlog item 9b: **the modification to Buddy needed
to deliver Klipper-as-puppy-app is one cmake flag.**

```
python3 utils/build.py --preset coreone --bootloader yes --skip-bootstrap \
  -DXBUDDY_EXTENSION_BINARY_PATH=/path/to/klipper-h5-puppy-app.bin
```

That produces a `.bbf` which, when installed via USB stick, will from
that point on cause Buddy to install Klipper on the H503 on every
boot. **Zero changes to `PuppyBootstrap.cpp`.**

Caveats:
1. The Klipper H5 build must produce a binary that fits the layout —
   `.isr_vector` at offset 0, total size ≤ 122,880 B, vector table
   relocation handled at runtime (Klipper's H5 startup probably does
   this via SCB->VTOR).
2. The Klipper image must include a `.fw_descriptor` section at the
   right offset (128 B zero pad is fine — only the crash-catcher path
   writes meaningful data there, which is optional).
3. Klipper's H5 build needs to be built with `BL_SIZE = 8K` (vectors
   at `0x08002000`, not `0x08000000`), matching `stm32h503_boot.ld`.
4. Hardware compatibility — the bootloader's `start_app()` does final
   sanity checks; we need to ensure Klipper's startup doesn't trip them.

## Open questions for the design pass (9b-3)

1. **Klipper H5 startup vector handling** — does Klipper compute its
   own `SCB->VTOR` value from a Kconfig flash-start setting, or is it
   hard-coded? (Klipper-port chat has the H5 overlay; ask them.)
2. **FreeRTOS coexistence** — does Klipper assume "I own the whole
   chip" at startup, or is it tolerant of pre-jumped-from-bootloader
   state? The bootloader's hand-off is "set MSP, set PC, branch" —
   essentially a cold start from Klipper's perspective.
3. **The `.fw_descriptor` section** — can Klipper's link script
   produce one with the 80 B zero-pad layout, or do we need a
   post-link tool to splat it in? (Pretty easy either way.)
4. **Resource image rebuild** — once `XBUDDY_EXTENSION_BINARY_PATH`
   changes, does the existing CMake dep graph rebuild the resources
   image correctly? Should — `add_resource` ought to track the
   variable as a dep — but worth verifying with a `touch` test.

## References (within this repo)

- `include/puppies/BootloaderProtocol.hpp` — full command set + sizes
- `src/puppies/BootloaderProtocol.cpp` — wire encoding/decoding impl
- `src/puppies/PuppyBootstrap.cpp` — orchestration flow (focus on `run()`,
  `flash_firmware()`, `start_app()`)
- `src/puppies/PuppyBus.cpp` — bus lock + reset pin GPIOs
- `src/puppies/PuppyModbus.cpp` — Modbus framing
- `src/puppy/shared/modbus/ModbusProtocol.{cpp,hpp}` — RTU layer
- `src/puppy/shared/hal/HAL_RS485.cpp` — physical layer (UART + DE)
- `src/puppy/xbuddy_extension/stm32h503{,_boot,_noboot}.ld` — H503 flash
  layouts
- `src/puppy/xbuddy_extension/main.cpp:11` — fw_descriptor placeholder
- `src/resources/CMakeLists.txt:115` — the `add_resource` line that
  abstracts delivery
- `include/puppies/puppy_constants.hpp` — puppy type / hw_type mapping
- `include/puppies/crash_dump_shared.hpp` — `FWDescriptor` struct
- External: github.com/prusa3d/Prusa-Bootloader-Puppy (the C side of
  the protocol, not in this repo)
