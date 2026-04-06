#include "status_renderer.h"

#include "marlin_client.hpp"
#include <marlin_vars.hpp>
#include <state/printer_state.hpp>
#include <transfers/monitor.hpp>
#include <segmented_json_macros.h>

#include "printers.h"

#include <option/buddy_enable_connect.h>
#if BUDDY_ENABLE_CONNECT()
    #include <connect/connect.hpp>
#endif

#include <marlin_server_types/general_response.hpp>

#if PRINTER_IS_PRUSA_COREONE()
    #include <feature/chamber/chamber.hpp>
    #include <feature/xbuddy_extension/xbuddy_extension.hpp>
    #include <leds/side_strip_handler.hpp>
#endif

using namespace marlin_server;
using transfers::Monitor;

namespace nhttp::handler {

json::JsonResult StatusRenderer::renderState(size_t resume_point, json::JsonOutput &output, StatusState &state) const {
    // Note: We allow stale, because if it was not there the state.transfer_id
    // will be nullopt and we will not send it anyway
    auto transfer_status = Monitor::instance.status(true);
    if (transfer_status.has_value() && transfer_status->id != state.transfer_id) {
        // if transfer changes mid report, bail out
        transfer_status.reset();
    }

    uint32_t time_to_end = marlin_vars().time_to_end;
    uint32_t time_to_pause = marlin_vars().time_to_pause;
    auto state_with_dialog = printer_state::get_state_with_dialog(false);
    auto link_state = state_with_dialog.device_state;

    // Dialog info for remote control
    bool has_dialog = state_with_dialog.dialog.has_value();
    uint32_t dialog_id = has_dialog ? state_with_dialog.dialog->dialog_id.to_uint32_t() : 0;
    uint32_t dialog_code = state_with_dialog.code_num();
    const Response *dialog_buttons = state_with_dialog.buttons();

#if PRINTER_IS_PRUSA_COREONE()
    auto chamber_temp = buddy::chamber().current_temperature();
    auto chamber_target = buddy::chamber().target_temperature();
    auto xbe_state = buddy::xbuddy_extension().get_fan12_state();
    int8_t chamber_fan_pwm = xbe_state.fan1_fan2_target_pwm.transform(buddy::XBuddyExtension::FanPWM::to_percent_static).value_or(-1);
    int8_t chamber_led = static_cast<int8_t>(static_cast<uint16_t>(leds::SideStripHandler::instance().get_max_brightness()) * 100 / 255);
#endif

    // Keep the indentation of the JSON in here!
    // clang-format off
    JSON_START;
    JSON_OBJ_START;
    if (printer_state::has_job()) {
        JSON_FIELD_OBJ("job");
            JSON_FIELD_INT("id", marlin_vars().job_id) JSON_COMMA;
            JSON_FIELD_FFIXED("progress", ((float)marlin_vars().sd_percent_done), 2) JSON_COMMA;
            if (time_to_end != TIME_TO_END_INVALID) {
                JSON_FIELD_INT("time_remaining", time_to_end) JSON_COMMA;
            }
            if (time_to_pause != TIME_TO_END_INVALID) {
                JSON_FIELD_INT("filament_change_in", time_to_pause) JSON_COMMA;
            }
            JSON_FIELD_INT("time_printing", marlin_vars().print_duration);
        JSON_OBJ_END JSON_COMMA;
    }
    JSON_FIELD_OBJ("storage");
        JSON_FIELD_STR("path", "/usb/") JSON_COMMA;
        JSON_FIELD_STR("name", "usb") JSON_COMMA;
        JSON_FIELD_BOOL("read_only", false);
    JSON_OBJ_END JSON_COMMA;
    if (state.transfer_id.has_value()) {
        JSON_FIELD_OBJ("transfer");
            JSON_FIELD_INT_G(transfer_status.has_value(), "id", transfer_status->id.to_uint32_t()) JSON_COMMA;
            JSON_FIELD_INT_G(transfer_status.has_value(), "time_transferring", transfer_status->time_transferring()) JSON_COMMA;
            JSON_FIELD_FFIXED_G(transfer_status.has_value(), "progress", transfer_status->progress_estimate(), 2) JSON_COMMA;
            JSON_FIELD_INT_G(transfer_status.has_value(), "transferred", transfer_status->download_progress.get_valid_size());
        JSON_OBJ_END JSON_COMMA;
    }
        JSON_FIELD_OBJ("printer");
            JSON_FIELD_STR("state", printer_state::to_str(link_state)) JSON_COMMA;
            JSON_FIELD_FFIXED("temp_bed", marlin_vars().temp_bed, 1) JSON_COMMA;
            JSON_FIELD_FFIXED("target_bed", marlin_vars().target_bed, 1) JSON_COMMA;
            JSON_FIELD_FFIXED("temp_nozzle", marlin_vars().active_hotend().temp_nozzle, 1) JSON_COMMA;
            JSON_FIELD_FFIXED("target_nozzle", marlin_vars().active_hotend().target_nozzle, 1) JSON_COMMA;
            // XYZE, mm
            JSON_FIELD_FFIXED("axis_z", marlin_vars().logical_curr_pos[2], 1) JSON_COMMA;
            if (!marlin_client::is_printing()) {
                JSON_FIELD_FFIXED("axis_x", marlin_vars().logical_curr_pos[0], 1) JSON_COMMA;
                JSON_FIELD_FFIXED("axis_y", marlin_vars().logical_curr_pos[1], 1) JSON_COMMA;
            }
            JSON_FIELD_INT("flow", marlin_vars().active_hotend().flow_factor) JSON_COMMA;
            JSON_FIELD_INT("speed", marlin_vars().print_speed) JSON_COMMA;
            JSON_FIELD_INT("fan_hotend", marlin_vars().active_hotend().heatbreak_fan_rpm) JSON_COMMA;
            JSON_FIELD_INT("fan_print", marlin_vars().active_hotend().print_fan_rpm);
        JSON_OBJ_END;
#if PRINTER_IS_PRUSA_COREONE()
        if (chamber_temp.has_value()) {
            JSON_COMMA;
            JSON_FIELD_OBJ("chamber");
                JSON_FIELD_FFIXED("temp", chamber_temp.value_or(0), 1) JSON_COMMA;
                JSON_FIELD_INT("target_temp", chamber_target.value_or(0)) JSON_COMMA;
                JSON_FIELD_INT("fan_1_rpm", xbe_state.fan1rpm) JSON_COMMA;
                JSON_FIELD_INT("fan_2_rpm", xbe_state.fan2rpm) JSON_COMMA;
                JSON_FIELD_INT("fan_pwm_target", chamber_fan_pwm) JSON_COMMA;
                JSON_FIELD_INT("led_intensity", chamber_led);
            JSON_OBJ_END;
        }
#endif
        if (has_dialog) {
            JSON_COMMA;
            JSON_FIELD_OBJ("dialog");
                JSON_FIELD_INT("id", dialog_id) JSON_COMMA;
                JSON_FIELD_INT("code", dialog_code) JSON_COMMA;
                JSON_FIELD_STR("button0", (dialog_buttons && dialog_buttons[0] != Response::_none) ? to_str(dialog_buttons[0]) : "") JSON_COMMA;
                JSON_FIELD_STR("button1", (dialog_buttons && dialog_buttons[0] != Response::_none && dialog_buttons[1] != Response::_none) ? to_str(dialog_buttons[1]) : "") JSON_COMMA;
                JSON_FIELD_STR("button2", (dialog_buttons && dialog_buttons[0] != Response::_none && dialog_buttons[1] != Response::_none && dialog_buttons[2] != Response::_none) ? to_str(dialog_buttons[2]) : "") JSON_COMMA;
                JSON_FIELD_STR("button3", (dialog_buttons && dialog_buttons[0] != Response::_none && dialog_buttons[1] != Response::_none && dialog_buttons[2] != Response::_none && dialog_buttons[3] != Response::_none) ? to_str(dialog_buttons[3]) : "");
            JSON_OBJ_END;
        }
    JSON_OBJ_END;
    JSON_END;
    // clang-format on
}

} // namespace nhttp::handler
