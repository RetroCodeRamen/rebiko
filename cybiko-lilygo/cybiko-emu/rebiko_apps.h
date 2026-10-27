#ifndef REBIKO_APPS_H
#define REBIKO_APPS_H

/*
 * Apps on and off a running Cybiko Classic, with no reboot.
 *
 * Into the Cybiko: CyOS's own serial console receives the file, the way
 * CyberLoad loaded it (rb_con_*). CyOS 1.2.46 lists a new app on its
 * desktop only after it restarts; "reboot" does that.
 * Out of the Cybiko: the emulator reads the file system straight from
 * the AT45 flash image (rb_cfs_*). CyOS's console cannot send files.
 *
 * rb_link_* is the command channel a PC tool (tools/rebiko_apps.py)
 * talks to: the Desktop over TCP, the T-Deck over USB serial.
 *
 * The console protocol (CyOS 1.2.46, read from its code):
 *  - A line from the PC is its text, "\n", then a 16-bit sum, low byte
 *    first: per character sum = ((sum ^ c) << 1) | bit 16 of sum, from 0.
 *    CyOS answers 0x02 (taken) or 0x01 (bad sum).
 *  - "rcv NAME SIZE" receives a file. CyOS asks for each block with
 *    "sendN"; a block is "C", N and the length (16 bits each, low byte
 *    first), up to 512 bytes, then a CRC-32 (zlib's) of N, the length and
 *    the data. CyOS answers "ack", or "crc err" / "wrong head" and asks
 *    again. A quoted name fails: CyOS keeps the quotes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the file system, read from the flash image ---- */

typedef struct {
    char name[60];
    uint32_t size;
} rb_cfs_entry_t;

/* Files on a Classic flash image (2048 pages of 264 bytes). Returns how
 * many, up to max; *free_bytes gets the room left, as CyOS counts it. */
int rb_cfs_list(const uint8_t *flash, size_t len, rb_cfs_entry_t *out, int max,
                uint32_t *free_bytes);
/* Copy a file's bytes into out (up to cap). Returns its size, or -1 if
 * there is no such file. */
long rb_cfs_read(const uint8_t *flash, size_t len, const char *name,
                 uint8_t *out, size_t cap);

/* ---- CyOS's serial console ---- */

typedef enum {
    RB_CON_IDLE,
    RB_CON_BUSY,
    RB_CON_DONE,
    RB_CON_FAILED,
} rb_con_state_t;

typedef struct {
    /* Host to Cybiko: queue bytes for CyOS, returns how many fit */
    int (*put)(void *ctx, const uint8_t *data, int n);
    void *ctx;

    rb_con_state_t state;
    uint8_t op;             /* RB_CON_OP_* */
    uint8_t step;
    uint8_t tries;
    uint32_t idle_ms;       /* since CyOS last said anything */
    char line[96];          /* the command being sent */
    const uint8_t *data;    /* a file being sent; the caller keeps it */
    uint32_t size;
    uint32_t blocks_sent;
    uint8_t pending[8 + 512 + 4];
    int pending_n, pending_off;
    char rx[1024];          /* what CyOS said since the command */
    int rx_n;
    char result[512];       /* command output, or what went wrong */
} rb_con_t;

void rb_con_init(rb_con_t *c, int (*put)(void *, const uint8_t *, int), void *ctx);
/* Every byte CyOS sends on its serial port */
void rb_con_rx(rb_con_t *c, uint8_t byte);
/* Once a frame, with the milliseconds since the last call */
void rb_con_poll(rb_con_t *c, uint32_t elapsed_ms);
/* Start a command, or a file (data must stay valid until done). False
 * while another is running. */
bool rb_con_command(rb_con_t *c, const char *line);
bool rb_con_send_file(rb_con_t *c, const char *name, const uint8_t *data, uint32_t size);
/* The checksummed line, for tools and tests: returns its length */
int rb_con_frame(const char *text, uint8_t *out, int cap);
uint32_t rb_apps_crc32(uint32_t crc, const uint8_t *p, size_t n);

/* ---- the PC tool's command channel ----
 *
 * Commands are text lines; replies are lines starting "@RB " so a tool
 * can pick them out of a log stream:
 *   ls                  @RB FILE name size ... then @RB OK free N
 *   get NAME            @RB DATA size, that many raw bytes, @RB OK
 *   put NAME SIZE       @RB MORE; send up to RB_LINK_CHUNK bytes per MORE
 *                       until SIZE are in, then @RB PROGRESS i n ... @RB OK.
 *                       A put that stalls RB_LINK_STALL_FRAMES is dropped.
 *   rm NAME             @RB OK
 *   reboot              @RB OK   (CyOS restarts; new apps show after)
 *   cmd TEXT            @RB OUT line ... then @RB OK
 *   info                @RB OK machine Classic
 * Any failure is "@RB ERR message". Over the T-Deck's USB, where other
 * bytes are keys, each command line starts with RB_LINK_START.
 */
#define RB_LINK_START 0x19
#define RB_LINK_CHUNK 1024      /* the T-Deck's USB buffer must hold one */
#define RB_LINK_STALL_FRAMES 300

typedef struct {
    rb_con_t *con;
    /* Reply bytes to the PC */
    void (*send)(void *ctx, const uint8_t *data, size_t n);
    void *ctx;
    /* The live flash image, for ls and get */
    const uint8_t *(*flash)(void *ctx, size_t *len);
    /* Room for a file being received (put); NULL frees */
    void *(*alloc)(size_t n);
    void (*release)(void *p);

    char cmd[160];
    int cmd_n;
    uint8_t *buf;           /* a put's bytes */
    uint32_t want, got;
    char name[60];
    uint8_t waiting;        /* RB_LINK_WAIT_* */
    uint32_t chunk_left;    /* bytes the PC may send before the next MORE */
    uint32_t stall;         /* frames since a put's last bytes */
    uint32_t last_progress;
} rb_link_t;

void rb_link_init(rb_link_t *l, rb_con_t *con);
/* Bytes from the PC */
void rb_link_rx(rb_link_t *l, const uint8_t *data, size_t n);
/* Once a frame, after rb_con_poll */
void rb_link_poll(rb_link_t *l);
/* True while a put is taking raw bytes, so the caller must not read
 * them as keys */
bool rb_link_receiving(const rb_link_t *l);

#ifdef __cplusplus
}
#endif

#endif
