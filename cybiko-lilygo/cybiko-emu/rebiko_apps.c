#include "rebiko_apps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the file system ----
 * AT45DB041: 2048 pages of 264 bytes; an 8-byte page header (CRC32, write
 * count, CRC16), 256 bytes of block, a 2-byte trailer. The first 5 pages
 * are the boot area. A used block (byte 0 bit 7) carries its data size
 * (byte 1), file id (2-3) and part (4-5), big-endian; part 0 holds the
 * name at 7 and data from 78, later parts data from 6. */

#define CFS_PAGE      264
#define CFS_HEAD      8
#define CFS_FIRST     5
#define CFS_FREE_UNIT 254   /* CyOS: "254 bytes each" */

static const uint8_t *cfs_block(const uint8_t *flash, size_t len, int page) {
    size_t off = (size_t)page * CFS_PAGE + CFS_HEAD;
    return off + 256 <= len ? flash + off : NULL;
}

static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }

int rb_cfs_list(const uint8_t *flash, size_t len, rb_cfs_entry_t *out, int max,
                uint32_t *free_bytes) {
    int pages = (int)(len / CFS_PAGE), n = 0;
    uint32_t free_blocks = 0;
    for (int p = CFS_FIRST; p < pages; p++) {
        const uint8_t *b = cfs_block(flash, len, p);
        if (!(b[0] & 0x80)) {
            free_blocks++;
            continue;
        }
        if (be16(b + 4) != 0 || n >= max)
            continue;
        rb_cfs_entry_t *e = &out[n++];
        memcpy(e->name, b + 7, sizeof e->name - 1);
        e->name[sizeof e->name - 1] = 0;
        e->size = 0;
        unsigned id = be16(b + 2);
        /* the size is the sum of every part's count */
        for (int q = CFS_FIRST; q < pages; q++) {
            const uint8_t *c = cfs_block(flash, len, q);
            if ((c[0] & 0x80) && be16(c + 2) == id)
                e->size += c[1];
        }
    }
    if (free_bytes)
        *free_bytes = free_blocks * CFS_FREE_UNIT;
    return n;
}

long rb_cfs_read(const uint8_t *flash, size_t len, const char *name,
                 uint8_t *out, size_t cap) {
    int pages = (int)(len / CFS_PAGE);
    int id = -1;
    for (int p = CFS_FIRST; p < pages && id < 0; p++) {
        const uint8_t *b = cfs_block(flash, len, p);
        if ((b[0] & 0x80) && be16(b + 4) == 0 &&
            strncmp((const char *)b + 7, name, 59) == 0)
            id = (int)be16(b + 2);
    }
    if (id < 0)
        return -1;
    /* parts in order; each part's data lands at the sum of the parts
     * before it, so walk part numbers until one is missing */
    size_t total = 0;
    for (unsigned part = 0;; part++) {
        const uint8_t *b = NULL;
        for (int p = CFS_FIRST; p < pages; p++) {
            const uint8_t *c = cfs_block(flash, len, p);
            if ((c[0] & 0x80) && (int)be16(c + 2) == id && be16(c + 4) == part) {
                b = c;
                break;
            }
        }
        if (!b)
            break;
        const uint8_t *data = b + (part == 0 ? 78 : 6);
        size_t n = b[1];
        if (part == 0 && n > 256 - 78) n = 256 - 78;
        if (part != 0 && n > 256 - 6) n = 256 - 6;
        if (total < cap)
            memcpy(out + total, data, total + n <= cap ? n : cap - total);
        total += n;
    }
    return (long)total;
}

/* ---- CyOS's serial console ---- */

enum { RB_CON_OP_CMD = 1, RB_CON_OP_PUT, RB_CON_OP_REBOOT };
enum { STEP_SEND, STEP_ACK, STEP_RUN, STEP_PROMPT };

#define ACK_MS     3000
#define IDLE_MS   20000

uint32_t rb_apps_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int rb_con_frame(const char *text, uint8_t *out, int cap) {
    uint32_t sum = 0;
    int n = 0;
    for (const char *p = text; *p && n < cap - 3; p++) {
        uint8_t ch = (uint8_t)*p;
        sum = ((sum ^ ch) << 1) | ((sum >> 16) & 1);
        out[n++] = ch;
    }
    out[n++] = '\n';
    out[n++] = (uint8_t)sum;
    out[n++] = (uint8_t)(sum >> 8);
    return n;
}

void rb_con_init(rb_con_t *c, int (*put)(void *, const uint8_t *, int), void *ctx) {
    memset(c, 0, sizeof *c);
    c->put = put;
    c->ctx = ctx;
}

static void con_queue(rb_con_t *c, const uint8_t *data, int n) {
    memcpy(c->pending, data, (size_t)n);
    c->pending_n = n;
    c->pending_off = 0;
}

static void con_flush(rb_con_t *c) {
    if (c->pending_off < c->pending_n)
        c->pending_off += c->put(c->ctx, c->pending + c->pending_off,
                                 c->pending_n - c->pending_off);
}

static void con_finish(rb_con_t *c, rb_con_state_t st, const char *msg) {
    c->state = st;
    if (msg)
        snprintf(c->result, sizeof c->result, "%s", msg);
}

static bool con_start(rb_con_t *c, int op, const char *line) {
    if (c->state == RB_CON_BUSY)
        return false;
    c->state = RB_CON_BUSY;
    c->op = (uint8_t)op;
    c->step = STEP_SEND;
    c->tries = 0;
    c->idle_ms = 0;
    c->rx_n = 0;
    c->rx[0] = 0;       /* or the last command's "receive successfull" ends this one */
    c->result[0] = 0;
    c->blocks_sent = 0;
    snprintf(c->line, sizeof c->line, "%s", line);
    return true;
}

bool rb_con_command(rb_con_t *c, const char *line) {
    return con_start(c, strcmp(line, "reboot") == 0 ? RB_CON_OP_REBOOT : RB_CON_OP_CMD, line);
}

bool rb_con_send_file(rb_con_t *c, const char *name, const uint8_t *data, uint32_t size) {
    char line[96];
    snprintf(line, sizeof line, "rcv %s %lu", name, (unsigned long)size);
    if (!con_start(c, RB_CON_OP_PUT, line))
        return false;
    c->data = data;
    c->size = size;
    return true;
}

void rb_con_rx(rb_con_t *c, uint8_t byte) {
    if (c->state != RB_CON_BUSY)
        return;
    c->idle_ms = 0;
    if (c->step == STEP_ACK && (byte == 0x01 || byte == 0x02)) {
        if (byte == 0x02) {
            c->step = STEP_RUN;
            c->rx_n = 0;
            c->rx[0] = 0;
        } else {
            c->step = STEP_SEND;     /* bad sum: send it again */
        }
        return;
    }
    if (byte == '\r' || c->step == STEP_ACK)
        return;
    if (c->rx_n >= (int)sizeof c->rx - 1) {   /* keep the newest half */
        memmove(c->rx, c->rx + sizeof c->rx / 2, sizeof c->rx / 2);
        c->rx_n -= (int)sizeof c->rx / 2;
    }
    c->rx[c->rx_n++] = (char)byte;
    c->rx[c->rx_n] = 0;
}

/* CyOS's request for block N: "sendN" at the start of a line. The echo
 * ">> rcv sender.app ..." must not pass for one. */
static char *find_send(char *rx) {
    for (char *m = strstr(rx, "send"); m; m = strstr(m + 1, "send"))
        if ((m == rx || m[-1] == '\n') && m[4] >= '0' && m[4] <= '9')
            return m;
    return NULL;
}

/* Drop what CyOS said up to and including the end of s */
static void con_consume(rb_con_t *c, const char *end) {
    int k = (int)(end - c->rx);
    memmove(c->rx, end, (size_t)(c->rx_n - k + 1));
    c->rx_n -= k;
}

static void con_send_block(rb_con_t *c, uint32_t n) {
    uint32_t off = n * 512;
    uint32_t len = off < c->size ? c->size - off : 0;
    if (len > 512) len = 512;
    uint8_t *b = c->pending;
    b[0] = 'C';
    b[1] = (uint8_t)n; b[2] = (uint8_t)(n >> 8);
    b[3] = (uint8_t)len; b[4] = (uint8_t)(len >> 8);
    memcpy(b + 5, c->data + off, len);
    uint32_t crc = rb_apps_crc32(0, b + 1, 4 + len);
    b[5 + len] = (uint8_t)crc;
    b[6 + len] = (uint8_t)(crc >> 8);
    b[7 + len] = (uint8_t)(crc >> 16);
    b[8 + len] = (uint8_t)(crc >> 24);
    c->pending_n = (int)(9 + len);
    c->pending_off = 0;
    if (n + 1 > c->blocks_sent)
        c->blocks_sent = n + 1;
}

void rb_con_poll(rb_con_t *c, uint32_t elapsed_ms) {
    if (c->state != RB_CON_BUSY)
        return;
    con_flush(c);
    c->idle_ms += elapsed_ms;

    if (c->step == STEP_SEND) {
        if (++c->tries > 3) {
            con_finish(c, RB_CON_FAILED, "CyOS did not take the command");
            return;
        }
        uint8_t f[100];
        c->step = STEP_ACK;     /* before sending: the answer may come at once */
        c->idle_ms = 0;
        con_queue(c, f, rb_con_frame(c->line, f, sizeof f));
        con_flush(c);
        return;
    }
    if (c->step == STEP_ACK) {
        if (c->idle_ms > ACK_MS)
            c->step = STEP_SEND;
        return;
    }
    if (c->op == RB_CON_OP_REBOOT) {   /* no prompt comes back */
        con_finish(c, RB_CON_DONE, "rebooting");
        return;
    }

    char *m;
    if (c->step == STEP_RUN && c->op == RB_CON_OP_PUT) {
        while ((m = find_send(c->rx)) != NULL) {
            char *nl = strchr(m, '\n');
            if (!nl)
                break;
            uint32_t n = (uint32_t)strtoul(m + 4, NULL, 10);
            con_consume(c, nl + 1);
            if (n * 512 > c->size && !(n == 0 && c->size == 0)) {
                con_finish(c, RB_CON_FAILED, "CyOS asked for a block past the end");
                return;
            }
            con_send_block(c, n);
            con_flush(c);
        }
        if ((m = strstr(c->rx, "receive successfull")) != NULL) {
            c->step = STEP_PROMPT;
        } else if (strstr(c->rx, "cancel\n") || strstr(c->rx, "* abort")) {
            con_finish(c, RB_CON_FAILED, "CyOS cancelled: no room, or a bad name");
            return;
        }
    }
    if ((c->step == STEP_RUN && c->op == RB_CON_OP_CMD) || c->step == STEP_PROMPT) {
        if ((m = strstr(c->rx, "Ready >")) != NULL) {
            *m = 0;
            /* the output is what follows CyOS's echo ">> line" */
            const char *out = c->rx;
            const char *echo = strstr(c->rx, ">> ");
            if (echo) {
                const char *nl = strchr(echo, '\n');
                out = nl ? nl + 1 : echo + strlen(echo);
            }
            while (*out == '\n') out++;
            size_t k = strlen(out);
            if (k > sizeof c->result - 1) k = sizeof c->result - 1;   /* long listings keep their start */
            memcpy(c->result, out, k);
            c->result[k] = 0;
            while (k && c->result[k - 1] == '\n') c->result[--k] = 0;
            con_finish(c, RB_CON_DONE, NULL);
            return;
        }
    }
    if (c->idle_ms > IDLE_MS)
        con_finish(c, RB_CON_FAILED, "CyOS stopped answering");
}

/* ---- the PC tool's command channel ---- */

enum { WAIT_NONE, WAIT_PUT_DATA, WAIT_CON };

static void link_say(rb_link_t *l, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#include <stdarg.h>
static void link_say(rb_link_t *l, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    l->send(l->ctx, (const uint8_t *)"@RB ", 4);
    l->send(l->ctx, (const uint8_t *)buf, (size_t)n);
}

void rb_link_init(rb_link_t *l, rb_con_t *con) {
    rb_con_t *keep = con;
    memset(l, 0, sizeof *l);
    l->con = keep;
}

bool rb_link_receiving(const rb_link_t *l) {
    return l->waiting == WAIT_PUT_DATA;
}

static bool name_ok(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 58) return false;
    for (; *s; s++)
        if (*s <= ' ' || *s == '"' || *s == '/' || *s == '\\' || *s > '~') return false;
    return true;
}

static void link_free(rb_link_t *l) {
    if (l->buf) {
        l->release(l->buf);
        l->buf = NULL;
    }
}

static void link_command(rb_link_t *l, char *line) {
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (strcmp(line, "info") == 0) {
        link_say(l, "OK machine Classic");
    } else if (strcmp(line, "ls") == 0) {
        size_t len = 0;
        const uint8_t *fl = l->flash(l->ctx, &len);
        static rb_cfs_entry_t list[256];
        uint32_t free_b = 0;
        int n = fl ? rb_cfs_list(fl, len, list, 256, &free_b) : 0;
        for (int i = 0; i < n; i++)
            link_say(l, "FILE %s %lu", list[i].name, (unsigned long)list[i].size);
        link_say(l, "OK free %lu", (unsigned long)free_b);
    } else if (strcmp(line, "get") == 0 && arg) {
        size_t len = 0;
        const uint8_t *fl = l->flash(l->ctx, &len);
        long size = fl ? rb_cfs_read(fl, len, arg, NULL, 0) : -1;
        uint8_t *buf = size >= 0 ? l->alloc(size ? (size_t)size : 1) : NULL;
        if (size < 0) {
            link_say(l, "ERR no file %s", arg);
        } else if (!buf) {
            link_say(l, "ERR no memory");
        } else {
            rb_cfs_read(fl, len, arg, buf, (size_t)size);
            link_say(l, "DATA %ld", size);
            l->send(l->ctx, buf, (size_t)size);
            l->release(buf);
            link_say(l, "OK");
        }
    } else if (strcmp(line, "put") == 0 && arg) {
        char *sz = strrchr(arg, ' ');
        unsigned long size = sz ? strtoul(sz + 1, NULL, 10) : 0;
        if (sz) *sz = 0;
        if (!sz || !name_ok(arg) || size > 600000) {
            link_say(l, "ERR bad put");
        } else if (l->con->state == RB_CON_BUSY) {
            link_say(l, "ERR busy");
        } else if (!(l->buf = l->alloc(size ? size : 1))) {
            link_say(l, "ERR no memory");
        } else {
            snprintf(l->name, sizeof l->name, "%s", arg);
            l->want = (uint32_t)size;
            l->got = 0;
            l->stall = 0;
            l->chunk_left = RB_LINK_CHUNK;
            l->waiting = WAIT_PUT_DATA;
            if (size == 0) {
                rb_con_send_file(l->con, l->name, l->buf, 0);
                l->waiting = WAIT_CON;
            } else {
                link_say(l, "MORE");
            }
        }
    } else if ((strcmp(line, "rm") == 0 && arg) || strcmp(line, "reboot") == 0 ||
               (strcmp(line, "cmd") == 0 && arg)) {
        char c[120];
        if (line[0] == 'r' && line[1] == 'm') {
            if (!name_ok(arg)) { link_say(l, "ERR bad name"); return; }
            snprintf(c, sizeof c, "rm -f \"%s\"", arg);
        } else if (line[0] == 'c') {
            if (strlen(arg) >= sizeof l->con->line) { link_say(l, "ERR command too long"); return; }
            snprintf(c, sizeof c, "%s", arg);
        } else {
            snprintf(c, sizeof c, "reboot");
        }
        if (!rb_con_command(l->con, c))
            link_say(l, "ERR busy");
        else
            l->waiting = WAIT_CON;
    } else {
        link_say(l, "ERR unknown command %s", line);
    }
}

void rb_link_rx(rb_link_t *l, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (l->waiting == WAIT_PUT_DATA) {
            size_t take = l->want - l->got;
            if (take > n - i) take = n - i;
            if (take > l->chunk_left) take = l->chunk_left;
            memcpy(l->buf + l->got, data + i, take);
            l->got += (uint32_t)take;
            l->stall = 0;
            i += take - 1;
            l->chunk_left -= (uint32_t)take;
            if (l->chunk_left == 0 && l->got < l->want) {
                l->chunk_left = RB_LINK_CHUNK;
                link_say(l, "MORE");
            }
            if (l->got == l->want) {
                rb_con_send_file(l->con, l->name, l->buf, l->want);
                l->waiting = WAIT_CON;
                l->last_progress = 0;
            }
            continue;
        }
        if (l->waiting == WAIT_CON)
            continue;       /* one thing at a time */
        char ch = (char)data[i];
        if (ch == RB_LINK_START) { l->cmd_n = 0; continue; }   /* a new command */
        if (ch == '\r') continue;
        if (ch == '\n') {
            l->cmd[l->cmd_n] = 0;
            l->cmd_n = 0;
            if (l->cmd[0])
                link_command(l, l->cmd);
        } else if (l->cmd_n < (int)sizeof l->cmd - 1) {
            l->cmd[l->cmd_n++] = ch;
        }
    }
}

void rb_link_poll(rb_link_t *l) {
    if (l->waiting == WAIT_PUT_DATA && ++l->stall > RB_LINK_STALL_FRAMES) {
        link_free(l);
        l->waiting = WAIT_NONE;
        link_say(l, "ERR the file stopped arriving");
        return;
    }
    if (l->waiting != WAIT_CON)
        return;
    rb_con_t *c = l->con;
    if (c->state == RB_CON_BUSY) {
        if (c->op == RB_CON_OP_PUT && c->blocks_sent != l->last_progress) {
            l->last_progress = c->blocks_sent;
            link_say(l, "PROGRESS %lu %lu", (unsigned long)c->blocks_sent,
                     (unsigned long)((c->size + 511) / 512));
        }
        return;
    }
    if (c->state == RB_CON_DONE) {
        if (c->op == RB_CON_OP_PUT && c->blocks_sent != l->last_progress)
            link_say(l, "PROGRESS %lu %lu", (unsigned long)c->blocks_sent,
                     (unsigned long)c->blocks_sent);
        if (c->op == RB_CON_OP_CMD) {
            char *s = c->result;
            while (*s) {
                char *nl = strchr(s, '\n');
                if (nl) *nl = 0;
                link_say(l, "OUT %s", s);
                if (!nl) break;
                s = nl + 1;
            }
        }
        link_say(l, "OK");
    } else {
        link_say(l, "ERR %s", c->result);
    }
    c->state = RB_CON_IDLE;
    link_free(l);
    l->waiting = WAIT_NONE;
}
