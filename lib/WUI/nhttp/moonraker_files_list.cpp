#include "moonraker_files_list.h"

#include <segmented_json_macros.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace nhttp::printer {

namespace {
    bool is_visible_file(const dirent *e) {
        if (!e || e->d_name[0] == '.') {
            return false;
        }
        return e->d_type == DT_REG;
    }
} // namespace

MoonrakerFilesListState::MoonrakerFilesListState()
    : dir { "/usb" } {
}

MoonrakerFilesList::MoonrakerFilesList()
    : JsonRenderer(MoonrakerFilesListState()) {
}

json::JsonResult MoonrakerFilesList::renderState(size_t resume_point, json::JsonOutput &output, MoonrakerFilesListState &state) const {
    // Scratch space declared at function-top: C++ doesn't allow case
    // labels to jump over local variable declarations, and JSON_OUT
    // generates case labels at each macro use. So everything we need
    // per-iteration goes here.
    struct stat st {};
    char path_buf[300];

    // clang-format off
    JSON_START;
    JSON_OBJ_START;
        JSON_FIELD_ARR("result");

        // Iterate the directory inside the renderer. Each JSON_FIELD_*
        // macro inside the loop is its own resume point, so if the
        // output buffer fills mid-entry we re-enter exactly where we
        // left off. Pattern mirrors FileInfo::DirRenderer::renderStateV1.
        while (state.dir && (state.current = state.dir.read())) {
            if (!is_visible_file(state.current)) {
                continue;
            }

            snprintf(path_buf, sizeof(path_buf), "/usb/%s", state.current->d_name);
            if (stat(path_buf, &st) != 0) {
                continue;
            }

            if (!state.first) {
                JSON_CONTROL(",");
            } else {
                state.first = false;
            }

            JSON_OBJ_START;
                JSON_FIELD_STR("path", state.current->d_name) JSON_COMMA;
                JSON_FIELD_INT("modified", static_cast<int>(st.st_mtime)) JSON_COMMA;
                JSON_FIELD_INT("size", static_cast<int>(st.st_size)) JSON_COMMA;
                JSON_FIELD_STR("permissions", "rw");
            JSON_OBJ_END;
        }
        JSON_ARR_END;
    JSON_OBJ_END;
    JSON_END;
    // clang-format on
}

} // namespace nhttp::printer
