/* SDL Classic with optional Heltec LoRa.
 *   v1_emu boot.bin flash.bin [--nvram save.flash] [--radio PORT]
 *         [--cyid HEX] [--shell img/cybiko.png]
 * PORT is /dev/ttyUSB0 on Linux or COMn on Windows. If omitted, Settings
 * and the radio attach the first Heltec CP2102 (10c4:ea60) found.
 * Build with tools/v1_emu.sh (Linux) or tools/v1_emu.bat (Windows). */
#define CYBIKO_EMU_VERSION "1.1026.3b"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 0
#endif
#define access _access
#define mkdir(p, m) _mkdir(p)
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#endif
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <png.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_shape.h>
#include <SDL2/SDL_syswm.h>
#include "font8x8.h"
#include "host_serial.h"
#include "core/emulator.h"
#include "core/speaker.h"
#include "radio_coproc.h"
#include "radio_lora.h"
#include "radio_serial_phy.h"

#define MAX_HELD 16
/* CyOS 1.2.46 misses a key held under 10 frames now and then (v1_chat) */
#define MIN_HOLD 12

/* img/cybiko.png (887x1774). Interior screen hole is ~189,638 509x308;
 * pad a few pixels under the bezel so scale rounding never leaves a rim. */
#define SHELL_SCREEN_X 186
#define SHELL_SCREEN_Y 635
#define SHELL_SCREEN_W 515
#define SHELL_SCREEN_H 314

#define MENU_N 5
#define MENU_W 168
#define MENU_ROW 28
#define SETTINGS_W 420
#define SETTINGS_H 490
#define ABOUT_W 440
#define ABOUT_H 490
#define STATUS_W 440
#define STATUS_H 380
#define OVERLAY_SETTINGS 1
#define OVERLAY_ABOUT 2
#define OVERLAY_STATUS 3

#define FLASH_IDLE 0
#define FLASH_BUSY 1
#define FLASH_OK 2
#define FLASH_ERR 3
#define FLASH_HELTEC 1
#define FLASH_TDECK 2

#define SHELL_BLUE 0
#define SHELL_NEON 1
#define SHELL_PURPLE 2
#define VIEW_BODY 0
#define VIEW_CLOSEUP 1

typedef struct {
    SDL_Window *win;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Texture *shell;
    SDL_Surface *shape_src;
    SDL_Surface *shell_base;
    SDL_Surface *app_icon;
    SDL_Rect lcd_dst;
    int img_w, img_h;
    int win_w, win_h;
    int fit_w, fit_h;
    int mouse_col;
    uint16_t mouse_mask;
    SDL_AudioDeviceID audio;
    uint16_t key_matrix[CYBIKO_KEYBOARD_COLUMNS];
    int held_col[MAX_HELD];
    uint16_t held_mask[MAX_HELD];
    int held_frames[MAX_HELD];
    bool held_release[MAX_HELD];
    int held_count;
    bool menu_open;
    int menu_x, menu_y, menu_hover;
    bool settings_open;
    SDL_Window *settings_win;
    SDL_Renderer *settings_ren;
    bool about_open;
    SDL_Window *about_win;
    SDL_Renderer *about_ren;
    bool status_open;
    SDL_Window *status_win;
    SDL_Renderer *status_ren;
    int overlay_front;
    cybiko_emu_t *emu;
    cybiko_lora_t *lora;
    cybiko_radio_coproc_t *coproc;
    uint32_t radio_cyid;
    bool powered;
    bool always_on_top;
    bool mute;
    int scale_pct;
    int color;
    int view;
    cybiko_serial_phy_t *phy;
    const char *radio_path;
    char radio_buf[128];
    char heltec_port[128];
    char heltec_label[80];
    char tdeck_port[128];
    char tdeck_label[80];
    bool radio_on;
    bool radio_hold;
    int flash_state;
    int flash_target;
    int flash_shown;
    char flash_pio[512];
    char flash_proj[4096];
    char flash_env[40];
    char flash_port[128];
    char flash_msg[192];
    SDL_Thread *flash_thread;
    bool *running;
    bool dragging;
    int drag_off_x, drag_off_y;
} sdl_ctx_t;

typedef struct { SDL_Scancode sc; int col; uint16_t mask; } keymap_t;

/* r > 0: circle at (x,y). r == 0: inclusive rect (x,y)-(x1,y1). */
typedef struct {
    int16_t x, y, r, x1, y1;
    uint8_t col, mask;
} shell_hit_t;

static const keymap_t KEYS[] = {
    { SDL_SCANCODE_F7, 0, 0x01 }, { SDL_SCANCODE_ESCAPE, 0, 0x02 },
    { SDL_SCANCODE_BACKSPACE, 0, 0x04 }, { SDL_SCANCODE_DELETE, 0, 0x04 },
    { SDL_SCANCODE_LEFT, 0, 0x08 }, { SDL_SCANCODE_Q, 0, 0x10 },
    { SDL_SCANCODE_A, 0, 0x20 }, { SDL_SCANCODE_GRAVE, 0, 0x40 },
    { SDL_SCANCODE_LSHIFT, 0, 0x80 }, { SDL_SCANCODE_RSHIFT, 0, 0x80 },
    { SDL_SCANCODE_F6, 1, 0x01 }, { SDL_SCANCODE_UP, 1, 0x02 },
    { SDL_SCANCODE_INSERT, 1, 0x04 }, { SDL_SCANCODE_2, 1, 0x08 },
    { SDL_SCANCODE_W, 1, 0x10 }, { SDL_SCANCODE_S, 1, 0x20 },
    { SDL_SCANCODE_Z, 1, 0x40 }, { SDL_SCANCODE_LCTRL, 1, 0x80 },
    { SDL_SCANCODE_RCTRL, 1, 0x80 },
    { SDL_SCANCODE_F5, 2, 0x01 }, { SDL_SCANCODE_F3, 2, 0x02 },
    { SDL_SCANCODE_SPACE, 2, 0x04 }, { SDL_SCANCODE_3, 2, 0x08 },
    { SDL_SCANCODE_E, 2, 0x10 }, { SDL_SCANCODE_D, 2, 0x20 },
    { SDL_SCANCODE_X, 2, 0x40 }, { SDL_SCANCODE_END, 2, 0x80 },
    { SDL_SCANCODE_F4, 3, 0x01 }, { SDL_SCANCODE_1, 3, 0x02 },
    { SDL_SCANCODE_TAB, 3, 0x04 }, { SDL_SCANCODE_4, 3, 0x08 },
    { SDL_SCANCODE_R, 3, 0x10 }, { SDL_SCANCODE_F, 3, 0x20 },
    { SDL_SCANCODE_C, 3, 0x40 }, { SDL_SCANCODE_LEFTBRACKET, 3, 0x80 },
    { SDL_SCANCODE_RIGHT, 4, 0x01 }, { SDL_SCANCODE_DOWN, 4, 0x02 },
    { SDL_SCANCODE_HOME, 4, 0x04 }, { SDL_SCANCODE_5, 4, 0x08 },
    { SDL_SCANCODE_T, 4, 0x10 }, { SDL_SCANCODE_G, 4, 0x20 },
    { SDL_SCANCODE_V, 4, 0x40 }, { SDL_SCANCODE_RIGHTBRACKET, 4, 0x80 },
    { SDL_SCANCODE_F2, 5, 0x01 }, { SDL_SCANCODE_SEMICOLON, 5, 0x02 },
    { SDL_SCANCODE_RETURN, 5, 0x04 }, { SDL_SCANCODE_6, 5, 0x08 },
    { SDL_SCANCODE_Y, 5, 0x10 }, { SDL_SCANCODE_H, 5, 0x20 },
    { SDL_SCANCODE_B, 5, 0x40 }, { SDL_SCANCODE_BACKSLASH, 5, 0x80 },
    { SDL_SCANCODE_F1, 6, 0x01 }, { SDL_SCANCODE_SLASH, 6, 0x02 },
    { SDL_SCANCODE_7, 6, 0x08 }, { SDL_SCANCODE_U, 6, 0x10 },
    { SDL_SCANCODE_J, 6, 0x20 }, { SDL_SCANCODE_N, 6, 0x40 },
    { SDL_SCANCODE_MINUS, 7, 0x01 }, { SDL_SCANCODE_PERIOD, 7, 0x02 },
    { SDL_SCANCODE_0, 7, 0x04 }, { SDL_SCANCODE_8, 7, 0x08 },
    { SDL_SCANCODE_I, 7, 0x10 }, { SDL_SCANCODE_K, 7, 0x20 },
    { SDL_SCANCODE_M, 7, 0x40 },
    { SDL_SCANCODE_APOSTROPHE, 8, 0x01 }, { SDL_SCANCODE_EQUALS, 8, 0x02 },
    { SDL_SCANCODE_9, 8, 0x04 }, { SDL_SCANCODE_P, 8, 0x08 },
    { SDL_SCANCODE_O, 8, 0x10 }, { SDL_SCANCODE_L, 8, 0x20 },
    { SDL_SCANCODE_COMMA, 8, 0x40 },
};

static const shell_hit_t SHELL_HITS[] = {
    /* F1–F7 round buttons, then the printed icons under them. */
    { 235, 521, 28, 0, 0, 6, 0x01 }, { 218, 596, 24, 0, 0, 6, 0x01 },
    { 302, 535, 28, 0, 0, 5, 0x01 }, { 299, 606, 24, 0, 0, 5, 0x01 },
    { 375, 546, 28, 0, 0, 2, 0x02 }, { 372, 608, 24, 0, 0, 2, 0x02 },
    { 444, 545, 28, 0, 0, 3, 0x01 }, { 447, 613, 24, 0, 0, 3, 0x01 },
    { 512, 541, 28, 0, 0, 2, 0x01 }, { 524, 609, 24, 0, 0, 2, 0x01 },
    { 584, 527, 28, 0, 0, 1, 0x01 }, { 592, 601, 24, 0, 0, 1, 0x01 },
    { 653, 502, 28, 0, 0, 0, 0x01 }, { 670, 592, 24, 0, 0, 0, 0x01 },
    /* D-pad triangles (before the big plate). */
    { 334, 1117, 28, 0, 0, 1, 0x02 },
    { 334, 1243, 28, 0, 0, 4, 0x02 },
    { 257, 1183, 28, 0, 0, 0, 0x08 },
    { 410, 1178, 28, 0, 0, 4, 0x01 },
    /* Esc, Del, Del, Ins, Select, Enter. */
    { 164, 1044, 0, 298, 1113, 0, 0x02 },
    { 507, 1035, 0, 597, 1078, 0, 0x04 },
    { 544, 1085, 0, 663, 1125, 0, 0x04 },
    { 554, 1141, 0, 689, 1181, 1, 0x04 },
    { 545, 1206, 0, 694, 1248, 4, 0x04 },
    { 507, 1266, 0, 699, 1329, 5, 0x04 },
    /* 1 2 3 4 5 6 7 8 9 0 BkSp */
    { 235, 1401, 20, 0, 0, 3, 0x02 },
    { 283, 1399, 20, 0, 0, 1, 0x08 },
    { 332, 1396, 20, 0, 0, 2, 0x08 },
    { 380, 1394, 20, 0, 0, 3, 0x08 },
    { 428, 1390, 20, 0, 0, 4, 0x08 },
    { 477, 1388, 20, 0, 0, 5, 0x08 },
    { 525, 1385, 20, 0, 0, 6, 0x08 },
    { 574, 1381, 20, 0, 0, 7, 0x08 },
    { 623, 1377, 20, 0, 0, 8, 0x04 },
    { 669, 1374, 20, 0, 0, 7, 0x04 },
    { 723, 1372, 22, 0, 0, 6, 0x04 },
    /* Q W E R T Y U I O P */
    { 255, 1466, 20, 0, 0, 0, 0x10 },
    { 304, 1464, 20, 0, 0, 1, 0x10 },
    { 353, 1462, 20, 0, 0, 2, 0x10 },
    { 402, 1460, 20, 0, 0, 3, 0x10 },
    { 451, 1458, 20, 0, 0, 4, 0x10 },
    { 499, 1454, 20, 0, 0, 5, 0x10 },
    { 548, 1451, 20, 0, 0, 6, 0x10 },
    { 596, 1448, 20, 0, 0, 7, 0x10 },
    { 645, 1445, 20, 0, 0, 8, 0x10 },
    { 693, 1441, 20, 0, 0, 8, 0x08 },
    /* A S D F G H J K L ; */
    { 237, 1533, 20, 0, 0, 0, 0x20 },
    { 287, 1531, 20, 0, 0, 1, 0x20 },
    { 336, 1531, 20, 0, 0, 2, 0x20 },
    { 384, 1529, 20, 0, 0, 3, 0x20 },
    { 433, 1528, 20, 0, 0, 4, 0x20 },
    { 482, 1525, 20, 0, 0, 5, 0x20 },
    { 530, 1521, 20, 0, 0, 6, 0x20 },
    { 579, 1519, 20, 0, 0, 7, 0x20 },
    { 628, 1517, 20, 0, 0, 8, 0x20 },
    { 678, 1516, 20, 0, 0, 5, 0x02 },
    /* Z X C V B N M . , / */
    { 258, 1598, 20, 0, 0, 1, 0x40 },
    { 308, 1597, 20, 0, 0, 2, 0x40 },
    { 355, 1597, 20, 0, 0, 3, 0x40 },
    { 405, 1596, 20, 0, 0, 4, 0x40 },
    { 453, 1594, 20, 0, 0, 5, 0x40 },
    { 501, 1592, 20, 0, 0, 6, 0x40 },
    { 551, 1593, 20, 0, 0, 7, 0x40 },
    { 599, 1590, 20, 0, 0, 7, 0x02 },
    { 647, 1590, 20, 0, 0, 8, 0x40 },
    { 692, 1588, 20, 0, 0, 6, 0x02 },
    /* Sym=Fn, Tab, Space, ?, !, ., Shift */
    { 225, 1647, 0, 289, 1672, 1, 0x80 },
    { 320, 1648, 0, 383, 1673, 3, 0x04 },
    { 417, 1646, 0, 533, 1685, 2, 0x04 },
    { 570, 1648, 0, 596, 1681, 6, 0x02 },
    { 619, 1648, 0, 640, 1681, 3, 0x02 },
    { 668, 1647, 0, 691, 1680, 7, 0x02 },
    { 718, 1644, 0, 748, 1683, 0, 0x80 },
};

static int find_held(sdl_ctx_t *s, int col, uint16_t mask);
static void drop_held(sdl_ctx_t *s, int i);
static void press_hold(sdl_ctx_t *s, int col, uint16_t mask, bool down);
static const shell_hit_t *hit_at(int x, int y);
static void win_to_img(const sdl_ctx_t *s, int wx, int wy, int *ix, int *iy);
static void present(sdl_ctx_t *s);

static int file_ok(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char *find_shell_png(const char *explicit) {
    static char buf[4096];
    if (explicit && file_ok(explicit)) return explicit;
    const char *env = getenv("CYBIKO_SHELL");
    if (env && file_ok(env)) return env;
    static const char *rel[] = {
        "img/cybiko.png", "../img/cybiko.png", "../../img/cybiko.png", NULL
    };
    for (int i = 0; rel[i]; i++)
        if (file_ok(rel[i])) return rel[i];
    char exe[4096];
    host_exe_dir(exe, sizeof exe);
    if (exe[0]) {
        snprintf(buf, sizeof buf, "%s/img/cybiko.png", exe);
        if (file_ok(buf)) return buf;
        snprintf(buf, sizeof buf, "%s/../img/cybiko.png", exe);
        if (file_ok(buf)) return buf;
        snprintf(buf, sizeof buf, "%s/../../img/cybiko.png", exe);
        if (file_ok(buf)) return buf;
    }
    return NULL;
}

static const char *find_icon_png(void) {
    static char buf[4096];
    static const char *names[] = { "CybikoIcon.png", "cybikoicon.png", NULL };
    char exe[4096];
    int n;
    for (n = 0; names[n]; n++) {
        const char *name = names[n];
        snprintf(buf, sizeof buf, "img/%s", name);
        if (file_ok(buf)) return buf;
        snprintf(buf, sizeof buf, "../img/%s", name);
        if (file_ok(buf)) return buf;
        snprintf(buf, sizeof buf, "../../img/%s", name);
        if (file_ok(buf)) return buf;
    }
    host_exe_dir(exe, sizeof exe);
    if (!exe[0]) return NULL;
    for (n = 0; names[n]; n++) {
        snprintf(buf, sizeof buf, "%s/img/%s", exe, names[n]);
        if (file_ok(buf)) return buf;
        snprintf(buf, sizeof buf, "%s/../img/%s", exe, names[n]);
        if (file_ok(buf)) return buf;
        snprintf(buf, sizeof buf, "%s/../../img/%s", exe, names[n]);
        if (file_ok(buf)) return buf;
    }
    return NULL;
}

static SDL_Surface *load_png_surface(const char *path, int *ow, int *oh) {
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) {
        fprintf(stderr, "shell png: %s\n", img.message);
        return NULL;
    }
    img.format = PNG_FORMAT_RGBA;
    png_bytep buf = malloc(PNG_IMAGE_SIZE(img));
    if (!buf || !png_image_finish_read(&img, NULL, buf, 0, NULL)) {
        fprintf(stderr, "shell png read: %s\n", img.message);
        free(buf);
        png_image_free(&img);
        return NULL;
    }
    int pw = (int)img.width, ph = (int)img.height;
    SDL_Surface *tmp = SDL_CreateRGBSurfaceFrom(
        buf, pw, ph, 32, pw * 4,
        0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
    SDL_Surface *surf = tmp ? SDL_ConvertSurfaceFormat(tmp, SDL_PIXELFORMAT_ARGB8888, 0)
                            : NULL;
    if (tmp) SDL_FreeSurface(tmp);
    free(buf);
    png_image_free(&img);
    if (!surf) return NULL;
    *ow = pw;
    *oh = ph;
    return surf;
}

#define APP_ICON_PX 64

static SDL_Surface *load_app_icon(void) {
    const char *path = find_icon_png();
    int w = 0, h = 0;
    SDL_Surface *src, *dst;
    if (!path) return NULL;
    src = load_png_surface(path, &w, &h);
    if (!src) return NULL;
    dst = SDL_CreateRGBSurfaceWithFormat(0, APP_ICON_PX, APP_ICON_PX, 32,
                                         SDL_PIXELFORMAT_ARGB8888);
    if (!dst) {
        SDL_FreeSurface(src);
        return NULL;
    }
    SDL_FillRect(dst, NULL, SDL_MapRGBA(dst->format, 0, 0, 0, 255));
    SDL_BlitScaled(src, NULL, dst, NULL);
    SDL_FreeSurface(src);
    return dst;
}

static void apply_app_icon(const sdl_ctx_t *s, SDL_Window *win) {
    if (s && s->app_icon && win)
        SDL_SetWindowIcon(win, s->app_icon);
}

static void rgb_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8, float *h, float *s, float *v) {
    float r = r8 / 255.f, g = g8 / 255.f, b = b8 / 255.f;
    float mx = fmaxf(r, fmaxf(g, b));
    float mn = fminf(r, fminf(g, b));
    float d = mx - mn;
    *v = mx;
    *s = (mx > 1e-6f) ? d / mx : 0.f;
    if (d < 1e-6f) *h = 0.f;
    else if (mx == r) *h = fmodf(60.f * (g - b) / d + 360.f, 360.f);
    else if (mx == g) *h = 60.f * (b - r) / d + 120.f;
    else *h = 60.f * (r - g) / d + 240.f;
}

static void hsv_to_rgb(float h, float s, float v, uint8_t *r8, uint8_t *g8, uint8_t *b8) {
    float c = v * s;
    float x = c * (1.f - fabsf(fmodf(h / 60.f, 2.f) - 1.f));
    float m = v - c;
    float r, g, b;
    if (h < 60.f) { r = c; g = x; b = 0.f; }
    else if (h < 120.f) { r = x; g = c; b = 0.f; }
    else if (h < 180.f) { r = 0.f; g = c; b = x; }
    else if (h < 240.f) { r = 0.f; g = x; b = c; }
    else if (h < 300.f) { r = x; g = 0.f; b = c; }
    else { r = c; g = 0.f; b = x; }
    *r8 = (uint8_t)((r + m) * 255.f + 0.5f);
    *g8 = (uint8_t)((g + m) * 255.f + 0.5f);
    *b8 = (uint8_t)((b + m) * 255.f + 0.5f);
}

/* Recolor only the dyed plastic; keep keys, antenna, labels, and the
 * highlight/shadow lighting so it still reads as clear translucent shell. */
static void tint_plastic(SDL_Surface *surf, int color) {
    if (!surf || color == SHELL_BLUE) return;
    float th = (color == SHELL_NEON) ? 74.f : 288.f;
    float smul = (color == SHELL_NEON) ? 1.06f : 1.10f;
    float vmul = (color == SHELL_NEON) ? 1.10f : 1.04f;
    int bpp = surf->format->BytesPerPixel;
    if (SDL_MUSTLOCK(surf) && SDL_LockSurface(surf) != 0) return;
    for (int y = 0; y < surf->h; y++) {
        Uint8 *row = (Uint8 *)surf->pixels + y * surf->pitch;
        for (int x = 0; x < surf->w; x++) {
            Uint32 px = 0;
            Uint8 r, g, b, a;
            float h, s, v;
            memcpy(&px, row + x * bpp, (size_t)bpp);
            SDL_GetRGBA(px, surf->format, &r, &g, &b, &a);
            if (a < 16) continue;
            rgb_to_hsv(r, g, b, &h, &s, &v);
            if (s <= 0.22f || v <= 0.06f || h <= 195.f || h >= 255.f) continue;
            s = fminf(1.f, s * smul);
            v = fminf(1.f, v * vmul);
            hsv_to_rgb(th, s, v, &r, &g, &b);
            px = SDL_MapRGBA(surf->format, r, g, b, a);
            memcpy(row + x * bpp, &px, (size_t)bpp);
        }
    }
    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
}

static void draw_text(SDL_Renderer *ren, int x, int y, int scale,
                      uint8_t r, uint8_t g, uint8_t b, const char *s) {
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) c = '?';
        const uint8_t *glyph = FONT8[c - 32];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (!(bits & (1u << col))) continue;
                SDL_Rect p = { x + (i * 8 + col) * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(ren, &p);
            }
        }
    }
}

static const char *MENU_LABEL[MENU_N] = {
    "Settings", "Status", "Help", "Minimize", "Close"
};

static void menu_rect(const sdl_ctx_t *s, SDL_Rect *out) {
    out->x = s->menu_x;
    out->y = s->menu_y;
    out->w = MENU_W;
    out->h = MENU_ROW * MENU_N;
}

static int menu_item_at(const sdl_ctx_t *s, int x, int y) {
    SDL_Rect r;
    menu_rect(s, &r);
    if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) return -1;
    return (y - r.y) / MENU_ROW;
}

static void draw_menu(sdl_ctx_t *s) {
    if (!s->menu_open) return;
    SDL_Rect box;
    menu_rect(s, &box);
    SDL_SetRenderDrawColor(s->renderer, 18, 24, 36, 240);
    SDL_RenderFillRect(s->renderer, &box);
    SDL_SetRenderDrawColor(s->renderer, 80, 170, 255, 255);
    SDL_RenderDrawRect(s->renderer, &box);
    for (int i = 0; i < MENU_N; i++) {
        SDL_Rect row = { box.x, box.y + i * MENU_ROW, box.w, MENU_ROW };
        if (i == s->menu_hover) {
            SDL_SetRenderDrawColor(s->renderer, 40, 90, 150, 255);
            SDL_RenderFillRect(s->renderer, &row);
        }
        draw_text(s->renderer, row.x + 14, row.y + 6, 2, 230, 240, 255, MENU_LABEL[i]);
    }
}

static char *prefs_path(void) {
    static char buf[512];
#ifdef _WIN32
    const char *app = getenv("APPDATA");
    if (!app || !app[0]) return NULL;
    snprintf(buf, sizeof buf, "%s/cybiko", app);
    mkdir(buf, 0755);
    snprintf(buf, sizeof buf, "%s/cybiko/v1_emu.conf", app);
    return buf;
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    snprintf(buf, sizeof buf, "%s/.config", home);
    mkdir(buf, 0755);
    snprintf(buf, sizeof buf, "%s/.config/cybiko", home);
    mkdir(buf, 0755);
    snprintf(buf, sizeof buf, "%s/.config/cybiko/v1_emu.conf", home);
    return buf;
#endif
}

static void load_prefs(sdl_ctx_t *s) {
    s->scale_pct = 100;
    s->always_on_top = false;
    s->mute = false;
    s->color = SHELL_BLUE;
    s->view = VIEW_BODY;
    const char *p = prefs_path();
    if (!p) return;
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "scale=%d", &v) == 1) {
            if (v < 50) v = 50;
            if (v > 150) v = 150;
            s->scale_pct = v;
        } else if (sscanf(line, "always_on_top=%d", &v) == 1)
            s->always_on_top = v != 0;
        else if (sscanf(line, "mute=%d", &v) == 1)
            s->mute = v != 0;
        else if (sscanf(line, "color=%d", &v) == 1) {
            if (v < SHELL_BLUE) v = SHELL_BLUE;
            if (v > SHELL_PURPLE) v = SHELL_PURPLE;
            s->color = v;
        } else if (sscanf(line, "view=%d", &v) == 1) {
            s->view = (v == VIEW_CLOSEUP) ? VIEW_CLOSEUP : VIEW_BODY;
        }
    }
    fclose(f);
}

static void save_prefs(const sdl_ctx_t *s) {
    const char *p = prefs_path();
    if (!p) return;
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "scale=%d\nalways_on_top=%d\nmute=%d\ncolor=%d\nview=%d\n",
            s->scale_pct, s->always_on_top ? 1 : 0, s->mute ? 1 : 0,
            s->color, s->view);
    fclose(f);
}

static int is_repo_root(const char *p) {
    char a[4200], b[4200];
    if (!p || !p[0]) return 0;
    snprintf(a, sizeof a, "%s/cybiko-heltec-modem/platformio.ini", p);
    snprintf(b, sizeof b, "%s/cybiko-lilygo/platformio.ini", p);
    return file_ok(a) && file_ok(b);
}

static int find_repo_root(char *out, size_t cap) {
    char exe[4096];
    if (is_repo_root(".")) {
        snprintf(out, cap, ".");
        return 1;
    }
    if (is_repo_root("..")) {
        snprintf(out, cap, "..");
        return 1;
    }
    host_exe_dir(exe, sizeof exe);
    if (!exe[0]) return 0;
    for (int i = 0; i < 6; i++) {
        char *slash;
        if (is_repo_root(exe)) {
            snprintf(out, cap, "%s", exe);
            return 1;
        }
        slash = strrchr(exe, '/');
#ifdef _WIN32
        {
            char *bs = strrchr(exe, '\\');
            if (!slash || (bs && bs > slash)) slash = bs;
        }
#endif
        if (!slash || slash == exe) break;
        *slash = 0;
    }
    return 0;
}

static int find_pio(char *out, size_t cap) {
    char a[512];
#ifndef _WIN32
    char b[512];
#endif
    const char *cands[8];
    int n = 0, i;
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (home && home[0]) {
        snprintf(a, sizeof a, "%s/.platformio/penv/Scripts/pio.exe", home);
        cands[n++] = a;
    }
#else
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(a, sizeof a, "%s/.platformio/penv/bin/pio", home);
        snprintf(b, sizeof b, "%s/.local/bin/pio", home);
        cands[n++] = a;
        cands[n++] = b;
    }
    cands[n++] = "/usr/local/bin/pio";
    cands[n++] = "/usr/bin/pio";
#endif
    for (i = 0; i < n; i++) {
        if (cands[i] && access(cands[i], X_OK) == 0) {
            snprintf(out, cap, "%s", cands[i]);
            return 1;
        }
    }
#ifdef _WIN32
    {
        FILE *p = popen("where pio 2>NUL", "r");
        if (p) {
            if (fgets(out, (int)cap, p)) {
                char *nl = strchr(out, '\n');
                if (nl) *nl = 0;
            } else {
                out[0] = 0;
            }
            pclose(p);
            if (out[0] && access(out, X_OK) == 0) return 1;
        }
    }
#else
    {
        FILE *p = popen("command -v pio 2>/dev/null", "r");
        if (p) {
            if (fgets(out, (int)cap, p)) {
                char *nl = strchr(out, '\n');
                if (nl) *nl = 0;
            } else {
                out[0] = 0;
            }
            pclose(p);
            if (out[0] && access(out, X_OK) == 0) return 1;
        }
    }
#endif
    return 0;
}

static const char *short_port(const char *port) {
    const char *s;
    if (!port || !port[0]) return NULL;
    s = strrchr(port, '/');
    if (s && s[1]) return s + 1;
#ifdef _WIN32
    s = strrchr(port, '\\');
    if (s && s[1]) return s + 1;
#endif
    return port;
}

static void refresh_devices(sdl_ctx_t *s) {
    s->heltec_port[0] = 0;
    s->heltec_label[0] = 0;
    s->tdeck_port[0] = 0;
    s->tdeck_label[0] = 0;
    host_find_kind(HOST_SERIAL_HELTEC, s->radio_path,
                   s->heltec_port, sizeof s->heltec_port,
                   s->heltec_label, sizeof s->heltec_label);
    host_find_kind(HOST_SERIAL_TDECK, NULL,
                   s->tdeck_port, sizeof s->tdeck_port,
                   s->tdeck_label, sizeof s->tdeck_label);
}

static void trim_line(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

static int flash_worker(void *arg) {
    sdl_ctx_t *s = arg;
    char cmd[4600], line[256], last[256] = "";
    snprintf(s->flash_msg, sizeof s->flash_msg, "Flashing %s on %s...",
             s->flash_target == FLASH_HELTEC ? "Heltec" : "T-Deck", s->flash_port);
    snprintf(cmd, sizeof cmd,
             "\"%s\" run -d \"%s\" -e %s -t upload --upload-port \"%s\" 2>&1",
             s->flash_pio, s->flash_proj, s->flash_env, s->flash_port);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(s->flash_msg, sizeof s->flash_msg, "could not start pio");
        s->flash_state = FLASH_ERR;
        return 0;
    }
    while (fgets(line, sizeof line, fp)) {
        fputs(line, stderr);
        trim_line(line);
        if (line[0])
            snprintf(last, sizeof last, "%s", line);
    }
    int st = pclose(fp);
    if (st != 0) {
        snprintf(s->flash_msg, sizeof s->flash_msg, "%s",
                 last[0] ? last : "pio upload failed");
        s->flash_state = FLASH_ERR;
        return 0;
    }
    /* USB re-enumerates after reset; give the node a moment. */
#ifdef _WIN32
    Sleep(1500);
#else
    usleep(1500 * 1000);
#endif
    snprintf(s->flash_msg, sizeof s->flash_msg, "%s flashed on %s",
             s->flash_target == FLASH_HELTEC ? "Heltec radio" : "T-Deck Classic",
             s->flash_port);
    s->flash_state = FLASH_OK;
    return 0;
}

static void fill_opaque_rect(SDL_Surface *surf, SDL_Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    SDL_FillRect(surf, &r, SDL_MapRGBA(surf->format, 255, 255, 255, 255));
}

/* Punch the rectangular window down to the device (plus the LCD hole,
 * which is transparent in the PNG). Bounding + input shape so empty
 * pixels are see-through and click-through. */
#ifdef _WIN32
static void x11_apply_shape(SDL_Window *win, SDL_Surface *mask) {
    (void)win;
    (void)mask;
}
static void x11_clear_shape(SDL_Window *win) { (void)win; }
static unsigned long pick_argb_visual(void) { return 0; }
#else
static void x11_apply_shape(SDL_Window *win, SDL_Surface *mask) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_X11)
        return;
    Display *dpy = info.info.x11.display;
    Window xw = info.info.x11.window;
    int w = mask->w, h = mask->h;
    int bpl = (w + 7) / 8;
    char *bits = calloc(1, (size_t)bpl * (size_t)h);
    if (!bits) return;
    if (SDL_MUSTLOCK(mask) && SDL_LockSurface(mask) != 0) {
        free(bits);
        return;
    }
    for (int y = 0; y < h; y++) {
        const Uint8 *row = (const Uint8 *)mask->pixels + y * mask->pitch;
        for (int x = 0; x < w; x++) {
            Uint8 a, r, g, b;
            Uint32 px = 0;
            memcpy(&px, row + x * mask->format->BytesPerPixel, mask->format->BytesPerPixel);
            SDL_GetRGBA(px, mask->format, &r, &g, &b, &a);
            if (a >= 16)
                bits[y * bpl + (x >> 3)] |= (char)(1 << (x & 7));
        }
    }
    if (SDL_MUSTLOCK(mask)) SDL_UnlockSurface(mask);
    Pixmap pm = XCreateBitmapFromData(dpy, xw, bits, (unsigned)w, (unsigned)h);
    free(bits);
    if (!pm) return;
    XShapeCombineMask(dpy, xw, ShapeBounding, 0, 0, pm, ShapeSet);
    XShapeCombineMask(dpy, xw, ShapeInput, 0, 0, pm, ShapeSet);
    XFreePixmap(dpy, pm);
    XSync(dpy, False);
}

static void x11_clear_shape(SDL_Window *win) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_X11)
        return;
    Display *dpy = info.info.x11.display;
    Window xw = info.info.x11.window;
    XShapeCombineMask(dpy, xw, ShapeBounding, 0, 0, None, ShapeSet);
    XShapeCombineMask(dpy, xw, ShapeInput, 0, 0, None, ShapeSet);
    XSync(dpy, False);
}
#endif

static int scale_floor(int v, int dst, int src) {
    return v * dst / src;
}

static int scale_ceil(int v, int dst, int src) {
    return (v * dst + src - 1) / src;
}

/* Map the PNG hole into window pixels, rounding the far edge up so the
 * LCD (and the black fill behind it) fully covers the transparent glass. */
static void layout_lcd(sdl_ctx_t *s) {
    if (s->img_w <= 0 || s->img_h <= 0 || s->win_w <= 0 || s->win_h <= 0)
        return;
    int x0 = scale_floor(SHELL_SCREEN_X, s->win_w, s->img_w);
    int y0 = scale_floor(SHELL_SCREEN_Y, s->win_h, s->img_h);
    int x1 = scale_ceil(SHELL_SCREEN_X + SHELL_SCREEN_W, s->win_w, s->img_w);
    int y1 = scale_ceil(SHELL_SCREEN_Y + SHELL_SCREEN_H, s->win_h, s->img_h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->win_w) x1 = s->win_w;
    if (y1 > s->win_h) y1 = s->win_h;
    s->lcd_dst.x = x0;
    s->lcd_dst.y = y0;
    s->lcd_dst.w = x1 - x0;
    s->lcd_dst.h = y1 - y0;
}

static void apply_window_shape(sdl_ctx_t *s) {
    if (!s->win || s->win_w <= 0 || s->win_h <= 0) return;
    if (s->view == VIEW_CLOSEUP || !s->shape_src) {
        x11_clear_shape(s->win);
        return;
    }
    SDL_Surface *scaled = SDL_CreateRGBSurfaceWithFormat(
        0, s->win_w, s->win_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!scaled) return;
    SDL_FillRect(scaled, NULL, SDL_MapRGBA(scaled->format, 0, 0, 0, 0));
    SDL_BlitScaled(s->shape_src, NULL, scaled, NULL);
    fill_opaque_rect(scaled, s->lcd_dst);
    if (s->menu_open) {
        SDL_Rect menu;
        menu_rect(s, &menu);
        fill_opaque_rect(scaled, menu);
    }
    SDL_WindowShapeMode mode;
    memset(&mode, 0, sizeof mode);
    mode.mode = ShapeModeBinarizeAlpha;
    mode.parameters.binarizationCutoff = 16;
    SDL_SetWindowShape(s->win, scaled, &mode);
    x11_apply_shape(s->win, scaled);
    SDL_FreeSurface(scaled);
}

static void apply_scale(sdl_ctx_t *s) {
    if (!s->win) return;
    int w, h;
    if (s->view == VIEW_CLOSEUP) {
        w = CYBIKO_LCD_WIDTH * 4 * s->scale_pct / 100;
        h = CYBIKO_LCD_HEIGHT * 4 * s->scale_pct / 100;
    } else if (s->fit_w > 0) {
        w = s->fit_w * s->scale_pct / 100;
        h = s->fit_h * s->scale_pct / 100;
    } else {
        w = CYBIKO_LCD_WIDTH * 3 * s->scale_pct / 100;
        h = CYBIKO_LCD_HEIGHT * 3 * s->scale_pct / 100;
    }
    if (w < 80) w = 80;
    if (h < 80) h = 80;
    SDL_SetWindowSize(s->win, w, h);
    s->win_w = w;
    s->win_h = h;
    if (s->view == VIEW_CLOSEUP || !s->shape_src) {
        s->lcd_dst.x = 0;
        s->lcd_dst.y = 0;
        s->lcd_dst.w = w;
        s->lcd_dst.h = h;
    } else {
        layout_lcd(s);
    }
    apply_window_shape(s);
}

static void apply_shell_tint(sdl_ctx_t *s) {
    if (!s->shape_src) return;
    if (s->shell_base)
        SDL_BlitSurface(s->shell_base, NULL, s->shape_src, NULL);
    tint_plastic(s->shape_src, s->color);
    if (!s->renderer) return;
    if (s->shell) SDL_DestroyTexture(s->shell);
    s->shell = SDL_CreateTextureFromSurface(s->renderer, s->shape_src);
    if (s->shell)
        SDL_SetTextureBlendMode(s->shell, SDL_BLENDMODE_BLEND);
}

static int shell_alpha_at(const sdl_ctx_t *s, int wx, int wy) {
    int ix, iy;
    win_to_img(s, wx, wy, &ix, &iy);
    if (!s->shape_src || ix < 0 || iy < 0 || ix >= s->img_w || iy >= s->img_h)
        return 0;
    const Uint8 *p = (const Uint8 *)s->shape_src->pixels
        + iy * s->shape_src->pitch + ix * s->shape_src->format->BytesPerPixel;
    Uint32 px = 0;
    memcpy(&px, p, s->shape_src->format->BytesPerPixel);
    Uint8 r, g, b, a;
    SDL_GetRGBA(px, s->shape_src->format, &r, &g, &b, &a);
    return a;
}

static int on_shell_body(const sdl_ctx_t *s, int wx, int wy) {
    if (wx >= s->lcd_dst.x && wy >= s->lcd_dst.y
        && wx < s->lcd_dst.x + s->lcd_dst.w && wy < s->lcd_dst.y + s->lcd_dst.h)
        return 1;
    return shell_alpha_at(s, wx, wy) >= 16;
}

#ifndef _WIN32
static unsigned long pick_argb_visual(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return 0;
    XVisualInfo tmpl;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.screen = DefaultScreen(d);
    tmpl.depth = 32;
    tmpl.class = TrueColor;
    int n = 0;
    XVisualInfo *v = XGetVisualInfo(d,
        VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
    unsigned long id = (v && n > 0) ? (unsigned long)v[0].visualid : 0;
    if (v) XFree(v);
    XCloseDisplay(d);
    return id;
}
#endif

/* Keep overlay windows above the Cybiko without stealing focus. */
static void raise_overlay(SDL_Window *win) {
    SDL_SysWMinfo info;
    if (!win) return;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info)) return;
#ifdef _WIN32
    if (info.subsystem == SDL_SYSWM_WINDOWS) {
        SetWindowPos(info.info.win.window, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#else
    if (info.subsystem == SDL_SYSWM_X11) {
        XRaiseWindow(info.info.x11.display, info.info.x11.window);
        XFlush(info.info.x11.display);
    }
#endif
}

static void restack_overlays(sdl_ctx_t *s) {
    raise_overlay(s->settings_win);
    raise_overlay(s->about_win);
    raise_overlay(s->status_win);
    if (s->overlay_front == OVERLAY_SETTINGS)
        raise_overlay(s->settings_win);
    else if (s->overlay_front == OVERLAY_ABOUT)
        raise_overlay(s->about_win);
    else if (s->overlay_front == OVERLAY_STATUS)
        raise_overlay(s->status_win);
}

static void apply_always_on_top(sdl_ctx_t *s) {
    if (s->win)
        SDL_SetWindowAlwaysOnTop(s->win, s->always_on_top);
    if (s->settings_win)
        SDL_SetWindowAlwaysOnTop(s->settings_win, s->always_on_top);
    if (s->about_win)
        SDL_SetWindowAlwaysOnTop(s->about_win, s->always_on_top);
    if (s->status_win)
        SDL_SetWindowAlwaysOnTop(s->status_win, s->always_on_top);
    restack_overlays(s);
}

static int overlay_next(const sdl_ctx_t *s) {
    if (s->status_open) return OVERLAY_STATUS;
    if (s->about_open) return OVERLAY_ABOUT;
    if (s->settings_open) return OVERLAY_SETTINGS;
    return 0;
}

static void close_settings(sdl_ctx_t *s) {
    if (s->settings_ren) {
        SDL_DestroyRenderer(s->settings_ren);
        s->settings_ren = NULL;
    }
    if (s->settings_win) {
        SDL_DestroyWindow(s->settings_win);
        s->settings_win = NULL;
    }
    s->settings_open = false;
    if (s->overlay_front == OVERLAY_SETTINGS)
        s->overlay_front = overlay_next(s);
}

static void close_about(sdl_ctx_t *s) {
    if (s->about_ren) {
        SDL_DestroyRenderer(s->about_ren);
        s->about_ren = NULL;
    }
    if (s->about_win) {
        SDL_DestroyWindow(s->about_win);
        s->about_win = NULL;
    }
    s->about_open = false;
    if (s->overlay_front == OVERLAY_ABOUT)
        s->overlay_front = overlay_next(s);
}

static void close_status(sdl_ctx_t *s) {
    if (s->status_ren) {
        SDL_DestroyRenderer(s->status_ren);
        s->status_ren = NULL;
    }
    if (s->status_win) {
        SDL_DestroyWindow(s->status_win);
        s->status_win = NULL;
    }
    s->status_open = false;
    if (s->overlay_front == OVERLAY_STATUS)
        s->overlay_front = overlay_next(s);
}

static void shell_accent(int color, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (color == SHELL_NEON) { *r = 210; *g = 240; *b = 40; }
    else if (color == SHELL_PURPLE) { *r = 210; *g = 80; *b = 255; }
    else { *r = 80; *g = 180; *b = 255; }
}

static void draw_swatch(SDL_Renderer *ren, int x, int y, int on,
                        uint8_t r, uint8_t g, uint8_t b, const char *label) {
    SDL_Rect box = { x, y, 100, 28 };
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderFillRect(ren, &box);
    SDL_SetRenderDrawColor(ren, on ? 255 : 40, on ? 255 : 48, on ? 255 : 60, 255);
    SDL_RenderDrawRect(ren, &box);
    if (on) {
        SDL_Rect inner = { x + 1, y + 1, 98, 26 };
        SDL_RenderDrawRect(ren, &inner);
    }
    draw_text(ren, x + 8, y + 32, 1, 230, 240, 255, label);
}

#define FLASH_BTN_X 24
#define FLASH_BTN_W 148
#define FLASH_BTN_H 28
#define DEVICE_BTN_Y 276
#define POWER_BTN_X 188
#define FLASH_HELTEC_Y 336
#define FLASH_TDECK_Y 374

static void draw_flash_btn(SDL_Renderer *ren, int x, int y,
                           int busy, int found,
                           uint8_t ar, uint8_t ag, uint8_t ab,
                           const char *label, const char *status) {
    SDL_Rect box = { x, y, FLASH_BTN_W, FLASH_BTN_H };
    int lw = (int)strlen(label) * 16;
    int tx = x + (FLASH_BTN_W - lw) / 2;
    if (tx < x + 4) tx = x + 4;
    if (busy) {
        SDL_SetRenderDrawColor(ren, ar, ag, ab, 255);
        SDL_RenderFillRect(ren, &box);
        draw_text(ren, tx, y + 6, 2, 16, 20, 28, label);
    } else {
        SDL_SetRenderDrawColor(ren, 28, 34, 46, 255);
        SDL_RenderFillRect(ren, &box);
        SDL_SetRenderDrawColor(ren, found ? ar : 72, found ? ag : 78, found ? ab : 90, 255);
        SDL_RenderDrawRect(ren, &box);
        draw_text(ren, tx, y + 6, 2, found ? 230 : 160, found ? 240 : 168, found ? 255 : 180, label);
    }
    if (status && status[0]) {
        uint8_t sr = 150, sg = 160, sb = 176;
        if (busy) { sr = ar; sg = ag; sb = ab; }
        else if (found) { sr = 200; sg = 210; sb = 220; }
        draw_text(ren, x + FLASH_BTN_W + 12, y + 10, 1, sr, sg, sb, status);
    }
}

static void start_flash(sdl_ctx_t *s, int target) {
    char root[4096];
    int heltec = target == FLASH_HELTEC;
    const char *port;
    if (s->flash_state == FLASH_BUSY) return;
    s->flash_target = target;
    s->flash_msg[0] = 0;
    refresh_devices(s);
    port = heltec ? s->heltec_port : s->tdeck_port;
    if (!port[0]) {
        snprintf(s->flash_msg, sizeof s->flash_msg,
                 heltec ? "No Heltec (CP2102 10c4:ea60) found"
                        : "No T-Deck (303a:1001) found");
        s->flash_state = FLASH_ERR;
        return;
    }
    if (!find_pio(s->flash_pio, sizeof s->flash_pio)) {
        snprintf(s->flash_msg, sizeof s->flash_msg, "pio not found");
        s->flash_state = FLASH_ERR;
        return;
    }
    if (!find_repo_root(root, sizeof root)) {
        snprintf(s->flash_msg, sizeof s->flash_msg, "firmware tree not found");
        s->flash_state = FLASH_ERR;
        return;
    }
    snprintf(s->flash_proj, sizeof s->flash_proj, "%s/%s", root,
             heltec ? "cybiko-heltec-modem" : "cybiko-lilygo");
    snprintf(s->flash_env, sizeof s->flash_env, "%s",
             heltec ? "heltec-modem" : "T-Deck-Cybiko-V1");
    snprintf(s->flash_port, sizeof s->flash_port, "%s", port);
    if (heltec && s->radio_on && s->phy) {
        cybiko_serial_phy_close(s->phy);
        s->radio_on = false;
        s->radio_hold = true;
    }
    s->flash_state = FLASH_BUSY;
    s->flash_shown = -1;
    snprintf(s->flash_msg, sizeof s->flash_msg, "Flashing %s on %s...",
             heltec ? "Heltec" : "T-Deck", s->flash_port);
    if (s->flash_thread) {
        SDL_WaitThread(s->flash_thread, NULL);
        s->flash_thread = NULL;
    }
    s->flash_thread = SDL_CreateThread(flash_worker, "flash", s);
    if (!s->flash_thread) {
        snprintf(s->flash_msg, sizeof s->flash_msg, "could not start flash");
        s->flash_state = FLASH_ERR;
        return;
    }
}

static void draw_settings(sdl_ctx_t *s) {
    uint8_t ar, ag, ab;
    if (!s->settings_ren) return;
    shell_accent(s->color, &ar, &ag, &ab);
    SDL_SetRenderDrawColor(s->settings_ren, 16, 20, 28, 255);
    SDL_RenderClear(s->settings_ren);
    draw_text(s->settings_ren, 20, 16, 2, ar, ag, ab, "Settings");
    const char *rows[] = {
        s->always_on_top ? "[x] Always on top" : "[ ] Always on top",
        s->mute ? "[x] Mute" : "[ ] Mute",
    };
    for (int i = 0; i < 2; i++)
        draw_text(s->settings_ren, 24, 52 + i * 32, 2, 230, 240, 255, rows[i]);
    char scale[40];
    snprintf(scale, sizeof scale, "Scale  -  %d%%  +", s->scale_pct);
    draw_text(s->settings_ren, 24, 116, 2, 230, 240, 255, scale);
    draw_text(s->settings_ren, 24, 148, 2, ar, ag, ab, "Shell");
    draw_swatch(s->settings_ren, 24, 172, s->color == SHELL_BLUE, 40, 110, 230, "Blue");
    draw_swatch(s->settings_ren, 140, 172, s->color == SHELL_NEON, 210, 240, 40, "Neon");
    draw_swatch(s->settings_ren, 256, 172, s->color == SHELL_PURPLE, 180, 40, 230, "Purple");
    draw_text(s->settings_ren, 24, 220, 2, 230, 240, 255,
              s->view == VIEW_BODY ? "[x] Body" : "[ ] Body");
    draw_text(s->settings_ren, 180, 220, 2, 230, 240, 255,
              s->view == VIEW_CLOSEUP ? "[x] 4x zoom" : "[ ] 4x zoom");
    draw_text(s->settings_ren, 24, 256, 2, ar, ag, ab, "Device");
    draw_flash_btn(s->settings_ren, FLASH_BTN_X, DEVICE_BTN_Y, 0, 1,
                   ar, ag, ab, "Reboot", NULL);
    draw_flash_btn(s->settings_ren, POWER_BTN_X, DEVICE_BTN_Y, 0, 1,
                   ar, ag, ab, s->powered ? "Power off" : "Power on", NULL);
    draw_text(s->settings_ren, 24, 316, 2, ar, ag, ab, "Flash");
    {
        char hs[28] = "not found", ts[28] = "not found";
        int hbusy = s->flash_state == FLASH_BUSY && s->flash_target == FLASH_HELTEC;
        int tbusy = s->flash_state == FLASH_BUSY && s->flash_target == FLASH_TDECK;
        if (hbusy) snprintf(hs, sizeof hs, "flashing...");
        else if (s->heltec_port[0])
            snprintf(hs, sizeof hs, "%s", short_port(s->heltec_port));
        if (tbusy) snprintf(ts, sizeof ts, "flashing...");
        else if (s->tdeck_port[0])
            snprintf(ts, sizeof ts, "%s", short_port(s->tdeck_port));
        draw_flash_btn(s->settings_ren, FLASH_BTN_X, FLASH_HELTEC_Y, hbusy,
                       s->heltec_port[0] != 0, ar, ag, ab, "Heltec", hs);
        draw_flash_btn(s->settings_ren, FLASH_BTN_X, FLASH_TDECK_Y, tbusy,
                       s->tdeck_port[0] != 0, ar, ag, ab, "T-Deck", ts);
    }
    if (s->flash_msg[0]) {
        uint8_t r = 230, g = 240, b = 255;
        if (s->flash_state == FLASH_ERR) { r = 255; g = 90; b = 90; }
        else if (s->flash_state == FLASH_OK) { r = 90; g = 220; b = 130; }
        else if (s->flash_state == FLASH_BUSY) { r = ar; g = ag; b = ab; }
        draw_text(s->settings_ren, 24, 414, 1, r, g, b, s->flash_msg);
    }
    draw_text(s->settings_ren, 24, 452, 2, ar, ag, ab, "[ Done ]");
    SDL_RenderPresent(s->settings_ren);
}

static void open_settings(sdl_ctx_t *s) {
    Uint32 flags = 0;
    s->menu_open = false;
    if (s->settings_open) {
        s->overlay_front = OVERLAY_SETTINGS;
        restack_overlays(s);
        return;
    }
    if (s->always_on_top) flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    s->settings_win = SDL_CreateWindow("ReBiko Settings",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SETTINGS_W, SETTINGS_H, flags);
    s->settings_ren = s->settings_win
        ? SDL_CreateRenderer(s->settings_win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (!s->settings_ren) {
        close_settings(s);
        return;
    }
    if (s->win)
        SDL_SetWindowModalFor(s->settings_win, s->win);
    apply_app_icon(s, s->settings_win);
    s->settings_open = true;
    s->overlay_front = OVERLAY_SETTINGS;
    apply_always_on_top(s);
    refresh_devices(s);
    draw_settings(s);
}

static void drop_all_keys(sdl_ctx_t *s) {
    memset(s->key_matrix, 0, sizeof s->key_matrix);
    s->held_count = 0;
    s->mouse_col = -1;
    s->mouse_mask = 0;
}

static void blank_lcd(sdl_ctx_t *s) {
    uint32_t *tex;
    int pitch;
    if (!s->texture) return;
    if (SDL_LockTexture(s->texture, NULL, (void **)&tex, &pitch) != 0)
        return;
    for (int i = 0; i < CYBIKO_LCD_WIDTH * CYBIKO_LCD_HEIGHT; i++)
        tex[i] = 0xFF000000u;
    SDL_UnlockTexture(s->texture);
    present(s);
}

static void guest_reboot(sdl_ctx_t *s) {
    drop_all_keys(s);
    if (s->emu)
        cybiko_reboot(s->emu);
    if (s->coproc) {
        cybiko_radio_coproc_init(s->coproc);
        if (s->lora)
            cybiko_radio_coproc_set_transport(s->coproc, &s->lora->base);
        if (s->emu)
            cybiko_attach_radio_id(s->emu, s->coproc, s->radio_cyid);
    }
    if (s->lora)
        cybiko_lora_seed_msg_id(s->lora, (uint32_t)SDL_GetPerformanceCounter());
    s->powered = true;
    if (s->audio) {
        SDL_ClearQueuedAudio(s->audio);
        SDL_PauseAudioDevice(s->audio, s->mute ? 1 : 0);
    }
    fprintf(stderr, "guest reboot\n");
}

static void guest_power_off(sdl_ctx_t *s) {
    if (!s->powered) return;
    s->powered = false;
    drop_all_keys(s);
    blank_lcd(s);
    if (s->audio) {
        SDL_PauseAudioDevice(s->audio, 1);
        SDL_ClearQueuedAudio(s->audio);
    }
    fprintf(stderr, "guest power off\n");
}

static void settings_click(sdl_ctx_t *s, int x, int y) {
    if (y >= 48 && y < 80) {
        s->always_on_top = !s->always_on_top;
        apply_always_on_top(s);
        save_prefs(s);
    } else if (y >= 80 && y < 112) {
        s->mute = !s->mute;
        if (s->audio) {
            SDL_ClearQueuedAudio(s->audio);
            SDL_PauseAudioDevice(s->audio, (s->mute || !s->powered) ? 1 : 0);
        }
        save_prefs(s);
    } else if (y >= 112 && y < 144) {
        if (x < 140 && s->scale_pct > 50) {
            s->scale_pct -= 10;
            apply_scale(s);
            save_prefs(s);
        } else if (x > 220 && s->scale_pct < 150) {
            s->scale_pct += 10;
            apply_scale(s);
            save_prefs(s);
        }
    } else if (y >= 164 && y < 212) {
        int next = s->color;
        if (x < 128) next = SHELL_BLUE;
        else if (x < 244) next = SHELL_NEON;
        else next = SHELL_PURPLE;
        if (next != s->color) {
            s->color = next;
            apply_shell_tint(s);
            save_prefs(s);
        }
    } else if (y >= 212 && y < 248) {
        int next = (x < 180) ? VIEW_BODY : VIEW_CLOSEUP;
        if (next != s->view) {
            s->view = next;
            apply_scale(s);
            save_prefs(s);
        }
    } else if (y >= DEVICE_BTN_Y && y < DEVICE_BTN_Y + FLASH_BTN_H) {
        if (x >= FLASH_BTN_X && x < FLASH_BTN_X + FLASH_BTN_W)
            guest_reboot(s);
        else if (x >= POWER_BTN_X && x < POWER_BTN_X + FLASH_BTN_W) {
            if (s->powered)
                guest_power_off(s);
            else
                guest_reboot(s);
        }
    } else if (x >= FLASH_BTN_X && x < FLASH_BTN_X + FLASH_BTN_W
               && y >= FLASH_HELTEC_Y && y < FLASH_HELTEC_Y + FLASH_BTN_H) {
        start_flash(s, FLASH_HELTEC);
    } else if (x >= FLASH_BTN_X && x < FLASH_BTN_X + FLASH_BTN_W
               && y >= FLASH_TDECK_Y && y < FLASH_TDECK_Y + FLASH_BTN_H) {
        start_flash(s, FLASH_TDECK);
    } else if (y >= 444) {
        close_settings(s);
        return;
    }
    draw_settings(s);
}

typedef struct { const char *t; uint8_t sec; } about_line_t;

static const about_line_t ABOUT[] = {
    { "The original pocket computer.", 0 },
    { "Still on the air.", 0 },
    { "", 0 },
    { "This is not a tribute app. It", 0 },
    { "boots the real CyOS. A Heltec", 0 },
    { "V3 is the PC radio. A LilyGO", 0 },
    { "T-Deck is a handheld Cybiko.", 0 },
    { "", 0 },
    { "Get going", 1 },
    { " Plug in a Heltec. The app", 0 },
    { " finds it. First boot asks", 0 },
    { " for a nickname, then you", 0 },
    { " land on the CyOS desktop.", 0 },
    { " Chat and Nearby see other", 0 },
    { " Cybikos on 915 MHz.", 0 },
    { " Right-click for Settings.", 0 },
    { "", 0 },
    { "PC", 1 },
    { " Keyboard types. Click the", 0 },
    { " photo keys. Arrows move.", 0 },
    { " Enter opens. Esc backs out.", 0 },
    { " Ctrl+Q quits. Drag the body", 0 },
    { " to move the window.", 0 },
    { " Settings: Reboot or Power.", 0 },
    { "", 0 },
    { "T-Deck", 1 },
    { " The keyboard is the Cybiko.", 0 },
    { " Trackball moves. Click under", 0 },
    { " 0.6s is Enter. Hold 0.6s+", 0 },
    { " is Esc.", 0 },
    { " $ then a letter:", 0 },
    { "  $x Esc    $h Help", 0 },
    { "  $s Select $i Ins  $d Del", 0 },
    { "  $g Tab    $q-$u F1-F7", 0 },
    { "  $$ types a dollar sign", 0 },
    { "  $p / $m volume up / down", 0 },
};

static void draw_about(sdl_ctx_t *s) {
    uint8_t ar, ag, ab;
    char ver[40];
    if (!s->about_ren) return;
    shell_accent(s->color, &ar, &ag, &ab);
    SDL_SetRenderDrawColor(s->about_ren, 16, 20, 28, 255);
    SDL_RenderClear(s->about_ren);
    draw_text(s->about_ren, 20, 16, 2, ar, ag, ab, "ReBiko Desktop");
    snprintf(ver, sizeof ver, "v%s  original CyOS", CYBIKO_EMU_VERSION);
    draw_text(s->about_ren, 20, 40, 1, 180, 190, 210, ver);
    for (size_t i = 0; i < sizeof ABOUT / sizeof ABOUT[0]; i++) {
        int y = 60 + (int)i * 11;
        if (ABOUT[i].sec)
            draw_text(s->about_ren, 20, y, 1, ar, ag, ab, ABOUT[i].t);
        else if (ABOUT[i].t[0])
            draw_text(s->about_ren, 20, y, 1, 230, 240, 255, ABOUT[i].t);
    }
    draw_text(s->about_ren, 20, ABOUT_H - 36, 2, ar, ag, ab, "[ Done ]");
    SDL_RenderPresent(s->about_ren);
}

static void open_about(sdl_ctx_t *s) {
    Uint32 flags = 0;
    s->menu_open = false;
    if (s->about_open) {
        s->overlay_front = OVERLAY_ABOUT;
        restack_overlays(s);
        return;
    }
    if (s->always_on_top) flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    s->about_win = SDL_CreateWindow("ReBiko Help",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ABOUT_W, ABOUT_H, flags);
    s->about_ren = s->about_win
        ? SDL_CreateRenderer(s->about_win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (!s->about_ren) {
        close_about(s);
        return;
    }
    if (s->win)
        SDL_SetWindowModalFor(s->about_win, s->win);
    apply_app_icon(s, s->about_win);
    s->about_open = true;
    s->overlay_front = OVERLAY_ABOUT;
    apply_always_on_top(s);
    draw_about(s);
}

static void about_click(sdl_ctx_t *s, int x, int y) {
    (void)x;
    if (y >= ABOUT_H - 44) {
        close_about(s);
        return;
    }
}

static void draw_status(sdl_ctx_t *s) {
    uint8_t ar, ag, ab;
    char line[56];
    cybiko_v1_radio_status_t st = {0};
    const char *port;
    int heltec_yes;
    SDL_Renderer *ren;
    if (!s->status_ren) return;
    ren = s->status_ren;
    refresh_devices(s);
    heltec_yes = s->heltec_port[0] != 0;
    port = heltec_yes ? short_port(s->heltec_port) : "not found";
    if (s->emu)
        cybiko_v1_radio_status(s->emu, &st);
    shell_accent(s->color, &ar, &ag, &ab);
    SDL_SetRenderDrawColor(ren, 16, 20, 28, 255);
    SDL_RenderClear(ren);
    draw_text(ren, 20, 16, 2, ar, ag, ab, "Status");

    draw_text(ren, 20, 52, 2, ar, ag, ab, "Heltec");
    snprintf(line, sizeof line, "  %s  %s", heltec_yes ? "YES" : "NO", port);
    draw_text(ren, 20, 76, 1, heltec_yes ? 90 : 255, heltec_yes ? 220 : 90,
              heltec_yes ? 130 : 90, line);
    if (s->radio_on)
        draw_text(ren, 20, 92, 1, 90, 220, 130, "  ReBiko Radio linked");
    else if (heltec_yes)
        draw_text(ren, 20, 92, 1, 255, 180, 80, "  seen, radio not open");
    else
        draw_text(ren, 20, 92, 1, 255, 90, 90, "  plug in a Heltec V3");
    if (s->phy && s->phy->hello[0]) {
        snprintf(line, sizeof line, "  hello %.36s", s->phy->hello);
        draw_text(ren, 20, 108, 1, 180, 190, 210, line);
    } else {
        draw_text(ren, 20, 108, 1, 180, 190, 210, "  no modem hello yet");
    }
    if (s->phy && s->radio_on) {
        snprintf(line, sizeof line, "  rssi %d dBm  snr %d",
                 s->phy->stats.last_rssi_dbm, s->phy->stats.last_snr_db);
        draw_text(ren, 20, 124, 1, 230, 240, 255, line);
        snprintf(line, sizeof line, "  usb rx %u  tx ok %u  lost %u",
                 s->phy->stats.rx_pkts, s->phy->stats.tx_sent,
                 s->phy->stats.tx_lost);
        draw_text(ren, 20, 140, 1, 230, 240, 255, line);
    }
    if (s->lora) {
        snprintf(line, sizeof line, "  air tx %u  rx %u  bad %u  missed %u",
                 s->lora->stats.tx_pkts, s->lora->stats.rx_msgs,
                 s->lora->stats.rx_bad, s->lora->stats.rx_lost);
        draw_text(ren, 20, 156, 1, 230, 240, 255, line);
    }

    draw_text(ren, 20, 184, 2, ar, ag, ab, "T-Deck");
    if (s->tdeck_port[0]) {
        snprintf(line, sizeof line, "  YES  %s", short_port(s->tdeck_port));
        draw_text(ren, 20, 208, 1, 90, 220, 130, line);
    } else {
        draw_text(ren, 20, 208, 1, 180, 190, 210, "  not on USB");
    }

    draw_text(ren, 20, 236, 2, ar, ag, ab, "CyOS radio");
    snprintf(line, sizeof line, "  radio %s  CyID %08X",
             st.radio_up ? "up" : "-",
             (unsigned)s->radio_cyid);
    draw_text(ren, 20, 260, 1, 230, 240, 255, line);
    snprintf(line, sizeof line, "  guest sent %u  delivered %u",
             st.tx_frames, st.rx_injected);
    draw_text(ren, 20, 276, 1, 230, 240, 255, line);

    draw_text(ren, 20, STATUS_H - 36, 2, ar, ag, ab, "[ Done ]");
    SDL_RenderPresent(ren);
}

static void open_status(sdl_ctx_t *s) {
    Uint32 flags = 0;
    s->menu_open = false;
    if (s->status_open) {
        s->overlay_front = OVERLAY_STATUS;
        restack_overlays(s);
        draw_status(s);
        return;
    }
    if (s->always_on_top) flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    s->status_win = SDL_CreateWindow("ReBiko Status",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        STATUS_W, STATUS_H, flags);
    s->status_ren = s->status_win
        ? SDL_CreateRenderer(s->status_win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (!s->status_ren) {
        close_status(s);
        return;
    }
    if (s->win)
        SDL_SetWindowModalFor(s->status_win, s->win);
    apply_app_icon(s, s->status_win);
    s->status_open = true;
    s->overlay_front = OVERLAY_STATUS;
    apply_always_on_top(s);
    draw_status(s);
}

static void status_click(sdl_ctx_t *s, int x, int y) {
    (void)x;
    if (y >= STATUS_H - 44)
        close_status(s);
}

static void menu_action(sdl_ctx_t *s, int item) {
    s->menu_open = false;
    if (item == 0) open_settings(s);
    else if (item == 1) open_status(s);
    else if (item == 2) open_about(s);
    else if (item == 3 && s->win) SDL_MinimizeWindow(s->win);
    else if (item == 4 && s->running) *s->running = false;
}

static const shell_hit_t *hit_at(int x, int y) {
    for (size_t i = 0; i < sizeof SHELL_HITS / sizeof SHELL_HITS[0]; i++) {
        const shell_hit_t *h = &SHELL_HITS[i];
        if (h->r > 0) {
            int dx = x - h->x, dy = y - h->y;
            if (dx * dx + dy * dy <= h->r * h->r)
                return h;
        } else if (x >= h->x && x <= h->x1 && y >= h->y && y <= h->y1) {
            return h;
        }
    }
    return NULL;
}

static void win_to_img(const sdl_ctx_t *s, int wx, int wy, int *ix, int *iy) {
    if (s->win_w <= 0 || s->win_h <= 0) {
        *ix = wx;
        *iy = wy;
        return;
    }
    *ix = wx * s->img_w / s->win_w;
    *iy = wy * s->img_h / s->win_h;
}

static void mouse_release(sdl_ctx_t *s) {
    if (s->mouse_col >= 0) {
        press_hold(s, s->mouse_col, s->mouse_mask, false);
        s->mouse_col = -1;
        s->mouse_mask = 0;
    }
}

static void mouse_to(sdl_ctx_t *s, int ix, int iy, bool down) {
    if (!down) {
        mouse_release(s);
        return;
    }
    const shell_hit_t *h = hit_at(ix, iy);
    if (!h) {
        mouse_release(s);
        return;
    }
    if (s->mouse_col == h->col && s->mouse_mask == h->mask)
        return;
    mouse_release(s);
    s->mouse_col = h->col;
    s->mouse_mask = h->mask;
    press_hold(s, h->col, h->mask, true);
}

static void present(sdl_ctx_t *s) {
    int body = s->shell && s->view != VIEW_CLOSEUP;
    SDL_SetRenderDrawBlendMode(s->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(s->renderer, 0, 0, 0, body ? 0 : 255);
    SDL_RenderClear(s->renderer);
    if (body) {
        SDL_SetRenderDrawColor(s->renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(s->renderer, &s->lcd_dst);
        SDL_SetRenderDrawBlendMode(s->renderer, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(s->renderer, s->texture, NULL, &s->lcd_dst);
        SDL_RenderCopy(s->renderer, s->shell, NULL, NULL);
    } else {
        SDL_SetRenderDrawBlendMode(s->renderer, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(s->renderer, s->texture, NULL, NULL);
    }
    draw_menu(s);
    SDL_RenderPresent(s->renderer);
}

static void hal_render(void *ctx, const uint8_t *px, int w, int h) {
    sdl_ctx_t *s = ctx;
    uint32_t *tex; int pitch;
    SDL_LockTexture(s->texture, NULL, (void **)&tex, &pitch);
    for (int i = 0; i < w * h; i++) {
        uint8_t g = px[i];
        tex[i] = 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
    }
    SDL_UnlockTexture(s->texture);
    present(s);
}

static void hal_keys(void *ctx, uint16_t *m, int n) {
    sdl_ctx_t *s = ctx;
    int cols = n < CYBIKO_KEYBOARD_COLUMNS ? n : CYBIKO_KEYBOARD_COLUMNS;
    memcpy(m, s->key_matrix, (size_t)cols * sizeof(uint16_t));
}

/* Most audio allowed to wait in SDL's queue: 100 ms. Past that a frame's
 * samples are dropped, so the speaker never lags the screen by more. */
#define AUDIO_QUEUE_MAX (SPEAKER_SAMPLE_RATE / 10)

static void hal_audio(void *ctx, const uint8_t *samples, int count) {
    sdl_ctx_t *s = ctx;
    if (!s->audio || s->mute || !s->powered) return;
    if (SDL_GetQueuedAudioSize(s->audio) > AUDIO_QUEUE_MAX) return;
    SDL_QueueAudio(s->audio, samples, (Uint32)count);
}

static void hal_serial(void *ctx, uint8_t byte) {
    (void)ctx;
    fputc(byte, stderr);
}

static int find_held(sdl_ctx_t *s, int col, uint16_t mask) {
    for (int i = 0; i < s->held_count; i++)
        if (s->held_col[i] == col && s->held_mask[i] == mask) return i;
    return -1;
}

static void drop_held(sdl_ctx_t *s, int i) {
    s->held_count--;
    if (i < s->held_count) {
        s->held_col[i] = s->held_col[s->held_count];
        s->held_mask[i] = s->held_mask[s->held_count];
        s->held_frames[i] = s->held_frames[s->held_count];
        s->held_release[i] = s->held_release[s->held_count];
    }
}

static void press_hold(sdl_ctx_t *s, int col, uint16_t mask, bool down) {
    if (down) {
        s->key_matrix[col] |= mask;
        int i = find_held(s, col, mask);
        if (i >= 0) {
            s->held_frames[i] = MIN_HOLD;
            s->held_release[i] = false;
        } else if (s->held_count < MAX_HELD) {
            int n = s->held_count++;
            s->held_col[n] = col;
            s->held_mask[n] = mask;
            s->held_frames[n] = MIN_HOLD;
            s->held_release[n] = false;
        }
    } else {
        int i = find_held(s, col, mask);
        if (i >= 0 && s->held_frames[i] > 0)
            s->held_release[i] = true;
        else {
            s->key_matrix[col] &= ~mask;
            if (i >= 0) drop_held(s, i);
        }
    }
}

static void tick_keys(sdl_ctx_t *s) {
    for (int i = 0; i < s->held_count; i++) {
        if (s->held_frames[i] > 0) s->held_frames[i]--;
        if (s->held_frames[i] <= 0 && s->held_release[i]) {
            s->key_matrix[s->held_col[i]] &= ~s->held_mask[i];
            drop_held(s, i);
            i--;
        }
    }
}

static void on_key(sdl_ctx_t *s, const SDL_KeyboardEvent *ev) {
    if (ev->repeat) return;
    bool down = ev->type == SDL_KEYDOWN;
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        if (KEYS[i].sc == ev->keysym.scancode) {
            press_hold(s, KEYS[i].col, KEYS[i].mask, down);
            return;
        }
    }
}

static void on_event(sdl_ctx_t *s, const SDL_Event *ev, bool *running) {
    int ix, iy;
    Uint32 settings_id = s->settings_win ? SDL_GetWindowID(s->settings_win) : 0;
    Uint32 about_id = s->about_win ? SDL_GetWindowID(s->about_win) : 0;
    Uint32 status_id = s->status_win ? SDL_GetWindowID(s->status_win) : 0;
    switch (ev->type) {
    case SDL_QUIT:
        *running = false;
        break;
    case SDL_KEYDOWN:
        if (ev->key.keysym.sym == SDLK_q && (ev->key.keysym.mod & KMOD_CTRL)) {
            *running = false;
            break;
        }
        if (s->menu_open && ev->key.keysym.sym == SDLK_ESCAPE) {
            s->menu_open = false;
            break;
        }
        if (s->about_open && about_id && ev->key.windowID == about_id
            && ev->key.keysym.sym == SDLK_ESCAPE) {
            close_about(s);
            break;
        }
        if (s->status_open && status_id && ev->key.windowID == status_id
            && ev->key.keysym.sym == SDLK_ESCAPE) {
            close_status(s);
            break;
        }
        /* fall through */
    case SDL_KEYUP:
        if ((settings_id && ev->key.windowID == settings_id)
            || (about_id && ev->key.windowID == about_id)
            || (status_id && ev->key.windowID == status_id))
            break;
        on_key(s, &ev->key);
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (settings_id && ev->button.windowID == settings_id) {
            if (ev->button.button == SDL_BUTTON_LEFT)
                settings_click(s, ev->button.x, ev->button.y);
            break;
        }
        if (about_id && ev->button.windowID == about_id) {
            if (ev->button.button == SDL_BUTTON_LEFT)
                about_click(s, ev->button.x, ev->button.y);
            break;
        }
        if (status_id && ev->button.windowID == status_id) {
            if (ev->button.button == SDL_BUTTON_LEFT)
                status_click(s, ev->button.x, ev->button.y);
            break;
        }
        if (s->settings_open || s->about_open || s->status_open)
            restack_overlays(s);
        if (ev->button.button == SDL_BUTTON_RIGHT) {
            s->menu_open = true;
            s->menu_x = ev->button.x;
            s->menu_y = ev->button.y;
            if (s->menu_x + MENU_W > s->win_w) s->menu_x = s->win_w - MENU_W;
            if (s->menu_y + MENU_ROW * MENU_N > s->win_h)
                s->menu_y = s->win_h - MENU_ROW * MENU_N;
            if (s->menu_x < 0) s->menu_x = 0;
            if (s->menu_y < 0) s->menu_y = 0;
            s->menu_hover = menu_item_at(s, ev->button.x, ev->button.y);
            mouse_release(s);
            apply_window_shape(s);
            break;
        }
        if (ev->button.button != SDL_BUTTON_LEFT) break;
        if (s->menu_open) {
            int item = menu_item_at(s, ev->button.x, ev->button.y);
            if (item >= 0) menu_action(s, item);
            else s->menu_open = false;
            apply_window_shape(s);
            break;
        }
        if (s->view == VIEW_CLOSEUP) {
            int gx, gy, wx, wy;
            SDL_GetGlobalMouseState(&gx, &gy);
            SDL_GetWindowPosition(s->win, &wx, &wy);
            s->dragging = true;
            s->drag_off_x = gx - wx;
            s->drag_off_y = gy - wy;
            break;
        }
        if (!s->shell) break;
        win_to_img(s, ev->button.x, ev->button.y, &ix, &iy);
        if (hit_at(ix, iy)) {
            mouse_to(s, ix, iy, true);
        } else if (on_shell_body(s, ev->button.x, ev->button.y)) {
            int gx, gy, wx, wy;
            SDL_GetGlobalMouseState(&gx, &gy);
            SDL_GetWindowPosition(s->win, &wx, &wy);
            s->dragging = true;
            s->drag_off_x = gx - wx;
            s->drag_off_y = gy - wy;
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (ev->button.button != SDL_BUTTON_LEFT) break;
        s->dragging = false;
        mouse_release(s);
        break;
    case SDL_MOUSEMOTION:
        if (s->menu_open && ev->motion.windowID != settings_id
            && ev->motion.windowID != about_id
            && ev->motion.windowID != status_id)
            s->menu_hover = menu_item_at(s, ev->motion.x, ev->motion.y);
        if (s->dragging && (ev->motion.state & SDL_BUTTON_LMASK) && s->win) {
            int gx, gy;
            SDL_GetGlobalMouseState(&gx, &gy);
            SDL_SetWindowPosition(s->win, gx - s->drag_off_x, gy - s->drag_off_y);
            break;
        }
        if (!(ev->motion.state & SDL_BUTTON_LMASK) || !s->shell || s->menu_open
            || s->view == VIEW_CLOSEUP) break;
        if ((settings_id && ev->motion.windowID == settings_id)
            || (about_id && ev->motion.windowID == about_id)
            || (status_id && ev->motion.windowID == status_id)) break;
        win_to_img(s, ev->motion.x, ev->motion.y, &ix, &iy);
        mouse_to(s, ix, iy, true);
        break;
    case SDL_WINDOWEVENT:
        if (settings_id && ev->window.windowID == settings_id) {
            if (ev->window.event == SDL_WINDOWEVENT_CLOSE)
                close_settings(s);
            else if (ev->window.event == SDL_WINDOWEVENT_EXPOSED)
                draw_settings(s);
            break;
        }
        if (about_id && ev->window.windowID == about_id) {
            if (ev->window.event == SDL_WINDOWEVENT_CLOSE)
                close_about(s);
            else if (ev->window.event == SDL_WINDOWEVENT_EXPOSED)
                draw_about(s);
            break;
        }
        if (status_id && ev->window.windowID == status_id) {
            if (ev->window.event == SDL_WINDOWEVENT_CLOSE)
                close_status(s);
            else if (ev->window.event == SDL_WINDOWEVENT_EXPOSED)
                draw_status(s);
            break;
        }
        if (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            s->win_w = ev->window.data1;
            s->win_h = ev->window.data2;
            if (s->view == VIEW_CLOSEUP || !s->shape_src) {
                s->lcd_dst.x = 0;
                s->lcd_dst.y = 0;
                s->lcd_dst.w = s->win_w;
                s->lcd_dst.h = s->win_h;
            } else {
                layout_lcd(s);
            }
            apply_window_shape(s);
        } else if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED
                   && (s->settings_open || s->about_open || s->status_open)) {
            restack_overlays(s);
        }
        break;
    default:
        break;
    }
}

static uint8_t *load_file(const char *path, size_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "read failed on '%s'\n", path);
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out = (size_t)n;
    return b;
}

static void radio_on_text(void *user, char type, const char *text) {
    (void)user;
    fprintf(stderr, "modem %s: %s\n", type == 'H' ? "hello" : "status", text);
}

static void radio_reopen(sdl_ctx_t *s) {
    if (!s->radio_hold || s->flash_state == FLASH_BUSY) return;
    s->radio_hold = false;
    refresh_devices(s);
    if (s->heltec_port[0]) {
        snprintf(s->radio_buf, sizeof s->radio_buf, "%s", s->heltec_port);
        s->radio_path = s->radio_buf;
    }
    if (!s->phy || !s->radio_path || !s->radio_path[0]) return;
    if (cybiko_serial_phy_open(s->phy, s->radio_path) != 0) {
        char extra[192];
        snprintf(extra, sizeof extra, "%s (radio closed)", s->flash_msg);
        snprintf(s->flash_msg, sizeof s->flash_msg, "%s", extra);
        if (s->flash_state == FLASH_OK) s->flash_state = FLASH_ERR;
        fprintf(stderr, "radio reopen '%s': %s\n", s->radio_path, strerror(errno));
        return;
    }
    s->phy->on_text = radio_on_text;
    cybiko_serial_phy_request_status(s->phy);
    s->radio_on = true;
    fprintf(stderr, "Radio reopened on %s\n", s->radio_path);
}

static void radio_report(const cybiko_emu_t *emu, const cybiko_lora_t *lt,
                         const cybiko_serial_phy_t *sp) {
    cybiko_v1_radio_status_t st = {0};
    cybiko_v1_radio_status(emu, &st);
    fprintf(stderr, "Radio: %s guest sent %u delivered %u  "
            "tx msgs %u pkts %u rejected %u  rx pkts %u msgs %u bad %u lost %u  "
            "rssi %d snr %d\n",
            st.radio_up ? "up" : "-",
            st.tx_frames, st.rx_injected,
            lt->stats.tx_msgs, lt->stats.tx_pkts,
            lt->stats.tx_rejected, lt->stats.rx_pkts, lt->stats.rx_msgs,
            lt->stats.rx_bad, lt->stats.rx_lost,
            sp->stats.last_rssi_dbm, sp->stats.last_snr_db);
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        fprintf(stdout, "ReBiko Desktop %s\n", CYBIKO_EMU_VERSION);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "ReBiko Desktop %s\n"
                "usage: %s boot.bin flash.bin [--nvram save.flash] "
                "[--radio PORT] [--cyid 4C000001] [--shell cybiko.png]\n"
                "  PORT is /dev/ttyUSB0 or COMn; omitted = first Heltec CP2102\n",
                CYBIKO_EMU_VERSION, argv[0]);
        return 1;
    }
    const char *boot_path = argv[1], *flash_path = argv[2];
    const char *nvram_path = NULL, *radio_path = NULL, *shell_arg = NULL;
    uint32_t cyid = 0x4C000001u;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--nvram") == 0 && i + 1 < argc) nvram_path = argv[++i];
        else if (strcmp(argv[i], "--radio") == 0 && i + 1 < argc) radio_path = argv[++i];
        else if (strcmp(argv[i], "--shell") == 0 && i + 1 < argc) shell_arg = argv[++i];
        else if (strcmp(argv[i], "--cyid") == 0 && i + 1 < argc)
            cyid = (uint32_t)strtoul(argv[++i], NULL, 16);
        else fprintf(stderr, "unknown option '%s'\n", argv[i]);
    }

    size_t boot_n = 0, flash_n = 0;
    uint8_t *boot = load_file(boot_path, &boot_n);
    uint8_t *flash = NULL;
    if (nvram_path)
        flash = load_file(nvram_path, &flash_n);
    if (!flash)
        flash = load_file(flash_path, &flash_n);
    if (!boot || !flash) return 1;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
#ifndef _WIN32
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    {
        unsigned long vis = pick_argb_visual();
        if (vis) {
            char buf[32];
            snprintf(buf, sizeof buf, "%lu", vis);
            SDL_SetHint(SDL_HINT_VIDEO_X11_WINDOW_VISUALID, buf);
            fprintf(stderr, "ARGB visual 0x%lx\n", vis);
        }
    }
    fprintf(stderr, "ReBiko Desktop %s\n", CYBIKO_EMU_VERSION);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    int win_w = CYBIKO_LCD_WIDTH * 3, win_h = CYBIKO_LCD_HEIGHT * 3;
    const char *shell_path = find_shell_png(shell_arg);
    sdl_ctx_t sdl;
    memset(&sdl, 0, sizeof sdl);
    sdl.mouse_col = -1;
    sdl.powered = true;
    load_prefs(&sdl);
    sdl.app_icon = load_app_icon();
    if (sdl.app_icon)
        fprintf(stderr, "app icon %s\n", find_icon_png());

    SDL_Window *win = NULL;
    if (shell_path) {
        sdl.shell_base = load_png_surface(shell_path, &sdl.img_w, &sdl.img_h);
        if (sdl.shell_base) {
            sdl.shape_src = SDL_ConvertSurfaceFormat(
                sdl.shell_base, SDL_PIXELFORMAT_ARGB8888, 0);
            if (!sdl.shape_src) {
                SDL_FreeSurface(sdl.shell_base);
                sdl.shell_base = NULL;
            }
        }
        if (sdl.shape_src) {
            SDL_DisplayMode dm;
            int max_h = 900, max_w = 520;
            if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
                max_h = dm.h - 40;
                max_w = dm.w - 40;
            }
            float sc = 1.f;
            if (sdl.img_h > max_h) sc = (float)max_h / (float)sdl.img_h;
            if (sdl.img_w * sc > (float)max_w)
                sc = (float)max_w / (float)sdl.img_w;
            sdl.fit_w = (int)(sdl.img_w * sc + 0.5f);
            sdl.fit_h = (int)(sdl.img_h * sc + 0.5f);
            if (sdl.fit_w < 1) sdl.fit_w = 1;
            if (sdl.fit_h < 1) sdl.fit_h = 1;
            if (sdl.view == VIEW_CLOSEUP) {
                win_w = CYBIKO_LCD_WIDTH * 4 * sdl.scale_pct / 100;
                win_h = CYBIKO_LCD_HEIGHT * 4 * sdl.scale_pct / 100;
            } else {
                win_w = sdl.fit_w * sdl.scale_pct / 100;
                win_h = sdl.fit_h * sdl.scale_pct / 100;
            }
            win = SDL_CreateShapedWindow("ReBiko Desktop",
                (unsigned)SDL_WINDOWPOS_CENTERED, (unsigned)SDL_WINDOWPOS_CENTERED,
                (unsigned)win_w, (unsigned)win_h,
                SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS);
            if (!win) {
                fprintf(stderr, "shaped window failed (%s), borderless instead\n",
                        SDL_GetError());
                win = SDL_CreateWindow("ReBiko Desktop",
                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                    win_w, win_h, SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS);
            }
        }
    }
    if (!win)
        win = SDL_CreateWindow("ReBiko Desktop",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_w, win_h, SDL_WINDOW_HIDDEN);
    /* Accelerated/GL targets stay rectangular; software + XShape actually
     * clips the window to the photo. */
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1,
        sdl.shape_src ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED) : NULL;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, CYBIKO_LCD_WIDTH, CYBIKO_LCD_HEIGHT) : NULL;
    if (!tex) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        return 1;
    }

    sdl.win = win;
    sdl.renderer = ren;
    sdl.texture = tex;
    apply_app_icon(&sdl, win);
    if (sdl.shape_src && ren) {
        apply_shell_tint(&sdl);
        if (sdl.shell) {
            sdl.win_w = win_w;
            sdl.win_h = win_h;
            if (sdl.view == VIEW_CLOSEUP) {
                sdl.lcd_dst.x = 0;
                sdl.lcd_dst.y = 0;
                sdl.lcd_dst.w = win_w;
                sdl.lcd_dst.h = win_h;
            } else {
                layout_lcd(&sdl);
            }
            apply_window_shape(&sdl);
            apply_always_on_top(&sdl);
            fprintf(stderr, "shell %s %dx%d window %dx%d (borderless)\n",
                    shell_path, sdl.img_w, sdl.img_h, win_w, win_h);
        } else {
            fprintf(stderr, "shell '%s' failed, LCD only\n", shell_path);
        }
    } else if (shell_path) {
        fprintf(stderr, "shell '%s' failed, LCD only\n", shell_path);
    } else {
        fprintf(stderr, "no img/cybiko.png, LCD only\n");
    }
    sdl.win_w = win_w;
    sdl.win_h = win_h;
    SDL_ShowWindow(win);
    apply_window_shape(&sdl);
    apply_always_on_top(&sdl);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = SPEAKER_SAMPLE_RATE;
    want.format = AUDIO_U8;
    want.channels = 1;
    want.samples = 2048;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    sdl.audio = audio;

    cybiko_hal_t hal = {0};
    hal.render_frame = hal_render;
    hal.audio_output = hal_audio;
    hal.keyboard_poll = hal_keys;
    hal.serial_output = hal_serial;
    hal.ctx = &sdl;
    cybiko_emu_t *emu = cybiko_create(&hal);
    if (!emu || !cybiko_load_boot_rom(emu, boot, boot_n) ||
        !cybiko_load_flash_rom(emu, flash, flash_n)) {
        fprintf(stderr, "emulator setup failed\n");
        return 1;
    }
    free(boot);
    free(flash);

    static cybiko_serial_phy_t phy;
    static cybiko_lora_t link;
    static cybiko_radio_coproc_t cop;
    sdl.phy = &phy;
    sdl.emu = emu;
    sdl.radio_cyid = cyid;
    if (!radio_path) {
        refresh_devices(&sdl);
        if (sdl.heltec_port[0]) {
            snprintf(sdl.radio_buf, sizeof sdl.radio_buf, "%s", sdl.heltec_port);
            radio_path = sdl.radio_buf;
            fprintf(stderr, "Radio auto-detected %s (%s)\n",
                    sdl.radio_buf,
                    sdl.heltec_label[0] ? sdl.heltec_label : "Heltec");
        }
    } else {
        snprintf(sdl.radio_buf, sizeof sdl.radio_buf, "%s", radio_path);
        radio_path = sdl.radio_buf;
    }
    sdl.radio_path = radio_path;
    if (radio_path) {
        if (cybiko_serial_phy_open(&phy, radio_path) != 0) {
            fprintf(stderr, "radio '%s': %s%s\n", radio_path, strerror(errno),
                    errno == EBUSY
                        ? " — this window will not use ReBiko Radio"
                        : ", continuing without radio");
        } else {
            phy.on_text = radio_on_text;
            cybiko_lora_init(&link, &phy.phy, cyid, true);
            cybiko_lora_seed_msg_id(&link, (uint32_t)SDL_GetPerformanceCounter());
            sdl.lora = &link;
            sdl.coproc = &cop;
            cybiko_radio_coproc_init(&cop);
            cybiko_radio_coproc_set_transport(&cop, &link.base);
            cybiko_attach_radio_id(emu, &cop, cyid);
            cybiko_serial_phy_request_status(&phy);
            sdl.radio_on = true;
            fprintf(stderr, "Radio on %s, CyID %08X\n", radio_path, (unsigned)cyid);
        }
    }
    cybiko_reset(emu);
    if (audio) SDL_PauseAudioDevice(audio, sdl.mute ? 1 : 0);

    bool running = true;
    sdl.running = &running;
    uint32_t radio_at = SDL_GetTicks() + 10000;
    uint32_t status_at = 0;
    /* Pace to a deadline 1/60 s apart. Waiting out 16 ms per frame ran the
     * guest 4% fast, and the audio queue grew by the difference. */
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 period = freq / CYBIKO_FPS;
    Uint64 next = SDL_GetPerformanceCounter();
    while (running) {
        uint32_t t0 = SDL_GetTicks();
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            on_event(&sdl, &ev, &running);
        if (sdl.powered) {
            cybiko_run_frame(emu);
            tick_keys(&sdl);
        }
        if (sdl.radio_on) {
            link.base.pump(&link.base);
            if ((int32_t)(SDL_GetTicks() - radio_at) >= 0) {
                radio_report(emu, &link, &phy);
                radio_at += 10000;
            }
        }
        radio_reopen(&sdl);
        if (sdl.settings_open &&
            (sdl.flash_state == FLASH_BUSY || sdl.flash_shown != sdl.flash_state)) {
            if (sdl.flash_shown != sdl.flash_state && sdl.flash_state != FLASH_BUSY)
                refresh_devices(&sdl);
            sdl.flash_shown = sdl.flash_state;
            draw_settings(&sdl);
        }
        if (sdl.status_open && (int32_t)(t0 - status_at) >= 250) {
            status_at = t0;
            draw_status(&sdl);
        }
        while (SDL_PollEvent(&ev))
            on_event(&sdl, &ev, &running);
        next += period;
        Uint64 now = SDL_GetPerformanceCounter();
        if (now < next)
            SDL_Delay((Uint32)((next - now) * 1000 / freq));
        else if (now - next > freq / 10)
            next = now;   /* fell behind: don't race to catch up */
    }

    if (nvram_path) {
        size_t n = 0;
        uint32_t writes = 0;
        const uint8_t *img = cybiko_get_spi_flash(emu, &n, &writes);
        if (img && n) {
            FILE *f = fopen(nvram_path, "wb");
            if (f) {
                fwrite(img, 1, n, f);
                fclose(f);
                fprintf(stderr, "saved %s (%zu bytes, %u page writes)\n",
                        nvram_path, n, writes);
            }
        }
    }
    if (sdl.flash_thread) {
        SDL_WaitThread(sdl.flash_thread, NULL);
        sdl.flash_thread = NULL;
    }
    if (sdl.radio_on) {
        radio_report(emu, &link, &phy);
        cybiko_serial_phy_close(&phy);
    }
    cybiko_destroy(emu);
    close_status(&sdl);
    close_about(&sdl);
    close_settings(&sdl);
    if (audio) SDL_CloseAudioDevice(audio);
    if (sdl.shell) SDL_DestroyTexture(sdl.shell);
    if (sdl.shape_src) SDL_FreeSurface(sdl.shape_src);
    if (sdl.shell_base) SDL_FreeSurface(sdl.shell_base);
    if (sdl.app_icon) SDL_FreeSurface(sdl.app_icon);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
