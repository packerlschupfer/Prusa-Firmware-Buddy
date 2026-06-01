#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace nhttp::handler {
struct Step;
}

namespace nhttp::printer {

/**
 * \brief Handler for POST /printer/print/start (Moonraker).
 *
 * Body: { "filename": "test.gcode" }
 *
 * Resolves the filename to a path under /usb/ and calls
 * marlin_client::print_start(). The Moonraker contract uses a flat
 * filename relative to the gcodes directory — there's no concept of
 * subdirectories at the API level (though local FS may have them).
 *
 * Responds with:
 *   200 application/json { "result": "ok" }                 on success
 *   400 application/json { "message": "..." }               on parse failure
 *   409 application/json { "message": "Printer busy" }      if a print is already in progress
 *
 * Print pause / resume / cancel have no body and are handled in the
 * URL dispatcher directly — they don't need this class.
 */
class MoonrakerPrintStart {
public:
    static const constexpr size_t BUFFER_LEN = 256;

private:
    std::array<uint8_t, BUFFER_LEN> buffer;
    size_t buffer_used = 0;
    size_t content_length;
    bool can_keep_alive;
    bool body_too_large = false;

public:
    MoonrakerPrintStart(size_t content_length, bool can_keep_alive);
    bool want_read() const { return true; }
    bool want_write() const { return false; }
    void step(std::string_view input, bool terminated_by_client, uint8_t *out_buf, size_t out_buf_len, handler::Step &out);
};

} // namespace nhttp::printer
