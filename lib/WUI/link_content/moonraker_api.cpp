#include "moonraker_api.h"

#include "../nhttp/headers.h"
#include "../nhttp/moonraker_access.h"
#include "../nhttp/moonraker_file_delete.h"
#include "../nhttp/moonraker_file_upload.h"
#include "../nhttp/moonraker_files_list.h"
#include "../nhttp/moonraker_gcode_script.h"
#include "../nhttp/moonraker_print_start.h"
#include "../nhttp/send_json.h"
#include "../nhttp/static_mem.h"
#include "../nhttp/status_page.h"
#include "../nhttp/server.h"
#include "../http_lifetime.h"
#include "prusa_api_helpers.hpp"

#include <marlin_client.hpp>
#include <marlin_vars.hpp>
#include <state/printer_state.hpp>
#include <segmented_json_macros.h>
#include <version/version.hpp>
#include <netdev.h>
#include <feature/chamber_filtration/chamber_filtration.hpp>
#include <feature/xbuddy_extension/xbuddy_extension.hpp>
#include <option/xbuddy_extension_variant.h>
#include <leds/side_strip_handler.hpp>

#include <cstring>

namespace nhttp::link_content {

using http::ContentType;
using http::Method;
using http::Status;
using nhttp::printer::access_token_valid;
using nhttp::printer::MoonrakerAccessAuth;
using nhttp::printer::MoonrakerFileDelete;
using nhttp::printer::MoonrakerFileUpload;
using nhttp::printer::MoonrakerFilesList;
using nhttp::printer::MoonrakerGcodeScript;
using nhttp::printer::MoonrakerPrintStart;
using nhttp::printer::WebSocketHandler;
using std::nullopt;
using std::string_view;
using namespace handler;
using namespace json;
using namespace marlin_server;

namespace {

    // ---- Static endpoints (always-same JSON, cheap to serve) ----

    // GET /server/info — Fluidd hits this first to verify Moonraker is alive.
    // Has to look like a real Moonraker response or Fluidd refuses to connect.
    constexpr const char server_info_response[] =
        "{\"result\":{\"klippy_connected\":true,"
        "\"klippy_state\":\"ready\","
        "\"components\":[\"server\",\"file_manager\",\"machine\",\"klippy_apis\",\"data_store\"],"
        "\"failed_components\":[],"
        "\"registered_directories\":[\"gcodes\"],"
        "\"warnings\":[],"
        "\"websocket_count\":0,"
        "\"moonraker_version\":\"0.8.0-prusalink-shim\","
        "\"api_version\":[1,4,0],"
        "\"api_version_string\":\"1.4.0\"}}";

    // GET /server/config — Fluidd reads this to check feature availability.
    // We claim minimal components so Fluidd doesn't try to use features we
    // don't implement.
    constexpr const char server_config_response[] =
        "{\"result\":{\"config\":{"
        "\"server\":{\"host\":\"0.0.0.0\",\"port\":80,\"klippy_uds_address\":\"/tmp/klippy_uds\"},"
        "\"file_manager\":{\"enable_object_processing\":false},"
        "\"machine\":{\"provider\":\"none\"},"
        "\"authorization\":{\"trusted_clients\":[\"0.0.0.0/0\"],\"cors_domains\":[\"*\"]}"
        "}}}";

    // GET /access/info — advertises login_required:true so Fluidd
    // walks its login UI flow. Once the user enters the printer's
    // API key as the "password", /access/login validates and hands
    // back a JWT that the WS upgrade also accepts.
    constexpr const char access_info_response[] =
        "{\"result\":{\"default_source\":\"moonraker\",\"available_sources\":[\"moonraker\"],"
        "\"login_required\":true}}";

    // ---- Dynamic JSON renderers ----

    JsonResult render_printer_info(size_t resume_point, JsonOutput &output) {
        char hostname[HOSTNAME_LEN + 1];
        netdev_get_hostname(netdev_get_active_id(), hostname, sizeof hostname);

        const auto link_state = printer_state::get_state(false);
        const char *state_str;
        const char *state_msg;
        switch (link_state) {
        case printer_state::DeviceState::Idle:
            state_str = "ready"; state_msg = "Printer is ready"; break;
        case printer_state::DeviceState::Printing:
            state_str = "printing"; state_msg = "Printing"; break;
        case printer_state::DeviceState::Paused:
            state_str = "paused"; state_msg = "Paused"; break;
        case printer_state::DeviceState::Attention:
            state_str = "error"; state_msg = "Attention required"; break;
        case printer_state::DeviceState::Finished:
            state_str = "ready"; state_msg = "Finished"; break;
        default:
            state_str = "ready"; state_msg = "Operational"; break;
        }

        // clang-format off
        JSON_START;
        JSON_OBJ_START;
            JSON_FIELD_OBJ("result");
                JSON_FIELD_STR("state", state_str) JSON_COMMA;
                JSON_FIELD_STR("state_message", state_msg) JSON_COMMA;
                JSON_FIELD_STR("hostname", hostname) JSON_COMMA;
                JSON_FIELD_STR("klipper_path", "/usr/local/lib/klipper") JSON_COMMA;
                JSON_FIELD_STR("python_path", "/usr/bin/python3") JSON_COMMA;
                JSON_FIELD_INT("process_id", 1) JSON_COMMA;
                JSON_FIELD_INT("user_id", 0) JSON_COMMA;
                JSON_FIELD_INT("group_id", 0) JSON_COMMA;
                JSON_FIELD_STR("log_file", "/dev/null") JSON_COMMA;
                JSON_FIELD_STR("config_file", "/etc/klipper/printer.cfg") JSON_COMMA;
                JSON_FIELD_STR("software_version", version::project_version_full) JSON_COMMA;
                JSON_FIELD_STR("cpu_info", "STM32F427");
            JSON_OBJ_END;
        JSON_OBJ_END;
        JSON_END;
        // clang-format on
    }

    // GET /printer/objects/list — what Klipper objects do we expose?
    constexpr const char printer_objects_list_response[] =
        "{\"result\":{\"objects\":["
        "\"webhooks\","
        "\"configfile\","
        "\"mcu\","
        "\"gcode_move\","
        "\"toolhead\","
        "\"extruder\","
        "\"heater_bed\","
        "\"fan\","
        "\"heater_fan heatbreak\","
        "\"fan_generic chamber_fan_1\","
        "\"fan_generic chamber_fan_2\","
        "\"fan_generic filtration_fan\","
        "\"output_pin chamber_led\","
        "\"print_stats\","
        "\"virtual_sdcard\","
        "\"idle_timeout\","
        "\"display_status\","
        "\"pause_resume\","
        "\"motion_report\","
        "\"mmu\""
        "]}}";

    // GET /printer/objects/query — current state of all objects.
    // Fluidd issues this without specifying which objects, so we dump
    // everything. (POST variant with body is treated identically.)
    JsonResult render_objects_query(size_t resume_point, JsonOutput &output) {
        auto &vars = marlin_vars();
        const auto link_state = printer_state::get_state(false);

        const float nozzle_t = vars.active_hotend().temp_nozzle;
        const float nozzle_tgt = vars.active_hotend().target_nozzle;
        const float bed_t = vars.temp_bed;
        const float bed_tgt = vars.target_bed;
        const float pos_x = vars.logical_curr_pos[0];
        const float pos_y = vars.logical_curr_pos[1];
        const float pos_z = vars.logical_curr_pos[2];
        const float pos_e = vars.native_curr_pos[3];
        const uint16_t print_fan = vars.print_fan_speed;
        const float fan_speed = print_fan / 255.0f;
        const uint32_t print_duration = vars.print_duration;
        const uint8_t sd_pct = vars.sd_percent_done;
#if XBUDDY_EXTENSION_VARIANT_IS_STANDARD()
        const uint16_t cf1 = buddy::xbuddy_extension().fan_rpm(buddy::XBuddyExtension::Fan::cooling_fan_1).value_or(0);
        const uint16_t cf2 = buddy::xbuddy_extension().fan_rpm(buddy::XBuddyExtension::Fan::cooling_fan_2).value_or(0);
        const bool filtration_enabled = buddy::chamber_filtration().is_enabled();
        const uint16_t ff = filtration_enabled
            ? buddy::xbuddy_extension().fan_rpm(buddy::XBuddyExtension::Fan::filtration_fan).value_or(0)
            : 0;
        const uint8_t led_w = leds::SideStripHandler::instance().color().w;
#endif

        const char *print_state;
        bool is_active = false;
        switch (link_state) {
        case printer_state::DeviceState::Printing:
            print_state = "printing"; is_active = true; break;
        case printer_state::DeviceState::Paused:
            print_state = "paused"; is_active = true; break;
        case printer_state::DeviceState::Finished:
            print_state = "complete"; break;
        case printer_state::DeviceState::Stopped:
            print_state = "cancelled"; break;
        case printer_state::DeviceState::Attention:
            print_state = "error"; break;
        default:
            print_state = "standby"; break;
        }

        // clang-format off
        JSON_START;
        JSON_OBJ_START;
            JSON_FIELD_OBJ("result");
                JSON_FIELD_FFIXED("eventtime", static_cast<double>(print_duration), 3) JSON_COMMA;
                JSON_FIELD_OBJ("status");
                JSON_FIELD_OBJ("extruder");
                    JSON_FIELD_FFIXED("temperature", static_cast<double>(nozzle_t), 2) JSON_COMMA;
                    JSON_FIELD_FFIXED("target", static_cast<double>(nozzle_tgt), 2) JSON_COMMA;
                    JSON_FIELD_FFIXED("power", 0.0, 2) JSON_COMMA;
                    JSON_FIELD_BOOL("can_extrude", nozzle_t > vars.extrude_min_temp) JSON_COMMA;
                    JSON_FIELD_FFIXED("pressure_advance", 0.0, 4) JSON_COMMA;
                    JSON_FIELD_FFIXED("smooth_time", 0.04, 4);
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("heater_bed");
                    JSON_FIELD_FFIXED("temperature", static_cast<double>(bed_t), 2) JSON_COMMA;
                    JSON_FIELD_FFIXED("target", static_cast<double>(bed_tgt), 2) JSON_COMMA;
                    JSON_FIELD_FFIXED("power", 0.0, 2);
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("toolhead");
                    JSON_FIELD_STR("homed_axes", "xyz") JSON_COMMA;
                    JSON_FIELD_STR("extruder", "extruder") JSON_COMMA;
                    JSON_CUSTOM("\"position\":[%.3f,%.3f,%.3f,%.3f]",
                                static_cast<double>(pos_x),
                                static_cast<double>(pos_y),
                                static_cast<double>(pos_z),
                                static_cast<double>(pos_e)) JSON_COMMA;
                    JSON_CONTROL("\"axis_minimum\":[0,0,0,0],");
                    JSON_CONTROL("\"axis_maximum\":[250,210,270,0],");
                    JSON_FIELD_FFIXED("max_velocity", 200.0, 1) JSON_COMMA;
                    JSON_FIELD_FFIXED("max_accel", 2000.0, 1) JSON_COMMA;
                    JSON_FIELD_FFIXED("square_corner_velocity", 5.0, 1) JSON_COMMA;
                    JSON_FIELD_FFIXED("estimated_print_time", 0.0, 1);
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("fan");
                    JSON_FIELD_FFIXED("speed", static_cast<double>(fan_speed), 3) JSON_COMMA;
                    JSON_FIELD_INT("rpm", vars.active_hotend().print_fan_rpm);
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("heater_fan heatbreak");
                    JSON_FIELD_FFIXED("speed", vars.active_hotend().heatbreak_fan_rpm > 0 ? 1.0 : 0.0, 1) JSON_COMMA;
                    JSON_FIELD_INT("rpm", vars.active_hotend().heatbreak_fan_rpm);
                JSON_OBJ_END JSON_COMMA;
#if XBUDDY_EXTENSION_VARIANT_IS_STANDARD()
                JSON_FIELD_OBJ("fan_generic chamber_fan_1");
                    JSON_FIELD_FFIXED("speed", cf1 > 0 ? 1.0 : 0.0, 1) JSON_COMMA;
                    JSON_FIELD_INT("rpm", cf1);
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("fan_generic chamber_fan_2");
                    JSON_FIELD_FFIXED("speed", cf2 > 0 ? 1.0 : 0.0, 1) JSON_COMMA;
                    JSON_FIELD_INT("rpm", cf2);
                JSON_OBJ_END JSON_COMMA;
                if (filtration_enabled) {
                    JSON_FIELD_OBJ("fan_generic filtration_fan");
                        JSON_FIELD_FFIXED("speed", ff > 0 ? 1.0 : 0.0, 1) JSON_COMMA;
                        JSON_FIELD_INT("rpm", ff);
                    JSON_OBJ_END JSON_COMMA;
                }
                JSON_FIELD_OBJ("output_pin chamber_led");
                    JSON_FIELD_FFIXED("value", static_cast<double>(led_w) / 255.0, 3);
                JSON_OBJ_END JSON_COMMA;
#endif
                JSON_FIELD_OBJ("print_stats");
                    JSON_FIELD_STR("filename", "") JSON_COMMA;
                    JSON_FIELD_INT("total_duration", print_duration) JSON_COMMA;
                    JSON_FIELD_INT("print_duration", print_duration) JSON_COMMA;
                    JSON_FIELD_FFIXED("filament_used", 0.0, 2) JSON_COMMA;
                    JSON_FIELD_STR("state", print_state) JSON_COMMA;
                    JSON_FIELD_STR("message", "");
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("virtual_sdcard");
                    JSON_FIELD_FFIXED("progress", static_cast<double>(sd_pct) / 100.0, 4) JSON_COMMA;
                    JSON_FIELD_BOOL("is_active", is_active) JSON_COMMA;
                    JSON_FIELD_INT("file_position", 0) JSON_COMMA;
                    JSON_FIELD_INT("file_size", 0);
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("idle_timeout");
                    JSON_FIELD_STR("state", is_active ? "Printing" : "Idle") JSON_COMMA;
                    JSON_FIELD_INT("printing_time", print_duration);
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("display_status");
                    JSON_FIELD_FFIXED("progress", static_cast<double>(sd_pct) / 100.0, 4) JSON_COMMA;
                    JSON_FIELD_STR("message", "");
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("webhooks");
                    JSON_FIELD_STR("state", "ready") JSON_COMMA;
                    JSON_FIELD_STR("state_message", "Printer is ready");
                JSON_OBJ_END JSON_COMMA;
                JSON_FIELD_OBJ("gcode_move");
                    JSON_FIELD_FFIXED("speed_factor", 1.0, 2) JSON_COMMA;
                    JSON_FIELD_FFIXED("speed", 100.0, 2) JSON_COMMA;
                    JSON_FIELD_FFIXED("extrude_factor", 1.0, 2) JSON_COMMA;
                    JSON_FIELD_BOOL("absolute_coordinates", true) JSON_COMMA;
                    JSON_FIELD_BOOL("absolute_extrude", false);
                JSON_OBJ_END JSON_COMMA;
                // Empty stub for OrcaSlicer's MoonrakerPrinterAgent
                // which polls `?mmu` looking for Prusa-MMU state. We
                // have no MMU on Core One+; returning an empty object
                // is the "no MMU configured" shape (vs. omitting,
                // which Orca treats as a poll failure).
                JSON_FIELD_OBJ("mmu");
                JSON_OBJ_END;
                JSON_OBJ_END; // close status
            JSON_OBJ_END; // close result
        JSON_OBJ_END; // close outer envelope
        JSON_END;
        // clang-format on
    }

    void serve_static_json(const char *body, const RequestParser &parser, Step &out) {
        out.next = SendStaticMemory(string_view(body), ContentType::ApplicationJson, parser.can_keep_alive());
    }

    // Moonraker print-control success body (shared for pause/resume/cancel).
    constexpr const char print_action_ok[] = "{\"result\":\"ok\"}";

    // Issue a bodyless print state-machine action. The Step.next is filled
    // with either the success response or a Conflict StatusPage if the
    // state machine refuses the action.
    enum class PrintAction { Pause, Resume, Cancel };

    void run_print_action(PrintAction action, const RequestParser &parser, Step &out) {
        const auto state = printer_state::get_state(false);
        bool allowed = false;
        switch (action) {
        case PrintAction::Pause:
            allowed = (state == printer_state::DeviceState::Printing);
            if (allowed) marlin_client::print_pause();
            break;
        case PrintAction::Resume:
            allowed = (state == printer_state::DeviceState::Paused);
            if (allowed) marlin_client::print_resume();
            break;
        case PrintAction::Cancel:
            allowed = (state == printer_state::DeviceState::Printing
                       || state == printer_state::DeviceState::Paused
                       || state == printer_state::DeviceState::Attention);
            if (allowed) marlin_client::print_abort();
            break;
        }
        if (allowed) {
            out.next = SendStaticMemory(string_view(print_action_ok),
                                        ContentType::ApplicationJson, parser.can_keep_alive());
        } else {
            out.next = StatusPage(Status::Conflict, parser);
        }
    }

} // namespace

Selector::Accepted MoonrakerApi::accept(const RequestParser &parser, Step &out) const {
    // Match the path portion only — clients may append `?access_token=...`
    // (Fluidd) or other query params. parser.uri() returns the full
    // request-line URL including any query string; strip it once so
    // every endpoint comparison below sees just the path.
    const string_view raw_uri = parser.uri();
    const auto _qpos = raw_uri.find('?');
    const string_view uri = (_qpos == string_view::npos) ? raw_uri : raw_uri.substr(0, _qpos);

    // OPTIONS preflight: browsers send this before any cross-origin
    // request with non-simple headers (e.g. Content-Type: application/json
    // on /access/login). Respond 204 No Content with CORS headers
    // (write_headers attaches them to every response globally).
    if (parser.method == Method::Options) {
        out.next = StatusPage(Status::NoContent,
            StatusPage::CloseHandling::KeepAlive, parser.accepts_json);
        return Accepted::Accepted;
    }

    // GET /websocket — RFC 6455 upgrade. We DO NOT gate the upgrade
    // on a URL token: browsers' WebSocket constructor doesn't expose
    // a way to add headers, and Fluidd's flow puts access_token in
    // the post-handshake `server.connection.identify` JSON-RPC call,
    // not the URL. We validate access_token there instead — see
    // websocket_handler.cpp.
    if (uri == "/websocket" && parser.is_websocket_upgrade()) {
        out.next = WebSocketHandler(parser);
        return Accepted::Accepted;
    }

    // /access/* endpoints MUST run BEFORE check_auth — they're the path
    // by which clients obtain credentials in the first place. Gating
    // them on X-Api-Key would deadlock the auth flow (Fluidd doesn't
    // know the X-Api-Key, only the user-typed "password").
    if (uri == "/access/info") {
        serve_static_json(access_info_response, parser, out);
        return Accepted::Accepted;
    }
    if (uri == "/access/login" && parser.method == Method::Post) {
        if (parser.content_length.has_value()) {
            out.next = MoonrakerAccessAuth(MoonrakerAccessAuth::Mode::Login,
                *parser.content_length, parser.can_keep_alive());
        } else {
            out.next = StatusPage(Status::LengthRequired, StatusPage::CloseHandling::ErrorClose, parser.accepts_json);
        }
        return Accepted::Accepted;
    }
    if (uri == "/access/refresh_jwt" && parser.method == Method::Post) {
        if (parser.content_length.has_value()) {
            out.next = MoonrakerAccessAuth(MoonrakerAccessAuth::Mode::RefreshJwt,
                *parser.content_length, parser.can_keep_alive());
        } else {
            out.next = StatusPage(Status::LengthRequired, StatusPage::CloseHandling::ErrorClose, parser.accepts_json);
        }
        return Accepted::Accepted;
    }
    if (uri == "/access/logout" && parser.method == Method::Post) {
        out.next = MoonrakerAccessAuth(MoonrakerAccessAuth::Mode::Logout,
            parser.content_length.value_or(0), parser.can_keep_alive());
        return Accepted::Accepted;
    }
    if (uri == "/access/oneshot_token" && (parser.method == Method::Get || parser.method == Method::Post)) {
        out.next = MoonrakerAccessAuth(MoonrakerAccessAuth::Mode::OneshotToken,
            parser.content_length.value_or(0), parser.can_keep_alive());
        return Accepted::Accepted;
    }

    if (!parser.check_auth(out)) {
        return Accepted::Accepted;
    }

    // GET /server/info
    if (uri == "/server/info") {
        serve_static_json(server_info_response, parser, out);
        return Accepted::Accepted;
    }

    // GET /server/config
    if (uri == "/server/config") {
        serve_static_json(server_config_response, parser, out);
        return Accepted::Accepted;
    }

    // GET|POST /server/database/item — stub. OrcaSlicer's
    // MoonrakerPrinterAgent polls `?namespace=lane_data` for AMS state
    // every ~250 ms; without this handler we'd 404-spam the logs.
    // We return an empty value object (the WS variant in
    // websocket_handler.cpp serves real persisted state when present;
    // HTTP-side is simpler — just acknowledge with empty).
    if (uri.starts_with("/server/database/item")) {
        static constexpr char db_empty_response[] =
            "{\"result\":{\"namespace\":\"\",\"key\":\"\",\"value\":{}}}";
        serve_static_json(db_empty_response, parser, out);
        return Accepted::Accepted;
    }

    // GET /printer/info
    if (uri == "/printer/info") {
        get_only(SendJson(EmptyRenderer(render_printer_info), parser.can_keep_alive()), parser, out);
        return Accepted::Accepted;
    }

    // GET /printer/objects/list
    if (uri == "/printer/objects/list") {
        serve_static_json(printer_objects_list_response, parser, out);
        return Accepted::Accepted;
    }

    // GET|POST /printer/objects/query — we ignore the body for now and
    // dump every object every time. Fluidd handles extra fields fine.
    if (uri.starts_with("/printer/objects/query")) {
        get_only(SendJson(EmptyRenderer(render_objects_query), parser.can_keep_alive()), parser, out);
        return Accepted::Accepted;
    }

    // POST /server/files/upload — accept multipart/form-data with a file
    // field, stream the content to /usb/<filename>.
    if (uri == "/server/files/upload" && parser.method == Method::Post) {
        out.next = MoonrakerFileUpload(parser);
        return Accepted::Accepted;
    }

    // GET /server/files/list — list all files in /usb/. Moonraker contract
    // accepts a `root` query param (default "gcodes"); we expose a single
    // root which maps to /usb/ regardless of what the client asks for.
    if (uri.starts_with("/server/files/list")) {
        get_only(SendJson(MoonrakerFilesList(), parser.can_keep_alive()), parser, out);
        return Accepted::Accepted;
    }

    // DELETE /server/files/{root}/{name} — remove a file from /usb/.
    // We take the URL basename (no .. traversal allowed) and hand it to
    // a dedicated MoonrakerFileDelete handler which holds a per-request
    // response buffer and embeds the filename in the Moonraker-shaped
    // response.
    if (parser.method == Method::Delete && uri.starts_with("/server/files/")) {
        const auto last_slash = uri.find_last_of('/');
        if (last_slash == string_view::npos || last_slash + 1 >= uri.size()) {
            out.next = StatusPage(Status::BadRequest, parser);
            return Accepted::Accepted;
        }
        const auto name = uri.substr(last_slash + 1);
        if (name.size() > MoonrakerFileDelete::MAX_FILENAME
            || name.find("..") != string_view::npos) {
            out.next = StatusPage(Status::BadRequest, parser);
            return Accepted::Accepted;
        }
        out.next = MoonrakerFileDelete(name, parser.can_keep_alive());
        return Accepted::Accepted;
    }

    // POST /printer/print/start — start a print of a USB-resident file.
    if (uri == "/printer/print/start" && parser.method == Method::Post) {
        if (parser.content_length.has_value()) {
            out.next = MoonrakerPrintStart(*parser.content_length, parser.can_keep_alive());
        } else {
            out.next = StatusPage(Status::LengthRequired, StatusPage::CloseHandling::ErrorClose, parser.accepts_json);
        }
        return Accepted::Accepted;
    }
    // POST /printer/print/{pause,resume,cancel} — bodyless state-machine actions.
    if (uri == "/printer/print/pause" && parser.method == Method::Post) {
        run_print_action(PrintAction::Pause, parser, out);
        return Accepted::Accepted;
    }
    if (uri == "/printer/print/resume" && parser.method == Method::Post) {
        run_print_action(PrintAction::Resume, parser, out);
        return Accepted::Accepted;
    }
    if (uri == "/printer/print/cancel" && parser.method == Method::Post) {
        run_print_action(PrintAction::Cancel, parser, out);
        return Accepted::Accepted;
    }

    // POST /printer/gcode/script — Moonraker gcode passthrough.
    // Dedicated handler with Moonraker-shaped {"result":"ok"} response;
    // kept separate from PrusaLink's ControlCommand so the two API surfaces
    // remain independently maintainable.
    if (uri == "/printer/gcode/script" && parser.method == Method::Post) {
        if (parser.content_length.has_value()) {
            out.next = MoonrakerGcodeScript(*parser.content_length, parser.can_keep_alive());
        } else {
            out.next = StatusPage(Status::LengthRequired, StatusPage::CloseHandling::ErrorClose, parser.accepts_json);
        }
        return Accepted::Accepted;
    }

    // Any other /server/* or /printer/* — return 501 so callers know
    // explicitly which endpoints are missing.
    if (uri.starts_with("/server/") || uri.starts_with("/printer/")) {
        out.next = StatusPage(Status::NotImplemented, parser);
        return Accepted::Accepted;
    }

    return Accepted::NextSelector;
}

const MoonrakerApi moonraker_api;

} // namespace nhttp::link_content
