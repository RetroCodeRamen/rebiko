/* Three Cybiko Classics on the in-process radio, all walked into Chat
 * (MAIN). A and C each type a marker; B types nothing.
 *   v1_three <boot.bin> <flash.bin> [frames] [outdir]
 * By default all three hear each other, and PASS needs every marker at
 * both other Classics. CHAIN=1 puts A and C out of each other's range,
 * both in B's: PASS then needs B to hold both markers and A and C not to
 * hold each other's, because CyOS and the Classic's AVR relay nothing.
 * Scripts can be replaced with SCRIPT_A / SCRIPT_B / SCRIPT_C, as in
 * v1_chat. The defaults walk CyOS 1.2.46's desktop; for a 1.2.57 save:
 *   W='<+90<+90<+90<+90|+300|+300|+400!|+150!'
 *   SCRIPT_A="@3000$W+60xzqv7|+400!" SCRIPT_B="@3100$W" \
 *   SCRIPT_C="@3200$W+900xwmp3|+400!" tools/v1_three.sh boot.bin save.flash
 * ('<' is Left; 1.2.57 opens the recipient list first, hence the extra
 * Enter.) Build with tools/v1_three.sh. */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/emulator.h"
#include "radio_coproc.h"
#include "radio_sim.h"

#define MARK_A "zqv7"
#define MARK_C "wmp3"
#define WALK "@3800>+90|+300|+300|+400!"

typedef struct { char c; int col; uint16_t mask; } vkey_t;
static const vkey_t KM[] = {
    {'q',0,0x10},{'a',0,0x20},{'~',0,0x02},{'<',0,0x08},
    {'2',1,0x08},{'w',1,0x10},{'s',1,0x20},{'z',1,0x40},
    {' ',2,0x04},{'3',2,0x08},{'e',2,0x10},{'d',2,0x20},{'x',2,0x40},
    {'1',3,0x02},{'4',3,0x08},{'r',3,0x10},{'f',3,0x20},{'c',3,0x40},
    {'>',4,0x01},{'5',4,0x08},{'t',4,0x10},{'g',4,0x20},{'v',4,0x40},
    {'|',5,0x04},{'6',5,0x08},{'y',5,0x10},{'h',5,0x20},{'b',5,0x40},
    {'7',6,0x08},{'u',6,0x10},{'j',6,0x20},{'n',6,0x40},
    {'0',7,0x04},{'8',7,0x08},{'i',7,0x10},{'k',7,0x20},{'m',7,0x40},
    {'9',8,0x04},{'p',8,0x08},{'o',8,0x10},{'l',8,0x20},
};

#define HOLD 12
#define GAP 3
#define REPEAT_GAP 10

typedef struct {
    const char *name, *script;
    int pos, wait_until, key_left, gap_left, dumps;
    const vkey_t *key, *last;
    uint16_t keys[CYBIKO_KEYBOARD_COLUMNS];
    const uint8_t *px;
    cybiko_emu_t *emu;
    cybiko_radio_transport_t shim;
    cybiko_radio_transport_t *air[2];   /* B is on both airs in a chain */
    cybiko_radio_coproc_t cop;
} side_t;

static const char *outdir;
static int frame;

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (!b || fread(b, 1, *n, f) != *n) { perror(p); exit(2); }
    fclose(f);
    return b;
}

static void dump(side_t *d, const char *tag) {
    if (!outdir || !d->px) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%02d_f%05d_%s.pgm", outdir, d->name, d->dumps++, frame, tag);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", CYBIKO_LCD_WIDTH, CYBIKO_LCD_HEIGHT);
    fwrite(d->px, 1, CYBIKO_LCD_WIDTH * CYBIKO_LCD_HEIGHT, f);
    fclose(f);
}

static void hal_keys(void *ctx, uint16_t *m, int n) {
    side_t *d = ctx;
    int cols = n < CYBIKO_KEYBOARD_COLUMNS ? n : CYBIKO_KEYBOARD_COLUMNS;
    memcpy(m, d->keys, (size_t)cols * sizeof(uint16_t));
}

static void hal_render(void *ctx, const uint8_t *px, int w, int h) {
    (void)w; (void)h;
    ((side_t *)ctx)->px = px;
}

/* CyOS 1.2.57 stops booting while its console output has nowhere to go */
static void console_sink(void *u, uint8_t b) { (void)u; (void)b; }

/* The shim sends on every air the side is on and logs each frame */
#define OWNER(s) ((side_t *)((char *)(s) - offsetof(side_t, shim)))

static int shim_send(cybiko_radio_transport_t *s, const uint8_t *data, size_t len, uint8_t ch) {
    side_t *o = OWNER(s);
    printf("%6d %s air type=%02X len=%3zu to %02X%02X%02X%02X\n", frame, o->name,
           len > CYBIKO_RADIO_HDR + 1 ? data[CYBIKO_RADIO_HDR + 1] : 0, len,
           data[0], data[1], data[2], data[3]);
    int rc = 0;
    for (int i = 0; i < 2 && o->air[i]; i++)
        rc |= o->air[i]->send_packet(o->air[i], data, len, ch);
    return rc;
}
static int shim_start(cybiko_radio_transport_t *s) { (void)s; return 0; }
static void shim_close(cybiko_radio_transport_t *s) { (void)s; }
static void shim_channel(cybiko_radio_transport_t *s, uint8_t c) {
    side_t *o = OWNER(s);
    for (int i = 0; i < 2 && o->air[i]; i++)
        o->air[i]->set_channel(o->air[i], c);
}
static void shim_listener(cybiko_radio_transport_t *s, cybiko_radio_on_packet_fn fn, void *u) {
    side_t *o = OWNER(s);
    for (int i = 0; i < 2 && o->air[i]; i++)
        o->air[i]->set_listener(o->air[i], fn, u);
}
static void shim_pump(cybiko_radio_transport_t *s) { (void)s; }

static const vkey_t *key_for(char c) {
    for (size_t i = 0; i < sizeof KM / sizeof KM[0]; i++)
        if (KM[i].c == c) return &KM[i];
    return NULL;
}

static void step_script(side_t *d) {
    memset(d->keys, 0, sizeof d->keys);
    if (d->key_left > 0) {
        d->key_left--;
        d->keys[d->key->col] |= d->key->mask;
        return;
    }
    if (d->gap_left > 0) { d->gap_left--; return; }
    if (frame < d->wait_until) return;
    const char *s = d->script;
    while (s[d->pos]) {
        char c = s[d->pos++];
        if (c == '@' || c == '+') {
            int n = atoi(s + d->pos);
            while (s[d->pos] >= '0' && s[d->pos] <= '9') d->pos++;
            d->wait_until = c == '@' ? n : frame + n;
            return;
        }
        if (c == '!') { dump(d, "snap"); continue; }
        const vkey_t *k = key_for(c);
        if (!k) continue;
        if (k == d->last && d->gap_left == 0) {
            d->pos--;
            d->gap_left = REPEAT_GAP - GAP;
            d->last = NULL;
            return;
        }
        d->key = d->last = k;
        d->key_left = HOLD - 1;
        d->gap_left = GAP;
        d->keys[k->col] |= k->mask;
        return;
    }
}

static void setup(side_t *d, const char *name, const char *script, uint32_t id,
                  radio_sim_node_t *a, radio_sim_node_t *b,
                  const uint8_t *boot, size_t bn, const uint8_t *flash, size_t fn) {
    memset(d, 0, sizeof *d);
    d->name = name;
    d->script = script;
    cybiko_hal_t hal = {0};
    hal.keyboard_poll = hal_keys;
    hal.render_frame = hal_render;
    hal.ctx = d;
    d->emu = cybiko_create(&hal);
    if (!d->emu || !cybiko_load_boot_rom(d->emu, boot, bn) ||
        !cybiko_load_flash_rom(d->emu, flash, fn)) {
        fprintf(stderr, "emulator setup failed\n");
        exit(2);
    }
    d->air[0] = &a->base;
    d->air[1] = b ? &b->base : NULL;
    d->shim = a->base;
    d->shim.start = shim_start;
    d->shim.close = shim_close;
    d->shim.send_packet = shim_send;
    d->shim.set_channel = shim_channel;
    d->shim.set_listener = shim_listener;
    d->shim.pump = shim_pump;
    cybiko_radio_coproc_init(&d->cop);
    cybiko_radio_coproc_set_transport(&d->cop, &d->shim);
    cybiko_attach_radio_id(d->emu, &d->cop, id);
    cybiko_reset(d->emu);
    cybiko_attach_serial(d->emu, console_sink, NULL);
}

static bool ram_has(cybiko_emu_t *e, const char *mark) {
    size_t m = strlen(mark);
    for (uint32_t a = 0x200000u; a + m <= 0x280000u; a++) {
        size_t i = 0;
        while (i < m && cybiko_peek8(e, a + (uint32_t)i) == (uint8_t)mark[i]) i++;
        if (i == m) return true;
    }
    return false;
}

static bool expect(const char *who, cybiko_emu_t *e, const char *mark, bool want) {
    bool got = ram_has(e, mark);
    printf("%s has '%s': %s (%s)\n", who, mark, got ? "yes" : "no",
           got == want ? "as expected" : "WRONG");
    return got == want;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s boot.bin flash.bin [frames] [outdir]\n", argv[0]);
        return 2;
    }
    size_t bn, fn;
    uint8_t *boot = slurp(argv[1], &bn), *flash = slurp(argv[2], &fn);
    int frames = argc > 3 ? atoi(argv[3]) : 9000;
    outdir = argc > 4 ? argv[4] : NULL;
    bool chain = getenv("CHAIN") != NULL;
    const char *sa = getenv("SCRIPT_A"), *sb = getenv("SCRIPT_B"), *sc = getenv("SCRIPT_C");
    if (!sa) sa = WALK "+60x" MARK_A "|+400!";
    if (!sb) sb = "@3900>+90|+300|+300|+400!";
    if (!sc) sc = "@4000>+90|+300|+300|+400!+900x" MARK_C "|+400!";

    /* Two airs. All in range: everyone on the first. Chain: A and B on
     * the first, B and C on the second. */
    static radio_sim_bus_t ab, bc;
    static radio_sim_node_t na, nb1, nb2, nc;
    static side_t A, B, C;
    radio_sim_bus_init(&ab);
    radio_sim_bus_init(&bc);
    radio_sim_node_init(&na, &ab, 0x4C000001u, 255);
    radio_sim_node_init(&nb1, &ab, 0x4C000002u, 255);
    radio_sim_node_init(&nb2, &bc, 0x4C000002u, 255);
    radio_sim_node_init(&nc, chain ? &bc : &ab, 0x4C000003u, 255);
    radio_sim_bus_add(&ab, &na);
    radio_sim_bus_add(&ab, &nb1);
    radio_sim_bus_add(chain ? &bc : &ab, &nc);
    if (chain) radio_sim_bus_add(&bc, &nb2);
    setup(&A, "A", sa, 0x4C000001u, &na, NULL, boot, bn, flash, fn);
    setup(&B, "B", sb, 0x4C000002u, &nb1, chain ? &nb2 : NULL, boot, bn, flash, fn);
    setup(&C, "C", sc, 0x4C000003u, &nc, NULL, boot, bn, flash, fn);
    free(boot);
    free(flash);

    side_t *all[3] = { &A, &B, &C };
    for (frame = 1; frame <= frames; frame++) {
        for (int i = 0; i < 3; i++) {
            step_script(all[i]);
            cybiko_run_frame(all[i]->emu);
        }
        radio_sim_bus_pump(&ab);
        radio_sim_bus_pump(&bc);
        if (frame % 1200 == 0)
            for (int i = 0; i < 3; i++) dump(all[i], "tick");
    }
    for (int i = 0; i < 3; i++) {
        dump(all[i], "end");
        cybiko_v1_radio_status_t st;
        cybiko_v1_radio_status(all[i]->emu, &st);
        printf("%s: radio %s, sent %u, delivered %u\n", all[i]->name,
               st.radio_up ? "up" : "-", st.tx_frames, st.rx_injected);
    }
    bool ok = expect("B", B.emu, MARK_A, true) & expect("B", B.emu, MARK_C, true) &
              expect("C", C.emu, MARK_A, !chain) & expect("A", A.emu, MARK_C, !chain);
    printf(ok ? (chain ? "PASS: B heard both; nothing was relayed between A and C\n"
                       : "PASS: every line reached both other Classics\n")
              : "FAIL\n");
    return ok ? 0 : 1;
}
