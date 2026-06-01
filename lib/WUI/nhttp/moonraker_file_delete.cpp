#include "moonraker_file_delete.h"
#include "handler.h"
#include "headers.h"
#include "status_page.h"
#include "websocket_handler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace nhttp::printer {

namespace {
    using nhttp::handler::Continue;
    using nhttp::handler::Step;
    using nhttp::handler::StatusPage;
    using nhttp::handler::Terminating;
    using nhttp::write_headers;
    using http::ConnectionHandling;
    using http::ContentType;
    using http::Status;
    using std::string_view;
} // namespace

MoonrakerFileDelete::MoonrakerFileDelete(string_view filename, bool keep_alive)
    : can_keep_alive(keep_alive) {
    response_buf.fill(0);

    // Build the absolute path and unlink. We refuse anything containing
    // a slash or "..", as a paranoid second line of defense — the
    // dispatcher should have stripped to basename already.
    char path[256];
    const size_t fn_len = std::min(filename.size(), MAX_FILENAME);
    bool unsafe = false;
    for (size_t i = 0; i < fn_len; ++i) {
        if (filename[i] == '/' || filename[i] == '\\') {
            unsafe = true;
            break;
        }
    }
    if (unsafe || (fn_len >= 2 && filename[0] == '.' && filename[1] == '.')) {
        unlink_failed = true;
        return;
    }

    snprintf(path, sizeof(path), "/usb/%.*s",
             static_cast<int>(fn_len), filename.data());
    if (unlink(path) != 0) {
        unlink_failed = true;
        return;
    }

    // Notify WS clients that a file was removed.
    {
        char path_z[MAX_FILENAME + 1] = {};
        std::memcpy(path_z, filename.data(), fn_len);
        publish_filelist_event(FilelistAction::DeleteFile, path_z);
    }

    // Build the Moonraker-shaped success response with the filename
    // embedded. Buffer size guarantees fit (MAX_FILENAME + envelope).
    response_len = snprintf(response_buf.data(), response_buf.size(),
                            "{\"result\":{\"action\":\"delete_file\","
                            "\"item\":{\"path\":\"%.*s\",\"root\":\"gcodes\"}}}",
                            static_cast<int>(fn_len), filename.data());
    if (response_len >= response_buf.size()) {
        // Truncated — shouldn't happen given size accounting, but be defensive.
        response_len = response_buf.size() - 1;
    }
}

void MoonrakerFileDelete::step(string_view, bool, uint8_t *buffer, size_t buff_len, Step &out) {
    if (unlink_failed) {
        // Defer to the standard StatusPage for the not-found case. Doing
        // so on first invocation is safe because StatusPage handles its
        // own response generation.
        out = Step { 0, 0,
                     StatusPage(Status::NotFound,
                                can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close,
                                /*json_errors=*/true,
                                std::nullopt,
                                "File not found") };
        return;
    }

    if (!buffer) {
        out = Step { 0, 0, Continue() };
        return;
    }

    size_t sent = 0;
    const ConnectionHandling handling = can_keep_alive ? ConnectionHandling::ContentLengthKeep : ConnectionHandling::Close;

    if (!headers_sent) {
        sent = write_headers(buffer, buff_len, Status::Ok, ContentType::ApplicationJson, handling,
                             response_len, std::nullopt, nullptr);
        headers_sent = true;
    }

    // Copy what fits, leave the rest for the next call.
    const size_t remaining = response_len;
    const size_t to_copy = std::min(buff_len - sent, remaining);
    memcpy(buffer + sent, response_buf.data(), to_copy);

    // Shift the unsent portion to the front of the buffer for the next
    // step() call.
    if (to_copy < remaining) {
        memmove(response_buf.data(), response_buf.data() + to_copy, remaining - to_copy);
        response_len = remaining - to_copy;
        out = Step { 0, sent + to_copy, Continue() };
    } else {
        response_len = 0;
        out = Step { 0, sent + to_copy, Terminating::for_handling(handling) };
    }
}

} // namespace nhttp::printer
