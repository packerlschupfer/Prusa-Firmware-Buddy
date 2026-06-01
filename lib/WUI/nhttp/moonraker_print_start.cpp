#include "moonraker_print_start.h"
#include "handler.h"
#include "json_parser.h"
#include "static_mem.h"
#include "status_page.h"

#include <marlin_client.hpp>
#include <state/printer_state.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nhttp::printer {

namespace {

    using nhttp::handler::ConnectionState;
    using nhttp::handler::Continue;
    using nhttp::handler::SendStaticMemory;
    using nhttp::handler::StatusPage;
    using nhttp::handler::Step;
    using http::ContentType;
    using http::Status;
    using json::Event;
    using json::Type;
    using std::string_view;

    constexpr const char success_response[] = "{\"result\":\"ok\"}";

    ConnectionState build_success(bool keep_alive) {
        return SendStaticMemory(string_view(success_response), ContentType::ApplicationJson, keep_alive);
    }

    ConnectionState build_error(Status s, const char *message, bool keep_alive) {
        return StatusPage(
            s,
            keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close,
            /*json_errors=*/true,
            std::nullopt,
            message);
    }

    ConnectionState parse_and_start(uint8_t *buf, size_t buf_used, size_t buf_capacity, bool keep_alive) {
        const char *filename_src = nullptr;
        size_t filename_len = 0;

        const auto parse_result = parse_command(
            reinterpret_cast<char *>(buf), buf_used,
            [&](const Event &event) {
                if (event.depth != 1 || event.type != Type::String || !event.key || !event.value) {
                    return;
                }
                if (*event.key == "filename") {
                    filename_src = event.value->data();
                    filename_len = event.value->size();
                }
            });

        if (parse_result == JsonParseResult::ErrMem) {
            return build_error(Status::BadRequest, "Too many JSON tokens", keep_alive);
        }
        if (parse_result == JsonParseResult::ErrReq) {
            return build_error(Status::BadRequest, "Invalid JSON body", keep_alive);
        }
        if (filename_src == nullptr) {
            return build_error(Status::BadRequest, "Missing 'filename' field", keep_alive);
        }

        // Reject if the printer is already printing/paused — Marlin would
        // accept a new print_start in some of those states but the user
        // probably did not mean that.
        const auto state = printer_state::get_state(false);
        if (state == printer_state::DeviceState::Printing
            || state == printer_state::DeviceState::Paused) {
            return build_error(Status::Conflict, "Printer is busy", keep_alive);
        }

        // Build the absolute path under /usb/. Same buffer reuse trick:
        // memmove the filename to start of buf (with null term), then
        // prepend "/usb/" into a second small stack scratch buffer.
        constexpr const char prefix[] = "/usb/";
        constexpr size_t prefix_len = sizeof(prefix) - 1;
        const size_t copy_len = std::min(filename_len, buf_capacity - prefix_len - 1);
        // Shift filename right to make room for prefix.
        memmove(buf + prefix_len, filename_src, copy_len);
        memcpy(buf, prefix, prefix_len);
        buf[prefix_len + copy_len] = '\0';

        // Skip all preview screens — Moonraker's contract is "start
        // immediately"; users of Fluidd/Mainsail don't expect to confirm
        // a print preview before it starts.
        marlin_client::print_start(reinterpret_cast<const char *>(buf),
                                   marlin_server::PreviewSkipIfAble::all);
        return build_success(keep_alive);
    }

} // namespace

MoonrakerPrintStart::MoonrakerPrintStart(size_t content_length, bool can_keep_alive)
    : content_length(content_length)
    , can_keep_alive(can_keep_alive) {
    if (content_length > BUFFER_LEN) {
        body_too_large = true;
    }
    buffer.fill(0);
}

void MoonrakerPrintStart::step(string_view input, bool terminated_by_client, uint8_t *, size_t, Step &out) {
    if (body_too_large) {
        out = Step { 0, 0, build_error(Status::PayloadTooLarge, "Request body too large", can_keep_alive) };
        return;
    }

    const size_t rest = content_length - buffer_used;
    const size_t to_read = std::min(input.size(), rest);
    memcpy(buffer.data() + buffer_used, input.data(), to_read);
    buffer_used += to_read;

    if (content_length > buffer_used) {
        if (terminated_by_client) {
            out = Step { to_read, 0, build_error(Status::BadRequest, "Truncated request body", can_keep_alive) };
            return;
        }
        out = Step { to_read, 0, Continue() };
        return;
    }

    out = Step { to_read, 0, parse_and_start(buffer.data(), buffer_used, buffer.size(), can_keep_alive) };
}

} // namespace nhttp::printer
