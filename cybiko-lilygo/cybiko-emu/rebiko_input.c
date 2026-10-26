#include "rebiko_input.h"

#include <stdio.h>
#include <string.h>

/* The keyboard controller's own tables (LilyGO Keyboard_ESP32C3): base
 * layer and Sym layer, [column][row]. 0 = no character (modifiers, Enter,
 * Backspace, Mic). Read from the keyboard on 26 Oct 2026: every key
 * reports its own bit, Mic included. */
static const char BASE[5][7] = {
    { 'q', 'w', 0,   'a', 0,   ' ', 0   },
    { 'e', 's', 'd', 'p', 'x', 'z', 0   },
    { 'r', 'g', 't', 0,   'v', 'c', 'f' },
    { 'u', 'h', 'y', 0,   'b', 'n', 'j' },
    { 'o', 'l', 'i', 0,   '$', 'm', 'k' },
};
static const char SYM[5][7] = {
    { '#', '1', 0,   '*', 0,   0,   '0'  },
    { '2', '4', '5', '@', '8', '7', 0    },
    { '3', '/', '(', 0,   '?', '9', '6'  },
    { '_', ':', ')', 0,   '!', ',', ';'  },
    { '+', '"', '-', 0,   0,   '.', '\'' },
};

const char *rb_phys_name(int phys) {
    static const char *const BALL[] = {
        "Ball up", "Ball down", "Ball left", "Ball right", "Ball click", "Ball hold"
    };
    static char letter[2][2];
    static int which;
    switch (phys) {
    case RB_PK_SYM:    return "Sym";
    case RB_PK_ALT:    return "Alt";
    case RB_PK_SPACE:  return "Space";
    case RB_PK_MIC:    return "Mic";
    case RB_PK_LSHIFT: return "L Shift";
    case RB_PK_RSHIFT: return "R Shift";
    case RB_PK_ENTER:  return "Enter";
    case RB_PK_BKSP:   return "Backspace";
    case RB_PK_DOLLAR: return "$";
    }
    if (phys >= RB_PK_BALL_UP && phys < RB_PK_COUNT)
        return BALL[phys - RB_PK_BALL_UP];
    if (phys >= 0 && phys < RB_PK_KEYS) {
        char c = BASE[phys / 7][phys % 7];
        if (c >= 'a' && c <= 'z') {
            which ^= 1;
            letter[which][0] = (char)(c - 'a' + 'A');
            letter[which][1] = 0;
            return letter[which];
        }
    }
    return "?";
}

char rb_phys_char(int phys, bool sym) {
    if (phys < 0 || phys >= RB_PK_KEYS) return 0;
    return sym ? SYM[phys / 7][phys % 7] : BASE[phys / 7][phys % 7];
}

int rb_phys_for_char(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == '\r' || c == '\n') return RB_PK_ENTER;
    if (c == '\b') return RB_PK_BKSP;
    for (int i = 0; i < RB_PK_KEYS; i++)
        if (BASE[i / 7][i % 7] == c) return i;
    return -1;
}

const char *rb_cybiko_key_name(int ck) {
    static const char *const N[RB_CK_COUNT] = {
        "Esc", "Enter", "Space", "BkSp", "Del", "Insert", "Select", "Help", "Tab",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7",
        "Up", "Down", "Left", "Right", "Shift", "Fn",
        "[", "]", "\\", "`", "=", "'", ";", "/",
    };
    return ck >= 0 && ck < RB_CK_COUNT ? N[ck] : "?";
}

const char *rb_command_name(int cmd) {
    static const char *const N[RB_CMD_COUNT] = {
        "Menu", "Volume up", "Volume down", "Key reference",
        "Backlight step", "Link view",
    };
    return cmd >= 0 && cmd < RB_CMD_COUNT ? N[cmd] : "?";
}

const char *rb_action_name(rb_action_t a) {
    switch (a.kind) {
    case RB_ACT_TYPE: return "Types its key";
    case RB_ACT_KEY:  return rb_cybiko_key_name(a.id);
    case RB_ACT_CMD:  return rb_command_name(a.id);
    }
    return "-";
}

void rb_chord_name(rb_chord_t c, char *buf, size_t n) {
    snprintf(buf, n, "%s%s", c.mod == RB_MOD_DOLLAR ? "$+" : "", rb_phys_name(c.phys));
}

bool rb_action_eq(rb_action_t a, rb_action_t b) {
    if (a.kind != b.kind) return false;
    return a.kind == RB_ACT_NONE || a.kind == RB_ACT_TYPE || a.id == b.id;
}

bool rb_chord_eq(rb_chord_t a, rb_chord_t b) {
    return a.phys == b.phys && a.mod == b.mod;
}

bool rb_chord_reserved(rb_chord_t c) {
    return c.phys == RB_PK_DOLLAR || c.phys == RB_PK_SYM || c.phys == RB_PK_BALL_HOLD
        || rb_chord_eq(c, RB_MENU_CHORD);
}

bool rb_chord_bindable(rb_chord_t c) {
    return c.phys < RB_PK_COUNT && c.mod <= RB_MOD_DOLLAR && !rb_chord_reserved(c);
}

#define KEY(k) ((rb_action_t){ RB_ACT_KEY, (k) })
#define CMD(k) ((rb_action_t){ RB_ACT_CMD, (k) })
#define NONE   ((rb_action_t){ RB_ACT_NONE, 0 })
#define TYPE   ((rb_action_t){ RB_ACT_TYPE, 0 })

/* $ + key. Esc stays on $+X, where it has always been. */
static const struct { char key; rb_action_t act; } DOLLAR_DEFAULTS[] = {
    { 'q', KEY(RB_CK_F1) }, { 'w', KEY(RB_CK_F2) }, { 'e', KEY(RB_CK_F3) },
    { 'r', KEY(RB_CK_F4) }, { 't', KEY(RB_CK_F5) }, { 'y', KEY(RB_CK_F6) },
    { 'u', KEY(RB_CK_F7) },
    { 'a', KEY(RB_CK_SELECT) }, { 's', KEY(RB_CK_INSERT) },
    { 'f', KEY(RB_CK_FN) }, { 'g', KEY(RB_CK_HELP) }, { 'h', KEY(RB_CK_TAB) },
    { 'j', KEY(RB_CK_LBRACKET) }, { 'k', KEY(RB_CK_RBRACKET) },
    { 'l', KEY(RB_CK_BACKSLASH) }, { 'z', KEY(RB_CK_BACKTICK) },
    { 'c', KEY(RB_CK_EQUALS) }, { 'v', KEY(RB_CK_APOSTROPHE) },
    { 'b', KEY(RB_CK_SEMICOLON) }, { 'n', KEY(RB_CK_SLASH) },
    { 'p', CMD(RB_CMD_VOL_UP) }, { 'm', CMD(RB_CMD_VOL_DOWN) },
    { 'x', KEY(RB_CK_ESC) }, { ' ', CMD(RB_CMD_KEYREF) },
};

rb_action_t rb_default_action(rb_chord_t c) {
    if (c.phys >= RB_PK_COUNT || c.mod > RB_MOD_DOLLAR) return NONE;
    if (c.mod == RB_MOD_DOLLAR) {
        if (c.phys == RB_PK_MIC) return CMD(RB_CMD_MENU);
        if (c.phys == RB_PK_BKSP) return KEY(RB_CK_DEL);
        char ch = rb_phys_char(c.phys, false);
        for (size_t i = 0; ch && i < sizeof DOLLAR_DEFAULTS / sizeof DOLLAR_DEFAULTS[0]; i++)
            if (DOLLAR_DEFAULTS[i].key == ch) return DOLLAR_DEFAULTS[i].act;
        return NONE;
    }
    switch (c.phys) {
    case RB_PK_BALL_UP:    return KEY(RB_CK_UP);
    case RB_PK_BALL_DOWN:  return KEY(RB_CK_DOWN);
    case RB_PK_BALL_LEFT:  return KEY(RB_CK_LEFT);
    case RB_PK_BALL_RIGHT: return KEY(RB_CK_RIGHT);
    case RB_PK_BALL_CLICK: return KEY(RB_CK_ENTER);
    case RB_PK_BALL_HOLD:  return KEY(RB_CK_ESC);
    case RB_PK_ENTER:      return KEY(RB_CK_ENTER);
    case RB_PK_BKSP:       return KEY(RB_CK_BKSP);
    case RB_PK_SPACE:      return KEY(RB_CK_SPACE);
    case RB_PK_LSHIFT:     return KEY(RB_CK_SHIFT);
    case RB_PK_RSHIFT:     return KEY(RB_CK_FN);
    case RB_PK_SYM: case RB_PK_ALT: case RB_PK_MIC: case RB_PK_DOLLAR:
        return NONE;
    }
    return rb_phys_char(c.phys, false) ? TYPE : NONE;
}

static int find_override(const rb_bindings_t *b, rb_chord_t c) {
    for (int i = 0; i < b->count; i++)
        if (rb_chord_eq(b->ov[i].chord, c)) return i;
    return -1;
}

static void remove_override(rb_bindings_t *b, int i) {
    b->ov[i] = b->ov[b->count - 1];
    b->count--;
}

rb_action_t rb_lookup(const rb_bindings_t *b, rb_chord_t c) {
    if (b && !rb_chord_reserved(c)) {
        int i = find_override(b, c);
        if (i >= 0) return b->ov[i].action;
    }
    return rb_default_action(c);
}

int rb_inputs_for(const rb_bindings_t *b, rb_action_t a, rb_chord_t *out, int max) {
    int n = 0;
    for (int mod = RB_MOD_NONE; mod <= RB_MOD_DOLLAR; mod++)
        for (int p = 0; p < RB_PK_COUNT; p++) {
            rb_chord_t c = { (uint8_t)p, (uint8_t)mod };
            if (rb_action_eq(rb_lookup(b, c), a)) {
                if (n < max) out[n] = c;
                n++;
            }
        }
    return n < max ? n : max;
}

bool rb_bind(rb_bindings_t *b, rb_chord_t c, rb_action_t a) {
    if (!rb_chord_bindable(c)) return false;
    int i = find_override(b, c);
    if (rb_action_eq(a, rb_default_action(c))) {
        if (i >= 0) remove_override(b, i);
        return true;
    }
    if (i < 0) {
        if (b->count >= RB_MAX_OVERRIDES) return false;
        i = b->count++;
        b->ov[i].chord = c;
    }
    b->ov[i].action = a;
    return true;
}

bool rb_change(rb_bindings_t *b, rb_action_t a, rb_chord_t c) {
    if (!rb_chord_bindable(c)) return false;
    rb_chord_t old[2 * RB_PK_COUNT];
    int n = rb_inputs_for(b, a, old, 2 * RB_PK_COUNT);
    rb_bindings_t save = *b;
    for (int i = 0; i < n; i++)
        if (rb_chord_bindable(old[i]))
            rb_bind(b, old[i], NONE);
    if (!rb_bind(b, c, a)) {
        *b = save;
        return false;
    }
    return true;
}

void rb_clear_custom(rb_bindings_t *b, rb_action_t a) {
    for (int i = 0; i < b->count; )
        if (rb_action_eq(b->ov[i].action, a)) remove_override(b, i);
        else i++;
}

void rb_restore_default(rb_bindings_t *b, rb_action_t a) {
    rb_clear_custom(b, a);
    for (int i = 0; i < b->count; )
        if (rb_action_eq(rb_default_action(b->ov[i].chord), a)) remove_override(b, i);
        else i++;
}

void rb_restore_all(rb_bindings_t *b) {
    b->count = 0;
}

static bool action_valid(rb_action_t a) {
    switch (a.kind) {
    case RB_ACT_NONE: case RB_ACT_TYPE: return true;
    case RB_ACT_KEY: return a.id < RB_CK_COUNT;
    case RB_ACT_CMD: return a.id < RB_CMD_COUNT && a.id != RB_CMD_MENU;
    }
    return false;
}

int rb_bindings_sanitize(rb_bindings_t *b) {
    int dropped = 0;
    if (b->count > RB_MAX_OVERRIDES) {
        dropped += b->count - RB_MAX_OVERRIDES;
        b->count = RB_MAX_OVERRIDES;
    }
    for (int i = 0; i < b->count; ) {
        bool bad = !rb_chord_bindable(b->ov[i].chord) || !action_valid(b->ov[i].action)
                || rb_action_eq(b->ov[i].action, rb_default_action(b->ov[i].chord));
        for (int j = 0; j < i && !bad; j++)
            bad = rb_chord_eq(b->ov[j].chord, b->ov[i].chord);
        if (bad) {
            /* keep order: later duplicates are the ones dropped */
            memmove(&b->ov[i], &b->ov[i + 1], (size_t)(b->count - i - 1) * sizeof b->ov[0]);
            b->count--;
            dropped++;
        } else {
            i++;
        }
    }
    return dropped;
}

static const rb_action_t TARGETS[] = {
    KEY(RB_CK_ESC), KEY(RB_CK_ENTER), KEY(RB_CK_BKSP), KEY(RB_CK_DEL),
    KEY(RB_CK_SPACE), KEY(RB_CK_SELECT), KEY(RB_CK_INSERT), KEY(RB_CK_HELP),
    KEY(RB_CK_TAB), KEY(RB_CK_SHIFT), KEY(RB_CK_FN),
    KEY(RB_CK_F1), KEY(RB_CK_F2), KEY(RB_CK_F3), KEY(RB_CK_F4),
    KEY(RB_CK_F5), KEY(RB_CK_F6), KEY(RB_CK_F7),
    KEY(RB_CK_UP), KEY(RB_CK_DOWN), KEY(RB_CK_LEFT), KEY(RB_CK_RIGHT),
    KEY(RB_CK_LBRACKET), KEY(RB_CK_RBRACKET), KEY(RB_CK_BACKSLASH),
    KEY(RB_CK_BACKTICK), KEY(RB_CK_EQUALS), KEY(RB_CK_APOSTROPHE),
    KEY(RB_CK_SEMICOLON), KEY(RB_CK_SLASH),
    CMD(RB_CMD_VOL_UP), CMD(RB_CMD_VOL_DOWN), CMD(RB_CMD_KEYREF),
    CMD(RB_CMD_BACKLIGHT), CMD(RB_CMD_LINK_VIEW),
};

int rb_target_count(void) { return (int)(sizeof TARGETS / sizeof TARGETS[0]); }
rb_action_t rb_target(int i) { return TARGETS[i]; }

/* ---- Router ---- */

void rb_router_init(rb_router_t *r, const rb_bindings_t *b, const rb_output_t *out) {
    memset(r, 0, sizeof *r);
    r->bindings = b;
    r->out = *out;
}

static void set_dollar(rb_router_t *r, bool on) {
    if (r->dollar == on) return;
    r->dollar = on;
    if (r->out.dollar) r->out.dollar(r->out.ctx, on);
}

void rb_router_set_mode(rb_router_t *r, rb_mode_t m) {
    r->mode = m;
    set_dollar(r, false);
}

static void dispatch(rb_router_t *r, rb_chord_t c, int phys) {
    /* Sym held: the key types its Sym character whatever it is bound to
     * (Sym+Mic is 0, and Mic alone does nothing by default) */
    bool sym = (r->down >> RB_PK_SYM) & 1;
    char sc = c.mod == RB_MOD_NONE && sym ? rb_phys_char(c.phys, true) : 0;
    if (sc) {
        if (r->out.type_char) r->out.type_char(r->out.ctx, sc, phys);
        return;
    }
    rb_action_t a = rb_lookup(r->bindings, c);
    switch (a.kind) {
    case RB_ACT_TYPE: {
        char ch = rb_phys_char(c.phys, (r->down >> RB_PK_SYM) & 1);
        if (ch && r->out.type_char) r->out.type_char(r->out.ctx, ch, phys);
        break;
    }
    case RB_ACT_KEY:
        if (r->out.cybiko_key) r->out.cybiko_key(r->out.ctx, a.id, phys);
        break;
    case RB_ACT_CMD:
        if (r->out.command) r->out.command(r->out.ctx, a.id);
        break;
    }
}

/* One press: the $ prefix, the menu chord, then whichever mode we are in */
static void press(rb_router_t *r, int phys, int from) {
    if (phys == RB_PK_DOLLAR) {
        if (r->dollar) {   /* $ $ types a $ */
            set_dollar(r, false);
            if (r->mode == RB_MODE_EMU && r->out.type_char)
                r->out.type_char(r->out.ctx, '$', -1);
        } else {
            set_dollar(r, true);
        }
        return;
    }
    if (phys == RB_PK_SYM) return;
    rb_chord_t c = { (uint8_t)phys, (uint8_t)(r->dollar ? RB_MOD_DOLLAR : RB_MOD_NONE) };
    set_dollar(r, false);
    if (r->mode == RB_MODE_CAPTURE) {
        if (phys == RB_PK_BALL_HOLD) {
            if (r->out.menu_nav) r->out.menu_nav(r->out.ctx, RB_NAV_BACK);
        } else if (r->out.capture) {
            r->out.capture(r->out.ctx, c);
        }
        return;
    }
    if (rb_chord_eq(c, RB_MENU_CHORD)) {
        if (r->out.command) r->out.command(r->out.ctx, RB_CMD_MENU);
        return;
    }
    if (r->mode == RB_MODE_MENU) {
        static const struct { uint8_t phys; rb_nav_t nav; } NAV[] = {
            { RB_PK_BALL_UP, RB_NAV_UP }, { RB_PK_BALL_DOWN, RB_NAV_DOWN },
            { RB_PK_BALL_LEFT, RB_NAV_LEFT }, { RB_PK_BALL_RIGHT, RB_NAV_RIGHT },
            { RB_PK_BALL_CLICK, RB_NAV_SELECT }, { RB_PK_BALL_HOLD, RB_NAV_BACK },
            { RB_PK_ENTER, RB_NAV_SELECT }, { RB_PK_BKSP, RB_NAV_BACK },
        };
        for (size_t i = 0; i < sizeof NAV / sizeof NAV[0]; i++)
            if (NAV[i].phys == phys && r->out.menu_nav)
                r->out.menu_nav(r->out.ctx, NAV[i].nav);
        return;
    }
    dispatch(r, c, from);
}

void rb_router_key(rb_router_t *r, int phys, bool down) {
    if (phys < 0 || phys >= RB_PK_KEYS) return;
    if (down) r->down |= 1ull << phys;
    else r->down &= ~(1ull << phys);
    if (down) press(r, phys, phys);
}

void rb_router_ball(rb_router_t *r, int phys, bool down) {
    if (phys < RB_PK_BALL_UP || phys >= RB_PK_COUNT) return;
    if (phys == RB_PK_BALL_HOLD) {
        if (down) r->down |= 1ull << phys;
        else r->down &= ~(1ull << phys);
    }
    if (down) press(r, phys, phys);
}

void rb_router_char(rb_router_t *r, char c) {
    int phys = rb_phys_for_char(c);
    if (c == '$' || r->dollar || r->mode != RB_MODE_EMU
        || c == '\r' || c == '\n' || c == '\b' || c == ' ') {
        if (phys < 0) {
            set_dollar(r, false);
            return;
        }
        if (phys == RB_PK_DOLLAR || r->dollar || r->mode != RB_MODE_EMU) {
            rb_router_key(r, phys, true);
            rb_router_key(r, phys, false);
        } else {
            press(r, phys, -1);   /* Enter, Backspace, Space: a tap */
        }
        return;
    }
    if (r->out.type_char) r->out.type_char(r->out.ctx, c, -1);
}

bool rb_router_held(const rb_router_t *r, int phys) {
    return phys >= 0 && phys < RB_PK_COUNT && ((r->down >> phys) & 1);
}
