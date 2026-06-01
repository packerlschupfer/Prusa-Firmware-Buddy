#pragma once

#include "../nhttp/handler.h"

namespace nhttp::link_content {

/// Moonraker-compatible REST API. Claims the `/server/*` and `/printer/*`
/// URL prefixes. WebSocket (`/websocket`) is NOT implemented yet — Fluidd
/// will fall back to HTTP polling, which is slow but functional.
///
/// This is a subset of Moonraker's API surface, tuned to the bits Fluidd
/// touches during initial dashboard load + basic interaction. Many
/// objects (kinematics, motion_report, mcu, etc.) are stubbed empty.
class MoonrakerApi final : public handler::Selector {
public:
    virtual Accepted accept(const handler::RequestParser &parser, handler::Step &out) const override;
};

extern const MoonrakerApi moonraker_api;

} // namespace nhttp::link_content
