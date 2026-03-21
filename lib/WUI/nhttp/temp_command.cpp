#include "temp_command.h"
#include "handler.h"
#include "json_parser.h"

#include <cassert>
#include <cstring>
#include <optional>

namespace nhttp::printer {

using namespace handler;
using http::Status;
using json::Event;
using json::Type;
using std::nullopt;
using std::optional;
using std::string_view;

TempCommand::TempCommand(size_t content_length, bool can_keep_alive, bool json_errors)
    : content_length(content_length)
    , can_keep_alive(can_keep_alive)
    , json_errors(json_errors) {
    memset(buffer.data(), 0, buffer.size());
}

StatusPage TempCommand::process() {
    optional<int> nozzle_temp;
    optional<int> bed_temp;
    optional<int> chamber_temp;

    const auto parse_result = parse_command(reinterpret_cast<char *>(buffer.data()), buffer_used, [&](const Event &event) {
        if (event.depth != 1 || event.type != Type::Primitive) {
            return;
        }
        const auto &key = event.key.value();
        const auto &value = event.value.value();
        int temp = atoi(value.data());
        if (key == "nozzle") {
            nozzle_temp = temp;
        } else if (key == "bed") {
            bed_temp = temp;
        } else if (key == "chamber") {
            chamber_temp = temp;
        }
    });

    switch (parse_result) {
    case JsonParseResult::ErrMem:
        return StatusPage(Status::PayloadTooLarge, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Too many JSON tokens");
    case JsonParseResult::ErrReq:
        return StatusPage(Status::BadRequest, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Couldn't parse JSON");
    case JsonParseResult::Ok:
        break;
    }

    if (!nozzle_temp.has_value() && !bed_temp.has_value() && !chamber_temp.has_value()) {
        return StatusPage(Status::BadRequest, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "No temperature fields provided");
    }

    if (nozzle_temp.has_value()) {
        set_nozzle_temp(*nozzle_temp);
    }
    if (bed_temp.has_value()) {
        set_bed_temp(*bed_temp);
    }
    if (chamber_temp.has_value()) {
        set_chamber_temp(*chamber_temp);
    }

    return StatusPage(Status::NoContent, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors);
}

void TempCommand::step(string_view input, bool terminated_by_client, uint8_t *, size_t, Step &out) {
    if (content_length > buffer.size()) {
        out = Step { 0, 0, StatusPage(Status::PayloadTooLarge, StatusPage::CloseHandling::ErrorClose, json_errors) };
        return;
    }

    const size_t rest = content_length - buffer_used;
    const size_t to_read = std::min(input.size(), rest);

    memcpy(buffer.data() + buffer_used, input.data(), to_read);
    buffer_used += to_read;

    if (content_length > buffer_used) {
        if (terminated_by_client) {
            out = Step { to_read, 0, StatusPage(Status::BadRequest, StatusPage::CloseHandling::ErrorClose, json_errors, nullopt, "Truncated request") };
            return;
        } else {
            out = Step { to_read, 0, Continue() };
            return;
        }
    }

    out = Step { to_read, 0, process() };
}

} // namespace nhttp::printer
