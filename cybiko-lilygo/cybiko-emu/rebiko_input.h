#ifndef REBIKO_INPUT_H
#define REBIKO_INPUT_H

/*
 * ReBiko input: physical T-Deck inputs, chords, bindings, and the router
 * that sends each input to the Cybiko, an emulator command, or the menu.
 *
 *   T-Deck keyboard (raw mode: 5 columns x 7 rows, press and release)
 *   and trackball  ->  rb_router_key / rb_router_ball
 *     -> chord: the input, alone or after the $ prefix
 *     -> binding lookup (user overrides, then the defaults)
 *     -> rb_output_t: a Cybiko key, a typed character, a command, or
 *        menu navigation while the menu is open
 *
 * Plain C with no Arduino calls, so tests/test_rebiko.c runs it on the PC.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Physical inputs. 0-34 are the keyboard matrix (column * 7 + row, as
 * the keyboard's raw mode reports them); then the trackball. */
enum {
    RB_PK_KEYS = 35,
    RB_PK_BALL_UP = RB_PK_KEYS, RB_PK_BALL_DOWN, RB_PK_BALL_LEFT,
    RB_PK_BALL_RIGHT, RB_PK_BALL_CLICK, RB_PK_BALL_HOLD,
    RB_PK_COUNT
};
#define RB_PK(col, row) ((col) * 7 + (row))
#define RB_PK_SYM    RB_PK(0, 2)
#define RB_PK_ALT    RB_PK(0, 4)
#define RB_PK_SPACE  RB_PK(0, 5)
#define RB_PK_MIC    RB_PK(0, 6)
#define RB_PK_LSHIFT RB_PK(1, 6)
#define RB_PK_RSHIFT RB_PK(2, 3)
#define RB_PK_ENTER  RB_PK(3, 3)
#define RB_PK_BKSP   RB_PK(4, 3)
#define RB_PK_DOLLAR RB_PK(4, 4)

const char *rb_phys_name(int phys);
/* Character the key types: base layer, or with Sym held. 0 for none. */
char rb_phys_char(int phys, bool sym);
/* Physical key whose base character is c, or -1 */
int rb_phys_for_char(char c);

/* Cybiko keys an input can press. Letters, digits and the punctuation
 * the T-Deck types directly go through RB_ACT_TYPE instead. */
typedef enum {
    RB_CK_ESC, RB_CK_ENTER, RB_CK_SPACE, RB_CK_BKSP, RB_CK_DEL,
    RB_CK_INSERT, RB_CK_SELECT, RB_CK_HELP, RB_CK_TAB,
    RB_CK_F1, RB_CK_F2, RB_CK_F3, RB_CK_F4, RB_CK_F5, RB_CK_F6, RB_CK_F7,
    RB_CK_UP, RB_CK_DOWN, RB_CK_LEFT, RB_CK_RIGHT,
    RB_CK_SHIFT, RB_CK_FN,
    RB_CK_LBRACKET, RB_CK_RBRACKET, RB_CK_BACKSLASH, RB_CK_BACKTICK,
    RB_CK_EQUALS, RB_CK_APOSTROPHE, RB_CK_SEMICOLON, RB_CK_SLASH,
    RB_CK_COUNT
} rb_cybiko_key_t;

typedef enum {
    RB_CMD_MENU, RB_CMD_VOL_UP, RB_CMD_VOL_DOWN, RB_CMD_KEYREF,
    RB_CMD_BACKLIGHT, RB_CMD_LINK_VIEW,
    RB_CMD_COUNT
} rb_command_t;

typedef enum { RB_ACT_NONE, RB_ACT_TYPE, RB_ACT_KEY, RB_ACT_CMD } rb_act_kind_t;

typedef struct { uint8_t kind, id; } rb_action_t;

#define RB_MOD_NONE   0
#define RB_MOD_DOLLAR 1
typedef struct { uint8_t phys, mod; } rb_chord_t;

typedef struct { rb_chord_t chord; rb_action_t action; } rb_binding_t;

#define RB_MAX_OVERRIDES 64
typedef struct {
    uint8_t count;
    rb_binding_t ov[RB_MAX_OVERRIDES];
} rb_bindings_t;

const char *rb_cybiko_key_name(int ck);
const char *rb_command_name(int cmd);
/* "Esc", "Volume up", "Types keys", "-" */
const char *rb_action_name(rb_action_t a);
/* "$+Q", "Ball click", "Enter" */
void rb_chord_name(rb_chord_t c, char *buf, size_t n);
bool rb_action_eq(rb_action_t a, rb_action_t b);
bool rb_chord_eq(rb_chord_t a, rb_chord_t b);

/* $ + Mic opens the menu. It cannot be rebound, and nothing else can be
 * bound to it, so the menu is always reachable. */
#define RB_MENU_CHORD ((rb_chord_t){ RB_PK_MIC, RB_MOD_DOLLAR })

/* $, Sym and the ball hold (which cancels capture) are never bindable */
bool rb_chord_reserved(rb_chord_t c);
/* Whether the chord can be bound at all (in range, not reserved) */
bool rb_chord_bindable(rb_chord_t c);

rb_action_t rb_default_action(rb_chord_t c);
rb_action_t rb_lookup(const rb_bindings_t *b, rb_chord_t c);

/* Every chord that resolves to the action. Returns the count. */
int rb_inputs_for(const rb_bindings_t *b, rb_action_t a, rb_chord_t *out, int max);
/* Bind chord to action. False if not bindable or the table is full. */
bool rb_bind(rb_bindings_t *b, rb_chord_t c, rb_action_t a);
/* Change: the action's only input becomes c */
bool rb_change(rb_bindings_t *b, rb_action_t a, rb_chord_t c);
/* Drop the user's own inputs for the action; its defaults stay as they are */
void rb_clear_custom(rb_bindings_t *b, rb_action_t a);
/* The action's inputs go back to exactly its defaults */
void rb_restore_default(rb_bindings_t *b, rb_action_t a);
void rb_restore_all(rb_bindings_t *b);
/* Drop overrides that are out of range, reserved, duplicated, or name
 * an unknown action. Returns how many were dropped. */
int rb_bindings_sanitize(rb_bindings_t *b);

/* Bindable targets shown in Controls, in menu order */
int rb_target_count(void);
rb_action_t rb_target(int i);

/* ---- Router ---- */

typedef enum { RB_NAV_UP, RB_NAV_DOWN, RB_NAV_LEFT, RB_NAV_RIGHT,
               RB_NAV_SELECT, RB_NAV_BACK } rb_nav_t;

typedef enum { RB_MODE_EMU, RB_MODE_MENU, RB_MODE_CAPTURE } rb_mode_t;

typedef struct {
    void *ctx;
    /* A Cybiko key goes down. phys is the input that pressed it (-1 from
     * the serial port); the key may stay down while rb_router_held(phys). */
    void (*cybiko_key)(void *ctx, int ck, int phys);
    /* A character typed as the T-Deck types it (phys as above) */
    void (*type_char)(void *ctx, char c, int phys);
    void (*command)(void *ctx, int cmd);
    void (*menu_nav)(void *ctx, rb_nav_t nav);
    void (*capture)(void *ctx, rb_chord_t c);
    /* The $ prefix is waiting for its key */
    void (*dollar)(void *ctx, bool pending);
} rb_output_t;

typedef struct {
    const rb_bindings_t *bindings;
    rb_output_t out;
    rb_mode_t mode;
    bool dollar;
    uint64_t down;   /* bit per physical input currently held */
} rb_router_t;

void rb_router_init(rb_router_t *r, const rb_bindings_t *b, const rb_output_t *out);
void rb_router_set_mode(rb_router_t *r, rb_mode_t m);
/* Keyboard key press or release (raw mode) */
void rb_router_key(rb_router_t *r, int phys, bool down);
/* Trackball: a step (UP..RIGHT, down only), a short click (on release,
 * down only), or the hold (down when it starts, up when released). */
void rb_router_ball(rb_router_t *r, int phys, bool down);
/* An ASCII character from the USB serial port, as the old key mode sent
 * them: for scripted tests */
void rb_router_char(rb_router_t *r, char c);
bool rb_router_held(const rb_router_t *r, int phys);

#ifdef __cplusplus
}
#endif

#endif
