#pragma once

#include "status_page.h"

#include <http/types.h>

#include <array>
#include <string_view>

namespace nhttp::printer {

/**
 * \brief Handler for POST /api/v1/control — unified printer control endpoint.
 *
 * Accepts JSON body with any combination of optional fields:
 *
 * Temperatures:
 *   "nozzle": 215        - set nozzle target (M104), 0 = off
 *   "bed": 60            - set bed target (M140), 0 = off
 *   "chamber": 40        - set chamber target, 0 = off (Core One only)
 *
 * Print parameters:
 *   "speed": 100          - print speed % (M220 S100)
 *   "flow": 100           - flow rate % (M221 S100)
 *
 * Fans:
 *   "fan_print": 255      - print fan PWM 0-255 (M106 S255)
 *   "chamber_fan": 50     - chamber fan PWM % (-1=auto, 0-100) (Core One only)
 *
 * Lighting:
 *   "led": 100            - chamber LED intensity 0-100 % (Core One only)
 *
 * Motion:
 *   "home": true          - home all axes (G28)
 *   "home_x": true        - home X axis
 *   "home_y": true        - home Y axis
 *   "home_z": true        - home Z axis
 *   "move_x": 10.0        - relative X move in mm
 *   "move_y": 10.0        - relative Y move in mm
 *   "move_z": 5.0         - relative Z move in mm
 *   "move_e": 5.0         - relative extrude in mm
 *   "feedrate": 1000      - feedrate for moves in mm/min
 *
 * Misc:
 *   "motors_off": true    - disable steppers (M18)
 *   "stop": true          - emergency stop (M112)
 *
 * All fields optional. Omitted fields are left unchanged.
 */
class ControlCommand {
private:
    static const constexpr size_t BUFFER_LEN = 256;
    std::array<uint8_t, BUFFER_LEN> buffer;
    size_t buffer_used = 0;
    size_t content_length;
    bool can_keep_alive;
    bool json_errors;

    handler::StatusPage process();

    // Lives in control_command_marlin.cpp for test isolation
    void set_nozzle_temp(int temp);
    void set_bed_temp(int temp);
    void set_chamber_temp(int temp);
    void set_speed(int percent);
    void set_flow(int percent);
    void set_fan_print(int pwm);
    void set_chamber_fan(int percent);
    void set_led(int percent);
    void home_axes(bool x, bool y, bool z);
    void relative_move(float x, float y, float z, float e, int feedrate);
    void motors_off();
    void emergency_stop();

public:
    ControlCommand(size_t content_length, bool can_keep_alive, bool json_errors);
    bool want_read() const { return true; }
    bool want_write() const { return false; }
    void step(std::string_view input, bool terminated_by_client, uint8_t *buffer, size_t buffer_size, handler::Step &out);
};

} // namespace nhttp::printer
