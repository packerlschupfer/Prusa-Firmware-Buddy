/**
 * \file
 *
 * Moonraker-style WebSocket handler (RFC 6455).
 *
 * Owns one upgraded connection for its entire lifetime. Sits inside the
 * nhttp ConnectionState variant just like any other handler, but its
 * step() never returns Terminating until the client (or our cleanup
 * logic) decides to close.
 *
 * After the HTTP 101 Switching Protocols response is sent, all bytes on
 * the connection are RFC 6455 frames. We parse incoming frames as
 * JSON-RPC 2.0 requests (Moonraker convention), dispatch them, and push
 * notify_status_update notifications to subscribed clients.
 *
 * Event-driven push:
 *
 *   The handler binds a Subscriber<> to marlin_server::idle_publisher.
 *   When marlin fires an idle tick (in the marlin thread), our
 *   callback runs in the marlin thread, sets a "dirty" flag, and
 *   posts a tcpip_callback. The tcpip callback wakes us in the
 *   tcpip thread, where we may call altcp_write.
 *
 *   We absolutely must NOT touch the altcp_pcb from the marlin thread.
 */
#pragma once

#include "req_parser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

struct altcp_pcb;
struct pbuf;

namespace nhttp::handler {
struct Step;
}

namespace nhttp::printer {

/**
 * \brief Moonraker WebSocket connection state.
 *
 * Lifetime: created by the WebSocketSelector after a valid Upgrade,
 * lives until the connection is closed.
 */
class WebSocketHandler {
public:
    // Maximum WS frame payload. Must stay below nhttp's BUFF_SIZE
    // (2 * TCP_MSS = 2048 since phase 4ze) minus the WS header reserve,
    // so the rendered frame always fits in the output buffer the framework
    // hands us. Going to 1900 gives every push path room to grow (status
    // update + chamber fans + LED + future fields without re-bumping).
    static constexpr size_t MAX_FRAME_PAYLOAD = 1900;
    // Header bytes (2..14) + payload buffer.
    static constexpr size_t MAX_FRAME_HEADER = 14;
    // Total size of one slot in the global frame pool.
    static constexpr size_t FRAME_BUF_SIZE = MAX_FRAME_HEADER + MAX_FRAME_PAYLOAD;
    // Concurrent WS connections supported. Matches the nhttp framework's
    // ACTIVE_CONNS — one frame buffer slot per connection. If all slots
    // are taken, a new upgrade is refused.
    static constexpr int MAX_SLOTS = 3;

    // Moonraker objects we know how to snapshot. Bitmask of these is
    // stored per-connection so we know which to include in updates.
    enum Subscription : uint16_t {
        SubNone = 0,
        SubExtruder = 1 << 0,
        SubHeaterBed = 1 << 1,
        SubToolhead = 1 << 2,
        SubPrintStats = 1 << 3,
        SubDisplayStatus = 1 << 4,
        SubVirtualSdcard = 1 << 5,
        SubAll = 0xFFFF,
    };

private:
    enum class State : uint8_t {
        SendingHandshake, // Streaming the 101 response into the send buffer.
        Framing, // Handshake done. Reading/writing RFC 6455 frames.
        Closing, // Sent close frame, draining.
        Closed, // Terminating.
    };

    // Inbound frame parser sub-state.
    enum class FrameState : uint8_t {
        WantHeader, // Need at least 2 bytes for header.
        WantExtLen, // Need 2 or 8 more bytes for extended length.
        WantMask, // Need 4 bytes for masking key.
        WantPayload, // Reading payload bytes.
    };

    State state = State::SendingHandshake;
    FrameState frame_state = FrameState::WantHeader;

    // Computed Sec-WebSocket-Accept (base64(SHA1(key + magic))).
    // 20 bytes SHA1 → 28 chars base64 + '\0'.
    std::array<char, 32> ws_accept {};

    // How much of the 101 handshake response has been emitted.
    size_t handshake_pos = 0;

    // Index into the global frame pool (g_ws_frame_pool, defined in the
    // .cpp). -1 means no slot was available at construction — the
    // handler will close itself on first step() in that case. Held
    // exclusively for the handler's lifetime; released in the destructor.
    // Each slot is FRAME_BUF_SIZE bytes, dual-purpose: holds the inbound
    // frame while parsing, then gets overwritten with the outbound
    // response (with WS header).
    int8_t pool_slot = -1;
    size_t frame_buf_used = 0;

    // Slot accessors. pool_slot must be >= 0; step() guards the public
    // entry point so private callers can trust this. Defined out-of-line
    // in the .cpp where the static g_ws_frame_pool is visible.
    uint8_t *frame_buf_data();
    const uint8_t *frame_buf_data() const;
    uint8_t &frame_buf_at(size_t i);
    const uint8_t &frame_buf_at(size_t i) const;
    // Parsed frame metadata.
    uint8_t frame_opcode = 0;
    bool frame_fin = false;
    bool frame_masked = false;
    uint64_t frame_payload_len = 0;
    uint8_t frame_mask[4] = {};
    size_t frame_payload_consumed = 0;

    // Response buffering: when we receive a frame that needs a reply,
    // we render the reply (WS-framed) into frame_buf and set these so
    // the next step() call with an out_buf drains it. Until drained,
    // want_write() returns true so the server allocates an out_buf for us.
    size_t response_len = 0;
    bool buf_holds_response = false;

    // Active subscriptions for this client.
    uint16_t subscriptions = Subscription::SubNone;

    // Poll-based push. Every step() we sample marlin_vars, compute a
    // cheap hash, and push if it changed since the last push OR a
    // heartbeat interval has elapsed. step() runs on every lwIP recv
    // and on the server's ~500ms poll, which gives us ~2 Hz push under
    // network silence and up to lwIP-callback frequency under traffic.
    //
    // This replaces a previous publisher-subscriber design that bound
    // to marlin_server::idle_publisher from the tcpip thread. The
    // publisher pattern required a static back-pointer table that
    // turned out to leak slots across handler move/destruct paths in
    // ways we couldn't nail down deterministically. Polling eliminates
    // the cross-thread back-pointer entirely — no slots, no race.
    uint32_t last_push_hash = 0;
    uint32_t last_push_ms = 0;
    // Heartbeat: force a push every N ms even if state didn't change,
    // so clients that joined mid-quiet-period get a fresh snapshot.
    static constexpr uint32_t PUSH_HEARTBEAT_MS = 5000;
    // Minimum gap between pushes, to avoid flooding the link with
    // back-to-back near-identical frames when many values churn.
    static constexpr uint32_t PUSH_MIN_GAP_MS = 250;

    // First push after connection must be notify_klippy_ready, not
    // notify_status_update. Fluidd & Mainsail gate the dashboard on
    // klippy being "ready" and only flip that flag when this specific
    // notification arrives. The check fires once per handler lifetime.
    bool klippy_ready_sent = false;

    // Authenticated by a valid access_token in server.connection.identify.
    // If the printer has no API key configured, this starts true. Else
    // false until a successful identify; non-identify RPC methods get
    // rejected with -32001 ("unauthenticated") until then.
    bool is_authenticated = false;

    // Tracks the last printer-state classification we emitted. When
    // the state transitions into Attention/Error we push
    // notify_klippy_disconnected; when it leaves we push
    // notify_klippy_ready. Lets Fluidd dim the dashboard during a
    // fault and recover automatically once the printer clears it.
    bool last_klippy_disconnected = false;

    // Last filelist event epoch this handler observed. The global epoch
    // counter increments on every upload/delete; mismatch means we owe
    // the client a notify_filelist_changed notification.
    uint32_t last_filelist_epoch = 0;

    // Per-handler cursor into the gcode response ring buffer. Each
    // step() emits at most one notify_gcode_response per call, draining
    // one entry per poll. New connections start at the current write
    // position (no replay of historical output).
    uint32_t gcode_log_read_idx = 0;
    bool gcode_log_read_initialized = false;

    // Throttle gcode_response push frequency. The browser's console
    // panel renders each line into the DOM; under heavy output (G29
    // probe storm) DOM updates block the JS main thread, the WS
    // receive buffer backs up, and Chrome eventually resets the
    // connection (last_lwip_err = ERR_RST = -14). 250 ms gap gives
    // the renderer headroom while still feeling live.
    uint32_t last_gcode_push_ms = 0;
    static constexpr uint32_t GCODE_PUSH_MIN_GAP_MS = 250;

    // Last time we emitted a notify_proc_stat_update frame. Moonraker
    // sends these every ~1s; we throttle to PROC_STAT_INTERVAL_MS to
    // match the chart-retention sampling Fluidd expects. 0 forces an
    // immediate emit on the first step() after handshake.
    uint32_t last_proc_stat_ms = 0;
    static constexpr uint32_t PROC_STAT_INTERVAL_MS = 2000;

    // Multi-frame mesh push. The full bed_mesh JSON is too big for one
    // WS frame (~3 KB vs MAX_FRAME_PAYLOAD), so it's split across
    // multiple WS frames via the RFC 6455 fragmentation protocol:
    //   first frame:  opcode=text(1), FIN=0
    //   middle frame: opcode=cont(0), FIN=0
    //   last frame:   opcode=cont(0), FIN=1
    // Per-handler cursor into the shared global mesh-render buffer.
    // mesh_send_total == 0 means no send in progress.
    uint16_t mesh_send_pos = 0;
    uint16_t mesh_send_total = 0;

    // Periodic mesh-change detection. Every MESH_CHECK_INTERVAL_MS we
    // hash ubl.z_values; if it differs from last_mesh_hash, a new
    // multi-frame mesh push gets armed. Drives Fluidd's Bed Mesh card
    // updating after a G29 completes, without us needing a marlin-side
    // hook.
    uint32_t last_mesh_hash = 0;
    uint32_t last_mesh_check_ms = 0;
    // Time of the last hash CHANGE (not push). Mesh push is armed
    // only after the hash has been stable for MESH_DEBOUNCE_MS, so a
    // running G29 (which mutates z_values every probe point) doesn't
    // trigger a fresh multi-frame push every 3 s.
    uint32_t last_mesh_change_ms = 0;
    static constexpr uint32_t MESH_CHECK_INTERVAL_MS = 3000;
    static constexpr uint32_t MESH_DEBOUNCE_MS = 10000;

    void compute_accept(const handler::RequestParser &request);

    // Write the canned 101 Switching Protocols response into out_buf.
    // Returns bytes written.
    size_t write_handshake(uint8_t *out_buf, size_t out_buf_len);

    // Try to parse one inbound frame from frame_buf. Returns true if
    // a complete frame was consumed; sets the state machine accordingly.
    bool try_parse_frame(std::string_view &input);

    // Handle a fully-received text frame (payload is in frame_buf at
    // payload_offset, length frame_payload_len, unmasked). Extracts
    // method/id, then OVERWRITES frame_buf with the response (WS-framed).
    // Returns response length (bytes in frame_buf to send), or 0 if no
    // response needed.
    size_t handle_text_frame_into_buf();

    // Handle ping/pong/close control frames. Same contract — writes
    // response WS frame into frame_buf starting at offset 0.
    size_t handle_control_frame_into_buf();

    // Render a JSON-RPC response/notification body into out_buf.
    // Returns bytes written (payload only — no WS header).
    size_t render_notify_status_update(uint8_t *out_buf, size_t out_buf_len);
    size_t render_jsonrpc_result(uint8_t *out_buf, size_t out_buf_len, int id, const char *result);
    size_t render_jsonrpc_error(uint8_t *out_buf, size_t out_buf_len, int id, int code, const char *msg);

    // server.files.get_directory: scans /usb/ and builds a full WS-framed
    // response into frame_buf. Returns total frame length (header + payload),
    // 0 on failure. Caps the file list at whatever fits in frame_buf;
    // truncation is silent. `path` filters: empty / "gcodes" → scan /usb/;
    // anything else → return empty list. For pagination/streaming over many
    // files, see the segmented JsonRenderer pattern in MoonrakerFilesList.
    size_t render_files_get_directory(int id, const char *path);

    // printer.objects.subscribe: writes the initial-snapshot response
    // (configfile.settings.gcode_macro_*, webhooks, idle_timeout,
    // heaters) directly into frame_buf. Bigger than the static-string
    // result_str path because the macros block is ~400 bytes.
    size_t render_objects_subscribe_response(int id);

    // server.database.get_item: reads /internal/fluidd_<key>.json and
    // renders {namespace,key,value:<file contents>} into frame_buf.
    // Returns 0 (caller falls through to "Key not found" error) if the
    // file doesn't exist or doesn't fit. `key` may be empty — in that
    // case we currently return 0 to trigger the "namespace dump
    // unsupported" path, since the full dump can blow past frame_buf.
    size_t render_database_get_item(int id, const char *key);

    // server.history.list — renders the in-memory print-history ring
    // buffer as the Moonraker {count, jobs:[...]} shape into frame_buf.
    // No persistence; ring resets on reboot.
    size_t render_history_list(int id);

public:
    explicit WebSocketHandler(const handler::RequestParser &request);
    ~WebSocketHandler();
    WebSocketHandler(const WebSocketHandler &) = delete;
    WebSocketHandler &operator=(const WebSocketHandler &) = delete;
    // Custom move ops: must transfer the pool_slot to the destination
    // and set the source's slot to -1 so the source's destructor
    // doesn't free the slot we just handed off. This matters when the
    // nhttp variant moves the handler between ConnectionState slots.
    WebSocketHandler(WebSocketHandler &&other) noexcept;
    WebSocketHandler &operator=(WebSocketHandler &&other) noexcept;

    bool want_read() const;
    bool want_write() const;
    void step(std::string_view input, bool terminated_by_client, uint8_t *out_buf, size_t out_buf_len, handler::Step &out);

};

// File operations (REST/WS upload, delete) call this to broadcast a
// notify_filelist_changed JSON-RPC notification to all active WS clients.
// The publish API is a simple "bump a global epoch + stash the latest
// event"; each WS handler picks it up on its next poll-driven step().
// No back-pointer table — same design principle as the push refactor.
enum class FilelistAction {
    CreateFile,
    DeleteFile,
};
void publish_filelist_event(FilelistAction action, const char *path);

// Called once per line of Marlin gcode output (echo / ok / position
// reports). Hooked from the USBSerial lineBufferHook in appmain.cpp.
// Cheap-and-thread-safe: pushes the line into a small ring buffer that
// WS handlers drain on each poll-driven step(). Never blocks.
void publish_gcode_response_line(const char *buf, int size);

// Translates a Klipper-style command (SET_PIN, SET_HEATER_TEMPERATURE,
// PAUSE/RESUME, etc.) sent by Fluidd's UI into the equivalent Marlin
// gcode or marlin_client call. Returns true if the line was handled
// (don't pass it to marlin_client::gcode); false if unrecognized
// (caller should fall through to marlin_client::gcode as-is).
bool dispatch_klipper_command(const char *line);

} // namespace nhttp::printer
