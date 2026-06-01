#pragma once

#include "segmented_json.h"
#include <common/directory.hpp>

#include <dirent.h>
#include <ctime>

namespace nhttp::printer {

/**
 * \brief Per-render state for MoonrakerFilesList — holds the open
 * Directory and current entry across resume points.
 */
class MoonrakerFilesListState {
public:
    Directory dir;
    dirent *current = nullptr; // owned by `dir`; populated by dir.read()
    bool first = true;         // whether we still need to emit a leading separator

    MoonrakerFilesListState();
};

/**
 * \brief Renderer for GET /server/files/list (Moonraker).
 *
 * Output shape (Moonraker contract):
 *   {
 *     "result": [
 *       { "path": "test.gcode", "modified": 1612904613.0, "size": 12345, "permissions": "rw" },
 *       ...
 *     ]
 *   }
 *
 * Each file under /usb/ is emitted as one array entry. The renderer
 * holds a Directory iterator + the dirent for the current entry across
 * resume points, so listings larger than one TCP segment work
 * correctly. Hidden files (leading `.`) and directories are skipped.
 *
 * Only the top-level /usb/ directory is enumerated. Sub-directory
 * recursion is a follow-up — Moonraker's contract supports it via
 * `path` containing slashes, but our PrusaLink FAT storage doesn't
 * usually nest gcodes anyway.
 */
class MoonrakerFilesList final : public json::JsonRenderer<MoonrakerFilesListState> {
public:
    MoonrakerFilesList();
    virtual json::JsonResult renderState(size_t resume_point, json::JsonOutput &output, MoonrakerFilesListState &state) const override;
};

} // namespace nhttp::printer
