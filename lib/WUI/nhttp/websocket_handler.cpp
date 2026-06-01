#include "websocket_handler.h"
#include "handler.h"
#include "status_page.h"
#include "headers.h"
#include "moonraker_access.h"
#include "server.h"
#include "step.h"
#include "../http_lifetime.h"

#include <marlin_client.hpp>
#include <marlin_vars.hpp>
#include <timing.h>
#include <feature/chamber/chamber.hpp>
#include <feature/chamber_filtration/chamber_filtration.hpp>
#include <feature/xbuddy_extension/xbuddy_extension.hpp>
#include <option/xbuddy_extension_variant.h>
#include <leds/side_strip_handler.hpp>
#include <state/printer_state.hpp>
#include <adc.hpp>
#include <cpu_utils.hpp>
#include <feature/filament_sensor/filament_sensors_handler.hpp>
#include <feature/filament_sensor/filament_sensor_states.hpp>
#include <inc/MarlinConfig.h>
#if ENABLED(AUTO_BED_LEVELING_UBL)
    #include <feature/bedlevel/bedlevel.h>  // defines bed_mesh_t before ubl.h uses it
    #include <feature/bedlevel/ubl/ubl.h>
#endif
#include <module/endstops.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <lwip/def.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nhttp::printer {

namespace {

constexpr const char *WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// ----------------------------------------------------------------------------
// Built-in macros surfaced to Fluidd's "Macros" panel.
//
// Fluidd reads `configfile.settings.gcode_macro <NAME>` to discover
// clickable macro buttons. The standard Klipper flow is: user clicks
// the button → Fluidd sends `printer.gcode.script` with the bare macro
// name → Klipper's gcode engine expands the macro into its `gcode:` body
// and runs it. Marlin has no concept of user-defined macros, so we
// intercept the name in our gcode/script dispatch and queue the
// expansion ourselves.
//
// Keep this list compact — every byte here lands in the subscribe
// response, and the response shares frame_buf with inbound frames.
// ----------------------------------------------------------------------------

struct GcodeMacro {
    const char *name;
    const char *gcode;
};

constexpr GcodeMacro kMacros[] = {
    { "PREHEAT_PLA", "M104 S210\nM140 S60" },
    { "PREHEAT_PETG", "M104 S240\nM140 S85" },
    { "PREHEAT_ABS", "M104 S255\nM140 S100" },
    { "COOL_DOWN", "M104 S0\nM140 S0\nM107" },
    { "HOME_ALL", "G28" },
    { "MOTORS_OFF", "M84" },
    // Klipper-named macros emitted by Fluidd UI buttons. Map to the
    // Marlin equivalents so the existing Bed Mesh card works.
    //
    // For UBL on Marlin, plain `G29` doesn't probe — it just prints
    // current mesh state. Full auto-probe is `G29 P1`. Following it
    // with `G29 P3` fills any unmeasured points by interpolation and
    // `G29 S0` saves to slot 0. The whole sequence is what Klipper's
    // BED_MESH_CALIBRATE does end-to-end.
    { "BED_MESH_CALIBRATE", "G29 P1\nG29 P3 R 999\nG29 S0" },
    { "BED_MESH_CLEAR", "M420 S0" },
    { "QUERY_ENDSTOPS", "M119" },
};

// Best-effort dispatcher for Klipper-style commands Fluidd / OrcaSlicer
// like to send. Returns true if the command was fully handled here
// (skip sending the line to Marlin) — false means "I didn't recognize
// it, send the line through as-is".
//
// Notes:
//  - Print-action commands (PAUSE/RESUME/CANCEL_PRINT) dispatch via
//    marlin_client API rather than mapping to M-codes, so they work
//    cleanly without depending on Marlin's pause/resume gcode behavior.
//  - SET_PIN PIN=chamber_led VALUE=N sets the side-strip W channel
//    via leds::SideStripHandler::set_custom_color. Held for 24 hours;
//    the next SET_PIN call replaces it.
//  - Unknown SET_PIN pins return false → fall through to Marlin (which
//    will say "Unknown command" but at least it's not silently dropped).
bool try_dispatch_klipper_command(const char *line) {
    // Skip leading whitespace.
    while (*line == ' ' || *line == '\t') ++line;
    // Extract first token (uppercase comparison).
    auto starts_with = [](const char *s, const char *prefix) -> bool {
        size_t i = 0;
        while (prefix[i]) {
            char c = s[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c != prefix[i]) return false;
            ++i;
        }
        // Token boundary
        return s[i] == '\0' || s[i] == ' ' || s[i] == '\t';
    };

    if (starts_with(line, "PAUSE")) {
        marlin_client::print_pause();
        return true;
    }
    if (starts_with(line, "RESUME")) {
        marlin_client::print_resume();
        return true;
    }
    if (starts_with(line, "CANCEL_PRINT")) {
        marlin_client::print_abort();
        return true;
    }
    if (starts_with(line, "SET_HEATER_TEMPERATURE")) {
        // SET_HEATER_TEMPERATURE HEATER=<name> TARGET=<n>
        //   extruder    → M104 S<n>
        //   heater_bed  → M140 S<n>
        const char *heater_eq = std::strstr(line, "HEATER=");
        const char *target_eq = std::strstr(line, "TARGET=");
        if (!heater_eq || !target_eq) return false;
        const char *h_start = heater_eq + 7;
        const char *h_end = h_start;
        while (*h_end && *h_end != ' ' && *h_end != '\t' && *h_end != '\r') ++h_end;
        const std::string_view heater(h_start, static_cast<size_t>(h_end - h_start));
        const int target = static_cast<int>(std::strtol(target_eq + 7, nullptr, 10));
        if (heater == "extruder") {
            marlin_client::gcode_printf("M104 S%d", target);
            return true;
        }
        if (heater == "heater_bed") {
            marlin_client::gcode_printf("M140 S%d", target);
            return true;
        }
        return false;
    }
    if (starts_with(line, "SET_FAN_SPEED")) {
        // SET_FAN_SPEED FAN=<name> SPEED=<n>   n is 0..1
        //   fan (part cooling) → M106 S<n*255>
        const char *fan_eq = std::strstr(line, "FAN=");
        const char *speed_eq = std::strstr(line, "SPEED=");
        if (!fan_eq || !speed_eq) return false;
        const char *f_start = fan_eq + 4;
        const char *f_end = f_start;
        while (*f_end && *f_end != ' ' && *f_end != '\t' && *f_end != '\r') ++f_end;
        const std::string_view fan(f_start, static_cast<size_t>(f_end - f_start));
        const float speed = std::strtof(speed_eq + 6, nullptr);
        const int s255 = static_cast<int>((speed < 0.0f ? 0.0f : speed > 1.0f ? 1.0f : speed) * 255.0f);
        if (fan == "fan") {
            marlin_client::gcode_printf("M106 S%d", s255);
            return true;
        }
        return false;
    }
    if (starts_with(line, "TURN_OFF_HEATERS")) {
        marlin_client::gcode("M104 S0");
        marlin_client::gcode("M140 S0");
        return true;
    }
    if (starts_with(line, "M84") || starts_with(line, "MOTOR_OFF")) {
        // M84 passes through Marlin natively; MOTOR_OFF doesn't.
        if (starts_with(line, "MOTOR_OFF")) {
            marlin_client::gcode("M84");
            return true;
        }
        return false;
    }
    if (starts_with(line, "SAVE_CONFIG")) {
        // Klipper's "save config + restart" persists pending settings.
        // Marlin equivalent is M500 (write EEPROM).
        marlin_client::gcode("M500");
        return true;
    }
    if (starts_with(line, "SET_VELOCITY_LIMIT")) {
        // Klipper: VELOCITY / ACCEL / ACCEL_TO_DECEL / SQUARE_CORNER_VELOCITY
        // Map what makes sense to Marlin; silently swallow the rest so
        // Fluidd's Limits card doesn't log-spam "Unknown command".
        if (const char *eq = std::strstr(line, "VELOCITY=")) {
            // Skip if it's SQUARE_CORNER_VELOCITY= (would also match)
            // by checking the char BEFORE "VELOCITY=" isn't a letter.
            if (eq == line || !(eq[-1] >= 'A' && eq[-1] <= 'Z') || (eq[-1] == ' ' || eq[-1] == '_')) {
                // Only handle the top-level VELOCITY=
                if (eq[-1] != '_') {
                    const int v = static_cast<int>(std::strtof(eq + 9, nullptr));
                    marlin_client::gcode_printf("M203 X%d Y%d", v, v);
                }
            }
        }
        if (const char *eq = std::strstr(line, "ACCEL=")) {
            if (eq == line || eq[-1] == ' ' || eq[-1] == '\t') {
                const int a = static_cast<int>(std::strtof(eq + 6, nullptr));
                marlin_client::gcode_printf("M204 S%d", a);
            }
        }
        // SQUARE_CORNER_VELOCITY and ACCEL_TO_DECEL — no direct Marlin
        // equivalent. Silently accepted.
        return true;
    }
    if (starts_with(line, "SET_GCODE_OFFSET")) {
        // Klipper: X=<n> Y=<n> Z=<n>, with optional MOVE=1.
        // Closest Marlin: M428 (set XYZ as home offset) or M206. For
        // Z probe offset specifically we'd want M851. For now swallow
        // the command so Fluidd doesn't log "Unknown".
        return true;
    }
    if (starts_with(line, "MANUAL_PROBE")) {
        // Klipper interactive bed-touch probe. Buddy's loadcell
        // auto-probes; there's no interactive equivalent. Swallow so
        // Fluidd's Tune view button doesn't log-spam.
        return true;
    }
    if (starts_with(line, "EMERGENCY_STOP")) {
        marlin_client::gcode("M112");
        return true;
    }
    if (starts_with(line, "FIRMWARE_RESTART") || starts_with(line, "RESTART")) {
        // Cancel any active print, then let the user power-cycle.
        // Don't issue M999 / NVIC_SystemReset here — would brick a
        // mid-print firmware update or BBF flash flow. Just abort.
        marlin_client::print_abort();
        return true;
    }
    if (starts_with(line, "SET_PIN")) {
        // Parse PIN=name VALUE=N (case sensitive — Klipper convention).
        const char *pin_eq = std::strstr(line, "PIN=");
        const char *val_eq = std::strstr(line, "VALUE=");
        if (!pin_eq || !val_eq) return false;
        const char *pin_start = pin_eq + 4;
        const char *pin_end = pin_start;
        while (*pin_end && *pin_end != ' ' && *pin_end != '\t' && *pin_end != '\r') ++pin_end;
        const std::string_view pin(pin_start, static_cast<size_t>(pin_end - pin_start));
        const float value = std::strtof(val_eq + 6, nullptr);
        if (pin == "chamber_led") {
#if XBUDDY_EXTENSION_VARIANT_IS_STANDARD()
            const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
            const uint8_t w = static_cast<uint8_t>(clamped * 255.0f);
            // Override the strip's W channel for ~24 h so the LED
            // stays set across the rest of the print/session. Next
            // SET_PIN replaces it.
            leds::SideStripHandler::instance().set_custom_color(
                leds::ColorRGBW { 0, 0, 0, w },
                /*duration_ms*/ 24u * 60u * 60u * 1000u,
                /*transition_ms*/ 0u);
            return true;
#else
            return false;
#endif
        }
        return false;
    }
    return false;
}

// Case-insensitive match against the first whitespace-delimited token
// of a gcode script line. Returns the expansion or nullptr.
const char *find_macro_expansion(std::string_view script) {
    while (!script.empty() && (script.front() == ' ' || script.front() == '\t')) {
        script.remove_prefix(1);
    }
    auto sp = script.find_first_of(" \t\r\n");
    auto token = (sp == std::string_view::npos) ? script : script.substr(0, sp);
    if (token.empty()) return nullptr;
    for (auto &m : kMacros) {
        size_t name_len = std::strlen(m.name);
        if (token.size() != name_len) continue;
        bool match = true;
        for (size_t i = 0; i < name_len; ++i) {
            char a = token[i], b = m.name[i];
            if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 32);
            if (a != b) { match = false; break; }
        }
        if (match) return m.gcode;
    }
    return nullptr;
}

// ----------------------------------------------------------------------------
// In-memory print-history ring buffer.
//
// Tracked from the WS handler's step() (any active connection drives
// the transition detector). Each completed/cancelled/errored print
// is appended. Capped at HISTORY_CAP — older entries shift out.
// Survives only the current session; clears on reboot.
//
// For real persistence we'd write each entry to /internal as it's
// created. Skipping that for now because: (a) the wear pattern on
// every-print writes deserves thought, (b) the user explicitly said
// rebooted printer history isn't critical.
// ----------------------------------------------------------------------------

struct HistoryEntry {
    char filename[48];
    uint32_t start_time;     // seconds since boot
    uint32_t total_duration; // seconds
    const char *status;      // "completed" / "cancelled" / "error" / "in_progress"
    uint32_t job_id;
};

constexpr size_t HISTORY_CAP = 5;
HistoryEntry g_history[HISTORY_CAP] = {};
size_t g_history_count = 0;
uint32_t g_history_next_id = 1;

// State of the currently-active print (filled in on transition INTO
// "Printing", drained on transition OUT).
printer_state::DeviceState g_last_print_state = printer_state::DeviceState::Idle;
HistoryEntry g_current_print = {};

void history_append_locked(const HistoryEntry &e) {
    if (g_history_count < HISTORY_CAP) {
        g_history[g_history_count++] = e;
    } else {
        for (size_t i = 0; i < HISTORY_CAP - 1; ++i) {
            g_history[i] = g_history[i + 1];
        }
        g_history[HISTORY_CAP - 1] = e;
    }
}

// Called from each WS handler's step() on tcpip thread. Detects
// transitions in/out of the Printing state and records history entries.
void history_update_from_marlin() {
    const auto cur = printer_state::get_state(false);
    if (cur == g_last_print_state) return;

    using printer_state::DeviceState;
    const bool was_printing = (g_last_print_state == DeviceState::Printing
                               || g_last_print_state == DeviceState::Paused);
    const bool is_printing = (cur == DeviceState::Printing
                              || cur == DeviceState::Paused);

    if (!was_printing && is_printing) {
        // Print started.
        g_current_print = {};
        marlin_vars().media_LFN.copy_to(g_current_print.filename, sizeof g_current_print.filename);
        // Strip leading /usb/ for the relative path Moonraker uses.
        if (std::strncmp(g_current_print.filename, "/usb/", 5) == 0) {
            std::memmove(g_current_print.filename, g_current_print.filename + 5,
                std::strlen(g_current_print.filename) - 4);
        }
        g_current_print.start_time = ticks_ms() / 1000;
        g_current_print.job_id = g_history_next_id++;
        g_current_print.status = "in_progress";
    } else if (was_printing && !is_printing && g_current_print.start_time != 0) {
        // Print ended — finalize and record.
        g_current_print.total_duration = (ticks_ms() / 1000) - g_current_print.start_time;
        switch (cur) {
        case DeviceState::Finished: g_current_print.status = "completed"; break;
        case DeviceState::Stopped:  g_current_print.status = "cancelled"; break;
        case DeviceState::Attention: g_current_print.status = "error"; break;
        default: g_current_print.status = "completed"; break;
        }
        history_append_locked(g_current_print);
        g_current_print = {};
    }
    g_last_print_state = cur;
}

// ----------------------------------------------------------------------------
// Persistent key/value storage for Fluidd's server.database.* methods.
//
// Stores each (key) as /internal/fluidd_<key>.json with the value's raw
// JSON written verbatim. Keys are restricted to ASCII alphanumeric +
// "._-" so they can be safely embedded in filenames without path-
// traversal risk. We only support the "fluidd" namespace.
// ----------------------------------------------------------------------------

constexpr const char *DB_PATH_PREFIX = "/internal/fluidd_";
constexpr const char *DB_PATH_SUFFIX = ".json";

bool db_key_safe(std::string_view key) {
    if (key.empty() || key.size() > 32) return false;
    for (char c : key) {
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool db_build_path(std::string_view key, char *out, size_t out_size) {
    if (!db_key_safe(key)) return false;
    int n = snprintf(out, out_size, "%s%.*s%s",
        DB_PATH_PREFIX, static_cast<int>(key.size()), key.data(), DB_PATH_SUFFIX);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

bool db_write(std::string_view key, const char *value_json, size_t value_len) {
    char path[80];
    if (!db_build_path(key, path, sizeof path)) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    size_t w = fwrite(value_json, 1, value_len, f);
    fclose(f);
    return w == value_len;
}

bool db_delete(std::string_view key) {
    char path[80];
    if (!db_build_path(key, path, sizeof path)) return false;
    return unlink(path) == 0;
}

// Read the stored JSON for `key` into `out`. Returns number of bytes
// written (without null terminator), or 0 on failure / too-large.
size_t db_read(std::string_view key, char *out, size_t out_size) {
    char path[80];
    if (!db_build_path(key, path, sizeof path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(out, 1, out_size, f);
    fclose(f);
    return n;
}

// Macros are exposed via TWO routes in the subscribe response:
//
//  1) TOP-LEVEL `gcode_macro <NAME>` keys (empty objects). Fluidd
//     discovers macros by scanning Object.keys(printer.printer) for
//     entries starting with "gcode_macro " — this is the discovery
//     path that drives the Dashboard's Macros card and Tune view.
//     If a macro is missing from the top level, Fluidd never sees it,
//     regardless of what's in configfile.settings.
//
//  2) configfile.settings.<gcode_macro NAME>: { description, gcode }
//     is OPTIONAL — Fluidd uses it only to render tooltips and the
//     "save_config_pending" warning. We keep settings minimal (no
//     bodies) so the subscribe response stays under MAX_FRAME_PAYLOAD.
//
// IMPORTANT: this block + the rest of the subscribe response must fit
// in frame_buf (~768 bytes after the WS header reserve).
constexpr const char kTopLevelMacrosJson[] =
    "\"gcode_macro PREHEAT_PLA\":{},"
    "\"gcode_macro PREHEAT_PETG\":{},"
    "\"gcode_macro PREHEAT_ABS\":{},"
    "\"gcode_macro COOL_DOWN\":{},"
    "\"gcode_macro HOME_ALL\":{},"
    "\"gcode_macro MOTORS_OFF\":{}";

constexpr const char kConfigfileSettingsJson[] =
    "\"configfile\":{\"settings\":{"
        // Empty stubs gate Fluidd's getSupportsBedMesh and getSupportsBeacon
        // getters (they only check for key presence). The actual bed-mesh
        // and beacon data live in the top-level `bed_mesh` / `beacon` objects.
        "\"bed_mesh\":{}"
    "},\"config\":{},\"warnings\":[],\"save_config_pending\":false}";

// Top-level kinematics stubs that drive Fluidd's Tune page:
//
//  * `motion_report.steppers` is the array `getSteppers` iterates; each
//    entry pulls `printer[name]` and `configfile.settings[name]` for
//    config rendering.
//  * `stepper_x/y/z/extruder` themselves are empty objects — Fluidd
//    spreads them into the row but they don't need any keys for the
//    EndStops card to appear (it only checks .length > 0).
//  * `query_endstops.last_query` keys are the rows in EndStopsCard;
//    Fluidd shows status=0 as "open" / non-zero as "triggered". We
//    don't get real endstop state without an M119, so these stay 0.
//  * `bed_mesh` empty stub so BedMeshCard renders (no profiles, but
//    the card surface, calibrate button etc. all appear).
constexpr const char kTopLevelKinematicsJson[] =
    // Fluidd's getSteppers iterates motion_report.steppers, then spreads
    // `e.printer[name]` into each row. We don't emit the per-stepper
    // top-level objects (saves ~50 bytes); the spread of `undefined` is
    // a no-op in JS so the row still renders correctly. `extruder` is
    // a top-level Klipper object too, but status_update writes
    // {temperature,target,...} into it, so we let that path own it.
    "\"motion_report\":{\"steppers\":[\"stepper_x\",\"stepper_y\",\"stepper_z\",\"extruder\"]},"
    "\"bed_mesh\":{}";

// ----------------------------------------------------------------------------
// Global filelist event publisher.
//
// File upload/delete handlers (running on tcpip thread) call
// publish_filelist_event(). Each WS handler tracks `last_filelist_epoch`
// in its own state; when the global differs, step() emits a
// notify_filelist_changed JSON-RPC notification.
//
// Same thread (tcpip) writes both publisher and reader, so the atomic
// + plain string is enough — no critical section needed.
// ----------------------------------------------------------------------------

std::atomic<uint32_t> g_filelist_epoch { 0 };
FilelistAction g_filelist_last_action = FilelistAction::CreateFile;
char g_filelist_last_path[64] = {};

// ----------------------------------------------------------------------------
// Bed-mesh render buffer.
//
// The full bed_mesh JSON (21×21 probed + interpolated matrix + bounds)
// runs ~3 KB — too big for one WS frame even at our raised
// MAX_FRAME_PAYLOAD=1500. We render it once into this static buffer
// when a client triggers `printer.bed_mesh.dump`, then each WS handler
// streams it out across multiple fragmented frames at its own pace
// (state in mesh_send_pos/mesh_send_total).
//
// Race policy: if a second client triggers a fresh dump while a first
// client is mid-stream, the buffer is overwritten in place. The first
// client may briefly see mixed content but never indexes past
// mesh_send_total, so the stream stays well-formed-but-stale rather
// than corrupted. Mesh changes are rare (post-G29 only), so this is
// acceptable for now.
// ----------------------------------------------------------------------------
// 21×21 matrix × 6 chars/cell × 2 matrices (probed + mesh) + envelope
// fits comfortably in 6 KB. Sized once at .bss; never grows.
constexpr size_t MESH_BUF_SIZE = 6144;
char g_mesh_buf[MESH_BUF_SIZE] = {};
uint16_t g_mesh_buf_size = 0;

// Render real bed_mesh data (from Marlin's UBL z_values) into g_mesh_buf
// as a notify_status_update JSON-RPC envelope. The matrix is the full
// 21x21 probed grid; mesh_matrix is emitted as the same data (Fluidd's
// "Mesh Matrix" view) since we don't interpolate. NaN cells (unprobed)
// are rendered as 0.0 — clients see a flat surface rather than a parse
// error.
//
// Thread safety: ubl.z_values is read directly here on the tcpip thread.
// Per feedback_buddy_internals.md scalar float reads on Cortex-M4 are
// atomic at the hardware level; the only writer is Marlin during G29,
// and a torn read at worst gives a mix of pre- and post-G29 cells (no
// crash, no parse-breaking output).
void render_mesh_into_buf() {
    // CoreOne probes a 7x7 sparse grid into the full 21x21 UBL array.
    // The actual probed cells sit at indices 1, 4, 7, 10, 13, 16, 19
    // (GRID_BORDER=1, GRID_MAJOR_STEP=3, GRID_MAJOR_POINTS=7). We emit
    // both probed_matrix and mesh_matrix as 7x7 with mesh_min/max
    // matching the probed-area bounds — this is the shape Klipper
    // users see and what Fluidd's three.js viewer is set up for.
    // mesh_matrix is the same data since we don't have a separate
    // interpolated view; both go through Fluidd's renderer, with the
    // user toggling "probed_matrix" / "mesh_matrix" view modes.
    constexpr int PROBED_N = 7;
    constexpr int PROBED_OFFSET = 1;  // GRID_BORDER
    constexpr int PROBED_STRIDE = 3;  // GRID_MAJOR_STEP
    const float step_x = (MESH_MAX_X - MESH_MIN_X) / (GRID_MAX_POINTS_X - 1);
    const float step_y = (MESH_MAX_Y - MESH_MIN_Y) / (GRID_MAX_POINTS_Y - 1);
    const float pmin_x = MESH_MIN_X + PROBED_OFFSET * step_x;
    const float pmin_y = MESH_MIN_Y + PROBED_OFFSET * step_y;
    const float pmax_x = MESH_MIN_X + (PROBED_OFFSET + (PROBED_N - 1) * PROBED_STRIDE) * step_x;
    const float pmax_y = MESH_MIN_Y + (PROBED_OFFSET + (PROBED_N - 1) * PROBED_STRIDE) * step_y;

    char *p = g_mesh_buf;
    char *end = g_mesh_buf + MESH_BUF_SIZE - 4;
    p += snprintf(p, end - p,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notify_status_update\","
        "\"params\":[{\"bed_mesh\":{"
        "\"profile_name\":\"default\","
        "\"mesh_min\":[%.2f,%.2f],"
        "\"mesh_max\":[%.2f,%.2f],"
        "\"probed_matrix\":[",
        static_cast<double>(pmin_x), static_cast<double>(pmin_y),
        static_cast<double>(pmax_x), static_cast<double>(pmax_y));

    auto emit_matrix = [&]() {
        for (int j = 0; j < PROBED_N; ++j) {
            const int yi = PROBED_OFFSET + j * PROBED_STRIDE;
            if (j > 0 && p < end) *p++ = ',';
            if (p < end) *p++ = '[';
            for (int i = 0; i < PROBED_N; ++i) {
                const int xi = PROBED_OFFSET + i * PROBED_STRIDE;
                if (i > 0 && p < end) *p++ = ',';
#if ENABLED(AUTO_BED_LEVELING_UBL)
                float v = ubl.z_values[xi][yi];
                if (!std::isfinite(v)) v = 0.0f;
#else
                float v = 0.0f;
#endif
                p += snprintf(p, end - p, "%.3f", static_cast<double>(v));
            }
            if (p < end) *p++ = ']';
        }
    };
    emit_matrix();
    p += snprintf(p, end - p, "],\"mesh_matrix\":[");
    emit_matrix();
    p += snprintf(p, end - p, "],\"profiles\":{}}},0.0]}");
    g_mesh_buf_size = static_cast<uint16_t>(p - g_mesh_buf);
}

// ----------------------------------------------------------------------------
// Gcode-response ring buffer.
//
// USBSerial's lineBufferHook fires once per line of Marlin's serial
// output ("ok", "X:0 Y:0 Z:0 E:0", etc.). We tee those lines into this
// ring buffer; each WS handler tracks its own read cursor into the
// monotonic write counter and emits a notify_gcode_response on each
// poll-driven step() when there are new entries.
// ----------------------------------------------------------------------------

constexpr size_t GCODE_LOG_LINES = 16;
// 256 fits the M115 FIRMWARE_NAME banner (~280 chars truncated to 255) and
// long Marlin echo lines. entry.len is uint8_t so the cap is 255 chars + NUL.
// Ring footprint: 16 * 256 = 4 KB static. Render path bounds per-frame
// payload against MAX_FRAME_PAYLOAD, so larger entries just mean fewer
// per frame, not overflow.
constexpr size_t GCODE_LOG_LINE_LEN = 256;
struct GcodeLogEntry {
    char data[GCODE_LOG_LINE_LEN];
    uint8_t len;
};
GcodeLogEntry g_gcode_log[GCODE_LOG_LINES] = {};
// Monotonic line counter. Only written from the USB thread (the
// lineBufferHook callsite); readers (WS handlers on tcpip thread) only
// load. Atomic gives us the release/acquire ordering for the entry
// payload that's written before the counter advances.
std::atomic<uint32_t> g_gcode_log_write_idx { 0 };

} // namespace

bool dispatch_klipper_command(const char *line) {
    return try_dispatch_klipper_command(line);
}

void publish_gcode_response_line(const char *buf, int size) {
    if (size <= 0 || !buf) return;
    // Strip trailing newlines/CR (Fluidd's console renders one line per
    // notification; embedded \n becomes ugly).
    while (size > 0 && (buf[size - 1] == '\n' || buf[size - 1] == '\r')) {
        --size;
    }
    if (size <= 0) return;

    uint32_t w = g_gcode_log_write_idx.load(std::memory_order_relaxed);
    auto &entry = g_gcode_log[w % GCODE_LOG_LINES];
    const size_t copy_len = std::min<size_t>(static_cast<size_t>(size), GCODE_LOG_LINE_LEN - 1);
    std::memcpy(entry.data, buf, copy_len);
    entry.data[copy_len] = '\0';
    entry.len = static_cast<uint8_t>(copy_len);
    g_gcode_log_write_idx.store(w + 1, std::memory_order_release);
}

void publish_filelist_event(FilelistAction action, const char *path) {
    g_filelist_last_action = action;
    std::strncpy(g_filelist_last_path, path ? path : "", sizeof g_filelist_last_path - 1);
    g_filelist_last_path[sizeof g_filelist_last_path - 1] = '\0';
    g_filelist_epoch.fetch_add(1, std::memory_order_release);
}

namespace {

// ----------------------------------------------------------------------------
// SHA1 + base64 for Sec-WebSocket-Accept
// ----------------------------------------------------------------------------

void compute_ws_accept(std::string_view client_key, std::array<char, 32> &out) {
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts_ret(&ctx);
    mbedtls_sha1_update_ret(&ctx, reinterpret_cast<const uint8_t *>(client_key.data()), client_key.size());
    mbedtls_sha1_update_ret(&ctx, reinterpret_cast<const uint8_t *>(WS_MAGIC), std::strlen(WS_MAGIC));
    uint8_t sha[20];
    mbedtls_sha1_finish_ret(&ctx, sha);
    mbedtls_sha1_free(&ctx);

    size_t out_len = 0;
    out.fill(0);
    // 28 chars + '\0' fits in 32.
    mbedtls_base64_encode(reinterpret_cast<uint8_t *>(out.data()), out.size(), &out_len, sha, sizeof sha);
}

// Build the 101 Switching Protocols response. The Sec-WebSocket-Accept
// is the pre-computed base64 string.
size_t build_handshake_response(uint8_t *out_buf, size_t out_buf_len, const char *accept) {
    int written = snprintf(reinterpret_cast<char *>(out_buf), out_buf_len,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept);
    if (written < 0 || static_cast<size_t>(written) >= out_buf_len) {
        return 0;
    }
    return static_cast<size_t>(written);
}

// Build an outgoing WS frame: opcode + payload. We never mask (server side).
// Returns total bytes written to out_buf.
size_t emit_ws_frame(uint8_t *out_buf, size_t out_buf_len, uint8_t opcode, bool fin, const uint8_t *payload, size_t payload_len) {
    size_t header_len = 2;
    if (payload_len >= 126) {
        header_len = 4;
    }
    if (header_len + payload_len > out_buf_len) {
        return 0;
    }
    out_buf[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    if (payload_len < 126) {
        out_buf[1] = static_cast<uint8_t>(payload_len);
    } else {
        out_buf[1] = 126;
        uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
        std::memcpy(out_buf + 2, &l, sizeof l);
    }
    if (payload_len > 0 && payload != nullptr) {
        std::memcpy(out_buf + header_len, payload, payload_len);
    }
    return header_len + payload_len;
}

// Tiny JSON helper: find the value substring of a top-level "key":<value>
// pair. Returns true if found; sets value_out to the substring (not
// unescaped; only the raw value range). For string values, the surrounding
// quotes ARE included.
//
// This is NOT a full JSON parser, just enough for the small JSON-RPC
// payloads Moonraker clients send.
bool find_json_value(std::string_view body, std::string_view key, std::string_view &value_out) {
    // Search for "key" followed by colon (possibly with whitespace).
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%.*s\"", static_cast<int>(key.size()), key.data());
    if (n <= 0 || static_cast<size_t>(n) >= sizeof needle) {
        return false;
    }
    auto pos = body.find(std::string_view(needle, n));
    if (pos == std::string_view::npos) {
        return false;
    }
    pos += n;
    // Skip whitespace then ':' then whitespace.
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != ':') {
        return false;
    }
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) {
        ++pos;
    }
    // Now find the end of the value. We support: string ("…"), number,
    // true/false/null, object {…}, array […]. We compute end by scanning
    // and balancing.
    size_t start = pos;
    if (pos >= body.size()) {
        return false;
    }
    char c = body[pos];
    if (c == '"') {
        ++pos;
        while (pos < body.size() && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < body.size()) {
                pos += 2;
            } else {
                ++pos;
            }
        }
        if (pos < body.size()) {
            ++pos; // closing quote
        }
    } else if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (pos < body.size()) {
            char ch = body[pos];
            if (ch == '"') {
                // skip string
                ++pos;
                while (pos < body.size() && body[pos] != '"') {
                    if (body[pos] == '\\' && pos + 1 < body.size()) {
                        pos += 2;
                    } else {
                        ++pos;
                    }
                }
                if (pos < body.size()) {
                    ++pos;
                }
                continue;
            }
            if (ch == open) {
                ++depth;
            } else if (ch == close) {
                --depth;
                if (depth == 0) {
                    ++pos;
                    break;
                }
            }
            ++pos;
        }
    } else {
        while (pos < body.size() && body[pos] != ',' && body[pos] != '}' && body[pos] != ']' && body[pos] != ' ' && body[pos] != '\t' && body[pos] != '\r' && body[pos] != '\n') {
            ++pos;
        }
    }
    value_out = body.substr(start, pos - start);
    return true;
}

// Pulls out a string "method":"<value>" — value WITHOUT quotes.
bool find_string_value(std::string_view body, std::string_view key, std::string_view &value_out) {
    std::string_view raw;
    if (!find_json_value(body, key, raw)) {
        return false;
    }
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        return false;
    }
    value_out = raw.substr(1, raw.size() - 2);
    return true;
}

// Pulls out an integer "id":N.
bool find_int_value(std::string_view body, std::string_view key, int &value_out) {
    std::string_view raw;
    if (!find_json_value(body, key, raw)) {
        return false;
    }
    char buf[16];
    if (raw.size() >= sizeof buf) {
        return false;
    }
    std::memcpy(buf, raw.data(), raw.size());
    buf[raw.size()] = '\0';
    char *end = nullptr;
    long v = std::strtol(buf, &end, 10);
    if (end == buf) {
        return false;
    }
    value_out = static_cast<int>(v);
    return true;
}

} // namespace

// ----------------------------------------------------------------------------
// Global frame-buffer pool.
//
// Moves frame_buf out of WebSocketHandler so the handler doesn't dominate
// the ConnectionState variant size (which limits tcpip stack headroom —
// see feedback_buddy_internals.md). Handler now holds just an int8_t
// slot index. Each slot is owned for the handler's lifetime; allocated
// in the constructor, released in the destructor. Linear search across
// MAX_SLOTS (=3) is fast.
//
// Thread model: ALL allocations and releases happen on the tcpip thread
// (constructor/destructor of nhttp ConnectionState handlers). No locking
// is needed; the bool array is touched only from that thread.
// ----------------------------------------------------------------------------
namespace {
std::array<std::array<uint8_t, WebSocketHandler::FRAME_BUF_SIZE>,
           WebSocketHandler::MAX_SLOTS> g_ws_frame_pool {};
std::array<bool, WebSocketHandler::MAX_SLOTS> g_ws_slot_used { false, false, false };

int8_t allocate_frame_slot() {
    for (int8_t i = 0; i < WebSocketHandler::MAX_SLOTS; ++i) {
        if (!g_ws_slot_used[i]) {
            g_ws_slot_used[i] = true;
            return i;
        }
    }
    return -1;
}

void release_frame_slot(int8_t slot) {
    if (slot >= 0 && slot < WebSocketHandler::MAX_SLOTS) {
        g_ws_slot_used[slot] = false;
    }
}
} // namespace

WebSocketHandler::WebSocketHandler(const handler::RequestParser &request) {
    pool_slot = allocate_frame_slot();
    compute_accept(request);
    // If no API key is configured, no auth needed — start authenticated.
    const char *api_key = httpd_instance()->get_password();
    is_authenticated = (api_key == nullptr || *api_key == '\0');
}

WebSocketHandler::~WebSocketHandler() {
    release_frame_slot(pool_slot);
    pool_slot = -1;
}

WebSocketHandler::WebSocketHandler(WebSocketHandler &&other) noexcept
    : state(other.state), frame_state(other.frame_state),
      ws_accept(other.ws_accept), handshake_pos(other.handshake_pos),
      pool_slot(other.pool_slot), frame_buf_used(other.frame_buf_used),
      frame_opcode(other.frame_opcode), frame_fin(other.frame_fin),
      frame_masked(other.frame_masked),
      frame_payload_len(other.frame_payload_len),
      frame_payload_consumed(other.frame_payload_consumed),
      response_len(other.response_len),
      buf_holds_response(other.buf_holds_response),
      subscriptions(other.subscriptions),
      last_push_hash(other.last_push_hash),
      last_push_ms(other.last_push_ms),
      klippy_ready_sent(other.klippy_ready_sent),
      last_filelist_epoch(other.last_filelist_epoch),
      gcode_log_read_idx(other.gcode_log_read_idx),
      gcode_log_read_initialized(other.gcode_log_read_initialized),
      last_proc_stat_ms(other.last_proc_stat_ms) {
    std::memcpy(frame_mask, other.frame_mask, sizeof frame_mask);
    other.pool_slot = -1;  // Source must NOT release the slot we took.
}

WebSocketHandler &WebSocketHandler::operator=(WebSocketHandler &&other) noexcept {
    if (this != &other) {
        release_frame_slot(pool_slot);
        state = other.state;
        frame_state = other.frame_state;
        ws_accept = other.ws_accept;
        handshake_pos = other.handshake_pos;
        pool_slot = other.pool_slot;
        frame_buf_used = other.frame_buf_used;
        frame_opcode = other.frame_opcode;
        frame_fin = other.frame_fin;
        frame_masked = other.frame_masked;
        frame_payload_len = other.frame_payload_len;
        std::memcpy(frame_mask, other.frame_mask, sizeof frame_mask);
        frame_payload_consumed = other.frame_payload_consumed;
        response_len = other.response_len;
        buf_holds_response = other.buf_holds_response;
        subscriptions = other.subscriptions;
        last_push_hash = other.last_push_hash;
        last_push_ms = other.last_push_ms;
        klippy_ready_sent = other.klippy_ready_sent;
        last_filelist_epoch = other.last_filelist_epoch;
        gcode_log_read_idx = other.gcode_log_read_idx;
        gcode_log_read_initialized = other.gcode_log_read_initialized;
        last_proc_stat_ms = other.last_proc_stat_ms;
        other.pool_slot = -1;
    }
    return *this;
}

uint8_t *WebSocketHandler::frame_buf_data() {
    return g_ws_frame_pool[pool_slot].data();
}

const uint8_t *WebSocketHandler::frame_buf_data() const {
    return g_ws_frame_pool[pool_slot].data();
}

uint8_t &WebSocketHandler::frame_buf_at(size_t i) {
    return g_ws_frame_pool[pool_slot][i];
}

const uint8_t &WebSocketHandler::frame_buf_at(size_t i) const {
    return g_ws_frame_pool[pool_slot][i];
}

void WebSocketHandler::compute_accept(const handler::RequestParser &request) {
    compute_ws_accept(
        std::string_view(request.sec_websocket_key.data(), request.sec_websocket_key_size),
        ws_accept);
}

bool WebSocketHandler::want_read() const {
    // Don't read while we have an unsent response — frame_buf is busy.
    if (buf_holds_response) return false;
    return state != State::Closed;
}

bool WebSocketHandler::want_write() const {
    if (state == State::SendingHandshake) return true;
    if (buf_holds_response) return true;
    // In poll-based push, we want step() called with an out_buf whenever
    // enough time has passed that a push might be due. Returning true
    // unconditionally in Framing would busy-loop the server; gate on a
    // simple time check (cheap — just one ticks_ms() read). If the hash
    // hasn't changed by the time step() runs, it just returns 0.
    if (state == State::Framing) {
        if (!klippy_ready_sent) return true;
        // Mid-stream mesh push: keep step() firing so we drain the
        // remaining chunks back-to-back (no waiting on poll cadence).
        if (mesh_send_total > 0) return true;
        const uint32_t now = ticks_ms();
        if (now - last_push_ms >= PUSH_MIN_GAP_MS) return true;
    }
    return false;
}

size_t WebSocketHandler::write_handshake(uint8_t *out_buf, size_t out_buf_len) {
    return build_handshake_response(out_buf, out_buf_len, ws_accept.data());
}

size_t WebSocketHandler::render_jsonrpc_result(uint8_t *out_buf, size_t out_buf_len, int id, const char *result) {
    int n = snprintf(reinterpret_cast<char *>(out_buf), out_buf_len,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}",
        id, result);
    if (n < 0 || static_cast<size_t>(n) >= out_buf_len) {
        return 0;
    }
    return static_cast<size_t>(n);
}

size_t WebSocketHandler::render_jsonrpc_error(uint8_t *out_buf, size_t out_buf_len, int id, int code, const char *msg) {
    int n = snprintf(reinterpret_cast<char *>(out_buf), out_buf_len,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
        id, code, msg);
    if (n < 0 || static_cast<size_t>(n) >= out_buf_len) {
        return 0;
    }
    return static_cast<size_t>(n);
}

size_t WebSocketHandler::render_history_list(int id) {
    char *buf_start = reinterpret_cast<char *>(frame_buf_data()) + 4;
    const size_t cap = FRAME_BUF_SIZE - 4;

    int n = snprintf(buf_start, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"count\":%u,\"jobs\":[",
        id, static_cast<unsigned>(g_history_count));
    if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
    size_t used = static_cast<size_t>(n);

    for (size_t i = 0; i < g_history_count; ++i) {
        const auto &e = g_history[i];
        // Reserve room for closing brackets.
        if (used + 250 >= cap) break; // safety stop
        int en = snprintf(buf_start + used, cap - used,
            "%s{\"job_id\":\"%06u\",\"exists\":true,\"end_time\":%u,\"filament_used\":0,"
            "\"filename\":\"%s\",\"metadata\":{},\"print_duration\":%u,"
            "\"status\":\"%s\",\"start_time\":%u,\"total_duration\":%u}",
            (i == 0) ? "" : ",",
            static_cast<unsigned>(e.job_id),
            static_cast<unsigned>(e.start_time + e.total_duration),
            e.filename,
            static_cast<unsigned>(e.total_duration),
            e.status,
            static_cast<unsigned>(e.start_time),
            static_cast<unsigned>(e.total_duration));
        if (en <= 0) break;
        used += static_cast<size_t>(en);
    }

    int cn = snprintf(buf_start + used, cap - used, "]}}");
    if (cn <= 0 || used + static_cast<size_t>(cn) >= cap) return 0;
    used += static_cast<size_t>(cn);

    const size_t payload_len = used;
    const size_t header_len = (payload_len < 126) ? 2 : 4;
    if (header_len != 4) {
        std::memmove(frame_buf_data() + header_len, buf_start, payload_len);
    }
    frame_buf_at(0) = 0x81;
    if (payload_len < 126) {
        frame_buf_at(1) = static_cast<uint8_t>(payload_len);
    } else {
        frame_buf_at(1) = 126;
        uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
        std::memcpy(frame_buf_data() + 2, &l, sizeof l);
    }
    return header_len + payload_len;
}

size_t WebSocketHandler::render_database_get_item(int id, const char *key) {
    if (!key || !*key) return 0; // namespace-dump unsupported; caller errors

    // Read into a scratch right after the JSON envelope we'll write.
    char *buf_start = reinterpret_cast<char *>(frame_buf_data()) + 4;
    const size_t cap = FRAME_BUF_SIZE - 4;

    // Reserve some bytes for the envelope; leave the rest for value bytes.
    // Envelope: {"jsonrpc":"2.0","id":N,"result":{"namespace":"fluidd","key":"<k>","value":<value>}}
    int prelude = snprintf(buf_start, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"namespace\":\"fluidd\",\"key\":\"%s\",\"value\":", id, key);
    if (prelude <= 0 || static_cast<size_t>(prelude) >= cap) return 0;

    // Read file contents directly into the place where the value goes.
    size_t value_len = db_read({key, std::strlen(key)},
        buf_start + prelude, cap - prelude - 4 /* room for "}}" + safety */);

    // If file doesn't exist, fall back to a default for keys we want
    // to pre-seed. Today: "macros" — categorize the built-in macros
    // (PREHEAT_*, COOL_DOWN, HOME_ALL, MOTORS_OFF) so they appear in
    // Fluidd's Macros card under "Heating" and "Motion" instead of
    // "Uncategorized". The user can re-arrange via Fluidd's UI;
    // their changes get persisted on the first non-empty POST, and
    // subsequent GETs read the persisted file instead of this default.
    if (value_len == 0 && std::strcmp(key, "macros") == 0) {
        static constexpr char kMacrosDefault[] =
            "{\"stored\":["
                "{\"name\":\"PREHEAT_PLA\",\"visible\":true,\"categoryId\":\"heating\"},"
                "{\"name\":\"PREHEAT_PETG\",\"visible\":true,\"categoryId\":\"heating\"},"
                "{\"name\":\"PREHEAT_ABS\",\"visible\":true,\"categoryId\":\"heating\"},"
                "{\"name\":\"COOL_DOWN\",\"visible\":true,\"categoryId\":\"heating\"},"
                "{\"name\":\"HOME_ALL\",\"visible\":true,\"categoryId\":\"motion\"},"
                "{\"name\":\"MOTORS_OFF\",\"visible\":true,\"categoryId\":\"motion\"}"
            "],\"categories\":["
                "{\"id\":\"heating\",\"name\":\"Heating\"},"
                "{\"id\":\"motion\",\"name\":\"Motion\"}"
            "],\"expanded\":[\"heating\",\"motion\"]}";
        constexpr size_t default_len = sizeof(kMacrosDefault) - 1;
        if (static_cast<size_t>(prelude) + default_len + 4 < cap) {
            std::memcpy(buf_start + prelude, kMacrosDefault, default_len);
            value_len = default_len;
        }
    }

    if (value_len == 0) return 0;

    // Append closing braces.
    size_t total = static_cast<size_t>(prelude) + value_len;
    if (total + 2 >= cap) return 0;
    buf_start[total++] = '}';
    buf_start[total++] = '}';

    const size_t payload_len = total;
    const size_t header_len = (payload_len < 126) ? 2 : 4;
    if (header_len != 4) {
        std::memmove(frame_buf_data() + header_len, buf_start, payload_len);
    }
    frame_buf_at(0) = 0x81;
    if (payload_len < 126) {
        frame_buf_at(1) = static_cast<uint8_t>(payload_len);
    } else {
        frame_buf_at(1) = 126;
        uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
        std::memcpy(frame_buf_data() + 2, &l, sizeof l);
    }
    return header_len + payload_len;
}

size_t WebSocketHandler::render_objects_subscribe_response(int id) {
    // Build directly into frame_buf (reserving 4 bytes for the WS header).
    // The payload mirrors what stock Moonraker sends to Fluidd at
    // subscribe time: webhooks + idle_timeout + heaters + configfile,
    // where configfile.settings carries the macro definitions Fluidd's
    // Macros panel reads.
    char *buf_start = reinterpret_cast<char *>(frame_buf_data()) + 4;
    const size_t cap = FRAME_BUF_SIZE - 4;

    // Filament sensor (extruder side) — read live state from the
    // FilamentSensors singleton. Safe from tcpip thread per
    // filament_sensors_handler.hpp (state stored in std::atomic).
    const FilamentSensorState fs_state = FSensors_instance().sensor_state(LogicalFilamentSensor::extruder);
    const bool fs_detected = (fs_state == FilamentSensorState::HasFilament);
    const bool fs_enabled = (fs_state != FilamentSensorState::Disabled
                          && fs_state != FilamentSensorState::NotInitialized);
    // Endstop state: read Marlin's live_state (atomic on Cortex-M4)
    // and pluck out X/Y/Z bits. On CoreOne X/Y endstops are loadcell
    // home-detection (binary force threshold) and Z is the loadcell
    // touch — all read through the same endstops.state() interface.
    const uint8_t es = static_cast<uint8_t>(endstops.state());
    const int es_x = (es >> X_MIN) & 1;
    const int es_y = (es >> Y_MIN) & 1;
    const int es_z = (es >> Z_MIN) & 1;
    // Filtration fan only when configured (see render_notify_status_update).
    const char *filtration_field_subscribe = "";
#if XBUDDY_EXTENSION_VARIANT_IS_STANDARD()
    if (buddy::chamber_filtration().is_enabled()) {
        filtration_field_subscribe =
            "\"fan_generic filtration_fan\":{\"speed\":0.0,\"rpm\":0},";
    }
#endif

    int n = snprintf(buf_start, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"eventtime\":0,\"status\":{"
        "\"webhooks\":{\"state\":\"ready\",\"state_message\":\"\"},"
        "\"idle_timeout\":{\"state\":\"Idle\"},"
        "\"heaters\":{\"available_heaters\":[\"extruder\",\"heater_bed\"],"
                     "\"available_sensors\":[\"temperature_sensor chamber\",\"temperature_sensor heatbreak\"],"
                     "\"available_monitors\":[]},"
        "\"heater_fan heatbreak\":{\"speed\":0.0,\"rpm\":0},"
        "\"fan_generic chamber_fan_1\":{\"speed\":0.0,\"rpm\":0},"
        "\"fan_generic chamber_fan_2\":{\"speed\":0.0,\"rpm\":0},"
        "%s"  // filtration_fan field if configured
        "\"output_pin chamber_led\":{\"value\":0.0},"
        "\"filament_switch_sensor extruder\":{\"filament_detected\":%s,\"enabled\":%s},"
        "\"query_endstops\":{\"last_query\":{\"x\":%d,\"y\":%d,\"z\":%d}},"
        "%s,%s,%s"
        "}}}",
        id,
        filtration_field_subscribe,
        fs_detected ? "true" : "false",
        fs_enabled ? "true" : "false",
        es_x, es_y, es_z,
        kTopLevelKinematicsJson, kTopLevelMacrosJson, kConfigfileSettingsJson);
    if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
    size_t payload_len = static_cast<size_t>(n);

    size_t header_len = (payload_len < 126) ? 2 : 4;
    if (header_len != 4) {
        std::memmove(frame_buf_data() + header_len, buf_start, payload_len);
    }
    frame_buf_at(0) = 0x81;
    if (payload_len < 126) {
        frame_buf_at(1) = static_cast<uint8_t>(payload_len);
    } else {
        frame_buf_at(1) = 126;
        uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
        std::memcpy(frame_buf_data() + 2, &l, sizeof l);
    }
    return header_len + payload_len;
}

size_t WebSocketHandler::render_files_get_directory(int id, const char *path) {
    // Fluidd queries by root: "gcodes" (file panel), "config" (settings tab),
    // "logs" (Other Files > Logs), "config_examples" (Other Files > Examples),
    // "docs" (Other Files > Docs). Empty path → gcodes (root).
    // We physically have files only under /usb (root "gcodes"); the other
    // roots are advertised so Fluidd's tabs don't error out, but they return
    // an empty list with their proper root_info name so "logs root is not
    // available" disappears.
    const bool empty_path = (path[0] == '\0');
    const bool root_gcodes = empty_path || std::strcmp(path, "gcodes") == 0;
    const bool root_known = root_gcodes
                         || std::strcmp(path, "config") == 0
                         || std::strcmp(path, "logs") == 0
                         || std::strcmp(path, "config_examples") == 0
                         || std::strcmp(path, "docs") == 0;
    if (!root_known) return 0;  // unknown root → error path in caller
    const bool list_usb = root_gcodes;
    const char *root_name = root_gcodes ? "gcodes" : path;

    char *buf_start = reinterpret_cast<char *>(frame_buf_data()) + 4; // reserve 4-byte WS header
    const size_t cap = FRAME_BUF_SIZE - 4;

    int n = snprintf(buf_start, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"dirs\":[],\"files\":[", id);
    if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
    size_t used = static_cast<size_t>(n);

    // Leave room for the closing envelope (root_info + disk_usage + braces).
    // Closing envelope size: `],"disk_usage":{"total":..,"used":..,"free":..},
    // "root_info":{"name":"config_examples","permissions":"rw"}}}`
    // ≈ 60 boilerplate + 3 × 20-digit uint64s + 16-char root name = ~140.
    // 192 leaves headroom for future fields without breaking the response.
    constexpr size_t CLOSING_RESERVE = 192;

    uint64_t accumulated_file_size = 0;
    if (list_usb) {
        DIR *d = opendir("/usb");
        if (d != nullptr) {
            dirent *e;
            bool first = true;
            while ((e = readdir(d)) != nullptr) {
                // Skip hidden, only regular files.
                if (e->d_name[0] == '.' || e->d_type != DT_REG) continue;
                char path_buf[120];
                int pn = snprintf(path_buf, sizeof path_buf, "/usb/%s", e->d_name);
                if (pn < 0 || static_cast<size_t>(pn) >= sizeof path_buf) continue;
                struct stat st {};
                if (stat(path_buf, &st) != 0) continue;
                accumulated_file_size += static_cast<uint64_t>(st.st_size);
                // Note: no JSON-escape on d_name. FAT files in /usb/ are
                // typically ASCII-safe (8.3 + LFN); a name containing `"`
                // or `\` would produce invalid JSON. TODO: escape.
                int en = snprintf(buf_start + used, cap - used,
                    "%s{\"path\":\"%s\",\"modified\":%ld,\"size\":%ld,\"permissions\":\"rw\"}",
                    first ? "" : ",",
                    e->d_name,
                    static_cast<long>(st.st_mtime),
                    static_cast<long>(st.st_size));
                if (en < 0) break;
                if (used + static_cast<size_t>(en) + CLOSING_RESERVE >= cap) {
                    // Truncate silently — the response buffer is small.
                    // Real solution is segmented JSON rendering across step()
                    // calls; this implementation is a Phase 3c compromise.
                    break;
                }
                used += static_cast<size_t>(en);
                first = false;
            }
            closedir(d);
        }
    }

    // Real disk usage via statvfs. Buddy's FAT/littlefs statvfs only
    // populates the FREE side (f_bfree/f_bavail), not f_blocks — and per
    // Buddy convention f_frsize is cluster size in *sectors* and f_bsize
    // is the sector size, so free bytes = f_bavail × f_frsize × f_bsize.
    // (See src/connect/marlin_printer.cpp for the same arithmetic.) Total
    // isn't directly available; approximate it as (visible files sum + free).
    // Walking subdirectories for a perfect total isn't worth the cost
    // for a status display.
    uint64_t free_bytes = 0;
    if (struct statvfs sv {}; statvfs("/usb/", &sv) == 0) {
        free_bytes = static_cast<uint64_t>(sv.f_bavail)
                   * static_cast<uint64_t>(sv.f_frsize)
                   * static_cast<uint64_t>(sv.f_bsize);
    }
    const uint64_t used_bytes = accumulated_file_size;
    const uint64_t total_bytes = used_bytes + free_bytes;

    int cn = snprintf(buf_start + used, cap - used,
        "],\"disk_usage\":{\"total\":%llu,\"used\":%llu,\"free\":%llu},"
        "\"root_info\":{\"name\":\"%s\",\"permissions\":\"rw\"}}}",
        static_cast<unsigned long long>(total_bytes),
        static_cast<unsigned long long>(used_bytes),
        static_cast<unsigned long long>(free_bytes),
        root_name);
    if (cn < 0 || used + static_cast<size_t>(cn) >= cap) return 0;
    used += static_cast<size_t>(cn);

    // Apply WS frame header in front of the rendered payload.
    size_t payload_len = used;
    size_t header_len = (payload_len < 126) ? 2 : 4;
    if (header_len != 4) {
        std::memmove(frame_buf_data() + header_len, buf_start, payload_len);
    }
    frame_buf_at(0) = 0x81; // FIN=1, opcode=text
    if (payload_len < 126) {
        frame_buf_at(1) = static_cast<uint8_t>(payload_len);
    } else {
        frame_buf_at(1) = 126;
        uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
        std::memcpy(frame_buf_data() + 2, &l, sizeof l);
    }
    return header_len + payload_len;
}

size_t WebSocketHandler::render_notify_status_update(uint8_t *out_buf, size_t out_buf_len) {
    // Read once into stack locals to minimize cross-thread inconsistency.
    // marlin_vars is read here on the tcpip thread; the marlin thread
    // writes these fields. They're 32-bit scalars, so torn reads on ARM
    // Cortex-M4 aren't a concern for individual fields. The set may be
    // briefly out of sync, but Fluidd is resilient to that.
    auto &vars = marlin_vars();
    const float nozzle_t = vars.active_hotend().temp_nozzle;
    const float nozzle_tgt = vars.active_hotend().target_nozzle;
    const float bed_t = vars.temp_bed;
    const float bed_tgt = vars.target_bed;
    const uint32_t print_duration = vars.print_duration;
    const uint8_t sd_pct = vars.sd_percent_done;
    const float pos_x = vars.logical_curr_pos[0];
    const float pos_y = vars.logical_curr_pos[1];
    const float pos_z = vars.logical_curr_pos[2];
    // Tune-view fields. Moonraker exposes speed/flow as ratios (1.0 = 100%);
    // Marlin stores them as integer percent. Fan is 0..255 in Marlin, 0..1
    // for the Moonraker fan.speed object.
    const float speed_factor = static_cast<float>(vars.print_speed) / 100.0f;
    const float extrude_factor = static_cast<float>(vars.active_hotend().flow_factor) / 100.0f;
    const float fan_speed = static_cast<float>(vars.print_fan_speed) / 255.0f;
    const uint16_t print_fan_rpm = vars.active_hotend().print_fan_rpm;
    const uint16_t heatbreak_fan_rpm = vars.active_hotend().heatbreak_fan_rpm;
    const float max_accel = vars.travel_acceleration;
    // Chamber fans + LED (CoreOne with xbuddy_extension variant=STANDARD/iX).
    // Three fans: cooling_fan_1, cooling_fan_2, filtration_fan. Each
    // fan_rpm() returns an optional — empty when the puppy isn't
    // connected. Chamber white LED reads from SideStripHandler's W
    // channel (0-255).
    uint16_t chamber_fan_rpm = 0;
    uint16_t chamber_fan2_rpm = 0;
    uint8_t chamber_led_w = 0;
    // Filtration field is emitted only when configured (backend != none) —
    // a printer without the filtration accessory shouldn't surface a
    // "filtration_fan" object in Fluidd's UI at all.
    char filtration_field[80] = {};
#if XBUDDY_EXTENSION_VARIANT_IS_STANDARD()
    if (auto r = buddy::xbuddy_extension().fan_rpm(buddy::XBuddyExtension::Fan::cooling_fan_1)) {
        chamber_fan_rpm = *r;
    }
    if (auto r = buddy::xbuddy_extension().fan_rpm(buddy::XBuddyExtension::Fan::cooling_fan_2)) {
        chamber_fan2_rpm = *r;
    }
    chamber_led_w = leds::SideStripHandler::instance().color().w;
    if (buddy::chamber_filtration().is_enabled()) {
        const uint16_t ff_rpm = buddy::xbuddy_extension()
            .fan_rpm(buddy::XBuddyExtension::Fan::filtration_fan).value_or(0);
        std::snprintf(filtration_field, sizeof filtration_field,
            "\"fan_generic filtration_fan\":{\"speed\":%.1f,\"rpm\":%u},",
            ff_rpm > 0 ? 1.0 : 0.0,
            static_cast<unsigned>(ff_rpm));
    }
#endif
    // Chamber sensor: see notes in compute_push_hash for why we use
    // thermistor_temperature() and not current_temperature().
    const float chamber_t = buddy::chamber().thermistor_temperature().value_or(0.0f);
    // Heatbreak (hotend cold-side) — MarlinVariable<float>, scalar
    // atomic read is safe from tcpip thread.
    const float heatbreak_t = vars.active_hotend().temp_heatbreak;

    // Print state: map Buddy's DeviceState onto Moonraker print_stats
    // state strings. Fluidd treats "printing"/"paused" as active and
    // shows controls; "standby"/"complete"/"cancelled" as idle.
    const char *print_state_str;
    switch (printer_state::get_state(false)) {
    case printer_state::DeviceState::Printing: print_state_str = "printing"; break;
    case printer_state::DeviceState::Paused: print_state_str = "paused"; break;
    case printer_state::DeviceState::Finished: print_state_str = "complete"; break;
    case printer_state::DeviceState::Stopped: print_state_str = "cancelled"; break;
    case printer_state::DeviceState::Attention: print_state_str = "error"; break;
    default: print_state_str = "standby"; break;
    }
    // Filename: prefer the LFN (user-readable) when available. We must
    // NOT call MarlinVariableString::get_ptr() from the tcpip thread —
    // it bsod's anywhere other than the marlin server task (the pointer
    // would be unstable for any other thread). copy_to() is the
    // lock-acquiring API safe for cross-thread reads.
    char filename_buf[64] = {};
    vars.media_LFN.copy_to(filename_buf, sizeof filename_buf);
    const char *filename = filename_buf;
    // Strip leading "/usb/" so Fluidd shows a relative path like
    // the rest of Moonraker.
    if (std::strncmp(filename, "/usb/", 5) == 0) {
        filename += 5;
    }
    // Filament sensor live state (atomic load, tcpip-safe).
    const FilamentSensorState fs_state = FSensors_instance().sensor_state(LogicalFilamentSensor::extruder);
    const bool fs_detected = (fs_state == FilamentSensorState::HasFilament);
    const bool fs_enabled = (fs_state != FilamentSensorState::Disabled
                          && fs_state != FilamentSensorState::NotInitialized);
    // Endstop live state (atomic on Cortex-M4).
    const uint8_t es = static_cast<uint8_t>(endstops.state());
    const int es_x = (es >> X_MIN) & 1;
    const int es_y = (es >> Y_MIN) & 1;
    const int es_z = (es >> Z_MIN) & 1;

    int n = snprintf(reinterpret_cast<char *>(out_buf), out_buf_len,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notify_status_update\",\"params\":[{"
        "\"webhooks\":{\"state\":\"ready\"},"
        "\"extruder\":{\"temperature\":%.2f,\"target\":%.2f,\"pressure_advance\":0.0,\"smooth_time\":0.04},"
        "\"heater_bed\":{\"temperature\":%.2f,\"target\":%.2f},"
        "\"temperature_sensor chamber\":{\"temperature\":%.2f},"
        "\"temperature_sensor heatbreak\":{\"temperature\":%.2f},"
        "\"toolhead\":{\"position\":[%.2f,%.2f,%.2f,0],\"homed_axes\":\"xyz\",\"max_velocity\":200.0,\"max_accel\":%.1f,\"square_corner_velocity\":5.0,\"minimum_cruise_ratio\":0.5},"
        "\"gcode_move\":{\"speed_factor\":%.2f,\"extrude_factor\":%.2f,\"absolute_coordinates\":true,\"absolute_extrude\":false,\"speed\":1500.0},"
        "\"fan\":{\"speed\":%.3f,\"rpm\":%u},"
        "\"heater_fan heatbreak\":{\"speed\":%.1f,\"rpm\":%u},"
        "\"fan_generic chamber_fan_1\":{\"speed\":%.1f,\"rpm\":%u},"
        "\"fan_generic chamber_fan_2\":{\"speed\":%.1f,\"rpm\":%u},"
        "%s"  // filtration_fan (only when configured)
        "\"output_pin chamber_led\":{\"value\":%.3f},"
        "\"filament_switch_sensor extruder\":{\"filament_detected\":%s,\"enabled\":%s},"
        "\"query_endstops\":{\"last_query\":{\"x\":%d,\"y\":%d,\"z\":%d}},"
        "\"display_status\":{\"progress\":%.3f,\"message\":null},"
        "\"print_stats\":{\"state\":\"%s\",\"filename\":\"%s\",\"print_duration\":%lu.0,\"total_duration\":%lu.0,\"filament_used\":0.0,\"info\":{\"total_layer\":null,\"current_layer\":null}}"
        "},%lu.0]}",
        static_cast<double>(nozzle_t), static_cast<double>(nozzle_tgt),
        static_cast<double>(bed_t), static_cast<double>(bed_tgt),
        static_cast<double>(chamber_t),
        static_cast<double>(heatbreak_t),
        static_cast<double>(pos_x), static_cast<double>(pos_y), static_cast<double>(pos_z),
        static_cast<double>(max_accel),
        static_cast<double>(speed_factor), static_cast<double>(extrude_factor),
        static_cast<double>(fan_speed),
        static_cast<unsigned>(print_fan_rpm),
        heatbreak_fan_rpm > 0 ? 1.0 : 0.0,
        static_cast<unsigned>(heatbreak_fan_rpm),
        chamber_fan_rpm > 0 ? 1.0 : 0.0, static_cast<unsigned>(chamber_fan_rpm),
        chamber_fan2_rpm > 0 ? 1.0 : 0.0, static_cast<unsigned>(chamber_fan2_rpm),
        filtration_field,
        static_cast<double>(chamber_led_w) / 255.0,
        fs_detected ? "true" : "false",
        fs_enabled ? "true" : "false",
        es_x, es_y, es_z,
        static_cast<double>(sd_pct) / 100.0,
        print_state_str, filename,
        static_cast<unsigned long>(print_duration),
        static_cast<unsigned long>(print_duration),
        static_cast<unsigned long>(print_duration));
    if (n < 0 || static_cast<size_t>(n) >= out_buf_len) {
        return 0;
    }
    return static_cast<size_t>(n);
}

bool WebSocketHandler::try_parse_frame(std::string_view &input) {
    // Pull bytes into frame_buf until we have a full frame. Returns true
    // when frame_buf_used == header_len + payload_len for some complete
    // frame.
    auto copy_in = [&](size_t want) {
        size_t to_copy = std::min(want, input.size());
        std::memcpy(frame_buf_data() + frame_buf_used, input.data(), to_copy);
        frame_buf_used += to_copy;
        input.remove_prefix(to_copy);
    };

    if (frame_state == FrameState::WantHeader) {
        if (frame_buf_used < 2) {
            copy_in(2 - frame_buf_used);
        }
        if (frame_buf_used < 2) {
            return false;
        }
        uint8_t b0 = frame_buf_at(0);
        uint8_t b1 = frame_buf_at(1);
        frame_fin = (b0 & 0x80) != 0;
        frame_opcode = b0 & 0x0F;
        frame_masked = (b1 & 0x80) != 0;
        uint8_t len7 = b1 & 0x7F;
        if (len7 < 126) {
            frame_payload_len = len7;
            frame_state = frame_masked ? FrameState::WantMask : FrameState::WantPayload;
        } else if (len7 == 126) {
            frame_state = FrameState::WantExtLen;
        } else {
            // 64-bit length — refuse, MAX_FRAME_PAYLOAD < 65536 anyway
            // Treat as protocol error: close connection.
            state = State::Closing;
            return false;
        }
    }
    if (frame_state == FrameState::WantExtLen) {
        if (frame_buf_used < 4) {
            copy_in(4 - frame_buf_used);
        }
        if (frame_buf_used < 4) {
            return false;
        }
        uint16_t l;
        std::memcpy(&l, frame_buf_data() + 2, sizeof l);
        frame_payload_len = lwip_ntohs(l);
        if (frame_payload_len > MAX_FRAME_PAYLOAD) {
            state = State::Closing;
            return false;
        }
        frame_state = frame_masked ? FrameState::WantMask : FrameState::WantPayload;
    }
    if (frame_state == FrameState::WantMask) {
        // Mask is 4 bytes immediately following the length.
        size_t mask_offset = (frame_payload_len < 126) ? 2 : 4;
        if (frame_buf_used < mask_offset + 4) {
            copy_in((mask_offset + 4) - frame_buf_used);
        }
        if (frame_buf_used < mask_offset + 4) {
            return false;
        }
        std::memcpy(frame_mask, frame_buf_data() + mask_offset, 4);
        frame_state = FrameState::WantPayload;
        frame_payload_consumed = 0;
    }
    if (frame_state == FrameState::WantPayload) {
        size_t mask_offset = (frame_payload_len < 126) ? 2 : 4;
        size_t payload_offset = mask_offset + (frame_masked ? 4 : 0);
        size_t need = payload_offset + frame_payload_len;
        if (need > FRAME_BUF_SIZE) {
            state = State::Closing;
            return false;
        }
        if (frame_buf_used < need) {
            copy_in(need - frame_buf_used);
        }
        if (frame_buf_used < need) {
            return false;
        }
        // Unmask in-place.
        if (frame_masked) {
            uint8_t *p = frame_buf_data() + payload_offset;
            for (size_t i = 0; i < frame_payload_len; ++i) {
                p[i] ^= frame_mask[i & 3];
            }
        }
        return true;
    }
    return false;
}

namespace {
// Render a WS-framed text-payload response into a buffer.
// Returns total bytes (header + payload), or 0 on failure.
// Writes header at offset 0, payload immediately after.
size_t frame_text_response(uint8_t *buf, size_t buf_len, const char *result_payload, int id) {
    constexpr size_t HEADER_RESERVE = 4;
    if (buf_len <= HEADER_RESERVE) return 0;
    int n = snprintf(reinterpret_cast<char *>(buf) + HEADER_RESERVE, buf_len - HEADER_RESERVE,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id, result_payload);
    if (n < 0 || static_cast<size_t>(n) >= buf_len - HEADER_RESERVE) return 0;
    size_t payload_len = static_cast<size_t>(n);
    size_t header_len = (payload_len < 126) ? 2 : 4;
    // Move payload into final position (right after the header).
    if (header_len != HEADER_RESERVE) {
        std::memmove(buf + header_len, buf + HEADER_RESERVE, payload_len);
    }
    buf[0] = 0x81; // FIN=1, opcode=text
    if (payload_len < 126) {
        buf[1] = static_cast<uint8_t>(payload_len);
    } else {
        buf[1] = 126;
        uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
        std::memcpy(buf + 2, &l, sizeof l);
    }
    return header_len + payload_len;
}

size_t frame_text_error(uint8_t *buf, size_t buf_len, int id, int code, const char *msg) {
    constexpr size_t HEADER_RESERVE = 4;
    if (buf_len <= HEADER_RESERVE) return 0;
    int n = snprintf(reinterpret_cast<char *>(buf) + HEADER_RESERVE, buf_len - HEADER_RESERVE,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}", id, code, msg);
    if (n < 0 || static_cast<size_t>(n) >= buf_len - HEADER_RESERVE) return 0;
    size_t payload_len = static_cast<size_t>(n);
    size_t header_len = (payload_len < 126) ? 2 : 4;
    if (header_len != HEADER_RESERVE) {
        std::memmove(buf + header_len, buf + HEADER_RESERVE, payload_len);
    }
    buf[0] = 0x81;
    if (payload_len < 126) {
        buf[1] = static_cast<uint8_t>(payload_len);
    } else {
        buf[1] = 126;
        uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
        std::memcpy(buf + 2, &l, sizeof l);
    }
    return header_len + payload_len;
}
} // namespace

size_t WebSocketHandler::handle_text_frame_into_buf() {
    // Read inbound body BEFORE we overwrite frame_buf.
    size_t mask_offset = (frame_payload_len < 126) ? 2 : 4;
    size_t payload_offset = mask_offset + (frame_masked ? 4 : 0);
    std::string_view body(reinterpret_cast<const char *>(frame_buf_data() + payload_offset), frame_payload_len);

    std::string_view method;
    int id = 0;
    bool have_id = find_int_value(body, "id", id);
    if (!find_string_value(body, "method", method)) {
        return 0;
    }

    // Extract `path` param up-front for file methods, so it survives the
    // frame_buf overwrite. Small stable buffer; "gcodes"/"config"/empty
    // cover all realistic values.
    char path_param[16] = {};
    {
        std::string_view path_sv;
        if (find_string_value(body, "path", path_sv) && path_sv.size() < sizeof path_param) {
            std::memcpy(path_param, path_sv.data(), path_sv.size());
        }
    }
    // Same for printer.print.start's `filename` param. Moonraker accepts
    // relative paths under the root (e.g. "foo.gcode"); we prepend /usb/
    // when handing off to marlin.
    char filename_param[128] = {};
    {
        std::string_view fn_sv;
        if (find_string_value(body, "filename", fn_sv) && fn_sv.size() < sizeof filename_param) {
            std::memcpy(filename_param, fn_sv.data(), fn_sv.size());
        }
    }
    // Same for server.database.* — extract key + raw value JSON BEFORE
    // we overwrite frame_buf with the response.
    char db_key_param[40] = {};
    {
        std::string_view k_sv;
        if (find_string_value(body, "key", k_sv) && k_sv.size() < sizeof db_key_param) {
            std::memcpy(db_key_param, k_sv.data(), k_sv.size());
        }
    }
    // Value can be any JSON type; capture the raw substring.
    std::string_view db_value_sv;
    find_json_value(body, "value", db_value_sv);

    // Decide which response without touching frame_buf yet. result_str
    // either points to a .rodata literal (most methods) or to scratch_buf
    // (built per-request, alive until we finish snprintf into frame_buf
    // below).
    const char *result_str = nullptr;
    char scratch_buf[224];
    bool is_error = false;
    int err_code = 0;
    const char *err_msg = nullptr;

    // Gate all RPCs except identify and the access.* family on
    // is_authenticated. identify is how a client authenticates; the
    // access.* methods are the alternative path Fluidd uses when its
    // WS is in "authenticating" state — it calls access.login over
    // WS to obtain a token rather than going through HTTP. server.info
    // is also allowed unauthenticated so clients can probe reachability.
    const bool method_open = (method == "server.connection.identify"
                           || method == "server.info"
                           || method == "access.login"
                           || method == "access.logout"
                           || method == "access.refresh_jwt"
                           || method == "access.oneshot_token"
                           || method == "access.info");
    if (!is_authenticated && !method_open) {
        is_error = true;
        err_code = -32001;
        err_msg = "Unauthorized";
    } else if (method == "server.info") {
        result_str = "{\"klippy_connected\":true,\"klippy_state\":\"ready\",\"components\":[\"klipper\",\"file_manager\",\"machine\",\"database\"],\"failed_components\":[],\"registered_directories\":[\"gcodes\",\"config\",\"logs\",\"config_examples\",\"docs\"],\"warnings\":[],\"websocket_count\":1,\"api_version\":[1,3,0],\"api_version_string\":\"1.3.0\"}";
    } else if (method == "server.connection.identify") {
        // Extract access_token from params and validate against the
        // printer's API key (raw) or our issued JWT. If the printer
        // has no API key, is_authenticated was already set true at
        // construction and this branch just records the identify.
        if (!is_authenticated) {
            std::string_view tok_sv;
            if (find_string_value(body, "access_token", tok_sv)
                && access_token_valid(tok_sv)) {
                is_authenticated = true;
            } else {
                is_error = true;
                err_code = -32001;
                err_msg = "Unauthorized";
            }
        }
        if (!is_error) {
            snprintf(scratch_buf, sizeof scratch_buf, "{\"connection_id\":%lu}",
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF));
            result_str = scratch_buf;
        }
    } else if (method == "server.config") {
        result_str = "{\"config\":{\"server\":{\"host\":\"0.0.0.0\",\"port\":80}}}";
    } else if (method == "server.database.get_item") {
        // If we have a stored value for this key, return it. Otherwise
        // return "Key not found" so Fluidd uses its baked-in defaults.
        // Custom render writes the value verbatim into frame_buf —
        // we can't use the static result_str path because stored
        // values can be arbitrary JSON up to a few hundred bytes.
        // No-key (namespace-wide) reads return "Key not found" — Fluidd
        // catches that, falls back to per-key fetches, and our per-key
        // render_database_get_item serves the macros default.
        //
        // We previously tried responding to the namespace-wide read
        // with a synthetic dump containing macros, but that hung
        // Fluidd's identify flow: connection got into a TCP-level
        // close loop every ~5s (close code 1006), reconnecting
        // indefinitely. Reason not fully isolated — possibly the
        // dump's shape made one of Fluidd's per-namespace migration
        // promises never resolve. Fluidd's per-key fallback path
        // works cleanly and gets the same default, so we leave the
        // namespace dump as an error.
        if (db_key_param[0] != '\0') {
            size_t resp = render_database_get_item(id, db_key_param);
            if (resp > 0) return resp;
        }
        is_error = true;
        err_code = -32601;
        err_msg = "Key not found";
    } else if (method == "server.database.post_item") {
        // Persist the value to /internal/fluidd_<key>.json. Special
        // case: Fluidd's init phase POSTs an empty `{}` for every key
        // it knows about (uiSettings, macros, layout, console, ...).
        // If we persist those, subsequent get_item returns success with
        // empty value, which Fluidd interprets as "user has explicitly
        // cleared this setting" and suppresses its built-in defaults
        // (including auto-discovery of gcode_macros from configfile).
        // So treat empty/whitespace-only/empty-object writes as
        // "clear" — delete any existing file so the next get_item
        // returns Key not found → defaults engage.
        if (db_key_param[0] == '\0') {
            is_error = true;
            err_code = -32602;
            err_msg = "key required";
        } else if (db_value_sv.empty()) {
            is_error = true;
            err_code = -32602;
            err_msg = "value required";
        } else {
            // Trim whitespace and check for the empty-object sentinel.
            std::string_view trimmed = db_value_sv;
            while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' || trimmed.front() == '\n' || trimmed.front() == '\r')) {
                trimmed.remove_prefix(1);
            }
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\n' || trimmed.back() == '\r')) {
                trimmed.remove_suffix(1);
            }
            const bool is_empty_obj = (trimmed == "{}" || trimmed == "[]" || trimmed.empty());
            std::string_view key_sv(db_key_param, std::strlen(db_key_param));
            if (is_empty_obj) {
                db_delete(key_sv);
            } else {
                db_write(key_sv, db_value_sv.data(), db_value_sv.size());
            }
            result_str = "{\"namespace\":\"fluidd\",\"key\":\"\",\"value\":{}}";
        }
    } else if (method == "server.database.delete_item") {
        if (db_key_param[0] != '\0') {
            db_delete({db_key_param, std::strlen(db_key_param)});
        }
        result_str = "{\"namespace\":\"fluidd\",\"key\":\"\",\"value\":{}}";
    } else if (method == "server.database.list") {
        result_str = "{\"namespaces\":[\"fluidd\"]}";
    } else if (method == "server.files.list") {
        result_str = "[]";
    } else if (method == "server.files.get_directory") {
        // Special path: render full WS frame into frame_buf and short-circuit.
        // method string_view dangles after this overwrite but we no longer
        // need it (dispatch decision is already made).
        return render_files_get_directory(id, path_param);
    } else if (method == "server.files.roots") {
        result_str = "[{\"name\":\"gcodes\",\"path\":\"/usb\",\"permissions\":\"rw\"},"
                     "{\"name\":\"config\",\"path\":\"/internal/config\",\"permissions\":\"rw\"},"
                     "{\"name\":\"logs\",\"path\":\"/internal/logs\",\"permissions\":\"r\"},"
                     "{\"name\":\"config_examples\",\"path\":\"/internal/config_examples\",\"permissions\":\"r\"},"
                     "{\"name\":\"docs\",\"path\":\"/internal/docs\",\"permissions\":\"r\"}]";
    } else if (method == "server.gcode_store") {
        result_str = "{\"gcode_store\":[]}";
    } else if (method == "server.temperature_store") {
        result_str = "{}";
    } else if (method == "server.history.list") {
        // Custom render — pulls from the in-memory history ring buffer.
        return render_history_list(id);
    } else if (method == "server.history.totals") {
        // Aggregate live across the current ring. Simple sum; we don't
        // track filament since marlin_vars doesn't expose it conveniently.
        uint32_t total_jobs = static_cast<uint32_t>(g_history_count);
        uint32_t total_time = 0, longest = 0;
        for (size_t i = 0; i < g_history_count; ++i) {
            total_time += g_history[i].total_duration;
            if (g_history[i].total_duration > longest) longest = g_history[i].total_duration;
        }
        char buf[180];
        snprintf(buf, sizeof buf,
            "{\"job_totals\":{\"total_jobs\":%u,\"total_time\":%u,\"total_print_time\":%u,"
            "\"total_filament_used\":0,\"longest_job\":%u,\"longest_print\":%u}}",
            total_jobs, total_time, total_time, longest, longest);
        std::strncpy(scratch_buf, buf, sizeof scratch_buf - 1);
        result_str = scratch_buf;
    } else if (method == "server.announcements.list") {
        result_str = "{\"entries\":[],\"feeds\":[]}";
    } else if (method == "server.webcams.list") {
        result_str = "{\"webcams\":[]}";
    } else if (method == "server.job_queue.status") {
        result_str = "{\"queued_jobs\":[],\"queue_state\":\"ready\"}";
    } else if (method == "printer.info") {
        result_str = "{\"state\":\"ready\",\"state_message\":\"Printer is ready\",\"hostname\":\"prusa-core-one-plus\",\"software_version\":\"buddy-firmware\",\"cpu_info\":\"STM32F427\"}";
    } else if (method == "printer.objects.list") {
        result_str = "{\"objects\":[\"webhooks\",\"heaters\",\"configfile\",\"gcode_move\",\"idle_timeout\",\"toolhead\",\"fan\",\"extruder\",\"heater_bed\",\"temperature_sensor chamber\",\"print_stats\",\"display_status\",\"virtual_sdcard\"]}";
    } else if (method == "printer.objects.subscribe") {
        subscriptions = SubAll;
        // Arm a multi-frame mesh push so Fluidd sees bed_mesh
        // immediately on connect. Safe now that MAX_FRAME_PAYLOAD
        // is capped at 1000 — each fragment is ≤ nhttp's per-conn
        // BUFF_SIZE (1024), so no wedge condition. The at-most-one-
        // chunk-per-step() throttle in section 4a1 of step() keeps
        // the buffer pool from being monopolized.
        render_mesh_into_buf();
        mesh_send_total = g_mesh_buf_size;
        mesh_send_pos = 0;
        return render_objects_subscribe_response(id);
    } else if (method == "printer.objects.query") {
        result_str = "{\"eventtime\":0,\"status\":{\"webhooks\":{\"state\":\"ready\",\"state_message\":\"\"},\"idle_timeout\":{\"state\":\"Idle\"},\"heaters\":{\"available_heaters\":[\"extruder\",\"heater_bed\"],\"available_sensors\":[\"temperature_sensor chamber\"]}}}";
    } else if (method == "printer.gcode.help") {
        result_str = "{}";
    } else if (method == "printer.print.start") {
        // Build the /usb/<filename> path. marlin's print_start does its
        // own validation, so we don't enforce too much here beyond the
        // length cap we already applied to filename_param.
        if (filename_param[0] == '\0') {
            is_error = true;
            err_code = -32602; // Invalid params
            err_msg = "filename required";
        } else {
            char path[160];
            // Strip any leading /usb/ so relative + absolute both work.
            const char *fn = filename_param;
            if (std::strncmp(fn, "/usb/", 5) == 0) fn += 5;
            snprintf(path, sizeof path, "/usb/%s", fn);
            marlin_client::print_start(path, marlin_server::PreviewSkipIfAble::all);
            result_str = "\"ok\"";
        }
    } else if (method == "printer.print.pause") {
        marlin_client::print_pause();
        result_str = "\"ok\"";
    } else if (method == "printer.print.resume") {
        marlin_client::print_resume();
        result_str = "\"ok\"";
    } else if (method == "printer.print.cancel") {
        marlin_client::print_abort();
        result_str = "\"ok\"";
    } else if (method == "machine.proc_stats") {
        // Real MCU die temperature from STM32's internal temp sensor.
        // AdcGet returns °C × 1 already, but the value is integer
        // (LL_ADC_CALC_TEMPERATURE returns int32). Skip if ADC vref
        // isn't ready (returns 0 in that case — value is just noise).
        const int32_t mcu_t = AdcGet::getMCUTemp();
        char buf[200];
        snprintf(buf, sizeof buf,
            "{\"moonraker_stats\":[],\"cpu_temp\":%ld,\"network\":{},"
            "\"system_cpu_usage\":{\"cpu\":0},\"system_uptime\":%lu.0,"
            "\"websocket_connections\":1}",
            static_cast<long>(mcu_t),
            static_cast<unsigned long>(ticks_ms() / 1000));
        // Stack-buffer string outlives the surrounding frame_text_response
        // snprintf below (same handle_text_frame_into_buf stack frame).
        // Copy to scratch_buf which is already declared in scope.
        std::strncpy(scratch_buf, buf, sizeof scratch_buf - 1);
        result_str = scratch_buf;
    } else if (method == "printer.network.stats") {
        // Diagnostic — returns nhttp server's running counters so we
        // can correlate the periodic network-death issue with TCP
        // congestion / buffer exhaustion. Sample over time:
        //   curl ws://.../websocket → call this every minute.
        const ::nhttp::NetworkStats ns = ::nhttp::get_network_stats();
        char buf[224];
        snprintf(buf, sizeof buf,
            "{\"altcp_write_failures\":%lu,\"buffer_starvations\":%lu,"
            "\"send_space_zero\":%lu,\"connection_aborts\":%lu,"
            "\"lwip_err_callbacks\":%lu,\"last_lwip_err\":%ld,"
            "\"uptime_s\":%lu}",
            static_cast<unsigned long>(ns.altcp_write_failures),
            static_cast<unsigned long>(ns.buffer_starvations),
            static_cast<unsigned long>(ns.send_space_zero),
            static_cast<unsigned long>(ns.connection_aborts),
            static_cast<unsigned long>(ns.lwip_err_callbacks),
            static_cast<long>(ns.last_lwip_err),
            static_cast<unsigned long>(ticks_ms() / 1000));
        std::strncpy(scratch_buf, buf, sizeof scratch_buf - 1);
        result_str = scratch_buf;
    } else if (method == "access.info") {
        result_str = "{\"default_source\":\"moonraker\",\"available_sources\":[\"moonraker\"],\"login_required\":true}";
    } else if (method == "access.login") {
        // Validate body's `password` against the printer API key.
        // On success, mark this connection authenticated and hand back
        // the JWT plus refresh token. Body lives in `body` (string_view).
        std::string_view password_sv;
        bool pwd_ok = false;
        if (find_string_value(body, "password", password_sv)) {
            const char *expected = httpd_instance()->get_password();
            if (expected && *expected != '\0') {
                const size_t elen = std::strlen(expected);
                pwd_ok = (password_sv.size() == elen
                       && std::memcmp(password_sv.data(), expected, elen) == 0);
            } else {
                pwd_ok = true; // no api_key configured → allow
            }
        }
        if (!pwd_ok) {
            is_error = true;
            err_code = -32001;
            err_msg = "Unauthorized";
        } else {
            is_authenticated = true;
            // Build the JWT-shaped token via the shared helper used by
            // /access/login over HTTP, so both paths return identical
            // strings (Fluidd stores them in localStorage and presents
            // them back on next identify).
            // access_token_valid() builds the JWT on first call via
            // build_session_jwt_if_needed(); we need to fetch the same
            // canonical string. Reuse moonraker_access's helper via
            // an HTTP loopback isn't possible — duplicate the JWT
            // build here by calling access_token_valid on a known-
            // invalid token (forces build) and then we can read it...
            // no — simpler: just take the api_key string as the token.
            // It's accepted by access_token_valid() too.
            const char *api_key = httpd_instance()->get_password();
            if (api_key && *api_key) {
                snprintf(scratch_buf, sizeof scratch_buf,
                    "{\"username\":\"buddy\",\"token\":\"%s\","
                    "\"refresh_token\":\"%s\","
                    "\"action\":\"user_logged_in\",\"source\":\"moonraker\"}",
                    api_key, api_key);
            } else {
                snprintf(scratch_buf, sizeof scratch_buf,
                    "{\"username\":\"buddy\",\"token\":\"\","
                    "\"refresh_token\":\"\","
                    "\"action\":\"user_logged_in\",\"source\":\"moonraker\"}");
            }
            result_str = scratch_buf;
        }
    } else if (method == "access.logout") {
        is_authenticated = false;
        result_str = "{\"action\":\"user_logged_out\",\"username\":\"buddy\"}";
    } else if (method == "access.refresh_jwt") {
        const char *api_key = httpd_instance()->get_password();
        if (api_key && *api_key) {
            snprintf(scratch_buf, sizeof scratch_buf,
                "{\"username\":\"buddy\",\"token\":\"%s\","
                "\"refresh_token\":\"%s\","
                "\"action\":\"user_jwt_refresh\",\"source\":\"moonraker\"}",
                api_key, api_key);
        } else {
            snprintf(scratch_buf, sizeof scratch_buf,
                "{\"username\":\"buddy\",\"token\":\"\","
                "\"refresh_token\":\"\","
                "\"action\":\"user_jwt_refresh\",\"source\":\"moonraker\"}");
        }
        result_str = scratch_buf;
    } else if (method == "access.oneshot_token") {
        const char *api_key = httpd_instance()->get_password();
        snprintf(scratch_buf, sizeof scratch_buf, "\"%s\"",
            (api_key && *api_key) ? api_key : "");
        result_str = scratch_buf;
    } else if (method == "machine.system_info") {
        char buf[200];
        snprintf(buf, sizeof buf,
            "{\"system_info\":{\"cpu_info\":{\"cpu_count\":1,\"processor\":\"STM32F427\",\"model\":\"COREONE\"},"
            "\"distribution\":{\"name\":\"buddy\"},\"available_services\":[],\"service_state\":{},"
            "\"system_uptime\":%lu.0}}",
            static_cast<unsigned long>(ticks_ms() / 1000));
        std::strncpy(scratch_buf, buf, sizeof scratch_buf - 1);
        result_str = scratch_buf;
    } else if (method == "printer.bed_mesh.dump") {
        // Render the full mesh into g_mesh_buf and arm a multi-frame
        // send. The "ok" reply goes out immediately as a normal frame;
        // step() then emits the mesh fragments over the next few ticks.
        render_mesh_into_buf();
        mesh_send_total = g_mesh_buf_size;
        mesh_send_pos = 0;
        result_str = "\"ok\"";
    } else if (method == "printer.gcode.script") {
        std::string_view script_sv;
        if (!find_string_value(body, "script", script_sv)) {
            is_error = true;
            err_code = -32602;
            err_msg = "script required";
        } else {
            // Snapshot the script into a stable buffer before frame_buf
            // gets overwritten — the string_view points into the inbound
            // payload region of frame_buf.
            char script_buf[192];
            const size_t copy_len = std::min(script_sv.size(), sizeof script_buf - 1);
            std::memcpy(script_buf, script_sv.data(), copy_len);
            script_buf[copy_len] = '\0';
            // Try macro expansion: if the script is just a known macro
            // name (case-insensitive), substitute the macro's gcode.
            const char *to_send = find_macro_expansion(script_buf);
            if (!to_send) to_send = script_buf;
            // Split on '\n' and send each non-empty, non-comment line.
            const char *p = to_send;
            while (*p) {
                const char *nl = std::strchr(p, '\n');
                size_t line_len = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
                char line[96];
                size_t cl = std::min(line_len, sizeof line - 1);
                std::memcpy(line, p, cl);
                line[cl] = '\0';
                if (char *semi = std::strchr(line, ';'); semi) *semi = '\0';
                char *ls = line;
                while (*ls == ' ' || *ls == '\t') ++ls;
                char *le = ls + std::strlen(ls);
                while (le > ls && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) {
                    --le; *le = '\0';
                }
                if (*ls && !try_dispatch_klipper_command(ls)) {
                    marlin_client::gcode(ls);
                }
                if (!nl) break;
                p = nl + 1;
            }
            result_str = "\"ok\"";
        }
    } else if (have_id) {
        is_error = true;
        err_code = -32601;
        err_msg = "Method not implemented";
    } else {
        return 0; // notification, no reply
    }

    // Now safe to overwrite frame_buf (method string_view becomes invalid).
    if (is_error) {
        return frame_text_error(frame_buf_data(), FRAME_BUF_SIZE, id, err_code, err_msg);
    }
    return frame_text_response(frame_buf_data(), FRAME_BUF_SIZE, result_str, id);
}

size_t WebSocketHandler::handle_control_frame_into_buf() {
    size_t mask_offset = (frame_payload_len < 126) ? 2 : 4;
    size_t payload_offset = mask_offset + (frame_masked ? 4 : 0);
    size_t payload_len = static_cast<size_t>(frame_payload_len);

    // Move payload to a safe temporary spot (the tail of frame_buf) before
    // we overwrite the start with the response header.
    uint8_t tmp[125]; // control frames are limited to 125 bytes by RFC
    if (payload_len > sizeof tmp) {
        // RFC violation; just close.
        state = State::Closing;
        frame_buf_at(0) = 0x88;
        frame_buf_at(1) = 0x00;
        return 2;
    }
    if (payload_len > 0) {
        std::memcpy(tmp, frame_buf_data() + payload_offset, payload_len);
    }

    uint8_t reply_opcode;
    if (frame_opcode == 0x9) {
        reply_opcode = 0xA; // Pong
    } else if (frame_opcode == 0x8) {
        state = State::Closing;
        reply_opcode = 0x8; // Close echo
    } else {
        // Pong from client or unknown — ignore.
        return 0;
    }

    frame_buf_at(0) = 0x80 | reply_opcode;
    frame_buf_at(1) = static_cast<uint8_t>(payload_len);
    if (payload_len > 0) {
        std::memcpy(frame_buf_data() + 2, tmp, payload_len);
    }
    return 2 + payload_len;
}

namespace {
// Cheap hash of the marlin_vars fields that drive Fluidd's UI. If the
// hash changed since the last push, it's worth emitting a status update.
uint32_t compute_push_hash() {
    auto &vars = marlin_vars();
    uint32_t h = 0;
    union { float f; uint32_t i; } cvt;
    cvt.f = vars.active_hotend().temp_nozzle; h ^= cvt.i;
    cvt.f = vars.active_hotend().target_nozzle; h = (h * 31) ^ cvt.i;
    cvt.f = vars.temp_bed; h = (h * 31) ^ cvt.i;
    cvt.f = vars.target_bed; h = (h * 31) ^ cvt.i;
    cvt.f = vars.logical_curr_pos[0]; h = (h * 31) ^ cvt.i;
    cvt.f = vars.logical_curr_pos[1]; h = (h * 31) ^ cvt.i;
    cvt.f = vars.logical_curr_pos[2]; h = (h * 31) ^ cvt.i;
    h = (h * 31) ^ vars.print_duration;
    h = (h * 31) ^ vars.sd_percent_done;
    auto ch = buddy::chamber().thermistor_temperature();
    cvt.f = ch.value_or(0.0f); h = (h * 31) ^ cvt.i;
    cvt.f = vars.active_hotend().temp_heatbreak; h = (h * 31) ^ cvt.i;
    h = (h * 31) ^ static_cast<uint8_t>(FSensors_instance().sensor_state(LogicalFilamentSensor::extruder));
    return h;
}
} // namespace

void WebSocketHandler::step(std::string_view input, bool terminated_by_client, uint8_t *out_buf, size_t out_buf_len, handler::Step &out) {
    using handler::Continue;
    using handler::Done;
    using handler::Terminating;
    out.read = 0;
    out.written = 0;
    out.next = Continue();

    if (state == State::Closed) {
        out.next = Terminating { false, Done::CloseFast };
        return;
    }

    // Refuse to operate without a frame-buffer slot. We end up here if
    // all MAX_SLOTS slots were taken at construction time (i.e. there
    // were already too many active WS connections). Closing fast is the
    // honest response — no slot means we cannot even render a close
    // frame, so we just drop the underlying TCP connection.
    if (pool_slot < 0) {
        state = State::Closed;
        out.next = Terminating { false, Done::CloseFast };
        return;
    }

    if (terminated_by_client) {
        state = State::Closed;
        out.next = Terminating { false, Done::CloseFast };
        return;
    }

    // 1) Handshake response goes out first.
    if (state == State::SendingHandshake) {
        if (out_buf_len > 0) {
            size_t w = write_handshake(out_buf, out_buf_len);
            out.written = w;
            if (w > 0) {
                state = State::Framing;
                // Push branch (4) will run next step() and fire
                // notify_klippy_ready unconditionally because
                // klippy_ready_sent is still false.
            }
        }
        return;
    }

    // 2) Drain a previously-buffered response if one is pending.
    if (buf_holds_response) {
        if (out_buf_len >= response_len) {
            std::memcpy(out_buf, frame_buf_data(), response_len);
            out.written = response_len;
            buf_holds_response = false;
            // frame_buf is now free for the next inbound frame.
            frame_buf_used = 0;
            frame_state = FrameState::WantHeader;
            if (state == State::Closing) {
                out.next = Terminating { false, Done::Close };
            }
        }
        return;
    }

    // 3) Try to read an inbound frame. Once complete, build the response
    //    INTO frame_buf (overwriting the inbound bytes — we already
    //    extracted everything we need from them).
    if (state == State::Framing || state == State::Closing) {
        std::string_view remaining = input;
        if (try_parse_frame(remaining)) {
            out.read = input.size() - remaining.size();

            size_t resp = 0;
            if (frame_opcode == 0x1 || frame_opcode == 0x2) {
                resp = handle_text_frame_into_buf();
            } else if (frame_opcode >= 0x8) {
                resp = handle_control_frame_into_buf();
            }

            if (resp > 0) {
                response_len = resp;
                buf_holds_response = true;
                // Drain immediately if we already have an out_buf.
                if (out_buf_len >= response_len) {
                    std::memcpy(out_buf, frame_buf_data(), response_len);
                    out.written = response_len;
                    buf_holds_response = false;
                    frame_buf_used = 0;
                    frame_state = FrameState::WantHeader;
                    if (state == State::Closing) {
                        out.next = Terminating { false, Done::Close };
                    }
                }
                return;
            }

            // No response needed (e.g. notification / pong / unknown).
            frame_buf_used = 0;
            frame_state = FrameState::WantHeader;
            return;
        }
        out.read = input.size() - remaining.size();
    }

    // 4a-pre-pre) Print history tracker: update each step. Detects
    //     state transitions in/out of Printing and records entries.
    //     Cheap (one printer_state::get_state() + a few compares); side
    //     effect is purely on global state, no frame emitted here.
    history_update_from_marlin();

    // 4a-pre) Gcode response: drain MULTIPLE ring-buffer entries per
    //     step() and emit them as a single notify_gcode_response with
    //     all lines joined by \n. Batching is critical under heavy
    //     output (e.g. G29 calibration's probe storm) — one-line-per-
    //     frame monopolized step() with gcode pushes, starving the
    //     status/heartbeat path and triggering Fluidd's "Reconnecting"
    //     popup. Batched, a single frame carries the whole drain.
    // Skip the gcode branch if (a) status_update is overdue, or
    // (b) we just pushed gcode within GCODE_PUSH_MIN_GAP_MS. The
    // throttle prevents flooding Chrome's WS receive buffer during
    // G29's probe storm — DOM updates in the console panel are
    // slow, and if frames arrive faster than they're drained, the
    // browser eventually RSTs the connection (we saw
    // last_lwip_err = -14 / ERR_RST in field testing).
    const uint32_t now_for_starvation = ticks_ms();
    const bool status_overdue = klippy_ready_sent
        && (now_for_starvation - last_push_ms > 1000);
    const bool gcode_throttled = (last_gcode_push_ms != 0)
        && (now_for_starvation - last_gcode_push_ms < GCODE_PUSH_MIN_GAP_MS);
    if (!status_overdue && !gcode_throttled
        && state == State::Framing && out_buf_len >= 128) {
        const uint32_t gcode_cur = g_gcode_log_write_idx.load(std::memory_order_acquire);
        if (!gcode_log_read_initialized) {
            gcode_log_read_idx = gcode_cur;
            gcode_log_read_initialized = true;
        }
        if (gcode_log_read_idx != gcode_cur) {
            // If we're far behind (ring wrapped), skip to the oldest still-valid entry.
            if (gcode_cur - gcode_log_read_idx > GCODE_LOG_LINES) {
                gcode_log_read_idx = gcode_cur - GCODE_LOG_LINES;
            }
            // Reserve room for the JSON envelope + closing quote/braces.
            constexpr size_t ENVELOPE_OVERHEAD = 70;
            const size_t out_cap = out_buf_len - 4 /*WS header*/;
            if (out_cap > ENVELOPE_OVERHEAD) {
                char *p = reinterpret_cast<char *>(out_buf + 4);
                int n = snprintf(p, out_cap,
                    "{\"jsonrpc\":\"2.0\",\"method\":\"notify_gcode_response\","
                    "\"params\":[\"");
                if (n <= 0 || static_cast<size_t>(n) >= out_cap) goto gcode_skip;
                size_t used = static_cast<size_t>(n);
                bool any_lines = false;
                // Append lines until the ring is drained or we run out of room.
                while (gcode_log_read_idx != gcode_cur) {
                    const auto &entry = g_gcode_log[gcode_log_read_idx % GCODE_LOG_LINES];
                    const size_t en = std::min<size_t>(entry.len, GCODE_LOG_LINE_LEN);
                    // Need: optional \n separator (1) + escaped line (≤en) + closing "]} (4) + safety
                    const size_t need = (any_lines ? 2 : 0) + en + 6;
                    if (used + need >= out_cap) break;
                    if (any_lines) {
                        // Embed \n as the literal two characters in the JSON string.
                        p[used++] = '\\';
                        p[used++] = 'n';
                    }
                    for (size_t i = 0; i < en; ++i) {
                        const char c = entry.data[i];
                        p[used++] = (c == '"' || c == '\\' || c < 0x20) ? ' ' : c;
                    }
                    any_lines = true;
                    ++gcode_log_read_idx;
                }
                if (any_lines) {
                    // Close the JSON: "]}
                    p[used++] = '"';
                    p[used++] = ']';
                    p[used++] = '}';
                    const size_t payload_len = used;
                    const size_t header_len = (payload_len < 126) ? 2 : 4;
                    if (header_len != 4) {
                        std::memmove(out_buf + header_len, p, payload_len);
                    }
                    out_buf[0] = 0x81;
                    if (payload_len < 126) {
                        out_buf[1] = static_cast<uint8_t>(payload_len);
                    } else {
                        out_buf[1] = 126;
                        uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
                        std::memcpy(out_buf + 2, &l, sizeof l);
                    }
                    out.written = header_len + payload_len;
                    last_gcode_push_ms = now_for_starvation;
                    return;
                }
            }
        }
gcode_skip: ;
    }

    // 4a) Filelist change notification: if the global epoch differs
    //     from what we've seen, emit notify_filelist_changed before
    //     anything else so file-panel clients see fresh state quickly.
    //     The notification is one-shot per epoch tick per handler.
    if (state == State::Framing && out_buf_len >= 64) {
        const uint32_t cur_epoch = g_filelist_epoch.load(std::memory_order_acquire);
        if (cur_epoch != last_filelist_epoch) {
            last_filelist_epoch = cur_epoch;
            const char *action = (g_filelist_last_action == FilelistAction::CreateFile)
                ? "create_file" : "delete_file";
            int n = snprintf(reinterpret_cast<char *>(out_buf), out_buf_len,
                "{\"jsonrpc\":\"2.0\",\"method\":\"notify_filelist_changed\","
                "\"params\":[{\"action\":\"%s\",\"item\":{\"path\":\"%s\","
                "\"root\":\"gcodes\",\"size\":0,\"modified\":0,\"permissions\":\"rw\"}}]}",
                action, g_filelist_last_path);
            if (n > 0 && static_cast<size_t>(n) < out_buf_len) {
                size_t payload_len = static_cast<size_t>(n);
                size_t header_len = (payload_len < 126) ? 2 : 4;
                uint8_t hdr[4];
                hdr[0] = 0x81;
                if (payload_len < 126) {
                    hdr[1] = static_cast<uint8_t>(payload_len);
                } else {
                    hdr[1] = 126;
                    uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
                    std::memcpy(hdr + 2, &l, sizeof l);
                }
                std::memmove(out_buf + header_len, out_buf, payload_len);
                std::memcpy(out_buf, hdr, header_len);
                out.written = header_len + payload_len;
                return;
            }
        }
    }

    // 4a1-pre) Mesh-change detection. Every MESH_CHECK_INTERVAL_MS,
    //      hash ubl.z_values and compare against the last seen hash.
    //      If different (a G29 just probed new points, or the mesh
    //      was cleared), arm a fresh multi-frame mesh push so Fluidd's
    //      Bed Mesh card updates without needing a manual reconnect.
    //      First check after subscribe just establishes the baseline
    //      (subscribe already armed an initial push, no need to re-fire).
    if (state == State::Framing && mesh_send_total == 0) {
        const uint32_t now = ticks_ms();
        if (now - last_mesh_check_ms >= MESH_CHECK_INTERVAL_MS) {
            const bool first_check = (last_mesh_check_ms == 0);
            last_mesh_check_ms = now;
#if ENABLED(AUTO_BED_LEVELING_UBL)
            // Cheap rolling hash of all GRID_MAX_POINTS_X *
            // GRID_MAX_POINTS_Y floats. NaN cells render as 0.0 in
            // the JSON, so we treat them as 0.0 in the hash too —
            // otherwise NaN bit patterns flip the hash spuriously.
            uint32_t h = 0;
            union { float f; uint32_t i; } cvt;
            for (int y = 0; y < GRID_MAX_POINTS_Y; ++y) {
                for (int x = 0; x < GRID_MAX_POINTS_X; ++x) {
                    float v = ubl.z_values[x][y];
                    cvt.f = std::isfinite(v) ? v : 0.0f;
                    h = (h * 31) ^ cvt.i;
                }
            }
            // Push only after the hash has been STABLE for
            // MESH_DEBOUNCE_MS — otherwise a running G29 mutates
            // z_values every probe and we'd push the mesh every
            // check cycle. The debounce ensures we wait until the
            // calibration has actually settled.
            if (h != last_mesh_hash) {
                last_mesh_hash = h;
                last_mesh_change_ms = first_check ? 0 : now;
            } else if (last_mesh_change_ms != 0
                    && (now - last_mesh_change_ms) >= MESH_DEBOUNCE_MS) {
                last_mesh_change_ms = 0;
                render_mesh_into_buf();
                mesh_send_total = g_mesh_buf_size;
                mesh_send_pos = 0;
            }
#endif
        }
    }

    // 4a1) Multi-frame bed_mesh fragment. Emits the next chunk of
    //      g_mesh_buf as a fragmented WS frame. Continues across step()
    //      calls until all bytes are sent (typically 4–6 chunks at
    //      a typical lwIP buffer cap of ~1024–1500). At-most-one chunk
    //      per step() so we yield back to the proc_stat / status push
    //      branches between mesh chunks rather than starving them.
    if (state == State::Framing && mesh_send_total > 0 && out_buf_len >= 256) {
        const uint16_t remaining = mesh_send_total - mesh_send_pos;
        // Chunk is bounded by the smaller of: bytes remaining, our
        // MAX_FRAME_PAYLOAD cap, and the actual lwIP out_buf capacity
        // (minus header reserve).
        const size_t max_by_buf = (out_buf_len > MAX_FRAME_HEADER) ? (out_buf_len - MAX_FRAME_HEADER) : 0;
        const size_t cap_combined = std::min<size_t>(MAX_FRAME_PAYLOAD, max_by_buf);
        const uint16_t chunk_payload =
            static_cast<uint16_t>(std::min<size_t>(remaining, cap_combined));
        if (chunk_payload == 0) {
            // Shouldn't happen given the 256 guard, but be safe.
            return;
        }
        const bool is_first = (mesh_send_pos == 0);
        const bool is_last = (chunk_payload == remaining);

        // WS frame opcode/FIN bits per RFC 6455 fragmentation.
        // first  : 0x01 (text, FIN=0)
        // middle : 0x00 (cont, FIN=0)
        // last   : 0x80 (cont, FIN=1)
        // single : 0x81 (text, FIN=1) — used when first and last coincide
        uint8_t opc;
        if (is_first && is_last) opc = 0x81;  // never expected (mesh > 1 frame) but safe
        else if (is_first) opc = 0x01;
        else if (is_last) opc = 0x80;
        else opc = 0x00;

        size_t header_len = (chunk_payload < 126) ? 2 : 4;
        uint8_t *p = out_buf;
        *p++ = opc;
        if (chunk_payload < 126) {
            *p++ = static_cast<uint8_t>(chunk_payload);
        } else {
            *p++ = 126;
            uint16_t l = lwip_htons(static_cast<uint16_t>(chunk_payload));
            std::memcpy(p, &l, sizeof l);
            p += sizeof l;
        }
        std::memcpy(p, g_mesh_buf + mesh_send_pos, chunk_payload);
        mesh_send_pos = static_cast<uint16_t>(mesh_send_pos + chunk_payload);
        out.written = header_len + chunk_payload;
        if (is_last) {
            mesh_send_total = 0;
            mesh_send_pos = 0;
        }
        return;
    }

    // 4a2) Periodic machine-stats push (notify_proc_stat_update).
    //      Drives Fluidd's System Utilization page: cpu_usage chart,
    //      uptime, MCU temp, websocket count, throttled_state. Without
    //      this push the fields stay null even though machine.proc_stats
    //      replies once on connect. Cadence (2 s) matches Moonraker's.
    if (state == State::Framing && out_buf_len >= 320) {
        const uint32_t now = ticks_ms();
        if (last_proc_stat_ms == 0 || now - last_proc_stat_ms >= PROC_STAT_INTERVAL_MS) {
            last_proc_stat_ms = now;
            const int32_t mcu_t = AdcGet::getMCUTemp();
            const uint32_t uptime_s = now / 1000;
            const int cpu_pct = osGetCPUUsage(); // FreeRTOS 0..100
            constexpr size_t HEADER_RESERVE = 4;
            char *p = reinterpret_cast<char *>(out_buf + HEADER_RESERVE);
            const size_t cap = out_buf_len - HEADER_RESERVE;
            int n = snprintf(p, cap,
                "{\"jsonrpc\":\"2.0\",\"method\":\"notify_proc_stat_update\","
                "\"params\":[{"
                "\"moonraker_stats\":{\"time\":%lu.0,\"cpu_usage\":%d.0,\"memory\":0,\"mem_units\":\"kB\"},"
                "\"cpu_temp\":%ld.0,"
                "\"system_cpu_usage\":{\"cpu\":%d.0},"
                "\"system_uptime\":%lu.0,"
                "\"websocket_connections\":1,"
                "\"throttled_state\":{\"bits\":0,\"flags\":[]}"
                "}]}",
                static_cast<unsigned long>(uptime_s), cpu_pct,
                static_cast<long>(mcu_t),
                cpu_pct,
                static_cast<unsigned long>(uptime_s));
            if (n > 0 && static_cast<size_t>(n) < cap) {
                size_t payload_len = static_cast<size_t>(n);
                size_t header_len = (payload_len < 126) ? 2 : 4;
                uint8_t *header_pos = out_buf + (HEADER_RESERVE - header_len);
                header_pos[0] = 0x81;
                if (payload_len < 126) {
                    header_pos[1] = static_cast<uint8_t>(payload_len);
                } else {
                    header_pos[1] = 126;
                    uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
                    std::memcpy(header_pos + 2, &l, sizeof l);
                }
                if (header_pos != out_buf) {
                    std::memmove(out_buf, header_pos, header_len);
                    std::memmove(out_buf + header_len, out_buf + HEADER_RESERVE, payload_len);
                }
                out.written = header_len + payload_len;
                return;
            }
        }
    }

    // 4a3) Klippy fault state transitions. When the printer enters
    //      DeviceState::Attention (Marlin error / FSM warning) we
    //      push notify_klippy_disconnected so Fluidd dims the
    //      dashboard. When it leaves we push notify_klippy_ready so
    //      Fluidd lights back up. Once-per-transition; cheap state
    //      compare each step().
    if (state == State::Framing && klippy_ready_sent && out_buf_len >= 80) {
        const auto ds = printer_state::get_state(false);
        const bool disconnected_now = (ds == printer_state::DeviceState::Attention
                                    || ds == printer_state::DeviceState::Error);
        if (disconnected_now != last_klippy_disconnected) {
            last_klippy_disconnected = disconnected_now;
            const char *method = disconnected_now
                ? "notify_klippy_disconnected"
                : "notify_klippy_ready";
            int n = snprintf(reinterpret_cast<char *>(out_buf), out_buf_len,
                "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":[]}", method);
            if (n > 0 && static_cast<size_t>(n) < out_buf_len) {
                size_t payload_len = static_cast<size_t>(n);
                size_t header_len = (payload_len < 126) ? 2 : 4;
                uint8_t hdr[4];
                hdr[0] = 0x81;
                if (payload_len < 126) {
                    hdr[1] = static_cast<uint8_t>(payload_len);
                } else {
                    hdr[1] = 126;
                    uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
                    std::memcpy(hdr + 2, &l, sizeof l);
                }
                std::memmove(out_buf + header_len, out_buf, payload_len);
                std::memcpy(out_buf, hdr, header_len);
                out.written = header_len + payload_len;
                return;
            }
        }
    }

    // 4b) Push: poll-based. Each step() (called by lwIP on recv or every
    //     ~500ms poll), check if anything client-visible changed in
    //     marlin_vars and emit a notify_status_update. The first push is
    //     always notify_klippy_ready (clients gate UI readiness on it).
    //     No back-pointer or wakeup mechanism — push frequency floors at
    //     lwIP's poll cadence (~2 Hz) and ceils at the recv callback rate.
    if (state == State::Framing && out_buf_len >= 64) {
        const uint32_t now = ticks_ms();
        const uint32_t since_push = now - last_push_ms;
        const bool should_push = !klippy_ready_sent
            || (since_push >= PUSH_MIN_GAP_MS && compute_push_hash() != last_push_hash)
            || (since_push >= PUSH_HEARTBEAT_MS);
        if (!should_push) {
            return;
        }
        last_push_ms = now;
        last_push_hash = compute_push_hash();
        constexpr size_t HEADER_RESERVE = 4;
        if (out_buf_len > HEADER_RESERVE) {
            size_t payload_len;
            if (!klippy_ready_sent) {
                klippy_ready_sent = true;
                // Bare notification, no params required by clients.
                int n = snprintf(reinterpret_cast<char *>(out_buf + HEADER_RESERVE),
                    out_buf_len - HEADER_RESERVE,
                    "{\"jsonrpc\":\"2.0\",\"method\":\"notify_klippy_ready\",\"params\":[]}");
                payload_len = (n > 0 && static_cast<size_t>(n) < out_buf_len - HEADER_RESERVE) ? static_cast<size_t>(n) : 0;
            } else {
                payload_len = render_notify_status_update(out_buf + HEADER_RESERVE, out_buf_len - HEADER_RESERVE);
            }
            if (payload_len > 0) {
                size_t header_len = (payload_len < 126) ? 2 : 4;
                uint8_t *header_pos = out_buf + (HEADER_RESERVE - header_len);
                header_pos[0] = 0x81;
                if (payload_len < 126) {
                    header_pos[1] = static_cast<uint8_t>(payload_len);
                } else {
                    header_pos[1] = 126;
                    uint16_t l = lwip_htons(static_cast<uint16_t>(payload_len));
                    std::memcpy(header_pos + 2, &l, sizeof l);
                }
                if (header_pos != out_buf) {
                    std::memmove(out_buf, header_pos, header_len);
                    std::memmove(out_buf + header_len, out_buf + HEADER_RESERVE, payload_len);
                }
                out.written = header_len + payload_len;
            }
        }
    }
}

} // namespace nhttp::printer
