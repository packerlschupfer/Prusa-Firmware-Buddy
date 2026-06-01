#include "moonraker_file_upload.h"
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

    // Locate `needle` of length `nlen` in `haystack` of length `hlen`.
    // Returns offset or `size_t(-1)` if not found.
    size_t mem_find(const uint8_t *haystack, size_t hlen, const char *needle, size_t nlen) {
        if (nlen == 0 || hlen < nlen) {
            return size_t(-1);
        }
        const size_t end = hlen - nlen;
        for (size_t i = 0; i <= end; ++i) {
            if (memcmp(haystack + i, needle, nlen) == 0) {
                return i;
            }
        }
        return size_t(-1);
    }
} // namespace

MoonrakerFileUpload::MoonrakerFileUpload(const handler::RequestParser &parser)
    : content_length(parser.content_length.value_or(0))
    , can_keep_alive(parser.can_keep_alive()) {

    // Copy boundary string from parser. RequestParser stores it as a
    // string_view that points into the request URL buffer — won't
    // survive past this handler's lifetime, so we copy it now.
    const auto b = parser.boundary();
    if (b.empty() || b.size() > MAX_BOUNDARY_LEN) {
        finalize_failure(400, "Missing or oversized multipart boundary");
        return;
    }
    memcpy(boundary.data(), b.data(), b.size());
    boundary_len = static_cast<uint8_t>(b.size());

    if (content_length == 0) {
        finalize_failure(400, "Missing Content-Length");
        return;
    }
}

MoonrakerFileUpload::~MoonrakerFileUpload() {
    if (out_file) {
        fclose(out_file);
        // If we never reached Done, the file is partial. Best effort
        // cleanup: unlink it.
        if (state != State::Done && filename_len > 0) {
            char path[256];
            snprintf(path, sizeof(path), "/usb/%.*s",
                     static_cast<int>(filename_len), filename.data());
            unlink(path);
        }
    }
}

bool MoonrakerFileUpload::want_read() const {
    return consumed < content_length && state != State::Failed;
}

bool MoonrakerFileUpload::want_write() const {
    return consumed >= content_length || state == State::Failed || state == State::Done;
}

void MoonrakerFileUpload::on_header_line(string_view line) {
    // Strip trailing CR if any (we already split on \n).
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        // End of part headers. Decide which state to transition to.
        if (current_part_is_file) {
            // Open the output file. Filename must have been seen in
            // a Content-Disposition header.
            if (filename_len == 0) {
                finalize_failure(400, "Missing filename in multipart Content-Disposition");
                return;
            }
            char path[256];
            snprintf(path, sizeof(path), "/usb/%.*s",
                     static_cast<int>(filename_len), filename.data());
            out_file = fopen(path, "wb");
            if (out_file == nullptr) {
                finalize_failure(500, "fopen failed");
                return;
            }
            state = State::WriteFileBody;
        } else {
            state = State::SkipPartBody;
        }
        return;
    }

    // Look for Content-Disposition: form-data; name="..."; filename="..."
    static constexpr char prefix[] = "Content-Disposition:";
    const size_t pl = sizeof(prefix) - 1;
    if (line.size() < pl) {
        return;
    }
    // Case-insensitive compare on the header name.
    if (strncasecmp(line.data(), prefix, pl) != 0) {
        return;
    }

    // Look for `name="..."` and `filename="..."` attributes. We must
    // make sure `name=` doesn't match the tail of `filename=`, so we
    // require the preceding byte to be a separator (';' or whitespace)
    // or the start of the attribute section.
    const auto find_attr = [&](const char *attr) -> string_view {
        const size_t alen = strlen(attr); // includes the leading opening `"`
        for (size_t i = pl; i + alen <= line.size(); ++i) {
            // Boundary check: char at i must be at a separator boundary.
            if (i > pl) {
                const char prev = line[i - 1];
                if (prev != ';' && prev != ' ' && prev != '\t') {
                    continue;
                }
            }
            if (memcmp(line.data() + i, attr, alen) == 0) {
                const size_t start = i + alen;
                const size_t end_pos = line.find('"', start);
                if (end_pos == string_view::npos) {
                    return {};
                }
                return line.substr(start, end_pos - start);
            }
        }
        return {};
    };

    bool is_file_field = false;
    const auto name = find_attr("name=\"");
    if (!name.empty() && name == "file") {
        is_file_field = true;
        current_part_is_file = true;
    }

    if (is_file_field) {
        const auto fn = find_attr("filename=\"");
        if (!fn.empty()) {
            const size_t copy = std::min<size_t>(fn.size(), MAX_FILENAME);
            // Reject path-traversal / nested paths.
            for (size_t i = 0; i < copy; ++i) {
                if (fn[i] == '/' || fn[i] == '\\') {
                    finalize_failure(400, "Filename must not contain path separators");
                    return;
                }
            }
            memcpy(filename.data(), fn.data(), copy);
            filename_len = static_cast<uint8_t>(copy);
            filename[copy] = '\0';
        }
    }
}

void MoonrakerFileUpload::process_chunk(string_view chunk) {
    if (state == State::Failed || state == State::Done) {
        return; // drain
    }

    // Combine tail + chunk in a working buffer. For long chunks we
    // process directly from chunk; for short ones we work via the tail.
    // For simplicity we always prepend tail to chunk into a small
    // working buffer of `tail + chunk` size... but `chunk` can be
    // arbitrarily large. So we have to be smart.
    //
    // Strategy: process bytes from `tail` first (if any), then bytes
    // from `chunk`. State-machine functions consume bytes by writing
    // them to file or scanning for boundary. When we run out of input
    // OR are unsure if remaining bytes are a partial boundary, we save
    // the trailing bytes back to `tail`.

    // Simplest viable approach: maintain a single contiguous buffer
    // (tail prepended to chunk). For large chunks this means a copy,
    // but only of the chunk into a heap buffer of size tail+chunk —
    // which is roughly TCP MSS (~1.5 KB) + tail (~80 B). That fits.
    //
    // BUT: chunks can be up to MTU = ~1500 B. Worst case combined ~1580 B.
    // We can use a stack-allocated working buffer for that.

    static constexpr size_t WORK_BUF_LEN = 1700;
    if (tail_used + chunk.size() > WORK_BUF_LEN) {
        // Should not happen given typical MSS, but defensively cap.
        finalize_failure(500, "Chunk too large to process");
        return;
    }
    uint8_t work[WORK_BUF_LEN];
    memcpy(work, tail.data(), tail_used);
    if (!chunk.empty()) {
        memcpy(work + tail_used, chunk.data(), chunk.size());
    }
    size_t work_len = tail_used + chunk.size();
    tail_used = 0;

    size_t pos = 0;
    while (pos < work_len && state != State::Failed && state != State::Done) {
        switch (state) {
        case State::ScanForFirstBoundary: {
            // Look for "--<boundary>\r\n" (or "--<boundary>--\r\n" if it's
            // the terminating boundary, but the first boundary isn't
            // terminating in a well-formed body).
            char marker[MAX_BOUNDARY_LEN + 5]; // "--" + boundary + "\r\n"
            const size_t mlen = 2 + boundary_len + 2;
            memcpy(marker, "--", 2);
            memcpy(marker + 2, boundary.data(), boundary_len);
            memcpy(marker + 2 + boundary_len, "\r\n", 2);
            const size_t found = mem_find(work + pos, work_len - pos, marker, mlen);
            if (found == size_t(-1)) {
                // Save tail = last (mlen-1) bytes so partial boundary at
                // end of chunk isn't lost.
                const size_t keep = std::min<size_t>(work_len - pos, mlen - 1);
                memcpy(tail.data(), work + pos + (work_len - pos) - keep, keep);
                tail_used = keep;
                pos = work_len;
                break;
            }
            // Found. Advance past it; transition to ReadPartHeaders.
            pos += found + mlen;
            header_line_used = 0;
            current_part_is_file = false;
            state = State::ReadPartHeaders;
            break;
        }
        case State::ReadPartHeaders: {
            // Accumulate line by line.
            while (pos < work_len) {
                uint8_t b = work[pos++];
                if (header_line_used < header_line.size()) {
                    header_line[header_line_used++] = b;
                }
                if (b == '\n') {
                    // End of one header line (including the empty separator).
                    const string_view line {
                        header_line.data(),
                        header_line_used - 1 // exclude '\n'; on_header_line trims \r
                    };
                    on_header_line(line);
                    header_line_used = 0;
                    if (state != State::ReadPartHeaders) {
                        break; // on_header_line changed state
                    }
                }
            }
            break;
        }
        case State::SkipPartBody:
        case State::WriteFileBody: {
            // Scan for "\r\n--<boundary>". We need 2 more bytes after
            // the match to disambiguate separator ("\r\n") from terminator
            // ("--"). Total look-back = mlen + 2.
            char marker[MAX_BOUNDARY_LEN + 5];
            const size_t mlen = 4 + boundary_len; // "\r\n--" + boundary
            memcpy(marker, "\r\n--", 4);
            memcpy(marker + 4, boundary.data(), boundary_len);
            const size_t found = mem_find(work + pos, work_len - pos, marker, mlen);
            if (found == size_t(-1)) {
                // No boundary in current buffer. Process all but the
                // last (mlen + 1) bytes; keep them as tail. (+1 because
                // we also need at least 2 trailing bytes after a future
                // match to decide separator vs terminator — being
                // generous with the keep window is cheap.)
                const size_t keep = std::min<size_t>(work_len - pos, mlen + 1);
                const size_t to_emit = (work_len - pos) - keep;
                if (state == State::WriteFileBody && to_emit > 0 && out_file) {
                    if (fwrite(work + pos, 1, to_emit, out_file) != to_emit) {
                        finalize_failure(500, "fwrite failed");
                        return;
                    }
                }
                memcpy(tail.data(), work + pos + to_emit, keep);
                tail_used = keep;
                pos = work_len;
                break;
            }

            // We need 2 bytes past the match to decide separator vs
            // terminator. If we don't have them yet, save the boundary
            // marker + whatever follows back into tail and wait.
            const size_t after_boundary = (work_len - pos) - found - mlen;
            if (after_boundary < 2) {
                const size_t keep = (work_len - pos) - found;
                memcpy(tail.data(), work + pos + found, keep);
                tail_used = keep;
                // Anything before the (eventual-)boundary is body
                // content we need to emit before saving the tail.
                if (state == State::WriteFileBody && found > 0 && out_file) {
                    if (fwrite(work + pos, 1, found, out_file) != found) {
                        finalize_failure(500, "fwrite failed");
                        return;
                    }
                }
                pos = work_len;
                break;
            }

            // Emit pre-boundary content (for file body) and close output.
            if (state == State::WriteFileBody && found > 0 && out_file) {
                if (fwrite(work + pos, 1, found, out_file) != found) {
                    finalize_failure(500, "fwrite failed");
                    return;
                }
            }
            if (state == State::WriteFileBody && out_file) {
                if (fclose(out_file) != 0) {
                    out_file = nullptr;
                    finalize_failure(500, "fclose failed");
                    return;
                }
                out_file = nullptr;
            }

            // Advance past boundary marker.
            pos += found + mlen;

            // Inspect 2 bytes after boundary.
            const uint8_t a = work[pos];
            const uint8_t b = work[pos + 1];
            pos += 2;

            if (a == '-' && b == '-') {
                // Terminating boundary. Done — even if we were just
                // skipping (not writing), we still treat the file as
                // missing if state isn't already past WriteFileBody.
                if (state == State::WriteFileBody || filename_len > 0) {
                    state = State::Done;
                } else {
                    // Reached terminator without ever entering file part.
                    finalize_failure(400, "Multipart body had no 'file' part");
                    return;
                }
            } else if (a == '\r' && b == '\n') {
                // Inter-part separator. Read next part's headers.
                state = State::ReadPartHeaders;
                header_line_used = 0;
                current_part_is_file = false;
            } else {
                finalize_failure(400, "Malformed multipart boundary suffix");
                return;
            }
            break;
        }
        case State::Done:
        case State::Failed:
            // Drained.
            pos = work_len;
            break;
        }
    }
}

void MoonrakerFileUpload::finalize_success() {
    response_code = 200;
    response_len = snprintf(response_buf.data(), response_buf.size(),
                            "{\"result\":{\"action\":\"create_file\","
                            "\"item\":{\"path\":\"%.*s\",\"root\":\"gcodes\"}}}",
                            static_cast<int>(filename_len), filename.data());
    // Bump the global filelist epoch so all active WS clients see a
    // notify_filelist_changed on their next step(). filename is a non
    // null-terminated array; copy into a local for the publisher.
    char path_z[MAX_FILENAME + 1] = {};
    const size_t copy_len = std::min<size_t>(filename_len, MAX_FILENAME);
    std::memcpy(path_z, filename.data(), copy_len);
    publish_filelist_event(FilelistAction::CreateFile, path_z);
}

void MoonrakerFileUpload::finalize_failure(int code, const char *message) {
    state = State::Failed;
    response_code = code;
    response_error_message = message;

    // Best-effort cleanup of partial file.
    if (out_file) {
        fclose(out_file);
        out_file = nullptr;
    }
    if (filename_len > 0) {
        char path[256];
        snprintf(path, sizeof(path), "/usb/%.*s",
                 static_cast<int>(filename_len), filename.data());
        unlink(path);
    }
}

void MoonrakerFileUpload::step(string_view input, bool terminated_by_client, uint8_t *out_buf, size_t out_buf_len, Step &out) {
    // Reading phase: feed incoming bytes through the multipart parser
    // until content_length is satisfied.
    if (consumed < content_length && state != State::Failed) {
        const size_t to_consume = std::min(input.size(), content_length - consumed);
        process_chunk(input.substr(0, to_consume));
        consumed += to_consume;

        if (terminated_by_client && consumed < content_length && state != State::Failed) {
            finalize_failure(400, "Connection closed before content_length");
        }

        if (consumed < content_length && state != State::Failed) {
            out = Step { to_consume, 0, Continue() };
            return;
        }
        // Consumed full body OR failed. Fall through to response.
        if (state == State::Done) {
            finalize_success();
        } else if (state != State::Failed) {
            finalize_failure(400, "Multipart body ended without complete file part");
        }
    }

    // Writing phase: emit response.
    if (state == State::Failed) {
        // Defer to StatusPage for error responses.
        const Status s = (response_code >= 500) ? Status::InternalServerError
                                                : Status::BadRequest;
        out = Step { input.size(), 0,
                     StatusPage(s,
                                can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close,
                                /*json_errors=*/true,
                                std::nullopt,
                                response_error_message ? response_error_message : "Upload failed") };
        return;
    }

    if (!out_buf) {
        out = Step { input.size(), 0, Continue() };
        return;
    }

    size_t sent = 0;
    const ConnectionHandling handling = can_keep_alive ? ConnectionHandling::ContentLengthKeep : ConnectionHandling::Close;

    if (!headers_sent) {
        sent = write_headers(out_buf, out_buf_len, Status::Ok, ContentType::ApplicationJson, handling,
                             response_len, std::nullopt, nullptr);
        headers_sent = true;
    }

    const size_t remaining = response_len;
    const size_t to_copy = std::min(out_buf_len - sent, remaining);
    memcpy(out_buf + sent, response_buf.data(), to_copy);
    if (to_copy < remaining) {
        memmove(response_buf.data(), response_buf.data() + to_copy, remaining - to_copy);
        response_len = remaining - to_copy;
        out = Step { input.size(), sent + to_copy, Continue() };
    } else {
        response_len = 0;
        response_terminating = true;
        out = Step { input.size(), sent + to_copy, Terminating::for_handling(handling) };
    }
}

} // namespace nhttp::printer
