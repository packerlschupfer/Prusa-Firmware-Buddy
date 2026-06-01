#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace nhttp::handler {
struct Step;
}

namespace nhttp::printer {

/**
 * \brief Handler for Moonraker's /access/* endpoints.
 *
 * Implements the minimum needed for Fluidd's auth flow:
 *
 *   POST /access/login              {username, password} → {token, refresh_token, ...}
 *   POST /access/refresh_jwt        {refresh_token}       → {token}
 *   POST /access/logout             {}                    → {}
 *   POST /access/oneshot_token      {}                    → token
 *
 * Validation model is intentionally minimal: a successful login requires
 * `password` to match the printer's existing API key (the same key used
 * for X-Api-Key on the HTTP endpoints). After a successful login we hand
 * back a single, long-lived JWT that the WS upgrade also accepts.
 * Refresh and logout don't gate behavior; they just return the
 * canonical JWT (refresh) or empty body (logout).
 *
 * The JWT is generated once on first use and stored in a process-static
 * buffer. Server-side validation is exact string match — we don't
 * actually parse or verify the JWT, just the issued copy. The "JWT
 * shape" matters only so Fluidd's localStorage-stored token decodes
 * cleanly and isn't treated as expired.
 */
class MoonrakerAccessAuth {
public:
    enum class Mode : uint8_t {
        Login,           // requires password match
        RefreshJwt,      // returns canonical JWT (token rotation no-op)
        Logout,          // returns empty body
        OneshotToken,    // returns the canonical JWT as a one-shot token
    };

    static const constexpr size_t BUFFER_LEN = 256;

    MoonrakerAccessAuth(Mode mode, size_t content_length, bool can_keep_alive);
    bool want_read() const { return true; }
    bool want_write() const { return false; }
    void step(std::string_view input, bool terminated_by_client, uint8_t *out_buf, size_t out_buf_len, handler::Step &out);

private:
    std::array<uint8_t, BUFFER_LEN> buffer {};
    size_t buffer_used = 0;
    size_t content_length;
    Mode mode;
    bool can_keep_alive;
    bool body_too_large = false;
};

// Returns true if `token` matches our issued session JWT (or the raw
// API key, kept as a fallback for non-Fluidd clients that prefer that
// shape). Builds the JWT on first call. Safe to call from tcpip
// thread.
bool access_token_valid(std::string_view token);

} // namespace nhttp::printer
