#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace nhttp::handler {
struct Step;
}

namespace nhttp::printer {

/**
 * \brief Handler for DELETE /server/files/{root}/{filename} (Moonraker).
 *
 * Removes a file from /usb/. Returns:
 *
 *   200 application/json
 *   { "result": { "action": "delete_file",
 *                 "item": { "path": "<filename>", "root": "gcodes" } } }
 *
 * The response body is built into a per-instance buffer at construction
 * time (request-scoped). SendStaticMemory uses a string_view that points
 * into this buffer, which is safe because the handler lives until the
 * response is fully written — the buffer outlives the SendStaticMemory's
 * use of it.
 *
 * If the unlink() fails, falls back to 404 NotFound.
 */
class MoonrakerFileDelete {
public:
    static const constexpr size_t MAX_FILENAME = 200;

private:
    // Response body buffer. Sized to fit the longest filename plus the
    // JSON envelope. Lives for the duration of the handler.
    std::array<char, MAX_FILENAME + 96> response_buf;
    size_t response_len = 0;

    bool headers_sent = false;
    bool unlink_failed = false;
    bool can_keep_alive;

public:
    /// @param filename basename only (no slashes); must be ≤ MAX_FILENAME
    /// @param keep_alive whether the underlying connection can be kept alive
    MoonrakerFileDelete(std::string_view filename, bool keep_alive);

    bool want_read() const { return false; }
    bool want_write() const { return true; }
    void step(std::string_view input, bool terminated_by_client, uint8_t *out_buf, size_t out_buf_len, handler::Step &out);
};

} // namespace nhttp::printer
