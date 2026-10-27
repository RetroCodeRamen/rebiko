/* Two Cybiko Classics on the in-process radio, both walked into Chat
 * (MAIN) by key scripts. Each types a marker; PASS when each marker is in
 * the other Classic's RAM. Screens go to OUTDIR as PGM.
 *   v1_chat <boot.bin> <flash.bin> [frames] [outdir]
 * Scripts can be replaced with SCRIPT_A / SCRIPT_B:
 *   '>' Right, '|' Enter, '~' Esc, '@N' wait to frame N, '+N' wait N
 *   frames, '!' save the screen, other characters are typed.
 * Build with tools/v1_chat.sh. */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/emulator.h"
#include "radio_coproc.h"
#include "radio_sim.h"

#define MARK_A "zqv7"
#define MARK_B "jxk5"
/* Right, Enter (Chat), Enter (past the description), Enter (MAIN). The
 * first typed key only clears "Hi, everybody!", so each line leads with
 * a throwaway 'x'. */
#define WALK "@3800>+90|+300|+300|+400!"

typedef struct { char c; int col; uint16_t mask; } vkey_t;
static const vkey_t KM[] = {
    {'q',0,0x10},{'a',0,0x20},{'~',0,0x02},
    {'2',1,0x08},{'w',1,0x10},{'s',1,0x20},{'z',1,0x40},
    {' ',2,0x04},{'3',2,0x08},{'e',2,0x10},{'d',2,0x20},{'x',2,0x40},
    {'1',3,0x02},{'4',3,0x08},{'r',3,0x10},{'f',3,0x20},{'c',3,0x40},
    {'>',4,0x01},{'5',4,0x08},{'t',4,0x10},{'g',4,0x20},{'v',4,0x40},
    {'|',5,0x04},{'6',5,0x08},{'y',5,0x10},{'h',5,0x20},{'b',5,0x40},
    {'7',6,0x08},{'u',6,0x10},{'j',6,0x20},{'n',6,0x40},
    {'0',7,0x04},{'8',7,0x08},{'i',7,0x10},{'k',7,0x20},{'m',7,0x40},
    {'9',8,0x04},{'p',8,0x08},{'o',8,0x10},{'l',8,0x20},
};

/* Frames a key is held, then all keys up before the next. HOLD and GAP in
 * the environment override them. */
static int hold_frames = 12, gap_frames = 3;   /* as the T-Deck types */
#define REPEAT_GAP 10   /* the same key twice needs this long up */

typedef struct {
    const char *name, *script;
    int pos, wait_until, key_left, gap_left, dumps;
    const vkey_t *key, *last;
    uint16_t keys[CYBIKO_KEYBOARD_COLUMNS];
    const uint8_t *px;
    cybiko_emu_t *emu;
    cybiko_radio_transport_t shim, *real;
    cybiko_radio_coproc_t cop;
} side_t;

static const char *outdir;
static int frame;
static side_t *cur;

void cybiko_radio_log(const char *fmt, ...) {
    va_list ap;
    printf("%6d %s ", frame, cur ? cur->name : "-");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

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

/* The transport wrapper prints every frame a guest puts on the air */
#define OWNER(s) ((side_t *)((char *)(s) - offsetof(side_t, shim)))

static int shim_send(cybiko_radio_transport_t *s, const uint8_t *data, size_t len, uint8_t ch) {
    side_t *o = OWNER(s);
    char t[49];
    size_t n = len < 48 ? len : 48;
    for (size_t i = 0; i < n; i++)
        t[i] = data[i] >= 0x20 && data[i] < 0x7f ? (char)data[i] : '.';
    t[n] = 0;
    printf("%6d %s air type=%02X len=%3zu |%s|\n", frame, o->name,
           len > CYBIKO_RADIO_HDR + 1 ? data[CYBIKO_RADIO_HDR + 1] : 0, len, t);
    return o->real->send_packet(o->real, data, len, ch);
}
static int shim_start(cybiko_radio_transport_t *s) { return OWNER(s)->real->start(OWNER(s)->real); }
static void shim_close(cybiko_radio_transport_t *s) { OWNER(s)->real->close(OWNER(s)->real); }
static void shim_channel(cybiko_radio_transport_t *s, uint8_t c) {
    OWNER(s)->real->set_channel(OWNER(s)->real, c);
}
static void shim_listener(cybiko_radio_transport_t *s, cybiko_radio_on_packet_fn fn, void *u) {
    OWNER(s)->real->set_listener(OWNER(s)->real, fn, u);
}
static void shim_pump(cybiko_radio_transport_t *s) { OWNER(s)->real->pump(OWNER(s)->real); }

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
        if (k == d->last && d->gap_left == 0 && gap_frames < REPEAT_GAP) {
            /* hold this key back until it has been up long enough */
            d->pos--;
            d->gap_left = REPEAT_GAP - gap_frames;
            d->last = NULL;
            return;
        }
        d->key = d->last = k;
        d->key_left = hold_frames - 1;
        d->gap_left = gap_frames;
        d->keys[k->col] |= k->mask;
        return;
    }
}

static void setup(side_t *d, const char *name, const char *script, uint32_t id,
                  radio_sim_node_t *node, const uint8_t *boot, size_t bn,
                  const uint8_t *flash, size_t fn) {
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
    d->real = &node->base;
    d->shim = node->base;
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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s boot.bin flash.bin [frames] [outdir]\n", argv[0]);
        return 2;
    }
    size_t bn, fn;
    uint8_t *boot = slurp(argv[1], &bn), *flash = slurp(argv[2], &fn);
    int frames = argc > 3 ? atoi(argv[3]) : 8000;
    outdir = argc > 4 ? argv[4] : NULL;
    if (getenv("HOLD")) hold_frames = atoi(getenv("HOLD"));
    if (getenv("GAP")) gap_frames = atoi(getenv("GAP"));
    const char *sa = getenv("SCRIPT_A"), *sb = getenv("SCRIPT_B");
    if (!sa) sa = WALK "+60x" MARK_A "|+400!+600!";
    if (!sb) sb = "@3900>+90|+300|+300|+400!+900x" MARK_B "|+400!+600!";

    static radio_sim_bus_t bus;
    static radio_sim_node_t na, nb;
    static side_t A, B;
    radio_sim_bus_init(&bus);
    radio_sim_node_init(&na, &bus, 0x4C000001u, 255);
    radio_sim_node_init(&nb, &bus, 0x4C000002u, 255);
    radio_sim_bus_add(&bus, &na);
    radio_sim_bus_add(&bus, &nb);
    setup(&A, "A", sa, 0x4C000001u, &na, boot, bn, flash, fn);
    setup(&B, "B", sb, 0x4C000002u, &nb, boot, bn, flash, fn);
    free(boot);
    free(flash);

    side_t *both[2] = { &A, &B };
    for (frame = 1; frame <= frames; frame++) {
        for (int i = 0; i < 2; i++) {
            cur = both[i];
            step_script(cur);
            cybiko_run_frame(cur->emu);
        }
        cur = NULL;
        radio_sim_bus_pump(&bus);
        if (frame % 1200 == 0) { dump(&A, "tick"); dump(&B, "tick"); }
    }
    dump(&A, "end");
    dump(&B, "end");
    for (int i = 0; i < 2; i++) {
        cybiko_v1_radio_status_t st;
        cybiko_v1_radio_status(both[i]->emu, &st);
        printf("%s: radio %s, sent %u, delivered %u\n", both[i]->name,
               st.radio_up ? "up" : "-", st.tx_frames, st.rx_injected);
    }
    bool b_got = ram_has(B.emu, MARK_A), a_got = ram_has(A.emu, MARK_B);
    printf("B received A's '%s': %s\nA received B's '%s': %s\n",
           MARK_A, b_got ? "yes" : "NO", MARK_B, a_got ? "yes" : "NO");
    printf(a_got && b_got ? "PASS: Chat crossed both ways\n" : "FAIL\n");
    return a_got && b_got ? 0 : 1;
}
