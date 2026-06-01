#include "moonraker_gcode_script.h"
#include "handler.h"
#include "json_parser.h"
#include "static_mem.h"
#include "status_page.h"
#include "websocket_handler.h"

#include <marlin_client.hpp>

#include <algorithm>
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

    // Moonraker convention for /printer/gcode/script: 200 with {"result":"ok"}.
    constexpr const char success_response[] = "{\"result\":\"ok\"}";

    // Send a single g-code line to Marlin. Skips empty / whitespace-only lines
    // so multi-newline scripts (e.g. "G28\n\nM115") work. Comments (anything
    // from ';' to EOL) are stripped — Fluidd's console pastes user input
    // verbatim including comments.
    void enqueue_one_line(char *line) {
        if (char *sc = strchr(line, ';')) {
            *sc = '\0';
        }
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        char *start = line;
        while (*start == ' ' || *start == '\t') {
            ++start;
        }
        if (*start == '\0') {
            return;
        }
        if (dispatch_klipper_command(start)) {
            return;
        }
        marlin_client::gcode(start);
    }

    // Split a NUL-terminated multi-line script on '\n' (in place — overwrites
    // each newline with '\0') and enqueue each line.
    void enqueue_script_lines(char *script) {
        char *line = script;
        while (line) {
            char *nl = strchr(line, '\n');
            if (nl) {
                *nl = '\0';
            }
            enqueue_one_line(line);
            line = nl ? nl + 1 : nullptr;
        }
    }

    ConnectionState build_success(bool keep_alive) {
        return SendStaticMemory(string_view(success_response), ContentType::ApplicationJson, keep_alive);
    }

    ConnectionState build_error(int code, const char *message, bool keep_alive) {
        // We could emit a Moonraker-shaped {"error":{...}} body, but
        // StatusPage already produces a JSON {"message":"..."} body when
        // json_errors=true. Fluidd extracts message from either shape
        // and surfaces it in the toast UI, so the simpler StatusPage
        // path is sufficient.
        Status s = (code == 413) ? Status::PayloadTooLarge : Status::BadRequest;
        return StatusPage(
            s,
            keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close,
            /*json_errors=*/true,
            std::nullopt,
            message);
    }

    // The JSON parser hands callbacks string_views with explicit length
    // but the underlying buffer is NOT null-terminated past the view —
    // the next byte is the JSON syntax that followed the token. We must
    // bound the script by length before processing, otherwise strchr/
    // strlen will scan into JSON syntax and pass garbage to Marlin.
    //
    // We reuse `buf` as the script destination: after parse_command,
    // the script value lives somewhere inside buf with known offset +
    // length. We memmove it to buf[0] and write '\0' at buf[length],
    // then process it. No extra allocation needed.
    ConnectionState parse_and_dispatch(uint8_t *buf, size_t buf_used,
                                       size_t buf_capacity, bool keep_alive) {
        const char *script_src = nullptr;
        size_t script_len = 0;

        const auto parse_result = parse_command(
            reinterpret_cast<char *>(buf), buf_used,
            [&](const Event &event) {
                if (event.depth != 1 || event.type != Type::String || !event.key || !event.value) {
                    return;
                }
                if (*event.key == "script") {
                    script_src = event.value->data();
                    script_len = event.value->size();
                }
            });

        if (parse_result == JsonParseResult::ErrMem) {
            return build_error(400, "Too many JSON tokens", keep_alive);
        }
        if (parse_result == JsonParseResult::ErrReq) {
            return build_error(400, "Invalid JSON body", keep_alive);
        }
        if (script_src == nullptr) {
            return build_error(400, "Missing 'script' field", keep_alive);
        }

        // memmove to buf[0] (overlap-safe). Cap at capacity-1 to leave
        // room for the trailing '\0'.
        const size_t copy_len = std::min(script_len, buf_capacity - 1);
        memmove(buf, script_src, copy_len);
        buf[copy_len] = '\0';

        enqueue_script_lines(reinterpret_cast<char *>(buf));
        return build_success(keep_alive);
    }

} // namespace

MoonrakerGcodeScript::MoonrakerGcodeScript(size_t content_length, bool can_keep_alive)
    : content_length(content_length)
    , can_keep_alive(can_keep_alive) {
    if (content_length > BUFFER_LEN) {
        body_too_large = true;
    }
    buffer.fill(0);
}

void MoonrakerGcodeScript::step(string_view input, bool terminated_by_client, uint8_t *, size_t, Step &out) {
    if (body_too_large) {
        out = Step { 0, 0, build_error(413, "Request body too large", can_keep_alive) };
        return;
    }

    const size_t rest = content_length - buffer_used;
    const size_t to_read = std::min(input.size(), rest);
    memcpy(buffer.data() + buffer_used, input.data(), to_read);
    buffer_used += to_read;

    if (content_length > buffer_used) {
        if (terminated_by_client) {
            out = Step { to_read, 0, build_error(400, "Truncated request body", can_keep_alive) };
            return;
        }
        out = Step { to_read, 0, Continue() };
        return;
    }

    out = Step { to_read, 0,
                 parse_and_dispatch(buffer.data(), buffer_used,
                                    buffer.size(), can_keep_alive) };
}

} // namespace nhttp::printer
