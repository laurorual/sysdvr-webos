#pragma once

#include "sysdvr/session_manager.h"
#include "sysdvr/sdl_shell.h"

#include <stdbool.h>
#include <SDL.h>
#include <SDL_ttf.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_ui {
    SDL_Renderer *renderer;

    TTF_Font *title_font;
    TTF_Font *body_font;
    TTF_Font *small_font;
    TTF_Font *button_font;

    SDL_Texture *left_icon;
    SDL_Texture *ok_icon;
    SDL_Texture *gamepad_icon;

    char font_path[512];
    char app_dir[512];

    bool ready;
} app_ui;

bool app_ui_init(
    app_ui *ui,
    SDL_Renderer *renderer
);

void app_ui_destroy(app_ui *ui);

void app_ui_render_browser(
    app_ui *ui,
    const session_manager *manager,
    bool scanner_active
);

void app_ui_render_connecting(
    app_ui *ui,
    const session_device *device,
    const char *message
);

void app_ui_render_reconnect(
    app_ui *ui,
    const session_manager *manager
);

void app_ui_render_error(
    app_ui *ui,
    const char *title,
    const char *detail
);

void app_ui_clear_for_video(app_ui *ui);

#ifdef __cplusplus
}
#endif
