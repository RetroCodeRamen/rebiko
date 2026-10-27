/* PC Classic on a Heltec vs the flashed T-Deck: beacons, Nearby, Chat.
 *   v1_airchat boot.bin flash.bin --radio /dev/ttyUSB0 [--tdeck /dev/ttyACM0]
 *       [--cyid 4C000001] [--seconds 150]
 * Build with tools/v1_airchat.sh. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "core/emulator.h"
#include "radio_coproc.h"
#include "radio_lora.h"
#include "radio_serial_phy.h"

#define MARKER "pingair"
#define DUMP_DIR "/tmp/v1air"

typedef struct { int col; uint16_t mask; } vkey_t;

static const vkey_t K_ENTER = { 5, 0x04 };
static const vkey_t K_ESC   = { 0, 0x02 };
static const vkey_t K_RIGHT = { 4, 0x01 };

static const vkey_t K_TYPE[] = {
    { 8, 0x08 }, { 7, 0x10 }, { 6, 0x40 }, { 4, 0x20 },
    { 0, 0x20 }, { 7, 0x10 }, { 3, 0x10 }, { 5, 0x04 },
};

static uint16_t keys[CYBIKO_KEYBOARD_COLUMNS];
static const uint8_t *last_px;
static uint32_t dumps;
static int tdeck_fd = -1;
static uint32_t seen_rx, seen_tx;
static char peer_name[12];
static uint32_t peer_id;
static int beacons, chats_rx, chats_tx;
static int tdeck_idle;

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (!b || fread(b, 1, *n, f) != *n) { perror(p); exit(1); }
    fclose(f);
    return b;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void hal_keys(void *ctx, uint16_t *m, int n) {
    (void)ctx;
    int cols = n < CYBIKO_KEYBOARD_COLUMNS ? n : CYBIKO_KEYBOARD_COLUMNS;
    memcpy(m, keys, (size_t)cols * sizeof(uint16_t));
}

static void hal_render(void *ctx, const uint8_t *px, int w, int h) {
    (void)ctx; (void)w; (void)h;
    last_px = px;
}

static void radio_on_text(void *user, char type, const char *text) {
    (void)user;
    fprintf(stderr, "modem %s: %s\n", type == 'H' ? "hello" : "status", text);
}

static void clear_keys(void) {
    memset(keys, 0, sizeof keys);
}

static void hold(const vkey_t *k) {
    keys[k->col] |= k->mask;
}

static bool contains(const uint8_t *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (!m || m > n) return false;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0) return true;
    return false;
}

static void dump_pgm(const char *tag, int frame) {
    if (!last_px) return;
    char path[128];
    snprintf(path, sizeof path, DUMP_DIR "/%03u_%s_f%d.pgm", dumps, tag, frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n160 100\n255\n");
    fwrite(last_px, 1, 160 * 100, f);
    fclose(f);
    dumps++;
    printf("  dumped %s\n", path);
}

static void name_from_beacon(const uint8_t *d, size_t n, char *out, size_t cap) {
    out[0] = 0;
    if (n < 20) return;
    size_t i;
    for (i = 0; i < 8 && i + 1 < cap; i++) {
        char c = (char)d[12 + i];
        if (c < 0x20 || c > 0x7e) break;
        out[i] = c;
    }
    while (i > 0 && out[i - 1] == ' ') i--;
    out[i] = 0;
}

static void note_frame(const char *dir, uint32_t id, const uint8_t *d, size_t n) {
    if (n <= CYBIKO_RADIO_HDR) return;
    d += CYBIKO_RADIO_HDR;   /* past the address header */
    n -= CYBIKO_RADIO_HDR;
    uint8_t type = n > 1 ? d[1] : 0;
    char nick[12];
    name_from_beacon(d, n, nick, sizeof nick);
    if (type == 0x01 && nick[0]) {
        beacons++;
        if (strcmp(dir, "rx") == 0 && !peer_name[0]) {
            snprintf(peer_name, sizeof peer_name, "%s", nick);
            peer_id = id;
            printf("peer beacon CyID %08X nick '%s' (%zu bytes)\n",
                   (unsigned)id, nick, n);
        }
    } else if (type == 0x20) {
        if (strcmp(dir, "rx") == 0) chats_rx++;
        else chats_tx++;
        const char *txt = n > 27 ? (const char *)(d + 27) : "";
        printf("%s chat CyID %08X len %zu text '%.40s'\n", dir, (unsigned)id, n, txt);
    }
}

static void watch_coproc(cybiko_radio_coproc_t *c) {
    if (c->stat_rx > seen_rx) {
        for (int i = 0; i < c->q_count; i++) {
            const cybiko_radio_frame_t *f =
                &c->frames[(c->q_head + i) % CYBIKO_RADIO_RX_CAP];
            note_frame("rx", f->sender_id, f->data, f->len);
        }
        seen_rx = c->stat_rx;
    }
    if (c->stat_tx > seen_tx) {
        note_frame("tx", 0, c->last_tx,
                   c->last_tx_len < sizeof c->last_tx ? c->last_tx_len
                                                      : sizeof c->last_tx);
        seen_tx = c->stat_tx;
    }
}

static int tdeck_open(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "tdeck '%s': %s\n", path, strerror(errno));
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= (CLOCAL | CREAD);
        t.c_cflag &= ~HUPCL;
        tcsetattr(fd, TCSANOW, &t);
    }
    int m = 0;
    if (ioctl(fd, TIOCMGET, &m) == 0) {
        m &= ~(TIOCM_DTR | TIOCM_RTS);
        ioctl(fd, TIOCMSET, &m);
    }
    return fd;
}

static void tdeck_poll(void) {
    if (tdeck_fd < 0) return;
    char buf[512];
    ssize_t n = read(tdeck_fd, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    fwrite(buf, 1, (size_t)n, stderr);
    if (strstr(buf, "loading phase over") || strstr(buf, " running"))
        tdeck_idle = 1;
    if (strstr(buf, "SX1262 begin") || strstr(buf, "emulator started"))
        tdeck_idle = 0;
}

static void tdeck_byte(uint8_t c) {
    if (tdeck_fd < 0) return;
    if (write(tdeck_fd, &c, 1) == 1)
        tcdrain(tdeck_fd);
}

static void tdeck_esc(void) {
    tdeck_byte('$');
    tdeck_byte('x');
}

static void report(int frame, cybiko_emu_t *e, const cybiko_radio_coproc_t *c,
                   const cybiko_lora_t *lt, const cybiko_serial_phy_t *sp) {
    (void)c;
    cybiko_v1_radio_status_t st;
    cybiko_v1_radio_status(e, &st);
    printf("frame %5d: radio %s  guest sent %u delivered %u  "
           "lora tx %u rx %u  rssi %d  peer '%s' beacons %d chat rx %d tx %d  tdeck %s\n",
           frame,
           st.radio_up ? "up" : "-",
           st.tx_frames, st.rx_injected,
           lt->stats.tx_pkts, lt->stats.rx_msgs,
           sp->stats.last_rssi_dbm, peer_name[0] ? peer_name : "-",
           beacons, chats_rx, chats_tx,
           tdeck_fd < 0 ? "none" : (tdeck_idle ? "idle" : "boot"));
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *boot_path = NULL, *flash_path = NULL, *radio_path = NULL;
    const char *tdeck_path = "/dev/ttyACM0";
    uint32_t cyid = 0x4C000001u;
    int seconds = 180;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--radio") && i + 1 < argc) radio_path = argv[++i];
        else if (!strcmp(argv[i], "--tdeck") && i + 1 < argc) tdeck_path = argv[++i];
        else if (!strcmp(argv[i], "--cyid") && i + 1 < argc)
            cyid = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!boot_path) boot_path = argv[i];
        else if (!flash_path) flash_path = argv[i];
        else {
            fprintf(stderr, "unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!boot_path || !flash_path || !radio_path) {
        fprintf(stderr, "usage: %s boot.bin flash.bin --radio /dev/ttyUSB0 "
                "[--tdeck /dev/ttyACM0] [--seconds 150]\n", argv[0]);
        return 2;
    }
    if (mkdir(DUMP_DIR, 0777) != 0 && errno != EEXIST)
        fprintf(stderr, "mkdir %s: %s\n", DUMP_DIR, strerror(errno));

    if (tdeck_path && tdeck_path[0]) {
        tdeck_fd = tdeck_open(tdeck_path);
        if (tdeck_fd >= 0)
            fprintf(stderr, "T-Deck keys+log on %s\n", tdeck_path);
    }

    size_t bn, fn;
    uint8_t *boot = slurp(boot_path, &bn), *flash = slurp(flash_path, &fn);
    cybiko_hal_t hal = {0};
    hal.keyboard_poll = hal_keys;
    hal.render_frame = hal_render;
    cybiko_emu_t *emu = cybiko_create(&hal);
    if (!emu || !cybiko_load_boot_rom(emu, boot, bn) ||
        !cybiko_load_flash_rom(emu, flash, fn)) {
        fprintf(stderr, "failed to create emulator or load ROMs\n");
        return 1;
    }
    free(boot);
    free(flash);

    static cybiko_serial_phy_t phy;
    static cybiko_lora_t link;
    static cybiko_radio_coproc_t cop;
    if (cybiko_serial_phy_open(&phy, radio_path) != 0) {
        fprintf(stderr, "radio '%s': %s\n", radio_path, strerror(errno));
        return 1;
    }
    phy.on_text = radio_on_text;
    cybiko_lora_init(&link, &phy.phy, cyid, true);
    cybiko_lora_seed_msg_id(&link, (uint32_t)time(NULL) ^ (uint32_t)getpid());
    cybiko_radio_coproc_init(&cop);
    cybiko_radio_coproc_set_transport(&cop, &link.base);
    cybiko_attach_radio_id(emu, &cop, cyid);
    cybiko_serial_phy_request_status(&phy);
    cybiko_reset(emu);
    fprintf(stderr, "Classic on %s, CyID %08X, %d s at 60 fps, marker %s\n",
            radio_path, (unsigned)cyid, seconds, MARKER);

    /* 0 wait desktop, 1 Esc, 2 Right-Right-Enter Chat, 3 wait room list,
     * 4 Enter MAIN, 5 wait compose, 6 type MARKER, 7 settle. */
    int phase = 0, phase_frame = 0, tdeck_step = 0;
    uint64_t tdeck_next = 0;
    bool ram_nick = false, ram_marker = false, typed = false, saw_rooms = false;
    uint64_t start = now_ms(), next = start, last_rep = start;
    int frame = 0;
    while ((int)(now_ms() - start) < seconds * 1000) {
        uint64_t t = now_ms();
        tdeck_poll();
        clear_keys();

        int since = frame - phase_frame;
        if (phase == 1) {
            if (since / 20 < 2 && since % 20 < 6) hold(&K_ESC);
            if (since >= 50) {
                phase = 2;
                phase_frame = frame;
                printf("frame %d: seeking Chat (You & Me -> right right enter)\n", frame);
                dump_pgm("desk", frame);
            }
        } else if (phase == 2) {
            const vkey_t seq[] = { K_RIGHT, K_RIGHT, K_ENTER };
            int slot = since / 28;
            if (slot < 3 && since % 28 < 8) hold(&seq[slot]);
            if (since >= 28 * 3 + 20) {
                phase = 3;
                phase_frame = frame;
                dump_pgm("after_enter", frame);
            }
        } else if (phase == 3) {
            if (since == 60) dump_pgm("splash", frame);
            if (saw_rooms || since >= 400) {
                phase = 4;
                phase_frame = frame;
                printf("frame %d: Chat rooms, opening MAIN\n", frame);
                dump_pgm("rooms", frame);
            }
        } else if (phase == 4) {
            if (since < 8) hold(&K_ENTER);
            if (since >= 40) {
                phase = 5;
                phase_frame = frame;
                dump_pgm("main", frame);
            }
        } else if (phase == 5) {
            if (since >= 90) {
                phase = 6;
                phase_frame = frame;
                printf("frame %d: typing %s\n", frame, MARKER);
            }
        } else if (phase == 6) {
            int slot = since / 16;
            int nkeys = (int)(sizeof K_TYPE / sizeof K_TYPE[0]);
            if (slot < nkeys && since % 16 < 6) hold(&K_TYPE[slot]);
            if (slot >= nkeys) {
                typed = true;
                phase = 7;
                phase_frame = frame;
                dump_pgm("typed", frame);
                printf("frame %d: typed %s\n", frame, MARKER);
            }
        }

        if (tdeck_fd >= 0 && t >= tdeck_next) {
            /* Esc, wait for desktop, Right Right Enter into Chat, wait splash,
             * Enter MAIN, wait compose. 1/2/3 = pause. */
            static const uint8_t steps[] = {
                0, 0, 1, 1, 1, 1, 0x14, 0x14, '\n',
                3, 3, 3, 3, 3, 3, 3, 3, '\n', 3, 3, 3
            };
            if ((tdeck_idle || t - start > 40000) && tdeck_step < (int)sizeof steps) {
                uint8_t s = steps[tdeck_step++];
                if (s == 0) tdeck_esc();
                else if (s == 0x14 || s == '\n') tdeck_byte(s);
                tdeck_next = t + (s <= 3 ? 700 : 400);
            }
        }

        cybiko_run_frame(emu);
        link.base.pump(&link.base);
        watch_coproc(&cop);
        frame++;

        size_t ram_n = 0;
        const uint8_t *ram = cybiko_get_nvram(emu, &ram_n);
        if (!ram_nick && peer_name[0] && ram && contains(ram, ram_n, peer_name)) {
            ram_nick = true;
            printf("frame %d: CyOS RAM holds peer nick '%s'\n", frame, peer_name);
        }
        if (!ram_marker && ram && contains(ram, ram_n, MARKER)) {
            ram_marker = true;
            printf("frame %d: CyOS RAM holds %s\n", frame, MARKER);
        }
        if (!saw_rooms && ram && contains(ram, ram_n, "Romantic")) {
            saw_rooms = true;
            printf("frame %d: Chat room list in RAM\n", frame);
        }

        if (phase == 0) {
            cybiko_v1_radio_status_t st;
            cybiko_v1_radio_status(emu, &st);
            int desk = ram && (contains(ram, ram_n, "You & Me") ||
                               contains(ram, ram_n, "Main Desktop"));
            if (st.radio_up && st.rx_injected > 0 && st.tx_frames > 0 &&
                (desk || frame > 7000)) {
                phase = 1;
                phase_frame = frame;
                printf("frame %d: radio up, desktop%s\n", frame, desk ? "" : " timeout");
                dump_pgm("radio_up", frame);
            }
        }

        t = now_ms();
        if (t - last_rep >= 5000) {
            report(frame, emu, &cop, &link, &phy);
            last_rep = t;
        }
        next += 16;
        if (next > t) usleep((useconds_t)((next - t) * 1000));
        else next = t;
    }

    dump_pgm("final", frame);
    report(frame, emu, &cop, &link, &phy);
    cybiko_v1_radio_status_t st;
    cybiko_v1_radio_status(emu, &st);
    int air = cop.stat_rx > 0 && st.rx_injected > 0 && link.stats.tx_pkts > 0;
    int nearby = peer_name[0] && (ram_nick || st.rx_injected > 2);
    int chat = chats_rx > 0 || ram_marker;
    printf("air %s  nearby %s (nick '%s' ram %s)  chat %s (rx %d ram_marker %s typed %s)\n",
           air ? "PASS" : "FAIL",
           nearby ? "PASS" : "FAIL", peer_name[0] ? peer_name : "-",
           ram_nick ? "yes" : "no",
           chat ? "PASS" : "FAIL",
           chats_rx, ram_marker ? "yes" : "no", typed ? "yes" : "no");
    int ok = air && nearby;
    printf(ok ? "PASS: CyOS frames crossed and the PC saw the T-Deck\n"
              : "FAIL: need two-way frames and a peer beacon\n");
    if (air && nearby && !chat)
        printf("note: Nearby is up; Chat UI/line not confirmed (screens in %s)\n", DUMP_DIR);
    cybiko_serial_phy_close(&phy);
    if (tdeck_fd >= 0) close(tdeck_fd);
    return ok ? 0 : 1;
}
