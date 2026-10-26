#include "rebiko_menu.h"

#include <stdio.h>
#include <string.h>

enum { OP_NONE, OP_RESTORE_KEYS, OP_RESTORE_ALL, OP_RESTART, OP_ASSIGN, OP_RESTORE_TARGET,
       OP_SWITCH };

enum { MAIN_CONTROLS, MAIN_PERF, MAIN_DISPLAY, MAIN_AUDIO, MAIN_SYSTEM, MAIN_RETURN, MAIN_ROWS };
enum { TGT_INPUTS, TGT_CHANGE, TGT_ADD, TGT_CLEAR, TGT_RESTORE, TGT_ROWS };
enum { DISP_LIGHT, DISP_SCALE, DISP_SLEEP, DISP_ROWS };
enum { AUD_SOUND, AUD_VOLUME, AUD_ROWS };
enum { SYS_VERSION, SYS_MACHINE, SYS_INFO, SYS_KEYREF, SYS_LINK, SYS_DEFAULTS, SYS_RESTART,
       SYS_RETURN, SYS_ROWS };

#define WANT(buf, n, ...) do { if (buf && n) snprintf(buf, n, __VA_ARGS__); } while (0)

static rb_menu_level_t *top_level(rb_menu_t *m) { return &m->stack[m->depth - 1]; }

void rb_menu_init(rb_menu_t *m, const rb_menu_hooks_t *hooks) {
    memset(m, 0, sizeof *m);
    m->hooks = *hooks;
    rb_config_defaults(&m->cfg);
}

static void push(rb_menu_t *m, rb_screen_t s) {
    if (m->depth >= RB_MENU_DEPTH) return;
    m->stack[m->depth].screen = (uint8_t)s;
    m->stack[m->depth].cursor = 0;
    m->stack[m->depth].top = 0;
    m->depth++;
    /* land on the first row that can be chosen */
    int rows = rb_menu_rows(m);
    for (int i = 0; i < rows; i++)
        if (rb_menu_row(m, i, NULL, 0, NULL, 0)) {
            top_level(m)->cursor = (uint8_t)i;
            break;
        }
}

void rb_menu_open(rb_menu_t *m, const rb_config_t *cfg, rb_screen_t first) {
    m->cfg = *cfg;
    m->depth = 0;
    push(m, first);   /* Back from a shortcut screen returns to emulation */
}

bool rb_menu_is_open(const rb_menu_t *m) { return m->depth > 0; }

rb_screen_t rb_menu_screen(const rb_menu_t *m) {
    return m->depth ? (rb_screen_t)m->stack[m->depth - 1].screen : RB_SCR_MAIN;
}

rb_mode_t rb_menu_mode(const rb_menu_t *m) {
    if (!m->depth) return RB_MODE_EMU;
    return rb_menu_screen(m) == RB_SCR_CAPTURE ? RB_MODE_CAPTURE : RB_MODE_MENU;
}

static rb_action_t cur_target(const rb_menu_t *m) { return rb_target(m->target); }

void rb_inputs_text(const rb_bindings_t *b, rb_action_t a, char *buf, size_t n) {
    rb_chord_t in[2 * RB_PK_COUNT];
    int k = rb_inputs_for(b, a, in, 2 * RB_PK_COUNT);
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < k && len + 1 < n; i++) {
        char one[24];
        rb_chord_name(in[i], one, sizeof one);
        len += (size_t)snprintf(buf + len, n - len, "%s%s", i ? ", " : "", one);
    }
    if (len >= n && n > 4) memcpy(buf + n - 4, "...", 4);
    if (!k) snprintf(buf, n, "none");
}

/* Key reference: every input that does something other than type */
static int keyref_row(const rb_menu_t *m, int want, rb_chord_t *out) {
    int n = 0;
    for (int mod = RB_MOD_NONE; mod <= RB_MOD_DOLLAR; mod++)
        for (int p = 0; p < RB_PK_COUNT; p++) {
            rb_chord_t c = { (uint8_t)p, (uint8_t)mod };
            rb_action_t a = rb_lookup(&m->cfg.keys, c);
            if (a.kind != RB_ACT_KEY && a.kind != RB_ACT_CMD) continue;
            if (n == want && out) *out = c;
            n++;
        }
    return n;
}

int rb_menu_rows(const rb_menu_t *m) {
    switch (rb_menu_screen(m)) {
    case RB_SCR_MAIN:     return MAIN_ROWS;
    case RB_SCR_CONTROLS: return 2 + rb_target_count();
    case RB_SCR_TARGET:   return TGT_ROWS;
    case RB_SCR_CAPTURE:  return 0;
    case RB_SCR_CONFIRM:  return 2;
    case RB_SCR_MESSAGE:  return 1;
    case RB_SCR_PERF:     return 1;
    case RB_SCR_DISPLAY:  return DISP_ROWS;
    case RB_SCR_AUDIO:    return AUD_ROWS;
    case RB_SCR_SYSTEM:   return SYS_ROWS;
    case RB_SCR_INFO:
        return m->hooks.info_count ? m->hooks.info_count(m->hooks.ctx) : 0;
    case RB_SCR_KEYREF:   return keyref_row(m, -1, NULL);
    }
    return 0;
}

int rb_menu_cursor(const rb_menu_t *m) { return m->depth ? m->stack[m->depth - 1].cursor : 0; }
int rb_menu_top(const rb_menu_t *m) { return m->depth ? m->stack[m->depth - 1].top : 0; }

const char *rb_menu_title(const rb_menu_t *m) {
    static const char *const T[] = {
        [RB_SCR_MAIN] = "Settings", [RB_SCR_CONTROLS] = "Controls & Key Mapping",
        [RB_SCR_TARGET] = "", [RB_SCR_CAPTURE] = "Press new input",
        [RB_SCR_CONFIRM] = "Confirm", [RB_SCR_MESSAGE] = "",
        [RB_SCR_PERF] = "Performance & CPU", [RB_SCR_DISPLAY] = "Display",
        [RB_SCR_AUDIO] = "Audio", [RB_SCR_SYSTEM] = "System",
        [RB_SCR_INFO] = "Hardware info", [RB_SCR_KEYREF] = "Key reference",
    };
    rb_screen_t s = rb_menu_screen(m);
    if (s == RB_SCR_TARGET) return rb_action_name(cur_target(m));
    return T[s];
}

static const char *on_off(int v) { return v ? "On" : "Off"; }

bool rb_menu_row(const rb_menu_t *m, int i, char *label, size_t ln, char *value, size_t vn) {
    if (value && vn) value[0] = 0;
    if (label && ln) label[0] = 0;
    const rb_config_t *c = &m->cfg;
    switch (rb_menu_screen(m)) {
    case RB_SCR_MAIN: {
        static const char *const L[MAIN_ROWS] = {
            "Controls & Key Mapping", "Performance & CPU", "Display", "Audio",
            "System", "Return to emulation" };
        if (i < 0 || i >= MAIN_ROWS) return false;
        WANT(label, ln, "%s", L[i]);
        if (i != MAIN_RETURN) WANT(value, vn, ">");
        return true;
    }
    case RB_SCR_CONTROLS:
        if (i == 0) {
            WANT(label, ln, "Restore all key mappings");
            return true;
        }
        if (i == 1 + rb_target_count()) {
            WANT(label, ln, "Menu");
            WANT(value, vn, "$+Mic (fixed)");
            return false;
        }
        if (i < 1 || i > rb_target_count()) return false;
        WANT(label, ln, "%s", rb_action_name(rb_target(i - 1)));
        if (value && vn) rb_inputs_text(&c->keys, rb_target(i - 1), value, vn);
        return true;
    case RB_SCR_TARGET: {
        static const char *const L[TGT_ROWS] = {
            "Now", "Change mapping", "Add another input", "Clear custom mapping",
            "Restore default" };
        if (i < 0 || i >= TGT_ROWS) return false;
        WANT(label, ln, "%s", L[i]);
        if (i == TGT_INPUTS) {
            if (value && vn) rb_inputs_text(&c->keys, cur_target(m), value, vn);
            return false;
        }
        return true;
    }
    case RB_SCR_CONFIRM:
        if (i < 0 || i > 1) return false;
        WANT(label, ln, "%s", i == 0 ? "Confirm" : "Cancel");
        return true;
    case RB_SCR_MESSAGE:
        if (i != 0) return false;
        WANT(label, ln, "OK");
        return true;
    case RB_SCR_PERF:
        if (i != 0) return false;
        WANT(label, ln, "CPU mode");
        WANT(value, vn, "%s", c->cpu_mode == RB_CPU_HIGH ? "< Always high >"
                             : c->cpu_mode == RB_CPU_LOW ? "< Always low >" : "< Dynamic >");
        return true;
    case RB_SCR_DISPLAY:
        switch (i) {
        case DISP_LIGHT:
            WANT(label, ln, "Brightness");
            WANT(value, vn, "< %d/%d >", RB_LIGHT_LEVELS - c->light, RB_LIGHT_LEVELS);
            return true;
        case DISP_SCALE:
            WANT(label, ln, "Scale");
            WANT(value, vn, "%s", c->scale == RB_SCALE_1X ? "< 1x original >" : "< 2x >");
            return true;
        case DISP_SLEEP:
            WANT(label, ln, "Screen off when CyOS sleeps");
            WANT(value, vn, "< %s >", on_off(c->screen_sleep));
            return true;
        }
        return false;
    case RB_SCR_AUDIO:
        switch (i) {
        case AUD_SOUND:
            WANT(label, ln, "Sound");
            WANT(value, vn, "< %s >", on_off(c->audio_on));
            return true;
        case AUD_VOLUME:
            WANT(label, ln, "Volume");
            WANT(value, vn, "< %d >", c->volume);
            return true;
        }
        return false;
    case RB_SCR_SYSTEM:
        switch (i) {
        case SYS_VERSION:
            WANT(label, ln, "Version");
            WANT(value, vn, "%s", m->hooks.version ? m->hooks.version : "?");
            return false;
        case SYS_MACHINE:
            WANT(label, ln, "Machine");
            WANT(value, vn, "%s >", m->hooks.machine ? m->hooks.machine : "?");
            return m->hooks.switch_machine != NULL;
        case SYS_INFO:     WANT(label, ln, "Hardware info"); WANT(value, vn, ">"); return true;
        case SYS_KEYREF:   WANT(label, ln, "Key reference"); WANT(value, vn, ">"); return true;
        case SYS_LINK:
            WANT(label, ln, "Radio link view");
            WANT(value, vn, "< %s >",
                 on_off(m->hooks.link_view && m->hooks.link_view(m->hooks.ctx)));
            return true;
        case SYS_DEFAULTS: WANT(label, ln, "Restore all defaults"); return true;
        case SYS_RESTART:  WANT(label, ln, "Restart ReBiko"); return true;
        case SYS_RETURN:   WANT(label, ln, "Return to emulation"); return true;
        }
        return false;
    case RB_SCR_INFO:
        if (i < 0 || i >= rb_menu_rows(m)) return false;
        if (m->hooks.info_row) {
            char l[48] = "", v[48] = "";
            m->hooks.info_row(m->hooks.ctx, i, l, sizeof l, v, sizeof v);
            WANT(label, ln, "%s", l);
            WANT(value, vn, "%s", v);
        }
        return false;
    case RB_SCR_KEYREF: {
        rb_chord_t ch;
        if (i < 0 || i >= keyref_row(m, i, &ch)) return false;
        if (label && ln) rb_chord_name(ch, label, ln);
        WANT(value, vn, "%s", rb_action_name(rb_lookup(&m->cfg.keys, ch)));
        return false;
    }
    case RB_SCR_CAPTURE:
        return false;
    }
    return false;
}

const char *rb_menu_line(const rb_menu_t *m, int i) {
    rb_screen_t s = rb_menu_screen(m);
    if (i < 0 || i > 2) return "";
    if (s == RB_SCR_CAPTURE || s == RB_SCR_CONFIRM || s == RB_SCR_MESSAGE)
        return m->lines[i];
    return "";
}

const char *rb_menu_hint(const rb_menu_t *m) {
    switch (rb_menu_screen(m)) {
    case RB_SCR_CAPTURE: return "Hold ball: cancel";
    case RB_SCR_PERF:
        if (m->cfg.cpu_mode == RB_CPU_HIGH) return "240 MHz always";
        if (m->cfg.cpu_mode == RB_CPU_LOW) return "160 MHz always: saves battery, may lag";
        return "240 MHz; 160 MHz while CyOS sleeps";
    case RB_SCR_INFO: case RB_SCR_KEYREF:
        return "Ball: scroll   Hold: back";
    default: break;
    }
    char v[48];
    if (rb_menu_row(m, rb_menu_cursor(m), NULL, 0, v, sizeof v) && v[0] == '<')
        return "Ball left/right: change   Hold: back";
    return "Ball: move   Click: select   Hold: back";
}

/* ---- actions ---- */

static void changed(rb_menu_t *m) {
    if (m->hooks.apply) m->hooks.apply(m->hooks.ctx, &m->cfg);
}

static int wrap(int v, int dir, int n) { return ((v + dir) % n + n) % n; }

/* Left/right (dir -1/+1) or select (+1, wrapping) on a value row */
static bool adjust(rb_menu_t *m, int row, int dir) {
    rb_config_t *c = &m->cfg;
    switch (rb_menu_screen(m)) {
    case RB_SCR_PERF:
        c->cpu_mode = (uint8_t)wrap(c->cpu_mode, dir, RB_CPU_MODES);
        break;
    case RB_SCR_DISPLAY:
        if (row == DISP_LIGHT) c->light = (uint8_t)wrap(c->light, -dir, RB_LIGHT_LEVELS);
        else if (row == DISP_SCALE) c->scale = (uint8_t)wrap(c->scale, dir, RB_SCALES);
        else if (row == DISP_SLEEP) c->screen_sleep ^= 1;
        else return false;
        break;
    case RB_SCR_AUDIO:
        if (row == AUD_SOUND) c->audio_on ^= 1;
        else if (row == AUD_VOLUME) c->volume = (uint8_t)wrap(c->volume, dir, RB_VOLUME_MAX + 1);
        else return false;
        break;
    case RB_SCR_SYSTEM:
        if (row != SYS_LINK || !m->hooks.set_link_view) return false;
        m->hooks.set_link_view(m->hooks.ctx,
                               !(m->hooks.link_view && m->hooks.link_view(m->hooks.ctx)));
        return true;
    default:
        return false;
    }
    changed(m);
    return true;
}

static void set_lines(rb_menu_t *m, const char *a, const char *b, const char *c) {
    snprintf(m->lines[0], sizeof m->lines[0], "%s", a);
    snprintf(m->lines[1], sizeof m->lines[1], "%s", b);
    snprintf(m->lines[2], sizeof m->lines[2], "%s", c);
}

static void confirm(rb_menu_t *m, int op, const char *a, const char *b, const char *c) {
    m->confirm_op = (uint8_t)op;
    set_lines(m, a, b, c);
    push(m, RB_SCR_CONFIRM);
    top_level(m)->cursor = 1;   /* Cancel is the safe default */
}

static void message(rb_menu_t *m, const char *a, const char *b) {
    set_lines(m, a, b, "");
    push(m, RB_SCR_MESSAGE);
}

static void pop(rb_menu_t *m) {
    if (m->depth) m->depth--;
}

static void pop_to(rb_menu_t *m, rb_screen_t s) {
    while (m->depth > 1 && rb_menu_screen(m) != s) m->depth--;
}

static void restore_target(rb_menu_t *m) {
    rb_restore_default(&m->cfg.keys, cur_target(m));
    changed(m);
}

static void do_confirm(rb_menu_t *m) {
    switch (m->confirm_op) {
    case OP_RESTORE_KEYS:
        rb_restore_all(&m->cfg.keys);
        changed(m);
        pop(m);
        break;
    case OP_RESTORE_ALL:
        rb_config_defaults(&m->cfg);
        if (m->hooks.set_link_view) m->hooks.set_link_view(m->hooks.ctx, false);
        changed(m);
        pop(m);
        break;
    case OP_RESTART:
        if (m->hooks.restart) m->hooks.restart(m->hooks.ctx, &m->cfg);
        pop(m);
        break;
    case OP_ASSIGN: {
        bool ok = m->capture_adds ? rb_bind(&m->cfg.keys, m->captured, cur_target(m))
                                  : rb_change(&m->cfg.keys, cur_target(m), m->captured);
        changed(m);
        pop_to(m, RB_SCR_TARGET);
        if (!ok) message(m, "Too many custom mappings.", "Restore some to defaults first.");
        break;
    }
    case OP_RESTORE_TARGET:
        restore_target(m);
        pop(m);
        break;
    case OP_SWITCH: {
        char a[48];
        pop(m);
        /* returns only when the other model isn't installed */
        m->hooks.switch_machine(m->hooks.ctx, &m->cfg);
        snprintf(a, sizeof a, "The %s isn't installed.", m->hooks.other_machine);
        message(m, a, "tools/flash_both.sh puts both on.");
        break;
    }
    }
    m->confirm_op = OP_NONE;
}

static void start_capture(rb_menu_t *m, bool adds) {
    char t[48];
    m->capture_adds = adds;
    snprintf(t, sizeof t, "for %s", rb_action_name(cur_target(m)));
    set_lines(m, "Press a key, a $ combo,", "or move or click the ball", t);
    push(m, RB_SCR_CAPTURE);
}

/* Restoring a target's defaults takes those inputs back from whatever
 * they were given to since. Ask first if that is the case. */
static void ask_restore_target(rb_menu_t *m) {
    rb_action_t a = cur_target(m);
    for (int mod = RB_MOD_NONE; mod <= RB_MOD_DOLLAR; mod++)
        for (int p = 0; p < RB_PK_COUNT; p++) {
            rb_chord_t c = { (uint8_t)p, (uint8_t)mod };
            if (!rb_action_eq(rb_default_action(c), a)) continue;
            rb_action_t now = rb_lookup(&m->cfg.keys, c);
            if (now.kind == RB_ACT_NONE || rb_action_eq(now, a)) continue;
            char name[24], b[48];
            rb_chord_name(c, name, sizeof name);
            snprintf(b, sizeof b, "%s is now %s;", name, rb_action_name(now));
            confirm(m, OP_RESTORE_TARGET, "Restore the default?", b,
                    "that mapping will be removed.");
            return;
        }
    restore_target(m);
    message(m, "Default restored.", "");
}

static void select_row(rb_menu_t *m, int row) {
    switch (rb_menu_screen(m)) {
    case RB_SCR_MAIN: {
        static const rb_screen_t S[] = { RB_SCR_CONTROLS, RB_SCR_PERF, RB_SCR_DISPLAY,
                                         RB_SCR_AUDIO, RB_SCR_SYSTEM };
        if (row == MAIN_RETURN) m->depth = 0;
        else push(m, S[row]);
        break;
    }
    case RB_SCR_CONTROLS:
        if (row == 0) {
            confirm(m, OP_RESTORE_KEYS, "Restore all key mappings?",
                    "Every custom input is removed.", "");
        } else {
            m->target = row - 1;
            push(m, RB_SCR_TARGET);
        }
        break;
    case RB_SCR_TARGET:
        if (row == TGT_CHANGE) start_capture(m, false);
        else if (row == TGT_ADD) start_capture(m, true);
        else if (row == TGT_CLEAR) {
            rb_clear_custom(&m->cfg.keys, cur_target(m));
            changed(m);
            message(m, "Custom inputs removed.", "");
        } else if (row == TGT_RESTORE) {
            ask_restore_target(m);
        }
        break;
    case RB_SCR_CONFIRM:
        if (row == 0) do_confirm(m);
        else pop(m);
        break;
    case RB_SCR_MESSAGE:
        pop(m);
        break;
    case RB_SCR_SYSTEM:
        if (row == SYS_INFO) push(m, RB_SCR_INFO);
        else if (row == SYS_KEYREF) push(m, RB_SCR_KEYREF);
        else if (row == SYS_MACHINE && m->hooks.switch_machine) {
            char q[48];
            snprintf(q, sizeof q, "Switch to the %s?", m->hooks.other_machine);
            confirm(m, OP_SWITCH, q, "Settings and CyOS's data are", "saved, then ReBiko restarts.");
        }
        else if (row == SYS_DEFAULTS)
            confirm(m, OP_RESTORE_ALL, "Restore all defaults?",
                    "Keys, CPU, display and audio.", "CyOS files are kept.");
        else if (row == SYS_RESTART)
            confirm(m, OP_RESTART, "Restart ReBiko?", "Settings and CyOS's flash",
                    "are saved first.");
        else if (row == SYS_RETURN) m->depth = 0;
        else adjust(m, row, +1);
        break;
    default:
        adjust(m, row, +1);
        break;
    }
}

static void move(rb_menu_t *m, int dir) {
    rb_menu_level_t *l = top_level(m);
    int rows = rb_menu_rows(m);
    rb_screen_t s = rb_menu_screen(m);
    if (s == RB_SCR_INFO || s == RB_SCR_KEYREF) {
        /* read-only lists scroll a page of rows instead of a cursor */
        int top = l->top + dir;
        if (top > rows - RB_MENU_VISIBLE) top = rows - RB_MENU_VISIBLE;
        if (top < 0) top = 0;
        l->top = (uint8_t)top;
        return;
    }
    int i = l->cursor, next = -1;
    for (int j = i + dir; j >= 0 && j < rows; j += dir)
        if (rb_menu_row(m, j, NULL, 0, NULL, 0)) {
            next = j;
            break;
        }
    if (next < 0) {
        /* at the end: still scroll to show rows that can't be chosen */
        if (dir > 0 && rows > l->top + RB_MENU_VISIBLE) l->top++;
        else if (dir < 0 && l->top > 0 && l->top > i - RB_MENU_VISIBLE + 1) l->top--;
        return;
    }
    l->cursor = (uint8_t)next;
    if (next < l->top) l->top = (uint8_t)next;
    if (next >= l->top + RB_MENU_VISIBLE) l->top = (uint8_t)(next - RB_MENU_VISIBLE + 1);
}

int rb_menu_nav(rb_menu_t *m, rb_nav_t nav) {
    if (!m->depth) return 0;
    int row = rb_menu_cursor(m);
    switch (nav) {
    case RB_NAV_UP:    move(m, -1); break;
    case RB_NAV_DOWN:  move(m, +1); break;
    case RB_NAV_LEFT:  adjust(m, row, -1); break;
    case RB_NAV_RIGHT: adjust(m, row, +1); break;
    case RB_NAV_SELECT:
        if (rb_menu_rows(m) > 0 && rb_menu_row(m, row, NULL, 0, NULL, 0))
            select_row(m, row);
        break;
    case RB_NAV_BACK:
        pop(m);
        break;
    }
    return RB_MENU_REDRAW | (m->depth ? 0 : RB_MENU_CLOSED);
}

int rb_menu_capture(rb_menu_t *m, rb_chord_t c) {
    if (rb_menu_screen(m) != RB_SCR_CAPTURE) return 0;
    char name[24], b[48];
    rb_chord_name(c, name, sizeof name);
    pop(m);   /* back to the target; confirm or message goes on top */
    if (rb_chord_eq(c, RB_MENU_CHORD)) {
        message(m, "$+Mic always opens this menu.", "Pick another input.");
        return RB_MENU_REDRAW;
    }
    if (!rb_chord_bindable(c)) {
        snprintf(b, sizeof b, "%s can't be mapped.", name);
        message(m, b, "Pick another input.");
        return RB_MENU_REDRAW;
    }
    rb_action_t a = cur_target(m), now = rb_lookup(&m->cfg.keys, c);
    if (rb_action_eq(now, a) && !m->capture_adds) {
        rb_chord_t in[2];
        if (rb_inputs_for(&m->cfg.keys, a, in, 2) == 1) {
            snprintf(b, sizeof b, "%s is already %s.", name, rb_action_name(a));
            message(m, b, "");
            return RB_MENU_REDRAW;
        }
    }
    m->captured = c;
    char q[48];
    snprintf(q, sizeof q, "%s %s to %s?", m->capture_adds ? "Add" : "Map", name,
             rb_action_name(a));
    if (now.kind == RB_ACT_NONE || rb_action_eq(now, a)) {
        confirm(m, OP_ASSIGN, q, "", "");
    } else if (now.kind == RB_ACT_TYPE) {
        snprintf(b, sizeof b, "%s will stop typing", name);
        confirm(m, OP_ASSIGN, q, b, "its own character.");
    } else {
        snprintf(b, sizeof b, "%s is now %s;", name, rb_action_name(now));
        confirm(m, OP_ASSIGN, q, b, "that mapping will be replaced.");
    }
    return RB_MENU_REDRAW;
}
