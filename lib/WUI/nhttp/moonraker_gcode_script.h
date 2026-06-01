#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace nhttp::handler {
struct Step;
}

namespace nhttp::printer {

/**
 * \brief Handler for POST /printer/gcode/script (Moonraker).
 *
 * Body: { "script": "G28\nM115" }   (multi-line allowed; \n splits commands)
 *
 * Responds with:
 *   200 application/json  { "result": "ok" }                          on success
 *   400 application/json  { "message": "<reason>" }                   on bad request
 *   413 application/json  { "message": "Request body too large" }     if body > BUFFER_LEN
 *
 * Reads at most BUFFER_LEN bytes of body, splits the script on '\n',
 * strips comments and whitespace, and enqueues each non-empty line via
 * marlin_client::gcode().
 *
 * Separate from PrusaLink's ControlCommand so the two API surfaces
 * stay independently maintainable. The two share no parsing code —
 * both are tiny anyway.
 */
class MoonrakerGcodeScript {
public:
    static const constexpr size_t BUFFER_LEN = 512;

private:
    // The body buffer also doubles as the post-parse script buffer:
    // once parse_command returns, the script value's bytes are memmoved
    // to the start of this buffer and explicitly null-terminated. No
    // separate allocation needed.
    std::array<uint8_t, BUFFER_LEN> buffer;
    size_t buffer_used = 0;
    size_t content_length;
    bool can_keep_alive;
    bool body_too_large = false;

public:
    MoonrakerGcodeScript(size_t content_length, bool can_keep_alive);
    bool want_read() const { return true; }
    bool want_write() const { return false; }
    void step(std::string_view input, bool terminated_by_client, uint8_t *out_buf, size_t out_buf_len, handler::Step &out);
};

} // namespace nhttp::printer
