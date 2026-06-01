#pragma once

#include "req_parser.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>

namespace nhttp::handler {
struct Step;
}

namespace nhttp::printer {

/**
 * \brief Handler for POST /server/files/upload (Moonraker).
 *
 * Parses multipart/form-data, finds the `file` form field, and streams
 * its content to /usb/<filename>. Filename comes from the multipart
 * Content-Disposition header (`filename="..."`).
 *
 * Other form fields ("root", "path", "print", "checksum") are tolerated
 * and ignored — we always upload to /usb/ and don't auto-start prints
 * from this endpoint (callers can POST /printer/print/start afterwards
 * if they want to).
 *
 * Streaming design:
 *   The body can be many MB (a typical gcode file). We never buffer
 *   the file content; instead we fwrite() it as bytes arrive. The
 *   state machine keeps a small "tail" of recent bytes (boundary
 *   length + 4) so we can detect boundaries that span TCP segment
 *   boundaries without holding the whole file.
 *
 * Responses:
 *   200 application/json
 *     { "result": { "action": "create_file",
 *                   "item": { "path": "<filename>", "root": "gcodes" } } }
 *   400 application/json { "message": "<reason>" } — parse failures
 *   409 application/json { "message": "<reason>" } — overwrite refused (none today)
 *   500 application/json { "message": "<reason>" } — fwrite/fopen failures
 *
 * Returns Moonraker error shape (StatusPage with json_errors=true).
 */
class MoonrakerFileUpload {
public:
    // Multipart boundary string is small (RFC 2046 says 1-70 chars).
    // Plus our scan needs the "\r\n--" prefix in front of it.
    static const constexpr size_t MAX_BOUNDARY_LEN = 70;
    static const constexpr size_t MAX_FILENAME = 100; // FAT LFN ≤ 255, but Moonraker filenames are usually short
    static const constexpr size_t LOOKAHEAD = MAX_BOUNDARY_LEN + 8; // boundary + "\r\n--" + safety

private:
    // State machine states. Order matters; we advance monotonically.
    enum class State : uint8_t {
        ScanForFirstBoundary, // Looking for first "--<boundary>\r\n"
        ReadPartHeaders,      // Accumulating header lines for the current part
        SkipPartBody,         // We're in a part we don't care about; skip until next boundary
        WriteFileBody,        // Inside the file part; stream to disk until next boundary
        Done,                 // File was written successfully and we're past it
        Failed,               // Hard error; the rest of the request is drained then we respond
    };

    // Boundary string (copied from RequestParser to outlive it).
    std::array<char, MAX_BOUNDARY_LEN + 1> boundary {};
    uint8_t boundary_len = 0;

    // Filename of the file part we're writing. Set when we parse a
    // Content-Disposition with filename="...".
    std::array<char, MAX_FILENAME + 1> filename {};
    uint8_t filename_len = 0;

    // Pending bytes that may form a partial boundary. We hold this
    // many bytes back before fwrite()ing or scanning, to ensure boundary
    // detection works across chunk boundaries.
    std::array<uint8_t, LOOKAHEAD> tail {};
    size_t tail_used = 0;

    // Header-line accumulator for ReadPartHeaders state.
    std::array<char, 256> header_line {};
    size_t header_line_used = 0;
    bool current_part_is_file = false;

    FILE *out_file = nullptr;
    size_t content_length;
    size_t consumed = 0;
    bool can_keep_alive;
    bool headers_sent = false;
    bool response_terminating = false;
    State state = State::ScanForFirstBoundary;

    // Final response body buffer + length. Lives for the whole handler
    // lifetime so SendStaticMemory-style sending works.
    std::array<char, MAX_FILENAME + 96> response_buf {};
    size_t response_len = 0;
    // HTTP status for the response. 200 on success, 400/500 on failure.
    int response_code = 200;
    const char *response_error_message = nullptr;

    // Step-internal helpers.
    void process_chunk(std::string_view chunk);
    void on_header_line(std::string_view line);
    void finalize_success();
    void finalize_failure(int code, const char *message);

public:
    MoonrakerFileUpload(const handler::RequestParser &parser);
    ~MoonrakerFileUpload();
    MoonrakerFileUpload(const MoonrakerFileUpload &) = delete;
    MoonrakerFileUpload &operator=(const MoonrakerFileUpload &) = delete;
    MoonrakerFileUpload(MoonrakerFileUpload &&) = default;
    MoonrakerFileUpload &operator=(MoonrakerFileUpload &&) = default;

    bool want_read() const;
    bool want_write() const;
    void step(std::string_view input, bool terminated_by_client, uint8_t *out_buf, size_t out_buf_len, handler::Step &out);
};

} // namespace nhttp::printer
