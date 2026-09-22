#include "sysdvr/sdl_shell.h"
#include "sysdvr/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool sdl_shell_preinit(void)
{
    setenv("EGL_PLATFORM", "wayland", 0);
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);

    if (SDL_Init(0) != 0) {
        LOGF( "[sdl] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    return true;
}

bool sdl_shell_open(sdl_shell *s, const char *title)
{
    memset(s, 0, sizeof(*s));

    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        LOGF( "[sdl] video init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(0, &mode) != 0) {
        LOGF( "[sdl] display mode failed: %s\n", SDL_GetError());
        return false;
    }

    s->width = mode.w;
    s->height = mode.h;

    s->window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        mode.w,
        mode.h,
        SDL_WINDOW_FULLSCREEN
    );

    if (!s->window) {
        LOGF( "[sdl] window failed: %s\n", SDL_GetError());
        return false;
    }

    (void)SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    s->renderer = SDL_CreateRenderer(
        s->window,
        -1,
        SDL_RENDERER_ACCELERATED
    );

    if (!s->renderer) {
        LOGF( "[sdl] renderer failed: %s\n", SDL_GetError());
        return false;
    }

    /*
     * All UI code renders in a fixed 1920x1080 logical coordinate system.
     * The hardware video plane is still positioned using physical dimensions.
     */
    if (SDL_RenderSetLogicalSize(s->renderer, 1920, 1080) != 0) {
        LOGF(
                "[sdl] warning: logical size failed: %s\n",
                SDL_GetError());
    }

    sdl_shell_clear_transparent(s);

    LOGF(
            "[sdl] fullscreen %dx%d, logical UI 1920x1080\n",
            s->width,
            s->height);

    return true;
}

static sdl_shell_action action_from_key(SDL_Scancode scancode)
{
    switch (scancode) {
        case SDL_SCANCODE_UP:
            return SDL_SHELL_ACTION_UP;

        case SDL_SCANCODE_DOWN:
            return SDL_SHELL_ACTION_DOWN;

        case SDL_SCANCODE_LEFT:
            return SDL_SHELL_ACTION_LEFT;

        case SDL_SCANCODE_RIGHT:
            return SDL_SHELL_ACTION_RIGHT;

        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SELECT:
        case SDL_SCANCODE_SPACE:
            return SDL_SHELL_ACTION_OK;

        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_AC_BACK:
            return SDL_SHELL_ACTION_BACK;

        default:
            return SDL_SHELL_ACTION_NONE;
    }
}

bool sdl_shell_poll(
    sdl_shell *s,
    sdl_shell_action *action)
{
    if (action != NULL) {
        *action = SDL_SHELL_ACTION_NONE;
    }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            s->quit_requested = true;
            continue;
        }

        if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0) {
            sdl_shell_action mapped =
                action_from_key(ev.key.keysym.scancode);

            LOGF(
                    "[input] key=%s scancode=%d action=%d\n",
                    SDL_GetScancodeName(ev.key.keysym.scancode),
                    (int)ev.key.keysym.scancode,
                    (int)mapped);

            if (mapped != SDL_SHELL_ACTION_NONE &&
                action != NULL &&
                *action == SDL_SHELL_ACTION_NONE) {
                *action = mapped;
            }
        }
    }

    return !s->quit_requested;
}

void sdl_shell_clear_transparent(sdl_shell *s)
{
    if (s == NULL || s->renderer == NULL) {
        return;
    }

    SDL_SetRenderDrawBlendMode(s->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(s->renderer, 0, 0, 0, 0);
    SDL_RenderClear(s->renderer);
    SDL_RenderPresent(s->renderer);
}

void sdl_shell_close(sdl_shell *s)
{
    if (!s) {
        return;
    }

    if (s->renderer) {
        SDL_DestroyRenderer(s->renderer);
        s->renderer = NULL;
    }

    if (s->window) {
        SDL_DestroyWindow(s->window);
        s->window = NULL;
    }

    SDL_Quit();
}
