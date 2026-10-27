/* Host tests for apps on and off the Classic (cybiko-emu/rebiko_apps.c):
 * the file system reader, the console driver against a pretend CyOS that
 * checks every frame and block as CyOS 1.2.46 does, and the PC link. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rebiko_apps.h"

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- a Classic flash image ---- */

#define PAGES 2048
#define PAGE  264
static uint8_t flash[PAGES * PAGE];

static uint8_t *block(int page) { return flash + page * PAGE + 8; }

static void format(void) {
    memset(flash, 0xFF, sizeof flash);
    for (int p = 5; p < PAGES; p++)
        block(p)[0] = 0x7F;
}

/* Lay a file out the way CyOS does: 178 bytes in part 0, 250 after */
static void add_file(int *page, unsigned id, const char *name, const uint8_t *d, int n) {
    for (unsigned part = 0; part == 0 || n > 0; part++) {
        uint8_t *b = block((*page)++);
        int cap = part ? 250 : 178, take = n < cap ? n : cap;
        b[0] = 0x80;
        b[1] = (uint8_t)take;
        b[2] = (uint8_t)(id >> 8); b[3] = (uint8_t)id;
        b[4] = (uint8_t)(part >> 8); b[5] = (uint8_t)part;
        b[6] = part ? 0x00 : 0x20;
        if (!part) {
            memset(b + 7, 0, 67);
            memcpy(b + 7, name, strlen(name));
        }
        memcpy(b + (part ? 6 : 78), d, (size_t)take);
        d += take;
        n -= take;
    }
}

static void test_cfs(void) {
    static uint8_t big[600], out[700];
    for (int i = 0; i < 600; i++) big[i] = (uint8_t)(i * 7);
    format();
    int page = 5;
    add_file(&page, 1, "small.txt", (const uint8_t *)"hello", 5);
    add_file(&page, 2, "big.app", big, 600);   /* parts 0, 1, 2 */

    rb_cfs_entry_t list[8];
    uint32_t free_b = 0;
    int n = rb_cfs_list(flash, sizeof flash, list, 8, &free_b);
    CHECK(n == 2);
    CHECK(strcmp(list[0].name, "small.txt") == 0 && list[0].size == 5);
    CHECK(strcmp(list[1].name, "big.app") == 0 && list[1].size == 600);
    CHECK(free_b == (uint32_t)(PAGES - 5 - 4) * 254);

    CHECK(rb_cfs_read(flash, sizeof flash, "big.app", out, sizeof out) == 600);
    CHECK(memcmp(out, big, 600) == 0);
    CHECK(rb_cfs_read(flash, sizeof flash, "big.app", NULL, 0) == 600);   /* size only */
    CHECK(rb_cfs_read(flash, sizeof flash, "none.app", out, sizeof out) == -1);
}

/* ---- a pretend CyOS console ---- */

typedef struct {
    rb_con_t *con;
    uint8_t in[2048];
    int in_n;
    int mode;               /* 0 lines, 1 receiving a file */
    char name[64];
    uint32_t size, got, next_block;
    uint8_t file[4096];
    int bad_sums, bad_crc_once;
    char last_line[100];
} cyos_t;

static void cyos_say(cyos_t *o, const char *s) {
    for (; *s; s++) {
        if (*s == '\n') rb_con_rx(o->con, '\r');
        rb_con_rx(o->con, (uint8_t)*s);
    }
}

static void cyos_ask_block(cyos_t *o) {
    char m[32];
    snprintf(m, sizeof m, "\nsend%u\n", (unsigned)o->next_block);
    cyos_say(o, m);
}

static void cyos_line(cyos_t *o, const char *line) {
    char echo[140];
    snprintf(o->last_line, sizeof o->last_line, "%s", line);
    snprintf(echo, sizeof echo, "\n>> %s\n", line);
    cyos_say(o, echo);
    if (strcmp(line, "ver") == 0) {
        cyos_say(o, "CyOS v. 1.2.46 built Apr 19 2000 18:16:52\nReady >\n ");
    } else if (strncmp(line, "rcv ", 4) == 0) {
        unsigned long sz;
        if (sscanf(line + 4, "%63s %lu", o->name, &sz) != 2 || o->name[0] == '"') {
            cyos_say(o, "\ncancel\n\nReady >\n ");
            return;
        }
        o->size = (uint32_t)sz;
        o->got = 0;
        o->next_block = 0;
        o->mode = 1;
        cyos_ask_block(o);
    } else if (strcmp(line, "reboot") == 0) {
        cyos_say(o, "Rebooting the system...\n");
    } else {
        cyos_say(o, "* Can't execute\nReady >\n ");
    }
}

/* The PC sent bytes: parse as CyOS would */
static int cyos_put(void *ctx, const uint8_t *data, int n) {
    cyos_t *o = ctx;
    memcpy(o->in + o->in_n, data, (size_t)n);
    o->in_n += n;
    for (;;) {
        if (o->mode == 0) {
            uint8_t *nl = memchr(o->in, '\n', (size_t)o->in_n);
            if (!nl || nl + 3 > o->in + o->in_n) return n;
            int len = (int)(nl - o->in);
            uint32_t sum = 0;
            for (int i = 0; i < len; i++)
                sum = ((sum ^ o->in[i]) << 1) | ((sum >> 16) & 1);
            unsigned got = (unsigned)(nl[1] | nl[2] << 8);
            char line[100];
            memcpy(line, o->in, (size_t)len);
            line[len] = 0;
            memmove(o->in, nl + 3, (size_t)(o->in_n - len - 3));
            o->in_n -= len + 3;
            if ((sum & 0xFFFF) != got) {
                o->bad_sums++;
                rb_con_rx(o->con, 0x01);
                continue;
            }
            rb_con_rx(o->con, 0x02);
            cyos_line(o, line);
        } else {
            if (o->in_n < 5) return n;
            int len = o->in[3] | o->in[4] << 8;
            if (o->in_n < 9 + len) return n;
            unsigned blk = (unsigned)(o->in[1] | o->in[2] << 8);
            uint32_t crc = rb_apps_crc32(0, o->in + 1, 4 + (size_t)len);
            uint32_t got = (uint32_t)o->in[5 + len] | (uint32_t)o->in[6 + len] << 8 |
                           (uint32_t)o->in[7 + len] << 16 | (uint32_t)o->in[8 + len] << 24;
            bool ok = o->in[0] == 'C' && blk == o->next_block && crc == got;
            if (ok && o->bad_crc_once) {   /* a line error: CyOS asks again */
                o->bad_crc_once = 0;
                ok = false;
            }
            if (ok) {
                memcpy(o->file + o->got, o->in + 5, (size_t)len);
                o->got += (uint32_t)len;
                o->next_block++;
            }
            memmove(o->in, o->in + 9 + len, (size_t)(o->in_n - 9 - len));
            o->in_n -= 9 + len;
            if (!ok) {
                cyos_say(o, "crc err");
                cyos_ask_block(o);
            } else if (o->got >= o->size) {
                o->mode = 0;
                cyos_say(o, "\nack\n\nreceive successfull, 2000 bytes/sec\nReady >\n ");
            } else {
                cyos_say(o, "\nack\n");
                cyos_ask_block(o);
            }
        }
    }
}

static void run(rb_con_t *c, int frames) {
    while (frames-- > 0 && c->state == RB_CON_BUSY)
        rb_con_poll(c, 16);
}

static void test_frame_and_crc(void) {
    uint8_t f[16];
    CHECK(rb_con_frame("ver", f, sizeof f) == 6);
    CHECK(memcmp(f, "ver\n", 4) == 0 && f[4] == 0xC0 && f[5] == 0x02);
    CHECK(rb_apps_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
}

static void test_console(void) {
    static rb_con_t con;
    static cyos_t os;
    memset(&os, 0, sizeof os);
    os.con = &con;
    rb_con_init(&con, cyos_put, &os);

    CHECK(rb_con_command(&con, "ver"));
    CHECK(!rb_con_command(&con, "ver"));    /* one at a time */
    run(&con, 100);
    CHECK(con.state == RB_CON_DONE);
    CHECK(strcmp(con.result, "CyOS v. 1.2.46 built Apr 19 2000 18:16:52") == 0);
    CHECK(os.bad_sums == 0);

    /* a file of three blocks, the middle one hit by a line error */
    static uint8_t app[1100];
    for (int i = 0; i < 1100; i++) app[i] = (uint8_t)(i ^ 0x5A);
    os.bad_crc_once = 0;
    CHECK(rb_con_send_file(&con, "game.app", app, sizeof app));
    run(&con, 3);
    os.bad_crc_once = 1;
    run(&con, 200);
    CHECK(con.state == RB_CON_DONE);
    CHECK(strcmp(os.last_line, "rcv game.app 1100") == 0);
    CHECK(os.got == 1100 && memcmp(os.file, app, 1100) == 0);
    CHECK(con.blocks_sent == 3);

    /* CyOS refuses: the console reports it */
    CHECK(rb_con_send_file(&con, "\"q.app\"", app, 10));
    run(&con, 100);
    CHECK(con.state == RB_CON_FAILED && strstr(con.result, "cancelled"));

}

/* ---- the PC link ---- */

static char sent[65536];
static size_t sent_n;
static void link_send(void *ctx, const uint8_t *d, size_t n) {
    (void)ctx;
    if (sent_n + n < sizeof sent) {
        memcpy(sent + sent_n, d, n);
        sent_n += n;
        sent[sent_n] = 0;
    }
}
static const uint8_t *link_flash(void *ctx, size_t *len) {
    (void)ctx;
    *len = sizeof flash;
    return flash;
}

static void link_say(rb_link_t *l, const char *s) {
    sent_n = 0;
    sent[0] = 0;
    rb_link_rx(l, (const uint8_t *)s, strlen(s));
}

static void test_link(void) {
    static rb_con_t con;
    static cyos_t os;
    static rb_link_t link;
    memset(&os, 0, sizeof os);
    os.con = &con;
    rb_con_init(&con, cyos_put, &os);
    rb_link_init(&link, &con);
    link.send = link_send;
    link.flash = link_flash;
    link.alloc = malloc;
    link.release = free;

    format();
    int page = 5;
    add_file(&page, 1, "small.txt", (const uint8_t *)"hello", 5);

    link_say(&link, "\x19ls\n");
    CHECK(strstr(sent, "@RB FILE small.txt 5\n") != NULL);
    CHECK(strstr(sent, "@RB OK free ") != NULL);

    link_say(&link, "\x19get small.txt\n");
    CHECK(strcmp(sent, "@RB DATA 5\nhello@RB OK\n") == 0);
    link_say(&link, "\x19get nope\n");
    CHECK(strncmp(sent, "@RB ERR", 7) == 0);

    /* put: the bytes may arrive in pieces, with the line */
    link_say(&link, "\x19put t.app 7\nabc");
    CHECK(strcmp(sent, "@RB MORE\n") == 0);
    CHECK(rb_link_receiving(&link));
    rb_link_rx(&link, (const uint8_t *)"defg", 4);
    CHECK(!rb_link_receiving(&link));
    for (int i = 0; i < 300 && link.waiting; i++) {
        rb_con_poll(&con, 16);
        rb_link_poll(&link);
    }
    CHECK(os.got == 7 && memcmp(os.file, "abcdefg", 7) == 0);
    CHECK(strstr(sent, "@RB PROGRESS 1 1\n") != NULL);
    CHECK(strstr(sent, "@RB OK\n") != NULL);
    CHECK(link.buf == NULL);

    /* 2.5 chunks: a MORE after each full one */
    static uint8_t big[2560];
    memset(big, 'z', sizeof big);
    link_say(&link, "\x19put z.app 2560\n");
    rb_link_rx(&link, big, sizeof big);
    CHECK(strcmp(sent, "@RB MORE\n@RB MORE\n@RB MORE\n") == 0);
    for (int i = 0; i < 400 && link.waiting; i++) {
        rb_con_poll(&con, 16);
        rb_link_poll(&link);
    }
    CHECK(os.got == 2560 && strstr(sent, "@RB OK\n") != NULL);

    /* a put whose bytes stop: the link drops it and takes commands again */
    link_say(&link, "\x19put s.app 9\nabc");
    for (int i = 0; i < RB_LINK_STALL_FRAMES + 1; i++)
        rb_link_poll(&link);
    CHECK(!rb_link_receiving(&link) && strstr(sent, "@RB ERR the file stopped") != NULL);
    CHECK(link.buf == NULL);
    link_say(&link, "\x19ls\n");
    CHECK(strstr(sent, "@RB OK free ") != NULL);

    link_say(&link, "\x19put ../x 3\n");
    CHECK(strncmp(sent, "@RB ERR", 7) == 0);
    link_say(&link, "\x19put bad\"name 3\n");
    CHECK(strncmp(sent, "@RB ERR", 7) == 0);

    link_say(&link, "\x19" "cmd ver\n");
    for (int i = 0; i < 300 && link.waiting; i++) {
        rb_con_poll(&con, 16);
        rb_link_poll(&link);
    }
    CHECK(strstr(sent, "@RB OUT CyOS v. 1.2.46") != NULL && strstr(sent, "@RB OK\n"));

    link_say(&link, "\x19" "frob\n");
    CHECK(strncmp(sent, "@RB ERR unknown", 15) == 0);
}

int main(void) {
    test_cfs();
    test_frame_and_crc();
    test_console();
    test_link();
    printf("rebiko_apps: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
