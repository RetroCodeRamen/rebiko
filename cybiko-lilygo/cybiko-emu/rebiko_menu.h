#ifndef REBIKO_MENU_H
#define REBIKO_MENU_H

/*
 * The ReBiko emulator menu: screens, cursor, and what each row does.
 * It edits a working copy of the settings and hands every change to
 * hooks.apply at once, so brightness or volume can be seen and heard
 * while the menu is open. The caller saves the copy when the menu
 * closes, if it differs from what is stored.
 *
 * Drawing is the caller's: it asks for the title, the text lines above
 * the list, and each visible row (label and value).
 */

#include <stdbool.h>
#include <stddef.h>

#include "rebiko_config.h"
#include "rebiko_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RB_SCR_MAIN, RB_SCR_CONTROLS, RB_SCR_TARGET, RB_SCR_CAPTURE,
    RB_SCR_CONFIRM, RB_SCR_MESSAGE, RB_SCR_PERF, RB_SCR_DISPLAY,
    RB_SCR_AUDIO, RB_SCR_SYSTEM, RB_SCR_INFO, RB_SCR_KEYREF,
} rb_screen_t;

#define RB_MENU_VISIBLE 8   /* rows on screen at once */
#define RB_MENU_DEPTH   8

typedef struct {
    void *ctx;
    /* Settings changed: apply them now (not saved yet) */
    void (*apply)(void *ctx, const rb_config_t *c);
    /* Hardware and build info, one row each */
    int  (*info_count)(void *ctx);
    void (*info_row)(void *ctx, int i, char *label, size_t ln, char *value, size_t vn);
    /* Save everything and restart the ESP32 */
    void (*restart)(void *ctx, const rb_config_t *c);
    bool (*link_view)(void *ctx);
    void (*set_link_view)(void *ctx, bool on);
    /* The other Cybiko model: save, then restart into it. Returns only if
     * it isn't installed. NULL hides the row. */
    bool (*switch_machine)(void *ctx, const rb_config_t *c);
    const char *version;
    const char *machine;        /* "Classic" or "Xtreme" */
    const char *other_machine;
} rb_menu_hooks_t;

typedef struct {
    uint8_t screen, cursor, top;
} rb_menu_level_t;

typedef struct {
    rb_config_t cfg;
    rb_menu_hooks_t hooks;
    rb_menu_level_t stack[RB_MENU_DEPTH];
    int depth;              /* 0 = closed */
    int target;             /* Controls: index into rb_target() */
    bool capture_adds;      /* capture adds an input instead of replacing */
    rb_chord_t captured;
    uint8_t confirm_op;
    char lines[3][48];      /* text above the list on capture/confirm/message */
} rb_menu_t;

/* Result flags from rb_menu_nav / rb_menu_capture */
#define RB_MENU_REDRAW 1
#define RB_MENU_CLOSED 2

void rb_menu_init(rb_menu_t *m, const rb_menu_hooks_t *hooks);
void rb_menu_open(rb_menu_t *m, const rb_config_t *cfg, rb_screen_t first);
bool rb_menu_is_open(const rb_menu_t *m);
int  rb_menu_nav(rb_menu_t *m, rb_nav_t nav);
int  rb_menu_capture(rb_menu_t *m, rb_chord_t c);
/* What the router should do with input: menu navigation or capture */
rb_mode_t rb_menu_mode(const rb_menu_t *m);

rb_screen_t rb_menu_screen(const rb_menu_t *m);
const char *rb_menu_title(const rb_menu_t *m);
int  rb_menu_rows(const rb_menu_t *m);
int  rb_menu_cursor(const rb_menu_t *m);
int  rb_menu_top(const rb_menu_t *m);
/* has_value: the row shows a value on the right (it may be adjustable
 * with left and right). Returns whether the row can be chosen. */
bool rb_menu_row(const rb_menu_t *m, int i, char *label, size_t ln,
                 char *value, size_t vn);
/* Lines of text above the list (capture, confirm, message); "" if none */
const char *rb_menu_line(const rb_menu_t *m, int i);
const char *rb_menu_hint(const rb_menu_t *m);

/* "$+Q", "Enter, Ball click", "none" */
void rb_inputs_text(const rb_bindings_t *b, rb_action_t a, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
