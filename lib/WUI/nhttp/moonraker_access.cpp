#include "moonraker_access.h"
#include "handler.h"
#include "json_parser.h"
#include "send_json.h"
#include "static_mem.h"
#include "status_page.h"
#include "../http_lifetime.h"
#include "server.h"

#include <mbedtls/base64.h>

#include <algorithm>
#include <atomic>
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

    // -------------------------------------------------------------------
    // Session JWT — built once at first use, then exact-matched on each
    // validation. We don't actually verify the signature; the JWT shape
    // exists so Fluidd's localStorage parser is happy.
    // -------------------------------------------------------------------
    //
    // Layout:
    //   "<header_b64>.<payload_b64>.<sig_b64>"
    //
    // Header  : {"alg":"HS256","typ":"JWT"}
    // Payload : {"username":"buddy","exp":2000000000,"token_type":"access","iss":"buddy"}
    // Sig     : 32 zero bytes base64url-encoded (Fluidd doesn't verify)
    //
    // exp = 2000000000 (May 2033) — far enough that Fluidd treats it as
    // non-expired for the printer's lifetime.

    constexpr size_t JWT_BUF_SIZE = 320;
    char g_session_jwt[JWT_BUF_SIZE] = {};
    std::atomic<bool> g_jwt_ready { false };

    void base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size) {
        // Standard base64 first, then in-place transform to base64url
        // (+ → -, / → _, strip = padding).
        size_t out_len = 0;
        mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out), out_size, &out_len,
            in, in_len);
        // Transform & strip padding.
        size_t w = 0;
        for (size_t r = 0; r < out_len; ++r) {
            char c = out[r];
            if (c == '=') continue;
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
            out[w++] = c;
        }
        out[w] = '\0';
    }

    void build_session_jwt_if_needed() {
        if (g_jwt_ready.load(std::memory_order_acquire)) return;
        // Static header — alg name doesn't matter since we don't verify.
        // Base64url("{\"alg\":\"HS256\",\"typ\":\"JWT\"}")
        static const char header_b64[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

        // Minimal payload — Buddy's URL buffer is only MAX_URL_LEN=168
        // bytes total, and `/websocket?access_token=<jwt>` has to fit.
        // We strip the JWT to header + tiny payload + 1-byte signature
        // shell. exp/token_type are the only fields Fluidd actually
        // reads off the token in normal flow.
        const char payload_json[] = "{\"exp\":2000000000,\"token_type\":\"access\"}";
        char payload_b64[80];
        base64url_encode(reinterpret_cast<const uint8_t *>(payload_json),
            sizeof(payload_json) - 1, payload_b64, sizeof payload_b64);

        // Single-byte placeholder signature — JWT format requires the
        // third segment, but we don't verify it.
        std::snprintf(g_session_jwt, JWT_BUF_SIZE, "%s.%s.x",
            header_b64, payload_b64);
        g_jwt_ready.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------
    // Response builders
    // -------------------------------------------------------------------
    //
    // The login / refresh / oneshot endpoints all need to return a body
    // containing the JWT. They share a small scratch in the handler.

    // Format the JSON response for /access/login and /access/refresh_jwt.
    // Returns the byte length written to `out`, or 0 on overflow.
    size_t render_login_response(char *out, size_t cap) {
        build_session_jwt_if_needed();
        // Moonraker wraps all bodies in {"result": ...}. Inner shape:
        //   { "username", "token", "refresh_token", "action", "source" }
        const int n = std::snprintf(out, cap,
            "{\"result\":{\"username\":\"buddy\",\"token\":\"%s\","
            "\"refresh_token\":\"%s\",\"action\":\"user_logged_in\","
            "\"source\":\"moonraker\"}}",
            g_session_jwt, g_session_jwt);
        return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
    }

    size_t render_oneshot_response(char *out, size_t cap) {
        build_session_jwt_if_needed();
        const int n = std::snprintf(out, cap, "{\"result\":\"%s\"}", g_session_jwt);
        return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
    }

    // Static success body for logout.
    constexpr const char logout_response[] =
        "{\"result\":{\"action\":\"user_logged_out\",\"username\":\"buddy\"}}";

    // Send a fixed-size JSON body. The contents are copied into a
    // static scratch buffer so the SendStaticMemory string_view stays
    // valid past the handler's lifetime — necessary because the
    // ConnectionState variant moves the handler around. Sized to fit
    // the login response (envelope + 2 × JWT ≈ 700 bytes worst case).
    char g_response_scratch[1024] = {};

    ConnectionState build_dynamic_response(const char *body, size_t len, bool keep_alive) {
        // Copy into the static scratch; truncate is paranoia (the
        // login response is ~250 bytes).
        const size_t copy = std::min(len, sizeof g_response_scratch - 1);
        std::memcpy(g_response_scratch, body, copy);
        g_response_scratch[copy] = '\0';
        return SendStaticMemory(
            string_view(g_response_scratch, copy),
            ContentType::ApplicationJson, keep_alive);
    }

    ConnectionState build_login_success(bool keep_alive) {
        char buf[1024];
        const size_t n = render_login_response(buf, sizeof buf);
        if (n == 0) {
            return StatusPage(Status::InternalServerError,
                keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close,
                /*json_errors=*/true);
        }
        return build_dynamic_response(buf, n, keep_alive);
    }

    ConnectionState build_oneshot_success(bool keep_alive) {
        char buf[1024];
        const size_t n = render_oneshot_response(buf, sizeof buf);
        if (n == 0) {
            return StatusPage(Status::InternalServerError,
                keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close,
                /*json_errors=*/true);
        }
        return build_dynamic_response(buf, n, keep_alive);
    }

    ConnectionState build_logout_success(bool keep_alive) {
        return SendStaticMemory(
            string_view(logout_response),
            ContentType::ApplicationJson, keep_alive);
    }

    ConnectionState build_auth_failure(bool keep_alive) {
        return StatusPage(
            Status::Unauthorized,
            keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close,
            /*json_errors=*/true,
            std::nullopt,
            "Invalid credentials");
    }

    ConnectionState build_bad_request(const char *msg, bool keep_alive) {
        return StatusPage(
            Status::BadRequest,
            keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close,
            /*json_errors=*/true,
            std::nullopt,
            msg);
    }

    // Parse the body looking for a top-level field of `name` (depth 1,
    // type string). Returns the value string_view (still pointing into
    // the buffer; caller must copy if it needs to mutate the buffer).
    bool find_top_string(char *body, size_t body_used,
                         string_view name, string_view &out_value) {
        const char *value_src = nullptr;
        size_t value_len = 0;
        const auto result = parse_command(body, body_used,
            [&](const Event &event) {
                if (event.depth != 1 || event.type != Type::String
                    || !event.key || !event.value) return;
                if (*event.key == name) {
                    value_src = event.value->data();
                    value_len = event.value->size();
                }
            });
        if (result != JsonParseResult::Ok) return false;
        if (value_src == nullptr) return false;
        out_value = string_view(value_src, value_len);
        return true;
    }

    ConnectionState parse_login(uint8_t *body, size_t body_used, bool keep_alive) {
        string_view password_sv;
        if (!find_top_string(reinterpret_cast<char *>(body), body_used,
                "password", password_sv)) {
            return build_bad_request("Missing 'password'", keep_alive);
        }
        const char *expected = httpd_instance()->get_password();
        if (!expected || *expected == '\0') {
            // No API key set on the printer — accept any login.
            return build_login_success(keep_alive);
        }
        const size_t exp_len = std::strlen(expected);
        if (password_sv.size() != exp_len
            || std::memcmp(password_sv.data(), expected, exp_len) != 0) {
            return build_auth_failure(keep_alive);
        }
        return build_login_success(keep_alive);
    }

    ConnectionState parse_refresh(uint8_t *body, size_t body_used, bool keep_alive) {
        string_view tok_sv;
        if (!find_top_string(reinterpret_cast<char *>(body), body_used,
                "refresh_token", tok_sv)) {
            return build_bad_request("Missing 'refresh_token'", keep_alive);
        }
        // Validate the supplied refresh_token actually matches the
        // canonical JWT (clients that never logged in shouldn't be
        // able to refresh themselves to a fresh access token).
        build_session_jwt_if_needed();
        const size_t jwt_len = std::strlen(g_session_jwt);
        if (tok_sv.size() != jwt_len
            || std::memcmp(tok_sv.data(), g_session_jwt, jwt_len) != 0) {
            return build_auth_failure(keep_alive);
        }
        return build_login_success(keep_alive);
    }

} // namespace

bool access_token_valid(string_view token) {
    if (token.empty()) return false;
    // Accept either the canonical JWT...
    build_session_jwt_if_needed();
    const size_t jwt_len = std::strlen(g_session_jwt);
    if (token.size() == jwt_len
        && std::memcmp(token.data(), g_session_jwt, jwt_len) == 0) {
        return true;
    }
    // ...or the raw API key (kept for non-Fluidd clients that send
    // ?token=<raw_api_key> directly).
    const char *expected = httpd_instance()->get_password();
    if (expected && *expected != '\0') {
        const size_t exp_len = std::strlen(expected);
        if (token.size() == exp_len
            && std::memcmp(token.data(), expected, exp_len) == 0) {
            return true;
        }
    }
    return false;
}

MoonrakerAccessAuth::MoonrakerAccessAuth(Mode mode, size_t content_length, bool can_keep_alive)
    : content_length(content_length)
    , mode(mode)
    , can_keep_alive(can_keep_alive) {
    if (content_length > BUFFER_LEN) {
        body_too_large = true;
    }
}

void MoonrakerAccessAuth::step(string_view input, bool terminated_by_client,
                               uint8_t *, size_t, Step &out) {
    if (body_too_large) {
        out = Step { 0, 0, build_bad_request("Request body too large", can_keep_alive) };
        return;
    }

    // Logout doesn't need to parse the body (it has none in practice;
    // Fluidd sends Content-Length: 0). Just dispatch immediately to
    // avoid blocking on a body that won't come.
    if (mode == Mode::Logout && content_length == 0) {
        out = Step { 0, 0, build_logout_success(can_keep_alive) };
        return;
    }

    // Oneshot token: same — no body required.
    if (mode == Mode::OneshotToken && content_length == 0) {
        out = Step { 0, 0, build_oneshot_success(can_keep_alive) };
        return;
    }

    const size_t rest = content_length - buffer_used;
    const size_t to_read = std::min(input.size(), rest);
    std::memcpy(buffer.data() + buffer_used, input.data(), to_read);
    buffer_used += to_read;

    if (content_length > buffer_used) {
        if (terminated_by_client) {
            out = Step { to_read, 0, build_bad_request("Truncated request body", can_keep_alive) };
        } else {
            out = Step { to_read, 0, Continue() };
        }
        return;
    }

    switch (mode) {
    case Mode::Login:
        out = Step { to_read, 0, parse_login(buffer.data(), buffer_used, can_keep_alive) };
        return;
    case Mode::RefreshJwt:
        out = Step { to_read, 0, parse_refresh(buffer.data(), buffer_used, can_keep_alive) };
        return;
    case Mode::Logout:
        out = Step { to_read, 0, build_logout_success(can_keep_alive) };
        return;
    case Mode::OneshotToken:
        out = Step { to_read, 0, build_oneshot_success(can_keep_alive) };
        return;
    }
}

} // namespace nhttp::printer
