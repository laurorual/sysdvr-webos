#pragma once

#include <stdbool.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sdl_shell_action {
    SDL_SHELL_ACTION_NONE = 0,
    SDL_SHELL_ACTION_UP,
    SDL_SHELL_ACTION_DOWN,
    SDL_SHELL_ACTION_LEFT,
    SDL_SHELL_ACTION_RIGHT,
    SDL_SHELL_ACTION_OK,
    SDL_SHELL_ACTION_BACK
} sdl_shell_action;

typedef struct sdl_shell {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int width;
    int height;
    bool quit_requested;
} sdl_shell;

bool sdl_shell_preinit(void);
bool sdl_shell_open(sdl_shell *shell, const char *title);

/*
 * Drains SDL events and returns at most one navigation action.
 * false means the application window itself was asked to close.
 */
bool sdl_shell_poll(
    sdl_shell *shell,
    sdl_shell_action *action
);

void sdl_shell_clear_transparent(sdl_shell *shell);
void sdl_shell_close(sdl_shell *shell);

#ifdef __cplusplus
}
#endif
