#include "control_command.h"

#include <marlin_client.hpp>
#include <cstdio>
#include "printers.h"

#if PRINTER_IS_PRUSA_COREONE()
    #include <feature/chamber/chamber.hpp>
    #include <feature/xbuddy_extension/xbuddy_extension.hpp>
    #include <pwm_utils.hpp>
#endif

#include <option/has_side_leds.h>
#if HAS_SIDE_LEDS() || defined(UNITTESTS)
    #include <leds/side_strip_handler.hpp>
#endif

namespace nhttp::printer {

void ControlCommand::set_nozzle_temp(int temp) {
    char gcode[20];
    snprintf(gcode, sizeof(gcode), "M104 S%d", temp);
    marlin_client::gcode(gcode);
}

void ControlCommand::set_bed_temp(int temp) {
    char gcode[20];
    snprintf(gcode, sizeof(gcode), "M140 S%d", temp);
    marlin_client::gcode(gcode);
}

void ControlCommand::set_chamber_temp(int temp) {
#if PRINTER_IS_PRUSA_COREONE()
    if (temp <= 0) {
        buddy::chamber().set_target_temperature(std::nullopt);
    } else {
        buddy::chamber().set_target_temperature(temp);
    }
#else
    (void)temp;
#endif
}

void ControlCommand::set_speed(int percent) {
    char gcode[20];
    snprintf(gcode, sizeof(gcode), "M220 S%d", percent);
    marlin_client::gcode(gcode);
}

void ControlCommand::set_flow(int percent) {
    char gcode[20];
    snprintf(gcode, sizeof(gcode), "M221 S%d", percent);
    marlin_client::gcode(gcode);
}

void ControlCommand::set_fan_print(int pwm) {
    char gcode[20];
    if (pwm <= 0) {
        marlin_client::gcode("M107");
    } else {
        snprintf(gcode, sizeof(gcode), "M106 S%d", pwm > 255 ? 255 : pwm);
        marlin_client::gcode(gcode);
    }
}

void ControlCommand::set_chamber_fan(int percent) {
#if PRINTER_IS_PRUSA_COREONE()
    // Match Connect planner logic: -1 = auto, 0-100 = manual PWM %
    buddy::xbuddy_extension().set_fan_target_pwm(
        buddy::XBuddyExtension::Fan::cooling_fan_1,
        percent < 0
            ? buddy::XBuddyExtension::FanPWMOrAuto(pwm_auto)
            : buddy::XBuddyExtension::FanPWM::from_percent(percent > 100 ? 100 : percent));
#else
    (void)percent;
#endif
}

void ControlCommand::set_led(int percent) {
#if PRINTER_IS_PRUSA_COREONE() && (HAS_SIDE_LEDS() || defined(UNITTESTS))
    uint8_t brightness = static_cast<uint8_t>(percent > 100 ? 100 : (percent < 0 ? 0 : percent)) * 255 / 100;
    leds::SideStripHandler::instance().set_max_brightness(brightness);
    leds::SideStripHandler::instance().activity_ping();
#else
    (void)percent;
#endif
}

void ControlCommand::home_axes(bool x, bool y, bool z) {
    char gcode[20] = "G28";
    if (!(x && y && z)) {
        // Selective homing
        char *p = gcode + 3;
        if (x) *p++ = ' ', *p++ = 'X';
        if (y) *p++ = ' ', *p++ = 'Y';
        if (z) *p++ = ' ', *p++ = 'Z';
        *p = '\0';
    }
    marlin_client::gcode(gcode);
}

void ControlCommand::relative_move(float x, float y, float z, float e, int feedrate) {
    // Switch to relative mode
    marlin_client::gcode("G91");

    char gcode[80];
    snprintf(gcode, sizeof(gcode), "G1 F%d X%.2f Y%.2f Z%.2f E%.2f", feedrate, (double)x, (double)y, (double)z, (double)e);
    marlin_client::gcode(gcode);

    // Switch back to absolute mode
    marlin_client::gcode("G90");
}

void ControlCommand::motors_off() {
    marlin_client::gcode("M18");
}

void ControlCommand::emergency_stop() {
    marlin_client::gcode("M112");
}

} // namespace nhttp::printer
