#include "sysdvr/ui.h"
#include "sysdvr/log.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UI_WIDTH 1920
#define UI_HEIGHT 1080

#define LEFT_ICON_W 240
#define LEFT_ICON_H 120
#define OK_ICON_W 180
#define OK_ICON_H 100
#define GAMEPAD_ICON_W 240
#define GAMEPAD_ICON_H 180

static const SDL_Color COLOR_BG = {46, 46, 46, 255};
static const SDL_Color COLOR_PANEL = {34, 37, 42, 255};
static const SDL_Color COLOR_TEXT = {245, 245, 245, 255};
static const SDL_Color COLOR_MUTED = {190, 190, 190, 255};
static const SDL_Color COLOR_LINE = {125, 125, 125, 255};
static const SDL_Color COLOR_FOCUS = {92, 224, 230, 255};
static const SDL_Color COLOR_ICON = {230, 230, 230, 255};

static bool executable_directory(
    char *out,
    size_t out_size)
{
    ssize_t len = readlink("/proc/self/exe", out, out_size - 1);
    if (len <= 0 || (size_t)len >= out_size) {
        return false;
    }

    out[len] = '\0';

    char *slash = strrchr(out, '/');
    if (slash == NULL) {
        return false;
    }

    *slash = '\0';
    return true;
}

static bool ends_with_ttf(const char *name)
{
    if (name == NULL) {
        return false;
    }

    size_t n = strlen(name);
    return n >= 4 &&
           name[n - 4] == '.' &&
           (name[n - 3] == 't' || name[n - 3] == 'T') &&
           (name[n - 2] == 't' || name[n - 2] == 'T') &&
           (name[n - 1] == 'f' || name[n - 1] == 'F');
}

static bool find_first_ttf(
    const char *directory,
    int depth,
    char *out,
    size_t out_size)
{
    if (depth < 0) {
        return false;
    }

    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return false;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[512];
        int n = snprintf(
            path,
            sizeof(path),
            "%s/%s",
            directory,
            entry->d_name
        );

        if (n <= 0 || (size_t)n >= sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        if (S_ISREG(st.st_mode) && ends_with_ttf(entry->d_name)) {
            snprintf(out, out_size, "%s", path);
            closedir(dir);
            return true;
        }

        if (S_ISDIR(st.st_mode) &&
            find_first_ttf(path, depth - 1, out, out_size)) {
            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    return false;
}

static bool find_system_font(
    char *out,
    size_t out_size)
{
    static const char *candidates[] = {
        "/usr/share/fonts/LG_Display.ttf",
        "/usr/share/fonts/LG_Display_Light.ttf",
        "/usr/share/fonts/PreludeCondensed-Medium.ttf",
        "/usr/share/fonts/tt7268m_804.ttf",
        "/usr/share/fonts/TTF/LG_Display.ttf",
        "/usr/share/fonts/TTF/PreludeCondensed-Medium.ttf",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (access(candidates[i], R_OK) == 0) {
            snprintf(out, out_size, "%s", candidates[i]);
            return true;
        }
    }

    return find_first_ttf("/usr/share/fonts", 4, out, out_size);
}

static TTF_Font *open_font(
    const char *path,
    int size)
{
    TTF_Font *font = TTF_OpenFont(path, size);
    if (font == NULL) {
        LOGF(
                "[ui] TTF_OpenFont(%s,%d) failed: %s\n",
                path,
                size,
                TTF_GetError());
    }
    return font;
}

static SDL_Texture *load_rgba_asset(
    app_ui *ui,
    const char *name,
    int width,
    int height)
{
    char path[768];
    int n = snprintf(
        path,
        sizeof(path),
        "%s/assets/%s",
        ui->app_dir,
        name
    );

    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return NULL;
    }

    size_t expected =
        (size_t)width * (size_t)height * 4u;

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        LOGF( "[ui] asset missing: %s\n", path);
        return NULL;
    }

    uint8_t *pixels = malloc(expected);
    if (pixels == NULL) {
        fclose(file);
        return NULL;
    }

    size_t got = fread(pixels, 1, expected, file);
    int trailing = fgetc(file);
    fclose(file);

    if (got != expected || trailing != EOF) {
        LOGF(
                "[ui] invalid asset size: %s (got=%zu expected=%zu)\n",
                path,
                got,
                expected);
        free(pixels);
        return NULL;
    }

    SDL_Surface *surface =
        SDL_CreateRGBSurfaceWithFormatFrom(
            pixels,
            width,
            height,
            32,
            width * 4,
            SDL_PIXELFORMAT_RGBA32
        );

    if (surface == NULL) {
        LOGF(
                "[ui] surface creation failed for %s: %s\n",
                path,
                SDL_GetError());
        free(pixels);
        return NULL;
    }

    SDL_Texture *texture =
        SDL_CreateTextureFromSurface(
            ui->renderer,
            surface
        );

    SDL_FreeSurface(surface);
    free(pixels);

    if (texture == NULL) {
        LOGF(
                "[ui] texture creation failed for %s: %s\n",
                path,
                SDL_GetError());
        return NULL;
    }

    (void)SDL_SetTextureBlendMode(
        texture,
        SDL_BLENDMODE_BLEND
    );

    LOGF(
            "[ui] loaded asset %s (%dx%d)\n",
            name,
            width,
            height);

    return texture;
}

bool app_ui_init(
    app_ui *ui,
    SDL_Renderer *renderer)
{
    memset(ui, 0, sizeof(*ui));
    ui->renderer = renderer;

    if (!executable_directory(
            ui->app_dir,
            sizeof(ui->app_dir))) {
        snprintf(ui->app_dir, sizeof(ui->app_dir), "%s", ".");
    }

    if (TTF_Init() != 0) {
        LOGF( "[ui] TTF_Init failed: %s\n", TTF_GetError());
        return false;
    }

    if (!find_system_font(ui->font_path, sizeof(ui->font_path))) {
        LOGF(
                "[ui] no readable TTF found under /usr/share/fonts\n");
        TTF_Quit();
        return false;
    }

    LOGF( "[ui] system font: %s\n", ui->font_path);

    ui->title_font = open_font(ui->font_path, 40);
    ui->body_font = open_font(ui->font_path, 32);
    ui->small_font = open_font(ui->font_path, 24);
    ui->button_font = open_font(ui->font_path, 23);

    if (!ui->title_font ||
        !ui->body_font ||
        !ui->small_font ||
        !ui->button_font) {
        app_ui_destroy(ui);
        return false;
    }

    ui->left_icon =
        load_rgba_asset(
            ui,
            "icon_left.rgba",
            LEFT_ICON_W,
            LEFT_ICON_H
        );

    ui->ok_icon =
        load_rgba_asset(
            ui,
            "icon_ok.rgba",
            OK_ICON_W,
            OK_ICON_H
        );

    ui->gamepad_icon =
        load_rgba_asset(
            ui,
            "icon_gamepad.rgba",
            GAMEPAD_ICON_W,
            GAMEPAD_ICON_H
        );

    if (!ui->left_icon ||
        !ui->ok_icon ||
        !ui->gamepad_icon) {
        LOGF(
                "[ui] one or more icon assets unavailable; "
                "vector fallbacks will be used\n");
    }

    ui->ready = true;
    return true;
}

void app_ui_destroy(app_ui *ui)
{
    if (ui == NULL) {
        return;
    }

    if (ui->left_icon) {
        SDL_DestroyTexture(ui->left_icon);
    }
    if (ui->ok_icon) {
        SDL_DestroyTexture(ui->ok_icon);
    }
    if (ui->gamepad_icon) {
        SDL_DestroyTexture(ui->gamepad_icon);
    }

    ui->left_icon = NULL;
    ui->ok_icon = NULL;
    ui->gamepad_icon = NULL;

    if (ui->title_font) {
        TTF_CloseFont(ui->title_font);
    }
    if (ui->body_font) {
        TTF_CloseFont(ui->body_font);
    }
    if (ui->small_font) {
        TTF_CloseFont(ui->small_font);
    }
    if (ui->button_font) {
        TTF_CloseFont(ui->button_font);
    }

    ui->title_font = NULL;
    ui->body_font = NULL;
    ui->small_font = NULL;
    ui->button_font = NULL;

    if (TTF_WasInit()) {
        TTF_Quit();
    }

    ui->ready = false;
}

static void set_color(
    SDL_Renderer *renderer,
    SDL_Color color)
{
    SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    );
}

static void fill_rect(
    SDL_Renderer *renderer,
    const SDL_Rect *rect,
    SDL_Color color)
{
    set_color(renderer, color);
    SDL_RenderFillRect(renderer, rect);
}

static void stroke_rect(
    SDL_Renderer *renderer,
    SDL_Rect rect,
    SDL_Color color,
    int thickness)
{
    set_color(renderer, color);

    for (int i = 0; i < thickness; ++i) {
        SDL_Rect r = {
            rect.x - i,
            rect.y - i,
            rect.w + i * 2,
            rect.h + i * 2,
        };
        SDL_RenderDrawRect(renderer, &r);
    }
}

static int text_width(
    TTF_Font *font,
    const char *text)
{
    int w = 0;
    int h = 0;

    if (TTF_SizeUTF8(font, text, &w, &h) != 0) {
        return 0;
    }

    return w;
}

static void draw_text(
    app_ui *ui,
    TTF_Font *font,
    const char *text,
    int x,
    int y,
    SDL_Color color)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    SDL_Surface *surface =
        TTF_RenderUTF8_Blended(
            font,
            text,
            color
        );

    if (surface == NULL) {
        return;
    }

    SDL_Texture *texture =
        SDL_CreateTextureFromSurface(
            ui->renderer,
            surface
        );

    if (texture != NULL) {
        SDL_Rect dst = {
            .x = x,
            .y = y,
            .w = surface->w,
            .h = surface->h,
        };

        SDL_RenderCopy(
            ui->renderer,
            texture,
            NULL,
            &dst
        );

        SDL_DestroyTexture(texture);
    }

    SDL_FreeSurface(surface);
}

static void draw_text_centered(
    app_ui *ui,
    TTF_Font *font,
    const char *text,
    int center_x,
    int y,
    SDL_Color color)
{
    int width = text_width(font, text);

    draw_text(
        ui,
        font,
        text,
        center_x - width / 2,
        y,
        color
    );
}

static void draw_line(
    SDL_Renderer *renderer,
    int x1,
    int y1,
    int x2,
    int y2,
    SDL_Color color)
{
    set_color(renderer, color);
    SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
}

static void fill_circle(
    SDL_Renderer *renderer,
    int cx,
    int cy,
    int radius,
    SDL_Color color)
{
    set_color(renderer, color);

    for (int y = -radius; y <= radius; ++y) {
        int span =
            (int)sqrt(
                (double)(radius * radius - y * y)
            );

        SDL_RenderDrawLine(
            renderer,
            cx - span,
            cy + y,
            cx + span,
            cy + y
        );
    }
}

static void draw_gamepad_fallback(
    app_ui *ui,
    int x,
    int y)
{
    SDL_Renderer *r = ui->renderer;
    set_color(r, COLOR_ICON);

    SDL_Rect body = {x + 12, y + 18, 100, 46};
    SDL_RenderDrawRect(r, &body);

    SDL_RenderDrawLine(r, x + 12, y + 35, x + 1, y + 47);
    SDL_RenderDrawLine(r, x + 1, y + 47, x + 9, y + 68);
    SDL_RenderDrawLine(r, x + 112, y + 35, x + 123, y + 47);
    SDL_RenderDrawLine(r, x + 123, y + 47, x + 115, y + 68);

    SDL_RenderDrawLine(r, x + 31, y + 43, x + 47, y + 43);
    SDL_RenderDrawLine(r, x + 39, y + 35, x + 39, y + 51);

    fill_circle(r, x + 84, y + 39, 4, COLOR_ICON);
    fill_circle(r, x + 95, y + 50, 4, COLOR_ICON);
}

static void draw_gamepad_icon(
    app_ui *ui,
    SDL_Rect rect)
{
    if (ui->gamepad_icon != NULL) {
        SDL_RenderCopy(
            ui->renderer,
            ui->gamepad_icon,
            NULL,
            &rect
        );
        return;
    }

    draw_gamepad_fallback(
        ui,
        rect.x,
        rect.y
    );
}

static void draw_left_fallback(
    app_ui *ui,
    SDL_Rect rect)
{
    SDL_Renderer *r = ui->renderer;
    set_color(r, COLOR_TEXT);

    int cy = rect.y + rect.h / 2;
    int left = rect.x + 5;
    int right = rect.x + rect.w - 5;

    SDL_RenderDrawLine(r, right, cy, left + 12, cy);
    SDL_RenderDrawLine(r, left + 12, cy, left + 24, cy - 10);
    SDL_RenderDrawLine(r, left + 12, cy, left + 24, cy + 10);
}

static void draw_ok_fallback(
    app_ui *ui,
    SDL_Rect rect)
{
    stroke_rect(
        ui->renderer,
        rect,
        COLOR_TEXT,
        2
    );

    draw_text(
        ui,
        ui->button_font,
        "OK",
        rect.x + 10,
        rect.y + 4,
        COLOR_TEXT
    );
}

static void draw_footer_action(
    app_ui *ui,
    int x,
    int y,
    bool ok,
    const char *label)
{
    if (ok) {
        SDL_Rect icon = {
            .x = x,
            .y = y + 2,
            .w = 58,
            .h = 32,
        };

        if (ui->ok_icon != NULL) {
            SDL_RenderCopy(
                ui->renderer,
                ui->ok_icon,
                NULL,
                &icon
            );
        } else {
            draw_ok_fallback(ui, icon);
        }

        draw_text(
            ui,
            ui->small_font,
            label,
            x + 74,
            y,
            COLOR_TEXT
        );
    } else {
        SDL_Rect icon = {
            .x = x,
            .y = y + 3,
            .w = 58,
            .h = 29,
        };

        if (ui->left_icon != NULL) {
            SDL_RenderCopy(
                ui->renderer,
                ui->left_icon,
                NULL,
                &icon
            );
        } else {
            draw_left_fallback(ui, icon);
        }

        draw_text(
            ui,
            ui->small_font,
            label,
            x + 70,
            y,
            COLOR_TEXT
        );
    }
}

static void begin_page(app_ui *ui)
{
    SDL_SetRenderDrawBlendMode(
        ui->renderer,
        SDL_BLENDMODE_NONE
    );

    fill_rect(
        ui->renderer,
        &(SDL_Rect){0, 0, UI_WIDTH, UI_HEIGHT},
        COLOR_BG
    );
}

static void draw_header(app_ui *ui)
{
    draw_text(
        ui,
        ui->title_font,
        "Choose a Nintendo Switch",
        76,
        42,
        COLOR_TEXT
    );

    draw_line(
        ui->renderer,
        46,
        112,
        1874,
        112,
        COLOR_LINE
    );
}

static void draw_footer(
    app_ui *ui,
    const char *left_label,
    bool can_connect)
{
    draw_line(
        ui->renderer,
        46,
        966,
        1874,
        966,
        COLOR_LINE
    );

    int left_x =
        can_connect ? 1470 : 1630;

    draw_footer_action(
        ui,
        left_x,
        999,
        false,
        left_label
    );

    if (can_connect) {
        draw_footer_action(
            ui,
            1688,
            999,
            true,
            "Connect"
        );
    }
}

static void device_secondary(
    const session_device *device,
    char *out,
    size_t out_size)
{
    if (device->manual) {
        snprintf(
            out,
            out_size,
            "%s  |  Manual address",
            device->info.host
        );
        return;
    }

    if (device->info.serial[0] != '\0') {
        snprintf(
            out,
            out_size,
            "%s  |  SysDVR %s  |  Protocol %s  |  %s",
            device->info.host,
            device->info.client_version,
            device->info.protocol,
            device->info.serial
        );
    } else {
        snprintf(
            out,
            out_size,
            "%s  |  SysDVR %s  |  Protocol %s",
            device->info.host,
            device->info.client_version,
            device->info.protocol
        );
    }
}

static void draw_device_card(
    app_ui *ui,
    const session_device *device,
    SDL_Rect rect,
    bool selected)
{
    fill_rect(
        ui->renderer,
        &rect,
        selected ? COLOR_PANEL : COLOR_BG
    );

    stroke_rect(
        ui->renderer,
        rect,
        selected ? COLOR_FOCUS : COLOR_MUTED,
        selected ? 4 : 2
    );

    SDL_Rect gamepad = {
        .x = rect.x + 58,
        .y = rect.y + 31,
        .w = 126,
        .h = 94,
    };

    draw_gamepad_icon(ui, gamepad);

    draw_text(
        ui,
        ui->body_font,
        "Nintendo Switch",
        rect.x + 245,
        rect.y + 38,
        COLOR_TEXT
    );

    char detail[320];
    device_secondary(
        device,
        detail,
        sizeof(detail)
    );

    draw_text(
        ui,
        ui->small_font,
        detail,
        rect.x + 245,
        rect.y + 91,
        COLOR_MUTED
    );
}

static void draw_spinner(
    app_ui *ui,
    int center_x,
    int center_y)
{
    uint32_t phase =
        (SDL_GetTicks() / 125u) % 8u;

    for (int i = 0; i < 8; ++i) {
        double angle =
            ((double)i / 8.0) *
            6.283185307179586;

        int x =
            center_x +
            (int)(cos(angle) * 45.0);

        int y =
            center_y +
            (int)(sin(angle) * 45.0);

        int distance =
            (i - (int)phase + 8) % 8;

        uint8_t shade =
            (uint8_t)(245 - distance * 22);

        SDL_Color color = {
            shade,
            shade,
            shade,
            255
        };

        fill_circle(
            ui->renderer,
            x,
            y,
            distance == 0 ? 7 : 5,
            color
        );
    }
}

void app_ui_render_browser(
    app_ui *ui,
    const session_manager *manager,
    bool scanner_active)
{
    if (ui == NULL ||
        !ui->ready ||
        manager == NULL) {
        return;
    }

    begin_page(ui);
    draw_header(ui);

    if (manager->count == 0) {
        draw_spinner(ui, 960, 435);

        draw_text_centered(
            ui,
            ui->body_font,
            "Searching for Nintendo Switches...",
            960,
            520,
            COLOR_TEXT
        );

        draw_text_centered(
            ui,
            ui->small_font,
            scanner_active
                ? "Make sure SysDVR is running in TCP Bridge mode."
                : "Discovery is unavailable.",
            960,
            573,
            COLOR_MUTED
        );
    } else {
        draw_text(
            ui,
            ui->small_font,
            "Select a console to connect.",
            330,
            154,
            COLOR_MUTED
        );

        const size_t visible = 4;
        size_t start = 0;

        if (manager->selected >= visible) {
            start =
                manager->selected -
                visible +
                1;
        }

        for (size_t row = 0;
             row < visible &&
             start + row < manager->count;
             ++row) {
            size_t index = start + row;

            SDL_Rect card = {
                .x = 330,
                .y = 205 + (int)row * 172,
                .w = 1260,
                .h = 145,
            };

            draw_device_card(
                ui,
                &manager->devices[index],
                card,
                index == manager->selected
            );
        }

        char count_text[96];
        snprintf(
            count_text,
            sizeof(count_text),
            "%zu console%s found",
            manager->count,
            manager->count == 1 ? "" : "s"
        );

        draw_text(
            ui,
            ui->small_font,
            count_text,
            330,
            908,
            COLOR_MUTED
        );
    }

    draw_footer(
        ui,
        "Close",
        manager->count > 0
    );

    SDL_RenderPresent(ui->renderer);
}

void app_ui_render_connecting(
    app_ui *ui,
    const session_device *device,
    const char *message)
{
    if (ui == NULL || !ui->ready) {
        return;
    }

    begin_page(ui);
    draw_header(ui);

    SDL_Rect gamepad = {
        .x = 860,
        .y = 320,
        .w = 200,
        .h = 150,
    };

    draw_gamepad_icon(
        ui,
        gamepad
    );

    draw_text_centered(
        ui,
        ui->body_font,
        message ? message : "Connecting...",
        960,
        500,
        COLOR_TEXT
    );

    if (device != NULL) {
        draw_text_centered(
            ui,
            ui->small_font,
            device->info.host,
            960,
            555,
            COLOR_MUTED
        );
    }

    draw_text_centered(
        ui,
        ui->small_font,
        "Video and audio will start automatically.",
        960,
        625,
        COLOR_MUTED
    );

    SDL_RenderPresent(ui->renderer);
}

void app_ui_render_reconnect(
    app_ui *ui,
    const session_manager *manager)
{
    if (ui == NULL || !ui->ready) {
        return;
    }

    begin_page(ui);
    draw_header(ui);
    draw_spinner(ui, 960, 410);

    draw_text_centered(
        ui,
        ui->body_font,
        "Waiting for the selected Nintendo Switch...",
        960,
        510,
        COLOR_TEXT
    );

    if (manager != NULL &&
        manager->have_preferred) {
        char detail[196];

        if (manager->preferred_serial[0] != '\0') {
            snprintf(
                detail,
                sizeof(detail),
                "%s  |  %s",
                manager->preferred_host,
                manager->preferred_serial
            );
        } else {
            snprintf(
                detail,
                sizeof(detail),
                "%s",
                manager->preferred_host
            );
        }

        draw_text_centered(
            ui,
            ui->small_font,
            detail,
            960,
            566,
            COLOR_MUTED
        );
    }

    draw_text_centered(
        ui,
        ui->small_font,
        "Wake the console to reconnect automatically.",
        960,
        625,
        COLOR_MUTED
    );

    draw_footer(
        ui,
        "Cancel",
        false
    );

    SDL_RenderPresent(ui->renderer);
}

void app_ui_render_error(
    app_ui *ui,
    const char *title,
    const char *detail)
{
    if (ui == NULL || !ui->ready) {
        return;
    }

    begin_page(ui);
    draw_header(ui);

    draw_text_centered(
        ui,
        ui->body_font,
        title ? title : "Connection failed",
        960,
        435,
        COLOR_TEXT
    );

    draw_text_centered(
        ui,
        ui->small_font,
        detail
            ? detail
            : "Returning to console selection...",
        960,
        500,
        COLOR_MUTED
    );

    draw_footer(
        ui,
        "Return",
        false
    );

    SDL_RenderPresent(ui->renderer);
}

void app_ui_clear_for_video(app_ui *ui)
{
    if (ui == NULL ||
        ui->renderer == NULL) {
        return;
    }

    SDL_SetRenderDrawBlendMode(
        ui->renderer,
        SDL_BLENDMODE_NONE
    );

    SDL_SetRenderDrawColor(
        ui->renderer,
        0,
        0,
        0,
        0
    );

    SDL_RenderClear(ui->renderer);
    SDL_RenderPresent(ui->renderer);
}
