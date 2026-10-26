/* Host tests for the ReBiko input router, settings and menu.
 *   tests/run_rebiko.sh */
#include <stdio.h>
#include <string.h>

#include "rebiko_config.h"
#include "rebiko_input.h"
#include "rebiko_menu.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- a recording output ---- */

typedef struct {
    char log[512];
    int keys, chars, cmds, navs, captures;
    int last_key, last_key_phys, last_cmd, last_nav;
    char last_char;
    rb_chord_t last_capture;
} rec_t;

static void rec_add(rec_t *r, const char *s) {
    size_t n = strlen(r->log);
    snprintf(r->log + n, sizeof r->log - n, "%s ", s);
}
static void o_key(void *ctx, int ck, int phys) {
    rec_t *r = ctx; r->keys++; r->last_key = ck; r->last_key_phys = phys;
    rec_add(r, rb_cybiko_key_name(ck));
}
static void o_char(void *ctx, char c, int phys) {
    rec_t *r = ctx; (void)phys; r->chars++; r->last_char = c;
    char s[4] = { '\'', c, '\'', 0 }; rec_add(r, s);
}
static void o_cmd(void *ctx, int cmd) {
    rec_t *r = ctx; r->cmds++; r->last_cmd = cmd; rec_add(r, rb_command_name(cmd));
}
static void o_nav(void *ctx, rb_nav_t nav) {
    rec_t *r = ctx; r->navs++; r->last_nav = nav; rec_add(r, "nav");
}
static void o_cap(void *ctx, rb_chord_t c) {
    rec_t *r = ctx; r->captures++; r->last_capture = c; rec_add(r, "capture");
}

static rb_router_t router;
static rb_bindings_t binds;
static rec_t rec;

static void setup(void) {
    memset(&rec, 0, sizeof rec);
    rb_restore_all(&binds);
    rb_output_t o = { &rec, o_key, o_char, o_cmd, o_nav, o_cap, NULL };
    rb_router_init(&router, &binds, &o);
}

static void tap(int phys) { rb_router_key(&router, phys, true); rb_router_key(&router, phys, false); }
static int PK(char c) { return rb_phys_for_char(c); }
#define KEYACT(k) ((rb_action_t){ RB_ACT_KEY, (k) })
#define CMDACT(k) ((rb_action_t){ RB_ACT_CMD, (k) })
#define CHORD(p, m) ((rb_chord_t){ (uint8_t)(p), (uint8_t)(m) })

/* ---- router ---- */

static void test_typing_and_symbols(void) {
    setup();
    tap(PK('q'));
    CHECK(rec.chars == 1 && rec.last_char == 'q');
    rb_router_key(&router, RB_PK_SYM, true);
    tap(PK('w'));                         /* Sym+W is 1 */
    rb_router_key(&router, RB_PK_SYM, false);
    CHECK(rec.last_char == '1');
    tap(RB_PK_DOLLAR); tap(RB_PK_DOLLAR); /* $ $ types $ */
    CHECK(rec.last_char == '$' && !router.dollar);
    CHECK(rec.keys == 0 && rec.cmds == 0);
}

static void test_backspace_vs_delete(void) {
    setup();
    tap(RB_PK_BKSP);
    CHECK(rec.keys == 1 && rec.last_key == RB_CK_BKSP);
    tap(RB_PK_DOLLAR); tap(RB_PK_BKSP);
    CHECK(rec.keys == 2 && rec.last_key == RB_CK_DEL);
    tap(RB_PK_BKSP);                      /* the prefix is used up */
    CHECK(rec.last_key == RB_CK_BKSP);
}

static void test_dollar_defaults(void) {
    static const struct { char k; int ck; } K[] = {
        {'q', RB_CK_F1}, {'u', RB_CK_F7}, {'a', RB_CK_SELECT}, {'s', RB_CK_INSERT},
        {'f', RB_CK_FN}, {'g', RB_CK_HELP}, {'h', RB_CK_TAB}, {'j', RB_CK_LBRACKET},
        {'k', RB_CK_RBRACKET}, {'l', RB_CK_BACKSLASH}, {'z', RB_CK_BACKTICK},
        {'c', RB_CK_EQUALS}, {'v', RB_CK_APOSTROPHE}, {'b', RB_CK_SEMICOLON},
        {'n', RB_CK_SLASH}, {'x', RB_CK_ESC},
    };
    setup();
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
        tap(RB_PK_DOLLAR); tap(PK(K[i].k));
        CHECK(rec.last_key == K[i].ck);
    }
    tap(RB_PK_DOLLAR); tap(PK('p')); CHECK(rec.last_cmd == RB_CMD_VOL_UP);
    tap(RB_PK_DOLLAR); tap(PK('m')); CHECK(rec.last_cmd == RB_CMD_VOL_DOWN);
    tap(RB_PK_DOLLAR); tap(RB_PK_SPACE); CHECK(rec.last_cmd == RB_CMD_KEYREF);
    tap(RB_PK_DOLLAR); tap(RB_PK_MIC); CHECK(rec.last_cmd == RB_CMD_MENU);
    int before = rec.keys + rec.chars + rec.cmds;
    tap(RB_PK_DOLLAR); tap(PK('d'));      /* unbound: nothing, prefix cleared */
    CHECK(rec.keys + rec.chars + rec.cmds == before && !router.dollar);
}

static void test_shift_keys_pass_through(void) {
    setup();
    rb_router_key(&router, RB_PK_LSHIFT, true);
    CHECK(rec.last_key == RB_CK_SHIFT && rec.last_key_phys == RB_PK_LSHIFT);
    CHECK(rb_router_held(&router, RB_PK_LSHIFT));
    tap(PK('a'));                         /* the letter itself, unshifted: CyOS shifts it */
    CHECK(rec.last_char == 'a');
    rb_router_key(&router, RB_PK_LSHIFT, false);
    CHECK(!rb_router_held(&router, RB_PK_LSHIFT));
    rb_router_key(&router, RB_PK_RSHIFT, true);
    CHECK(rec.last_key == RB_CK_FN && rec.last_key_phys == RB_PK_RSHIFT);
}

static void test_ball_in_emulation(void) {
    setup();
    rb_router_ball(&router, RB_PK_BALL_UP, true);    CHECK(rec.last_key == RB_CK_UP);
    rb_router_ball(&router, RB_PK_BALL_RIGHT, true); CHECK(rec.last_key == RB_CK_RIGHT);
    rb_router_ball(&router, RB_PK_BALL_CLICK, true);
    CHECK(rec.last_key == RB_CK_ENTER && rec.last_key_phys == RB_PK_BALL_CLICK);
    CHECK(!rb_router_held(&router, RB_PK_BALL_CLICK));
    rb_router_ball(&router, RB_PK_BALL_HOLD, true);
    CHECK(rec.last_key == RB_CK_ESC && rec.last_key_phys == RB_PK_BALL_HOLD);
    CHECK(rb_router_held(&router, RB_PK_BALL_HOLD));
    rb_router_ball(&router, RB_PK_BALL_HOLD, false);
    CHECK(!rb_router_held(&router, RB_PK_BALL_HOLD));
}

static void test_menu_mode_blocks_cybiko(void) {
    setup();
    rb_router_set_mode(&router, RB_MODE_MENU);
    for (int p = 0; p < RB_PK_KEYS; p++)
        if (p != RB_PK_DOLLAR) tap(p);
    rb_router_ball(&router, RB_PK_BALL_UP, true);
    rb_router_ball(&router, RB_PK_BALL_DOWN, true);
    rb_router_ball(&router, RB_PK_BALL_CLICK, true);
    rb_router_ball(&router, RB_PK_BALL_HOLD, true);
    rb_router_ball(&router, RB_PK_BALL_HOLD, false);
    CHECK(rec.keys == 0 && rec.chars == 0 && rec.cmds == 0);
    CHECK(rec.navs == 6);   /* Enter, Backspace, up, down, click, hold */
    tap(RB_PK_DOLLAR); tap(RB_PK_MIC);   /* closes the menu */
    CHECK(rec.cmds == 1 && rec.last_cmd == RB_CMD_MENU);
    rb_router_set_mode(&router, RB_MODE_EMU);
    tap(PK('k'));
    CHECK(rec.chars == 1 && rec.last_char == 'k');
}

static void test_capture_mode(void) {
    setup();
    rb_router_set_mode(&router, RB_MODE_CAPTURE);
    tap(RB_PK_DOLLAR); tap(PK('e'));
    CHECK(rec.captures == 1 && rb_chord_eq(rec.last_capture, CHORD(PK('e'), RB_MOD_DOLLAR)));
    rb_router_ball(&router, RB_PK_BALL_LEFT, true);
    CHECK(rec.captures == 2 && rec.last_capture.phys == RB_PK_BALL_LEFT);
    rb_router_ball(&router, RB_PK_BALL_HOLD, true);   /* cancels, is not captured */
    CHECK(rec.captures == 2 && rec.navs == 1 && rec.last_nav == RB_NAV_BACK);
    CHECK(rec.keys == 0 && rec.chars == 0);
}

static void test_serial_chars(void) {
    setup();
    rb_router_char(&router, 'Q');
    CHECK(rec.last_char == 'Q');
    rb_router_char(&router, '$'); rb_router_char(&router, 'x');
    CHECK(rec.last_key == RB_CK_ESC);
    rb_router_char(&router, '\b');
    CHECK(rec.last_key == RB_CK_BKSP);
    rb_router_char(&router, '\r');
    CHECK(rec.last_key == RB_CK_ENTER);
}

/* ---- bindings ---- */

static void test_conflicts_and_restore(void) {
    rb_bindings_t b = { 0 };
    rb_chord_t dq = CHORD(PK('q'), RB_MOD_DOLLAR), dx = CHORD(PK('x'), RB_MOD_DOLLAR);
    CHECK(rb_action_eq(rb_lookup(&b, dq), KEYACT(RB_CK_F1)));
    /* Esc moves to $+Q: F1 loses it, $+X stops being Esc, the ball hold stays */
    CHECK(rb_change(&b, KEYACT(RB_CK_ESC), dq));
    CHECK(rb_action_eq(rb_lookup(&b, dq), KEYACT(RB_CK_ESC)));
    CHECK(rb_lookup(&b, dx).kind == RB_ACT_NONE);
    rb_chord_t in[8];
    CHECK(rb_inputs_for(&b, KEYACT(RB_CK_F1), in, 8) == 0);
    int n = rb_inputs_for(&b, KEYACT(RB_CK_ESC), in, 8);
    CHECK(n == 2);   /* $+Q and the ball hold */
    /* F1 back to its default takes $+Q back */
    rb_restore_default(&b, KEYACT(RB_CK_F1));
    CHECK(rb_action_eq(rb_lookup(&b, dq), KEYACT(RB_CK_F1)));
    CHECK(rb_lookup(&b, dx).kind == RB_ACT_NONE);
    rb_restore_default(&b, KEYACT(RB_CK_ESC));
    CHECK(rb_action_eq(rb_lookup(&b, dx), KEYACT(RB_CK_ESC)));
    CHECK(b.count == 0);
    /* add a second input, then clear only the custom one */
    CHECK(rb_bind(&b, CHORD(RB_PK_ALT, RB_MOD_NONE), KEYACT(RB_CK_HELP)));
    CHECK(rb_inputs_for(&b, KEYACT(RB_CK_HELP), in, 8) == 2);
    rb_clear_custom(&b, KEYACT(RB_CK_HELP));
    CHECK(rb_inputs_for(&b, KEYACT(RB_CK_HELP), in, 8) == 1);
    /* a letter mapped elsewhere stops typing; restore all brings it back */
    CHECK(rb_bind(&b, CHORD(PK('o'), RB_MOD_NONE), KEYACT(RB_CK_SELECT)));
    CHECK(rb_lookup(&b, CHORD(PK('o'), RB_MOD_NONE)).kind == RB_ACT_KEY);
    rb_restore_all(&b);
    CHECK(rb_lookup(&b, CHORD(PK('o'), RB_MOD_NONE)).kind == RB_ACT_TYPE);
}

static void test_menu_chord_is_safe(void) {
    rb_bindings_t b = { 0 };
    CHECK(!rb_bind(&b, RB_MENU_CHORD, KEYACT(RB_CK_ESC)));
    CHECK(!rb_change(&b, KEYACT(RB_CK_ESC), RB_MENU_CHORD));
    CHECK(!rb_bind(&b, CHORD(RB_PK_DOLLAR, 0), KEYACT(RB_CK_ESC)));
    CHECK(!rb_bind(&b, CHORD(RB_PK_SYM, 0), KEYACT(RB_CK_ESC)));
    /* a stored override on it (old or damaged settings) is dropped */
    b.count = 1;
    b.ov[0].chord = RB_MENU_CHORD;
    b.ov[0].action = KEYACT(RB_CK_ESC);
    CHECK(rb_bindings_sanitize(&b) == 1 && b.count == 0);
    /* even if the lookup were handed one, $+Mic stays the menu */
    b.count = 1;
    b.ov[0].chord = RB_MENU_CHORD;
    b.ov[0].action = KEYACT(RB_CK_ESC);
    CHECK(rb_action_eq(rb_lookup(&b, RB_MENU_CHORD), CMDACT(RB_CMD_MENU)));
    /* mapping everything else away still leaves $+Mic */
    rb_bindings_t full = { 0 };
    for (int p = 0; p < RB_PK_COUNT; p++)
        rb_bind(&full, CHORD(p, RB_MOD_DOLLAR), KEYACT(RB_CK_TAB));
    CHECK(rb_action_eq(rb_lookup(&full, RB_MENU_CHORD), CMDACT(RB_CMD_MENU)));
}

/* ---- config ---- */

static void test_config_roundtrip(void) {
    rb_config_t a, b;
    uint8_t blob[RB_CONFIG_BLOB_MAX];
    rb_config_defaults(&a);
    a.cpu_mode = RB_CPU_HIGH; a.light = 3; a.volume = 2; a.audio_on = 0;
    a.scale = RB_SCALE_1X; a.screen_sleep = 0;
    rb_change(&a.keys, KEYACT(RB_CK_HELP), CHORD(RB_PK_MIC, RB_MOD_NONE));
    rb_bind(&a.keys, CHORD(RB_PK_BALL_LEFT, RB_MOD_DOLLAR), CMDACT(RB_CMD_BACKLIGHT));
    size_t n = rb_config_pack(&a, blob, sizeof blob);
    CHECK(n > 0);
    CHECK(rb_config_unpack(&b, blob, n) == RB_CFG_OK);
    CHECK(rb_config_equal(&a, &b));
    CHECK(rb_action_eq(rb_lookup(&b.keys, CHORD(RB_PK_MIC, RB_MOD_NONE)), KEYACT(RB_CK_HELP)));
    CHECK(rb_lookup(&b.keys, CHORD(PK('g'), RB_MOD_DOLLAR)).kind == RB_ACT_NONE);
    /* the largest table fits */
    for (int p = 0; a.keys.count < RB_MAX_OVERRIDES && p < RB_PK_COUNT; p++)
        rb_bind(&a.keys, CHORD(p, RB_MOD_DOLLAR), KEYACT(RB_CK_TAB));
    for (int p = 0; a.keys.count < RB_MAX_OVERRIDES && p < RB_PK_COUNT; p++)
        rb_bind(&a.keys, CHORD(p, RB_MOD_NONE), KEYACT(RB_CK_TAB));
    CHECK(a.keys.count == RB_MAX_OVERRIDES);
    n = rb_config_pack(&a, blob, sizeof blob);
    CHECK(n > 0 && rb_config_unpack(&b, blob, n) == RB_CFG_OK && rb_config_equal(&a, &b));
}

static void test_config_damage(void) {
    rb_config_t a, b, d;
    uint8_t blob[RB_CONFIG_BLOB_MAX];
    rb_config_defaults(&d);
    rb_config_defaults(&a);
    a.volume = 3;
    size_t n = rb_config_pack(&a, blob, sizeof blob);

    CHECK(rb_config_unpack(&b, NULL, 0) == RB_CFG_MISSING && rb_config_equal(&b, &d));
    blob[9] ^= 0x40;   /* a payload byte: CRC fails */
    CHECK(rb_config_unpack(&b, blob, n) == RB_CFG_CORRUPT && rb_config_equal(&b, &d));
    blob[9] ^= 0x40;
    CHECK(rb_config_unpack(&b, blob, n - 1) == RB_CFG_CORRUPT);
    blob[0] = 'X';
    CHECK(rb_config_unpack(&b, blob, n) == RB_CFG_CORRUPT && b.volume == d.volume);
    blob[0] = 'R';
    CHECK(rb_config_unpack(&b, blob, n) == RB_CFG_OK && b.volume == 3);

    /* in-range CRC, out-of-range values: fixed field by field */
    a.light = 9; a.volume = 77; a.cpu_mode = 5;
    a.keys.count = 2;
    a.keys.ov[0].chord = CHORD(PK('t'), RB_MOD_NONE);
    a.keys.ov[0].action = KEYACT(RB_CK_F2);
    a.keys.ov[1].chord = CHORD(200, RB_MOD_NONE);
    a.keys.ov[1].action = KEYACT(RB_CK_F2);
    n = rb_config_pack(&a, blob, sizeof blob);
    CHECK(rb_config_unpack(&b, blob, n) == RB_CFG_REPAIRED);
    CHECK(b.light == d.light && b.volume == d.volume && b.cpu_mode == d.cpu_mode);
    CHECK(b.keys.count == 1);

    /* an older, shorter payload: the missing fields take their defaults */
    uint8_t old[32];
    memcpy(old, "RBKO", 4);
    old[4] = 1; old[5] = 3; old[6] = 0;   /* payload: cpu_mode, light, volume */
    old[7] = RB_CPU_HIGH; old[8] = 2; old[9] = 4;
    uint32_t crc = rb_crc32(old, 10);
    for (int i = 0; i < 4; i++) old[10 + i] = (uint8_t)(crc >> (8 * i));
    CHECK(rb_config_unpack(&b, old, 14) == RB_CFG_OK);
    CHECK(b.cpu_mode == RB_CPU_HIGH && b.light == 2 && b.volume == 4);
    CHECK(b.audio_on == d.audio_on && b.scale == d.scale && b.keys.count == 0);
}

static void test_cpu_modes(void) {
    CHECK(rb_cpu_mhz(RB_CPU_DYNAMIC, true) == 240);
    CHECK(rb_cpu_mhz(RB_CPU_DYNAMIC, false) == 160);
    CHECK(rb_cpu_mhz(RB_CPU_HIGH, true) == 240);
    CHECK(rb_cpu_mhz(RB_CPU_HIGH, false) == 240);
    CHECK(rb_cpu_mhz(RB_CPU_LOW, true) == 160);
    CHECK(rb_cpu_mhz(RB_CPU_LOW, false) == 160);
}

/* ---- menu ---- */

static rb_config_t applied;
static int applies;
static bool linkv;
static void h_apply(void *ctx, const rb_config_t *c) { (void)ctx; applied = *c; applies++; }
static bool h_link(void *ctx) { (void)ctx; return linkv; }
static void h_set_link(void *ctx, bool on) { (void)ctx; linkv = on; }

static rb_menu_t menu;
static void menu_setup(void) {
    rb_menu_hooks_t h = { 0 };
    h.apply = h_apply; h.link_view = h_link; h.set_link_view = h_set_link;
    h.version = "test";
    rb_menu_init(&menu, &h);
    rb_config_t c;
    rb_config_defaults(&c);
    rb_menu_open(&menu, &c, RB_SCR_MAIN);
    applies = 0;
}
static void go(rb_nav_t n, int times) { while (times--) rb_menu_nav(&menu, n); }

static void test_menu_open_close(void) {
    menu_setup();
    CHECK(rb_menu_is_open(&menu) && rb_menu_mode(&menu) == RB_MODE_MENU);
    go(RB_NAV_BACK, 1);
    CHECK(!rb_menu_is_open(&menu) && rb_menu_mode(&menu) == RB_MODE_EMU);
    menu_setup();
    go(RB_NAV_DOWN, 10);   /* stops on the last row: Return to emulation */
    CHECK(rb_menu_cursor(&menu) == 5);
    int r = rb_menu_nav(&menu, RB_NAV_SELECT);
    CHECK((r & RB_MENU_CLOSED) && !rb_menu_is_open(&menu));
    /* a shortcut screen returns to emulation on Back */
    rb_config_t c; rb_config_defaults(&c);
    rb_menu_open(&menu, &c, RB_SCR_KEYREF);
    CHECK(rb_menu_rows(&menu) > 20);
    go(RB_NAV_BACK, 1);
    CHECK(!rb_menu_is_open(&menu));
}

static void test_menu_cpu_mode(void) {
    menu_setup();
    go(RB_NAV_DOWN, 1); go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_PERF);
    go(RB_NAV_RIGHT, 1);
    CHECK(applies == 1 && applied.cpu_mode == RB_CPU_HIGH);
    go(RB_NAV_RIGHT, 1);
    CHECK(applied.cpu_mode == RB_CPU_LOW);
    CHECK(strstr(rb_menu_hint(&menu), "160") != NULL);
    go(RB_NAV_RIGHT, 1);
    CHECK(applied.cpu_mode == RB_CPU_DYNAMIC);
    go(RB_NAV_LEFT, 1);
    CHECK(applied.cpu_mode == RB_CPU_LOW);
    go(RB_NAV_SELECT, 1);   /* select wraps forward */
    CHECK(applied.cpu_mode == RB_CPU_DYNAMIC);
}

static void test_menu_display_audio(void) {
    menu_setup();
    go(RB_NAV_DOWN, 2); go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_DISPLAY);
    go(RB_NAV_RIGHT, 1);                 /* brighter: 3/4 -> 4/4 */
    CHECK(applied.light == 0);
    go(RB_NAV_DOWN, 1); go(RB_NAV_RIGHT, 1);
    CHECK(applied.scale == RB_SCALE_1X);
    go(RB_NAV_BACK, 1); go(RB_NAV_DOWN, 1); go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_AUDIO);
    go(RB_NAV_SELECT, 1);
    CHECK(applied.audio_on == 0);
    go(RB_NAV_DOWN, 1); go(RB_NAV_LEFT, 3);
    CHECK(applied.volume == 5);
}

/* Controls -> Esc -> Change -> $+Q: warns that $+Q is F1, then maps it */
static void test_menu_remap_with_conflict(void) {
    menu_setup();
    go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_CONTROLS);
    go(RB_NAV_DOWN, 1);                  /* row 1: Esc */
    char label[48], value[48];
    rb_menu_row(&menu, rb_menu_cursor(&menu), label, sizeof label, value, sizeof value);
    CHECK(strcmp(label, "Esc") == 0 && strstr(value, "$+X") && strstr(value, "Ball hold"));
    go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_TARGET && rb_menu_cursor(&menu) == 1);
    go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_mode(&menu) == RB_MODE_CAPTURE);
    rb_menu_capture(&menu, CHORD(PK('q'), RB_MOD_DOLLAR));
    CHECK(rb_menu_screen(&menu) == RB_SCR_CONFIRM);
    CHECK(strstr(rb_menu_line(&menu, 1), "F1") != NULL);
    CHECK(rb_menu_cursor(&menu) == 1);   /* Cancel first */
    go(RB_NAV_SELECT, 1);                /* cancel: nothing changes */
    CHECK(rb_menu_screen(&menu) == RB_SCR_TARGET && applies == 0);
    go(RB_NAV_SELECT, 1);                /* Change again */
    rb_menu_capture(&menu, CHORD(PK('q'), RB_MOD_DOLLAR));
    go(RB_NAV_UP, 1); go(RB_NAV_SELECT, 1);   /* Confirm */
    CHECK(rb_menu_screen(&menu) == RB_SCR_TARGET && applies == 1);
    CHECK(rb_action_eq(rb_lookup(&applied.keys, CHORD(PK('q'), RB_MOD_DOLLAR)), KEYACT(RB_CK_ESC)));
    CHECK(rb_lookup(&applied.keys, CHORD(PK('x'), RB_MOD_DOLLAR)).kind == RB_ACT_NONE);
    /* Restore default on Esc gives $+X back */
    go(RB_NAV_DOWN, 3);
    CHECK(rb_menu_cursor(&menu) == 4);
    go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_MESSAGE);
    go(RB_NAV_SELECT, 1);
    CHECK(rb_action_eq(rb_lookup(&menu.cfg.keys, CHORD(PK('x'), RB_MOD_DOLLAR)), KEYACT(RB_CK_ESC)));
}

static void test_menu_capture_refuses_menu_chord(void) {
    menu_setup();
    go(RB_NAV_SELECT, 1); go(RB_NAV_DOWN, 1); go(RB_NAV_SELECT, 2);
    CHECK(rb_menu_mode(&menu) == RB_MODE_CAPTURE);
    rb_menu_capture(&menu, RB_MENU_CHORD);
    CHECK(rb_menu_screen(&menu) == RB_SCR_MESSAGE && applies == 0);
    CHECK(strstr(rb_menu_line(&menu, 0), "$+Mic") != NULL);
    go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_TARGET);
    /* and capture can always be backed out of */
    go(RB_NAV_SELECT, 1);
    go(RB_NAV_BACK, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_TARGET && applies == 0);
}

static void test_menu_restore_all(void) {
    menu_setup();
    menu.cfg.volume = 1;
    menu.cfg.cpu_mode = RB_CPU_HIGH;
    rb_bind(&menu.cfg.keys, CHORD(RB_PK_ALT, 0), KEYACT(RB_CK_HELP));
    linkv = true;
    go(RB_NAV_DOWN, 4); go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_SYSTEM);
    go(RB_NAV_DOWN, 3);                  /* Info, Key reference, Link view, Restore */
    char label[48];
    rb_menu_row(&menu, rb_menu_cursor(&menu), label, sizeof label, NULL, 0);
    CHECK(strcmp(label, "Restore all defaults") == 0);
    go(RB_NAV_SELECT, 1);
    CHECK(rb_menu_screen(&menu) == RB_SCR_CONFIRM);
    go(RB_NAV_UP, 1); go(RB_NAV_SELECT, 1);
    rb_config_t d; rb_config_defaults(&d);
    CHECK(rb_config_equal(&applied, &d) && !linkv);
    CHECK(rb_menu_screen(&menu) == RB_SCR_SYSTEM);
}

static void test_menu_controls_scrolls_to_end(void) {
    menu_setup();
    go(RB_NAV_SELECT, 1);
    go(RB_NAV_DOWN, 100);
    int rows = rb_menu_rows(&menu);
    CHECK(rb_menu_cursor(&menu) == rows - 2);       /* last bindable target */
    CHECK(rb_menu_top(&menu) + RB_MENU_VISIBLE >= rows); /* the fixed Menu row shows */
}

int main(void) {
    struct { const char *name; void (*fn)(void); } T[] = {
        { "typing and Sym layer", test_typing_and_symbols },
        { "Backspace vs $+Backspace", test_backspace_vs_delete },
        { "$ defaults", test_dollar_defaults },
        { "Shift keys pass through", test_shift_keys_pass_through },
        { "trackball in emulation", test_ball_in_emulation },
        { "menu mode blocks the Cybiko", test_menu_mode_blocks_cybiko },
        { "capture mode", test_capture_mode },
        { "serial characters", test_serial_chars },
        { "conflicts and restore", test_conflicts_and_restore },
        { "menu chord can't be lost", test_menu_chord_is_safe },
        { "config round trip", test_config_roundtrip },
        { "config damage", test_config_damage },
        { "CPU modes", test_cpu_modes },
        { "menu open and close", test_menu_open_close },
        { "menu CPU mode", test_menu_cpu_mode },
        { "menu display and audio", test_menu_display_audio },
        { "menu remap with conflict", test_menu_remap_with_conflict },
        { "capture refuses $+Mic", test_menu_capture_refuses_menu_chord },
        { "menu restore all", test_menu_restore_all },
        { "Controls scrolls to the end", test_menu_controls_scrolls_to_end },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        int before = failures;
        T[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", T[i].name);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
