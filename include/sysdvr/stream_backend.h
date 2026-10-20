#pragma once

#include "sysdvr/session_manager.h"
#include "sysdvr/sdl_shell.h"
#include "sysdvr/ui.h"

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum session_result {
    SESSION_RESULT_REMOTE_END = 0,
    SESSION_RESULT_TRANSPORT_LOST = 1,
    SESSION_RESULT_ERROR = 2,
    SESSION_RESULT_USER_BACK = 3
} session_result;

session_result frametap_run_stream_session(
    const session_device *device,
    sdl_shell *shell,
    app_ui *ui,
    const char *app_id,
    volatile sig_atomic_t *global_stop
);

#ifdef __cplusplus
}
#endif
