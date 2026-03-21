#pragma once

#include "status_page.h"

#include <http/types.h>

#include <array>
#include <string_view>

namespace nhttp::printer {

/**
 * \brief Handler for POST /api/v1/preheat and temperature control endpoints.
 *
 * Accepts JSON body with optional fields:
 *   {"nozzle": 215, "bed": 60, "chamber": 40}
 * Any field omitted is left unchanged.
 */
class TempCommand {
private:
    static const constexpr size_t BUFFER_LEN = 128;
    std::array<uint8_t, BUFFER_LEN> buffer;
    size_t buffer_used = 0;
    size_t content_length;
    bool can_keep_alive;
    bool json_errors;

    handler::StatusPage process();

    // Lives in temp_command_marlin.cpp for test isolation
    bool set_nozzle_temp(int temp);
    bool set_bed_temp(int temp);
    bool set_chamber_temp(int temp);

public:
    TempCommand(size_t content_length, bool can_keep_alive, bool json_errors);
    bool want_read() const { return true; }
    bool want_write() const { return false; }
    void step(std::string_view input, bool terminated_by_client, uint8_t *buffer, size_t buffer_size, handler::Step &out);
};

} // namespace nhttp::printer
