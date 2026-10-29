/* ReBiko Desktop's chat bot: see rebiko_bot.h */
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601   /* getaddrinfo on MinGW; before any Windows header */
#endif
#include "rebiko_bot.h"
#include "radio_coproc.h"
#include "tdeck_keymap.h"
#include "rebiko_apps.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET rb_sock_t;
#define RB_BAD_SOCK INVALID_SOCKET
#define rb_close_sock closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int rb_sock_t;
#define RB_BAD_SOCK (-1)
#define rb_close_sock close
#endif

const rb_room_t RB_ROOM[RB_ROOMS] = {
    { "MAIN", 4 }, { "Games", 52 }, { "Music", 54 },
    { "Computers", 8 }, { "Sports", 10 }, { "Romantic", 6 },
};

/* ---- the in-app air ---- */

static void air_deliver(rb_air_t *a, int skip, const uint8_t *data, size_t len,
                        uint8_t channel, uint32_t sender) {
    for (int i = 0; i < RB_AIR_PORTS; i++) {
        if (i == skip) continue;
        if (a->heard) a->heard(a->heard_user, i, data, len, channel);
        if (a->port[i].fn) a->port[i].fn(a->port[i].fn_user, data, len, channel, sender);
    }
}

static int port_send(cybiko_radio_transport_t *self, const uint8_t *data, size_t len, uint8_t channel) {
    rb_air_port_t *p = self->impl;
    air_deliver(p->air, p->idx, data, len, channel, self->device_id);
    if (p->air->link)
        return p->air->link->send_packet(p->air->link, data, len, channel);
    return 0;
}

static void port_channel(cybiko_radio_transport_t *self, uint8_t channel) {
    rb_air_port_t *p = self->impl;
    if (p->idx == 0 && p->air->link)   /* the link records the user's channel */
        p->air->link->set_channel(p->air->link, channel);
}

static void port_listener(cybiko_radio_transport_t *self, cybiko_radio_on_packet_fn fn, void *user) {
    rb_air_port_t *p = self->impl;
    p->fn = fn;
    p->fn_user = user;
}

static int port_start(cybiko_radio_transport_t *self) { (void)self; return 0; }
static void port_close(cybiko_radio_transport_t *self) { (void)self; }
static void port_pump(cybiko_radio_transport_t *self) { (void)self; }   /* the main loop pumps the link */

void rb_air_init(rb_air_t *a, uint32_t id0, uint32_t id1) {
    memset(a, 0, sizeof *a);
    for (int i = 0; i < RB_AIR_PORTS; i++) {
        rb_air_port_t *p = &a->port[i];
        p->air = a;
        p->idx = i;
        p->base = (cybiko_radio_transport_t){ i == 0 ? id0 : id1, p, port_start, port_close,
                                              port_send, port_channel, port_listener, port_pump };
    }
}

cybiko_radio_transport_t *rb_air_port(rb_air_t *a, int i) { return &a->port[i].base; }

static void from_link(void *user, const uint8_t *data, size_t len, uint8_t channel, uint32_t sender) {
    air_deliver(user, -1, data, len, channel, sender);
}

void rb_air_set_link(rb_air_t *a, cybiko_radio_transport_t *link) {
    a->link = link;
    if (link && link->set_listener)
        link->set_listener(link, from_link, a);
}

/* ---- HTTP, enough for Ollama ---- */

static void net_up(void) {
#ifdef _WIN32
    static bool up;
    if (!up) {
        WSADATA w;
        up = WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }
#endif
}

/* "http://host:port", "host:port" or "host"; Ollama's port when none */
static bool parse_server(const char *server, char *host, size_t hcap, char *port, size_t pcap,
                         char *err, size_t ecap) {
    const char *s = server;
    while (*s == ' ') s++;
    if (strncmp(s, "https://", 8) == 0) {
        snprintf(err, ecap, "https is not supported; use http://");
        return false;
    }
    if (strncmp(s, "http://", 7) == 0) s += 7;
    size_t n = strcspn(s, ":/ ");
    if (n == 0 || n >= hcap) {
        snprintf(err, ecap, "No server address");
        return false;
    }
    memcpy(host, s, n);
    host[n] = 0;
    if (s[n] == ':') {
        size_t m = strspn(s + n + 1, "0123456789");
        if (m == 0 || m >= pcap) { snprintf(err, ecap, "Bad port"); return false; }
        memcpy(port, s + n + 1, m);
        port[m] = 0;
    } else {
        snprintf(port, pcap, "11434");
    }
    return true;
}

static rb_sock_t connect_to(const char *host, const char *port, char *err, size_t ecap) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        snprintf(err, ecap, "Cannot find %s", host);
        return RB_BAD_SOCK;
    }
    rb_sock_t sk = RB_BAD_SOCK;
    for (struct addrinfo *ai = res; ai && sk == RB_BAD_SOCK; ai = ai->ai_next) {
        sk = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sk == RB_BAD_SOCK) continue;
        /* connect with a 5 s limit, then reads and writes wait at most 120 s
         * (a model loading into memory takes a while) */
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(sk, FIONBIO, &nb);
#else
        fcntl(sk, F_SETFL, fcntl(sk, F_GETFL) | O_NONBLOCK);
#endif
        connect(sk, ai->ai_addr, (int)ai->ai_addrlen);
        fd_set w;
        FD_ZERO(&w);
        FD_SET(sk, &w);
        struct timeval tv = { 5, 0 };
        int soerr = 1;
        socklen_t sl = sizeof soerr;
        if (select((int)sk + 1, NULL, &w, NULL, &tv) <= 0 ||
            getsockopt(sk, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) != 0 || soerr != 0) {
            rb_close_sock(sk);
            sk = RB_BAD_SOCK;
            continue;
        }
#ifdef _WIN32
        nb = 0;
        ioctlsocket(sk, FIONBIO, &nb);
        DWORD ms = 120000;
        setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
        setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
        fcntl(sk, F_SETFL, fcntl(sk, F_GETFL) & ~O_NONBLOCK);
        struct timeval to = { 120, 0 };
        setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
        setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
#endif
    }
    freeaddrinfo(res);
    if (sk == RB_BAD_SOCK)
        snprintf(err, ecap, "No answer from %s:%s", host, port);
    return sk;
}

/* One request; *body_out is malloc'd. Returns the HTTP status, or -1. */
static int http(const char *server, const char *method, const char *path, const char *body,
                char **body_out, char *err, size_t ecap) {
    char host[96], port[8];
    *body_out = NULL;
    net_up();
    if (!parse_server(server, host, sizeof host, port, sizeof port, err, ecap))
        return -1;
    rb_sock_t sk = connect_to(host, port, err, ecap);
    if (sk == RB_BAD_SOCK)
        return -1;
    char head[512];
    size_t blen = body ? strlen(body) : 0;
    int hn = snprintf(head, sizeof head,
                      "%s %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      method, path, host, port, blen);
    if (send(sk, head, hn, 0) != hn || (blen && send(sk, body, (int)blen, 0) != (int)blen)) {
        snprintf(err, ecap, "Could not send to %s", host);
        rb_close_sock(sk);
        return -1;
    }
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
        if (n == cap) {
            if (cap >= 4u << 20) break;
            cap *= 2;
            buf = realloc(buf, cap + 1);
        }
        int r = recv(sk, buf + n, (int)(cap - n), 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    rb_close_sock(sk);
    buf[n] = 0;
    char *sep = strstr(buf, "\r\n\r\n");
    int status = 0;
    if (!sep || sscanf(buf, "HTTP/%*s %d", &status) != 1) {
        snprintf(err, ecap, "%s did not answer like Ollama", host);
        free(buf);
        return -1;
    }
    *sep = 0;
    char *b = sep + 4;
    /* chunked: join the chunks */
    if (strstr(buf, "chunked") || strstr(buf, "Chunked")) {
        char *in = b, *out = b;
        for (;;) {
            unsigned long k = strtoul(in, &in, 16);
            if (!k || !(in = strstr(in, "\r\n"))) break;
            in += 2;
            memmove(out, in, k);
            out += k;
            in += k + 2;
        }
        *out = 0;
    }
    *body_out = strdup(b);
    free(buf);
    return status;
}

/* The JSON string after "key": at or after from; escapes undone, non-ASCII
 * dropped */
static const char *json_string(const char *from, const char *key, char *out, size_t cap) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(from, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            c = e == 'n' ? '\n' : e == 't' ? ' ' : e == 'u' ? '?' : e;
            if (e == 'u') {   /* ’ and friends: an apostrophe is likeliest */
                unsigned u = 0;
                sscanf(p, "%4x", &u);
                p += 4;
                c = u == 0x2019 || u == 0x2018 ? '\'' : u == 0x201c || u == 0x201d ? '"' :
                    u == 0x2014 || u == 0x2013 ? '-' : u < 0x80 ? (char)u : ' ';
            }
        }
        if (n + 1 < cap && (unsigned char)c < 0x80) out[n++] = c;
    }
    out[n] = 0;
    return p;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c >= 0x20 && c < 0x80) out[n++] = (char)c;
    }
    out[n] = 0;
}

static int list_worker(void *arg) {
    rb_llm_list_t *job = arg;
    char *body = NULL;
    int st = http(job->server, "GET", "/api/tags", NULL, &body, job->err, sizeof job->err);
    if (st == 200 && body) {
        const char *p = body;
        while (job->n < RB_LLM_MAX_MODELS &&
               (p = json_string(p, "name", job->names[job->n], sizeof job->names[0])) != NULL)
            job->n++;
        if (!job->n)
            snprintf(job->err, sizeof job->err, "Connected, but the server has no models: "
                                                "ollama pull llama3.2:3b");
    } else if (st > 0) {
        snprintf(job->err, sizeof job->err, "The server answered HTTP %d", st);
    }
    free(body);
    SDL_MemoryBarrierRelease();
    job->done = true;
    return 0;
}

void rb_llm_list_start(rb_llm_list_t *job, const char *server) {
    memset(job, 0, sizeof *job);
    snprintf(job->server, sizeof job->server, "%s", server);
    job->thread = SDL_CreateThread(list_worker, "rebiko-llm-list", job);
    if (!job->thread) {
        snprintf(job->err, sizeof job->err, "Could not start a thread");
        job->done = true;
    }
}

void rb_llm_list_finish(rb_llm_list_t *job) {
    if (job->thread) SDL_WaitThread(job->thread, NULL);
    job->thread = NULL;
}

/* ---- Chat lines in frames ---- */

bool rb_chat_line(const uint8_t *f, size_t len, uint8_t *channel, uint32_t *from,
                  char *text, size_t cap) {
    /* header (8), 0xC0|channel, 0x28, ..., 0x25, the length, the text
     * (0x25 at 29 from CyOS 1.2.46) */
    if (len < 32 || (f[8] & 0xC0) != 0xC0 || f[9] != 0x28)
        return false;
    for (size_t i = 20; i < 44 && i + 3 <= len; i++) {
        size_t k = f[i + 1];
        if (f[i] != 0x25 || k == 0 || k > 120 || i + 2 + k > len || f[i + 2] < 0x20 || f[i + 2] >= 0x7F)
            continue;
        size_t n = 0;
        for (size_t j = 0; j < k && f[i + 2 + j]; j++)
            if (n + 1 < cap && f[i + 2 + j] >= 0x20 && f[i + 2 + j] < 0x7F)
                text[n++] = (char)f[i + 2 + j];
        text[n] = 0;
        if (!n) return false;
        *channel = f[8] & 0x3F;
        *from = (uint32_t)f[4] << 24 | (uint32_t)f[5] << 16 | (uint32_t)f[6] << 8 | f[7];
        return true;
    }
    return false;
}

void rb_bot_clean(const char *in, char *out, size_t max) {
    size_t n = 0;
    bool space = true;   /* no leading spaces */
    /* the first line, and not the quotes a model likes to wrap it in */
    while (*in == ' ' || *in == '"' || *in == '\n') in++;
    for (; *in && *in != '\n' && n < max; in++) {
        char c = *in;
        if (c == '\t') c = ' ';
        if (c == '"') continue;
        if (c == ' ') {
            if (!space) out[n++] = ' ';
            space = true;
            continue;
        }
        tdeck_key_event_t ev;
        if (!tdeck_map_ascii(c, &ev) || ev.needs_fn) continue;   /* not typeable with Shift alone */
        out[n++] = c;
        space = false;
    }
    if (n == max && *in && *in != '\n') {   /* too long: the last whole sentence, or word */
        size_t w = n;
        while (w > max / 3 && out[w - 1] != '.' && out[w - 1] != '!' && out[w - 1] != '?') w--;
        if (w > max / 3) {
            n = w;
        } else {
            w = n;
            while (w > max / 2 && out[w - 1] != ' ') w--;
            if (w > max / 2) n = w - 1;
        }
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '"')) n--;
    out[n] = 0;
}

int rb_bot_split(const char *in, char lines[][RB_LINE + 1], int max_lines) {
    char all[RB_LINE * 4 + 1];
    size_t cap = (size_t)RB_LINE * (size_t)max_lines;
    if (cap > sizeof all - 1) cap = sizeof all - 1;
    rb_bot_clean(in, all, cap);
    const char *p = all;
    int n = 0;
    while (*p && n < max_lines) {
        size_t k = strlen(p);
        if (k > RB_LINE) {   /* break at the last space that fits */
            k = RB_LINE;
            while (k > RB_LINE / 2 && p[k] != ' ') k--;
            if (p[k] != ' ') k = RB_LINE;
        }
        memcpy(lines[n], p, k);
        lines[n][k] = 0;
        n++;
        p += k;
        while (*p == ' ') p++;
    }
    return n;
}

/* ---- Wikipedia, for lines that start with '?' ---- */

/* The question in a Chat line, or NULL. The '?' may come second: a
 * Cybiko's first key into a filled-in line is sometimes kept ("x? ...") */
static const char *question(const char *line) {
    if (line[0] && line[0] != '?' && line[1] == '?') line++;
    if (line[0] != '?') return NULL;
    line++;
    while (*line == ' ') line++;
    return *line ? line : NULL;
}

/* The intros of the best Wikipedia articles for q, as "Title: text" lines.
 * Through the curl program: Wikipedia is HTTPS only. Everything but
 * letters, digits and -._~ is %-encoded, so nothing in q reaches the shell. */
static bool wiki_notes(const char *q, char *out, size_t cap, char *err, size_t ecap) {
    char enc[400], cmd[800];
    size_t n = 0;
    for (; *q && n + 4 < sizeof enc; q++) {
        unsigned char c = (unsigned char)*q;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') enc[n++] = (char)c;
        else n += (size_t)snprintf(enc + n, sizeof enc - n, "%%%02X", c);
    }
    enc[n] = 0;
    snprintf(cmd, sizeof cmd,
             "curl -s --max-time 12 -A \"ReBiko/1.0 (https://github.com/RetroCodeRamen/rebiko)\" "
             "\"https://en.wikipedia.org/w/api.php?action=query&generator=search&gsrsearch=%s"
             "&gsrlimit=2&prop=extracts&exintro=1&explaintext=1&exchars=700&redirects=1"
             "&format=json&formatversion=2\"", enc);
    FILE *f = popen(cmd, "r");
    if (!f) {
        snprintf(err, ecap, "Cannot run curl for Wikipedia");
        return false;
    }
    size_t bn = 0, bcap = 16384;
    char *body = malloc(bcap + 1);
    size_t r;
    while (body && (r = fread(body + bn, 1, bcap - bn, f)) > 0) {
        bn += r;
        if (bn == bcap) break;
    }
    pclose(f);
    if (!body || !bn) {
        snprintf(err, ecap, "No answer from Wikipedia (is curl installed, the PC online?)");
        free(body);
        return false;
    }
    body[bn] = 0;
    /* each page: "title", then "extract" */
    size_t o = 0;
    const char *p = body;
    char title[120], text[800];
    out[0] = 0;
    while ((p = json_string(p, "title", title, sizeof title)) != NULL) {
        const char *e = json_string(p, "extract", text, sizeof text);
        if (!e) break;
        o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "%s: %s\n", title, text);
        p = e;
    }
    free(body);
    if (!out[0]) {
        snprintf(err, ecap, "Wikipedia has nothing on that");
        return false;
    }
    return true;
}

/* ---- the bot ---- */

#define BOT_HISTORY 8
#define BOT_GAP_MS 2500      /* between one reply and the next */
/* A tap: Shift down BOT_SHIFT frames before the key and up BOT_SHIFT after
 * it, the key held BOT_HOLD frames, then all keys up BOT_UP frames
 * (BOT_UP_SAME before the same key again). Measured on CyOS 1.2.46 Chat
 * typing "hello Aa coffee Bb": all up 8 frames dropped letters ("cofee"),
 * 10 and 12 did not. Shift with the key, or up to 6 frames around it,
 * was now and then lost or stuck to the next letter ("astro", "CAfeine");
 * 10 typed "Hi A Tyler OK Dude Bb" and "So Cool, Japan Nagoya!" right. */
#define BOT_SHIFT 10
#define BOT_HOLD 12
#define BOT_UP 12
#define BOT_UP_SAME 20
#define BOT_DESKTOP_WAIT 900 /* from the console's first "Ready >" to the desktop */
#define BOT_FAST 3           /* frames a Desktop frame until it is in the room */
#define BOT_NICK_AT 0xB0     /* the nickname in res-finder.dat */
#define BOT_NICK_MAX 11
enum { PH_BOOT, PH_WALK, PH_LISTEN };
enum { ST_KEY, ST_WAIT, ST_CONSOLE, ST_ARRIVED };

typedef struct { uint8_t kind; int8_t col; uint16_t mask; bool shift; } bot_step_t;

struct rb_bot {
    cybiko_emu_t *emu;
    cybiko_radio_coproc_t cop;
    rb_con_t con;           /* CyOS's console, to start Chat */
    rb_air_t *air;
    uint32_t cyid;
    uint16_t keys[CYBIKO_KEYBOARD_COLUMNS];
    const uint16_t *user_keys;   /* the person's keys, while they are at it */
    void (*view)(void *ctx, const uint8_t *px, int w, int h);
    void *view_ctx;
    char save_path[4096];
    char server[128], model[64], nick[16];
    int room, phase, ready_frames;
    bool cyos_ready, cyos_46;
    char console_tail[32];
    bot_step_t q[400];
    int qh, qn;
    bot_step_t cur;
    bool cur_on;
    int at, len;            /* frame within the current step, its length */
    int8_t last_col;
    uint16_t last_mask;
    struct { bool mine; char text[160]; } hist[BOT_HISTORY];
    int hist_n;
    struct { uint32_t from; char text[160]; uint32_t at; } seen[8];
    int seen_i;
    bool pending;
    uint32_t next_ms;
    SDL_Thread *thread;
    SDL_atomic_t job_done;
    bool job_running;
    char job_body[9000];
    char job_q[160];        /* a '?' question: looked up on the worker */
    char job_out[600], job_err[160];
    char status[200], heard[200], said[200];
};

static void bot_keys(void *ctx, uint16_t *m, int n) {
    rb_bot_t *b = ctx;
    for (int i = 0; i < n && i < CYBIKO_KEYBOARD_COLUMNS; i++)
        m[i] = b->keys[i] | (b->user_keys ? b->user_keys[i] : 0);
}

static void bot_render(void *ctx, const uint8_t *px, int w, int h) {
    rb_bot_t *b = ctx;
    if (b->view) b->view(b->view_ctx, px, w, h);
}

void rb_bot_set_view(rb_bot_t *b, void (*fn)(void *ctx, const uint8_t *px, int w, int h),
                     void *ctx) {
    b->view = fn;
    b->view_ctx = ctx;
}

void rb_bot_set_user_keys(rb_bot_t *b, const uint16_t *keys) { b->user_keys = keys; }

/* CyOS's console output: the app link's replies, and whether CyOS is up */
static void bot_console(void *user, uint8_t byte) {
    rb_bot_t *b = user;
    char *t = b->console_tail;
    rb_con_rx(&b->con, byte);
    memmove(t, t + 1, sizeof b->console_tail - 2);
    t[sizeof b->console_tail - 2] = (char)byte;
    if (strstr(t, "OS version 1.2.4")) b->cyos_46 = true;
    else if (strstr(t, "Ready >")) b->cyos_ready = true;
}

static int bot_put(void *ctx, const uint8_t *data, int n) {
    return cybiko_serial_put(ctx, data, n);
}

static void bot_push(rb_bot_t *b, bot_step_t s) {
    if (b->qn < (int)(sizeof b->q / sizeof b->q[0]))
        b->q[(b->qh + b->qn++) % (int)(sizeof b->q / sizeof b->q[0])] = s;
}

static void push_key(rb_bot_t *b, int col, uint16_t mask, bool shift) {
    bot_push(b, (bot_step_t){ ST_KEY, (int8_t)col, mask, shift });
}

static void push_wait(rb_bot_t *b, int frames) {
    bot_push(b, (bot_step_t){ ST_WAIT, 0, (uint16_t)frames, false });
}

#define KEY_DOWN  TDECK_PAD_DOWN_COL, TDECK_PAD_DOWN_MASK
#define KEY_ENTER TDECK_ENTER_COL, TDECK_ENTER_MASK
#define KEY_BKSP  6, 0x04      /* the Classic's BkSp (tdeck_keymap.c) */
#define KEY_THROWAWAY 2, 0x40  /* x */

static void push_text(rb_bot_t *b, const char *t) {
    for (; *t; t++) {
        tdeck_key_event_t ev;
        if (tdeck_map_ascii(*t, &ev))
            push_key(b, ev.col, ev.mask, ev.needs_shift);
    }
    push_key(b, KEY_ENTER, false);
}

static void step_keys(rb_bot_t *b) {
    memset(b->keys, 0, sizeof b->keys);
    if (b->cur_on && b->at >= b->len)
        b->cur_on = false;
    if (!b->cur_on) {
        if (!b->qn) return;
        b->cur = b->q[b->qh];
        b->qh = (b->qh + 1) % (int)(sizeof b->q / sizeof b->q[0]);
        b->qn--;
        b->cur_on = true;
        b->at = 0;
        b->len = 0;
        if (b->cur.kind == ST_WAIT) {
            b->len = b->cur.mask;
        } else if (b->cur.kind == ST_CONSOLE) {
            if (!rb_con_command(&b->con, "chat_visual.app"))
                snprintf(b->status, sizeof b->status, "The bot's console is busy");
        } else if (b->cur.kind == ST_ARRIVED) {
            b->phase = PH_LISTEN;
            if (b->cop.current_channel != RB_ROOM[b->room].channel)
                snprintf(b->status, sizeof b->status, "Lost on the way to %s (channel %u)",
                         RB_ROOM[b->room].name, b->cop.current_channel);
            else
                snprintf(b->status, sizeof b->status, "%s is in %s, listening", b->nick,
                         RB_ROOM[b->room].name);
            if (getenv("CYBIKO_BOT_SAY"))   /* test hook: a line to type on arrival */
                push_text(b, getenv("CYBIKO_BOT_SAY"));
        } else {
            bool same = b->cur.col == b->last_col && b->cur.mask == b->last_mask;
            int s = b->cur.shift ? BOT_SHIFT : 0;
            b->len = s + BOT_HOLD + s + (same ? BOT_UP_SAME : BOT_UP);
            b->last_col = b->cur.col;
            b->last_mask = b->cur.mask;
        }
    }
    if (b->cur.kind == ST_KEY) {
        int s = b->cur.shift ? BOT_SHIFT : 0;
        if (b->at < s + BOT_HOLD + s && b->cur.shift)
            b->keys[TDECK_SHIFT_COL] |= TDECK_SHIFT_MASK;
        if (b->at >= s && b->at < s + BOT_HOLD)
            b->keys[b->cur.col] |= b->cur.mask;
    }
    b->at++;
}

/* Chat from the console (it opens on the room list), down to the room */
static void walk_to_room(rb_bot_t *b) {
    /* where each room is in Chat's list: 1.2.57 lists them as RB_ROOM,
     * 1.2.46 as MAIN, Romantic, Computers, Sports, Games, Music */
    static const int ROW_57[RB_ROOMS] = { 0, 1, 2, 3, 4, 5 };
    static const int ROW_46[RB_ROOMS] = { 0, 4, 5, 2, 3, 1 };
    bot_push(b, (bot_step_t){ ST_CONSOLE, 0, 0, false });
    push_wait(b, 300);
    int row = (b->cyos_46 ? ROW_46 : ROW_57)[b->room];
    for (int i = 0; i < row; i++) {
        push_key(b, KEY_DOWN, false);
        push_wait(b, 20);
    }
    push_key(b, KEY_ENTER, false);
    push_wait(b, 300);
    if (!b->cyos_46) {   /* 1.2.57 asks who to talk to: all */
        push_key(b, KEY_ENTER, false);
        push_wait(b, 120);
    }
    /* the first key typed only clears "Hi, everybody!"; BkSp takes it out
     * if it was typed after all */
    push_key(b, KEY_THROWAWAY, false);
    push_key(b, KEY_BKSP, false);
    push_wait(b, 30);
    bot_push(b, (bot_step_t){ ST_ARRIVED, 0, 0, false });
}

static void heard(void *user, int port, const uint8_t *data, size_t len, uint8_t channel) {
    rb_bot_t *b = user;
    uint8_t ch;
    uint32_t from;
    char text[160];
    (void)channel;
    if (port != 1 || b->phase != PH_LISTEN || !rb_chat_line(data, len, &ch, &from, text, sizeof text))
        return;
    if (ch != RB_ROOM[b->room].channel || from == b->cyid)
        return;
    /* CyOS sends each line three times */
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < 8; i++)
        if (b->seen[i].from == from && strcmp(b->seen[i].text, text) == 0 &&
            now - b->seen[i].at < 15000)
            return;
    b->seen[b->seen_i].from = from;
    b->seen[b->seen_i].at = now;
    snprintf(b->seen[b->seen_i].text, sizeof b->seen[0].text, "%s", text);
    b->seen_i = (b->seen_i + 1) % 8;
    if (b->hist_n == BOT_HISTORY) {
        memmove(b->hist, b->hist + 1, sizeof b->hist[0] * (BOT_HISTORY - 1));
        b->hist_n--;
    }
    b->hist[b->hist_n].mine = false;
    snprintf(b->hist[b->hist_n].text, sizeof b->hist[0].text, "%s", text);
    b->hist_n++;
    snprintf(b->heard, sizeof b->heard, "%s", text);
    b->pending = true;
}

/* The model's request for a looked-up question: the notes in, only the
 * question asked (the chat history would only pull it off them) */
static void lookup_body(rb_bot_t *b, const char *notes) {
    char sys[3200], esc[3600], qe[400];
    snprintf(sys, sizeof sys,
             "You are %s, a goofy 14-year-old in the year 2000, chatting on your Cybiko. Your "
             "Cybiko has a magic web link that just looked this up on Wikipedia:\n%s\n"
             "Answer the question using ONLY these notes, in %s's voice, in at most 2 short "
             "sentences (under %d characters in all). If the notes do not answer it, say your "
             "web link came up empty. Plain text: no emoji, no lists, no quotes.",
             b->nick, notes, b->nick, RB_LINE * RB_LOOKUP_LINES - 10);
    json_escape(sys, esc, sizeof esc);
    json_escape(b->job_q, qe, sizeof qe);
    snprintf(b->job_body, sizeof b->job_body,
             "{\"model\":\"%s\",\"stream\":false,\"keep_alive\":\"30m\","
             "\"options\":{\"num_predict\":90,\"temperature\":0.3},"
             "\"messages\":[{\"role\":\"system\",\"content\":\"%s\"},"
             "{\"role\":\"user\",\"content\":\"%s\"}]}",
             b->model, esc, qe);
}

static int chat_worker(void *arg) {
    rb_bot_t *b = arg;
    char *body = NULL;
    b->job_out[0] = b->job_err[0] = 0;
    if (b->job_q[0]) {
        char notes[2000];
        if (!wiki_notes(b->job_q, notes, sizeof notes, b->job_err, sizeof b->job_err)) {
            /* tell the room, in character */
            snprintf(b->job_out, sizeof b->job_out, "Dude, my web link came up empty on that one.");
            b->job_err[0] = 0;
            SDL_AtomicSet(&b->job_done, 1);
            return 0;
        }
        lookup_body(b, notes);
    }
    int st = http(b->server, "POST", "/api/chat", b->job_body, &body, b->job_err, sizeof b->job_err);
    if (st == 200 && body) {
        const char *m = strstr(body, "\"message\"");
        if (!m || !json_string(m, "content", b->job_out, sizeof b->job_out))
            snprintf(b->job_err, sizeof b->job_err, "No reply in the answer");
    } else if (st > 0) {
        char e[120] = "";
        if (body) json_string(body, "error", e, sizeof e);
        snprintf(b->job_err, sizeof b->job_err, "HTTP %d %s", st, e);
    }
    free(body);
    SDL_AtomicSet(&b->job_done, 1);
    return 0;
}

static void ask(rb_bot_t *b) {
    char esc[1200], sys[1100];
    snprintf(sys, sizeof sys,
             "You are %s, a goofy 14-year-old in the year 2000, chatting on your Cybiko (a wireless "
             "handheld) in its %s chat room. You love your N64, Pokemon cards, Surge soda, "
             "skateboarding badly, blink-182 and Tony Hawk's Pro Skater; you think the Y2K bug "
             "nearly got you; you say things like \"dude\", \"sweet\", \"no way\" and \"psych!\". "
             "You are friendly, a bit dorky, and you make silly jokes. You know nothing after 2001. "
             "Answer the last message in one short line of at most %d characters, like a text "
             "between friends. Plain text: no emoji, no lists, no quotes, no hashtags.",
             b->nick, RB_ROOM[b->room].name, RB_LINE - 5);
    json_escape(sys, esc, sizeof esc);
    int n = snprintf(b->job_body, sizeof b->job_body,
                     "{\"model\":\"%s\",\"stream\":false,\"keep_alive\":\"30m\","
                     "\"options\":{\"num_predict\":40,\"temperature\":0.9},"
                     "\"messages\":[{\"role\":\"system\",\"content\":\"%s\"}",
                     b->model, esc);
    for (int i = 0; i < b->hist_n && n < (int)sizeof b->job_body - 400; i++) {
        json_escape(b->hist[i].text, esc, sizeof esc);
        n += snprintf(b->job_body + n, sizeof b->job_body - (size_t)n,
                      ",{\"role\":\"%s\",\"content\":\"%s\"}",
                      b->hist[i].mine ? "assistant" : "user", esc);
    }
    snprintf(b->job_body + n, sizeof b->job_body - (size_t)n, "]}");
    const char *q = b->hist_n ? question(b->hist[b->hist_n - 1].text) : NULL;
    snprintf(b->job_q, sizeof b->job_q, "%s", q ? q : "");
    SDL_AtomicSet(&b->job_done, 0);
    b->thread = SDL_CreateThread(chat_worker, "rebiko-llm-chat", b);
    b->job_running = b->thread != NULL;
    b->pending = false;
    if (b->job_running && b->job_q[0])
        snprintf(b->status, sizeof b->status, "%s is looking up \"%.60s\" on Wikipedia...",
                 b->nick, b->job_q);
    else if (b->job_running)
        snprintf(b->status, sizeof b->status, "%s is thinking (%s)...", b->nick, b->model);
    else
        snprintf(b->status, sizeof b->status, "Could not start a thread");
}

static void answered(rb_bot_t *b) {
    SDL_WaitThread(b->thread, NULL);
    b->thread = NULL;
    b->job_running = false;
    b->next_ms = SDL_GetTicks() + BOT_GAP_MS;
    if (b->job_err[0]) {
        snprintf(b->status, sizeof b->status, "LLM: %s", b->job_err);
        return;
    }
    char lines[RB_LOOKUP_LINES][RB_LINE + 1], line[RB_LINE * RB_LOOKUP_LINES + 1] = "";
    int n = rb_bot_split(b->job_out, lines, b->job_q[0] ? RB_LOOKUP_LINES : 1);
    if (!n) {
        snprintf(b->status, sizeof b->status, "%s is in %s, listening (the reply had nothing to type)",
                 b->nick, RB_ROOM[b->room].name);
        return;
    }
    for (int i = 0; i < n; i++) {
        push_text(b, lines[i]);
        snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", i ? " " : "", lines[i]);
    }
    if (b->hist_n == BOT_HISTORY) {
        memmove(b->hist, b->hist + 1, sizeof b->hist[0] * (BOT_HISTORY - 1));
        b->hist_n--;
    }
    b->hist[b->hist_n].mine = true;
    snprintf(b->hist[b->hist_n].text, sizeof b->hist[0].text, "%s", line);
    b->hist_n++;
    snprintf(b->said, sizeof b->said, "%s", line);
    snprintf(b->status, sizeof b->status, "%s is in %s, listening", b->nick, RB_ROOM[b->room].name);
}

bool rb_bot_set_nick(uint8_t *flash, size_t len, const char *nick) {
    uint8_t name[BOT_NICK_MAX] = {0};
    memcpy(name, nick, strnlen(nick, BOT_NICK_MAX - 1));
    return rb_cfs_patch(flash, len, "res-finder.dat", BOT_NICK_AT, name, sizeof name);
}

rb_bot_t *rb_bot_start(const uint8_t *boot, size_t boot_n, const uint8_t *flash, size_t flash_n,
                       const char *save_path, uint32_t cyid, rb_air_t *air, const char *nick,
                       const char *server, const char *model, int room) {
    rb_bot_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->air = air;
    b->cyid = cyid;
    b->room = room < 0 || room >= RB_ROOMS ? 3 : room;
    b->last_col = -1;
    snprintf(b->save_path, sizeof b->save_path, "%s", save_path ? save_path : "");
    snprintf(b->nick, sizeof b->nick, "%s", nick);
    snprintf(b->server, sizeof b->server, "%s", server);
    snprintf(b->model, sizeof b->model, "%s", model);
    cybiko_hal_t hal = {0};
    hal.keyboard_poll = bot_keys;
    hal.render_frame = bot_render;
    hal.ctx = b;
    b->emu = cybiko_create(&hal);
    if (!b->emu || !cybiko_load_boot_rom(b->emu, boot, boot_n) ||
        !cybiko_load_flash_rom(b->emu, flash, flash_n)) {
        if (b->emu) cybiko_destroy(b->emu);
        free(b);
        return NULL;
    }
    cybiko_radio_coproc_init(&b->cop);
    cybiko_radio_coproc_set_transport(&b->cop, rb_air_port(air, 1));
    cybiko_attach_radio_id(b->emu, &b->cop, cyid);
    cybiko_reset(b->emu);
    cybiko_attach_serial(b->emu, bot_console, b);
    rb_con_init(&b->con, bot_put, b->emu);
    air->heard = heard;
    air->heard_user = b;
    snprintf(b->status, sizeof b->status, "%s's Cybiko is starting...", b->nick);
    return b;
}

void rb_bot_frame(rb_bot_t *b) {
    if (!b) return;
    int runs = b->phase == PH_LISTEN ? 1 : BOT_FAST;
    for (int i = 0; i < runs; i++) {
        if (b->phase == PH_BOOT && b->cyos_ready && ++b->ready_frames >= BOT_DESKTOP_WAIT) {
            b->phase = PH_WALK;
            snprintf(b->status, sizeof b->status, "%s is walking into %s...", b->nick,
                     RB_ROOM[b->room].name);
            walk_to_room(b);
        }
        step_keys(b);
        cybiko_run_frame(b->emu);
        rb_con_poll(&b->con, 16);
    }
    if (b->job_running && SDL_AtomicGet(&b->job_done))
        answered(b);
    if (b->phase == PH_LISTEN && b->pending && !b->job_running && !b->qn && !b->cur_on &&
        (int32_t)(SDL_GetTicks() - b->next_ms) >= 0)
        ask(b);
}

void rb_bot_stop(rb_bot_t *b) {
    if (!b) return;
    if (b->thread) SDL_WaitThread(b->thread, NULL);
    if (b->air->heard_user == b) {
        b->air->heard = NULL;
        b->air->heard_user = NULL;
    }
    b->air->port[1].fn = NULL;
    if (b->save_path[0]) {
        size_t n = 0;
        uint32_t w = 0;
        const uint8_t *img = cybiko_get_spi_flash(b->emu, &n, &w);
        FILE *f = img ? fopen(b->save_path, "wb") : NULL;
        if (f) {
            fwrite(img, 1, n, f);
            fclose(f);
        }
    }
    cybiko_destroy(b->emu);
    free(b);
}

const char *rb_bot_status(const rb_bot_t *b) { return b ? b->status : "Off"; }
const char *rb_bot_heard(const rb_bot_t *b) { return b ? b->heard : ""; }
const char *rb_bot_said(const rb_bot_t *b) { return b ? b->said : ""; }
