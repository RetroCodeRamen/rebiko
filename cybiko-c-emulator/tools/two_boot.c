/*
 * Two headless XT emulators on the in-process radio.
 *
 *   ./build/two_boot boot.bin flash.bin [--frames N]
 *
 * boot.bin is cyrom150.bin (32 KB). flash.bin is cyos_v1508.bin (512 KB).
 * Each side gets a different CyID, patched the way the Java emulator
 * patches flash offset 0x7F818. This does not flash a T-Deck.
 */
#include "core/cfs.h"
#include "core/emulator.h"
#include "radio/radio_coproc.h"
#include "radio/radio_sim.h"

extern uint32_t cybiko_sci2_reg_writes;
extern uint32_t cybiko_sci2_dtc_runs;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ID_A 0x10000001u
#define ID_B 0x10000002u
#define MARKER "pingdeck"

static const struct {
    uint32_t hash;
    const char *name;
} k_phases[] = {
    { 0x65232E60u, "boot logo" },
    { 0xAA3312CCu, "loading CyOS" },
    { 0xC0DBEF72u, "animated logo" },
    { 0xFAEDD600u, "welcome" },
    { 0x6C55765Au, "home" },
    { 0xB5EB2FA8u, "home" },
};

static const char *phase_name(uint32_t hash) {
    for (size_t i = 0; i < sizeof k_phases / sizeof k_phases[0]; i++) {
        if (k_phases[i].hash == hash)
            return k_phases[i].name;
    }
    return "unknown";
}

static bool ui_hash(uint32_t hash) {
    return hash == 0xFAEDD600u || hash == 0x6C55765Au || hash == 0xB5EB2FA8u;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

typedef struct {
    char buf[2048];
    size_t len;
    uint16_t keys[15];
} side_t;

static void on_serial(void *ctx, uint8_t byte) {
    side_t *s = ctx;
    if (s->len + 1 < sizeof s->buf)
        s->buf[s->len++] = (char)byte;
}

static void on_keys(void *ctx, uint16_t *matrix, int num_columns) {
    side_t *s = ctx;
    int n = num_columns < 15 ? num_columns : 15;
    memcpy(matrix, s->keys, (size_t)n * sizeof(uint16_t));
}

static void clear_keys(side_t *s) {
    memset(s->keys, 0, sizeof s->keys);
}

static void set_key(side_t *s, int column, uint16_t bit, bool pressed) {
    if (pressed)
        s->keys[column] |= bit;
    else
        s->keys[column] &= (uint16_t)~bit;
}

static bool nickname_hash(uint32_t hash) {
    return hash == 0xD0B10E57u || hash == 0x2190D7EEu;
}

static bool gender_hash(uint32_t hash) {
    return hash == 0x811F5093u || hash == 0xF3277345u;
}

static bool desktop_hash(uint32_t hash) {
    return hash == 0x14F09E18u || hash == 0x6C55765Au || hash == 0xB5EB2FA8u ||
           hash == 0x6682B63Cu || hash == 0xBF3CEFCEu ||
           hash == 0x37B9C29Cu || hash == 0xEE079B6Eu;
}

/* Del seven times, type "deck", Tab to OK, Enter. Then M, Tab, Enter. */
static const struct {
    int column;
    uint16_t bit;
} k_nick_keys[] = {
    { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 },
    { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 },
    { 2, 0x0100 }, { 3, 0x0080 }, { 2, 0x0200 }, { 0, 0x0800 },
    { 5, 0x0080 }, { 4, 0x0008 },
};

/* Desktop grid: Profiles -> Right, Down, Down -> System, Enter. */
static const struct {
    int column;
    uint16_t bit;
} k_system_keys[] = {
    { 6, 0x1000 }, { 6, 0x2000 }, { 6, 0x2000 }, { 4, 0x0008 },
    { 4, 0x0008 }, { 4, 0x0010 },
};

/* Desktop: Profiles -> Right -> Chat, Enter. */
static const struct {
    int column;
    uint16_t bit;
} k_chat_icon[] = {
    { 6, 0x1000 }, { 4, 0x0008 },
};

static const struct {
    int column;
    uint16_t bit;
} k_gender_keys[] = {
    { 0, 0x0100 },
    { 5, 0x0080 },
    { 4, 0x0008 },
};

static void dump_pgm(const char *path, const uint8_t *vram) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P5\n160 100\n255\n");
    uint8_t row[160];
    for (int y = 0; y < 100; y++) {
        int off = (99 - y) * 40;
        for (int x = 0; x < 160; x += 4) {
            uint8_t data = vram[off + x / 4];
            row[x]     = (uint8_t)(255 - ((data >> 6) & 3) * 85);
            row[x + 1] = (uint8_t)(255 - ((data >> 4) & 3) * 85);
            row[x + 2] = (uint8_t)(255 - ((data >> 2) & 3) * 85);
            row[x + 3] = (uint8_t)(255 - (data & 3) * 85);
        }
        fwrite(row, 1, 160, f);
    }
    fclose(f);
}

/* Returns true once every key in the list has been pressed and released. */
static bool play_keys_timed(side_t *a, side_t *b, const void *table, int nkeys,
                            int since, int slot_frames) {
    const struct { int column; uint16_t bit; } *keys = table;
    int slot = since / slot_frames;
    int pos = since % slot_frames;
    if (slot >= 0 && slot < nkeys && pos < 6) {
        set_key(a, keys[slot].column, keys[slot].bit, true);
        set_key(b, keys[slot].column, keys[slot].bit, true);
    }
    return slot >= nkeys;
}

static bool play_keys(side_t *a, side_t *b, const void *table, int nkeys, int since) {
    return play_keys_timed(a, b, table, nkeys, since, 16);
}

static bool play_keys_one(side_t *s, const void *table, int nkeys, int since) {
    const struct { int column; uint16_t bit; } *keys = table;
    int slot = since / 16;
    int pos = since % 16;
    if (slot >= 0 && slot < nkeys && pos < 6)
        set_key(s, keys[slot].column, keys[slot].bit, true);
    return slot >= nkeys;
}

/* Del the prefilled chat line, type pingdeck, Enter. A only. */
static const struct {
    int column;
    uint16_t bit;
} k_chat_type[] = {
    { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 },
    { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 },
    { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 },
    { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 }, { 5, 0x0100 },
    { 9, 0x0010 }, { 0, 0x1000 }, { 1, 0x0008 }, { 1, 0x0002 },
    { 2, 0x0100 }, { 3, 0x0080 }, { 2, 0x0200 }, { 0, 0x0800 },
    { 4, 0x0008 },
};

static bool contains(const uint8_t *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || m > n)
        return false;
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0)
            return true;
    }
    return false;
}

static void dump_settings(const uint8_t *nvram, size_t n, const char *tag) {
    if (!nvram || n < CFS_IMAGE_SIZE)
        return;
    cfs_image_t *img = malloc(sizeof *img);
    if (!img)
        return;
    memcpy(img->data, nvram, CFS_IMAGE_SIZE);
    uint8_t body[64];
    size_t len = 0;
    if (!cfs_read_file(img, "settings.dat", body, sizeof body, &len) || len < 42) {
        printf("%s: settings.dat unreadable\n", tag);
        free(img);
        return;
    }
    printf("%s: settings b12=%02X b13=%02X b41=%02X  first:", tag,
           body[12], body[13], body[41]);
    size_t show = len < 16 ? len : 16;
    for (size_t i = 0; i < show; i++)
        printf(" %02X", body[i]);
    printf("\n");
    free(img);
}

/* Mark the wizard done and turn wireless on in the guest save, then reboot. */
static bool enable_radio_save(cybiko_emu_t *emu) {
    size_t n = 0;
    uint8_t *nvram = cybiko_nvram(emu, &n);
    if (!nvram || n < CFS_IMAGE_SIZE)
        return false;
    cfs_image_t *img = malloc(sizeof *img);
    if (!img)
        return false;
    memcpy(img->data, nvram, CFS_IMAGE_SIZE);
    bool ok = cfs_poke_file(img, "settings.dat", 12, 0x01) &&
              cfs_poke_file(img, "settings.dat", 13, 0x00) &&
              cfs_poke_file(img, "settings.dat", 41, 0x01);
    if (ok)
        memcpy(nvram, img->data, CFS_IMAGE_SIZE);
    free(img);
    return ok;
}

static void print_radio_object(const cybiko_emu_t *emu, int frame) {
    size_t rlen = 0;
    const uint8_t *ram = cybiko_get_nvram(emu, &rlen);
    size_t off = 0x4B49FEu - 0x400000u;
    if (!ram || off + 4 > rlen)
        return;
    uint32_t obj = ((uint32_t)ram[off] << 24) | ((uint32_t)ram[off + 1] << 16) |
                   ((uint32_t)ram[off + 2] << 8) | ram[off + 3];
    int state = -1;
    if (obj >= 0x400000u && obj < 0x600000u) {
        size_t so = (obj - 0x400000u) + 0x335Au;
        if (so < rlen)
            state = ram[so];
    }
    printf("frame %d: B radio object %08X state %d\n", frame, obj, state);
}

int main(int argc, char **argv) {
    const char *boot_path = NULL;
    const char *flash_path = NULL;
    int frames = 600;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (!boot_path) {
            boot_path = argv[i];
        } else if (!flash_path) {
            flash_path = argv[i];
        } else {
            fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!boot_path || !flash_path || frames <= 0) {
        fprintf(stderr, "usage: %s boot.bin flash.bin [--frames N]\n", argv[0]);
        return 2;
    }

    size_t boot_len = 0, flash_len = 0;
    uint8_t *boot = read_file(boot_path, &boot_len);
    uint8_t *flash = read_file(flash_path, &flash_len);
    if (!boot || !flash) {
        free(boot);
        free(flash);
        return 2;
    }
    printf("boot %zu bytes, flash %zu bytes, %d frames\n", boot_len, flash_len, frames);
    printf("CyID A=0x%08X B=0x%08X\n", ID_A, ID_B);

    side_t side_a, side_b;
    memset(&side_a, 0, sizeof side_a);
    memset(&side_b, 0, sizeof side_b);
    cybiko_hal_t hal_a, hal_b;
    memset(&hal_a, 0, sizeof hal_a);
    memset(&hal_b, 0, sizeof hal_b);
    hal_a.serial_output = on_serial;
    hal_a.keyboard_poll = on_keys;
    hal_a.ctx = &side_a;
    hal_b.serial_output = on_serial;
    hal_b.keyboard_poll = on_keys;
    hal_b.ctx = &side_b;

    cybiko_emu_t *a = cybiko_create(&hal_a);
    cybiko_emu_t *b = cybiko_create(&hal_b);
    if (!a || !b) {
        fprintf(stderr, "emulator alloc failed\n");
        return 1;
    }
    cybiko_load_boot_rom(a, boot, boot_len);
    cybiko_load_boot_rom(b, boot, boot_len);
    cybiko_load_flash_rom(a, flash, flash_len);
    cybiko_load_flash_rom(b, flash, flash_len);
    if (!cybiko_patch_cyid(a, ID_A) || !cybiko_patch_cyid(b, ID_B)) {
        fprintf(stderr, "CyID patch failed (flash shorter than 512 KB)\n");
        return 1;
    }

    radio_sim_bus_t bus;
    radio_sim_node_t node_a, node_b;
    cybiko_radio_coproc_t cop_a, cop_b;
    radio_sim_bus_init(&bus);
    radio_sim_node_init(&node_a, &bus, ID_A, 255);
    radio_sim_node_init(&node_b, &bus, ID_B, 255);
    radio_sim_bus_add(&bus, &node_a);
    radio_sim_bus_add(&bus, &node_b);
    cybiko_radio_coproc_init(&cop_a);
    cybiko_radio_coproc_init(&cop_b);
    cybiko_radio_coproc_set_transport(&cop_a, &node_a.base);
    cybiko_radio_coproc_set_transport(&cop_b, &node_b.base);
    cybiko_attach_radio(a, &cop_a);
    cybiko_attach_radio(b, &cop_b);
    cybiko_reset(a);
    cybiko_reset(b);

    bool injected = false;
    bool ui_a = false, ui_b = false;
    bool saw_ui = false;
    bool dumped_radio = false;
    int welcome_frame = 0;
    int phase_frame = 0;
    int state_hist[16];
    memset(state_hist, 0, sizeof state_hist);
    int state_other = 0;
    int state1_first = 0, state1_last = 0, state1_after = 0;
    int dumps = 0;
    uint32_t sci2_seen = 0;
    bool radio_save_patched = false;
    int last_pending_b = 0;
    uint32_t last_tx_a = 0;
    uint32_t prev_b = 0;
    uint32_t last_obj = 0xFFFFFFFFu;
    /* 0 idle, 1 Enter through OK pages, 2 nickname keys, 3 wait, 4 gender keys, 5 later Enter. */
    int key_phase = 0;
    uint32_t hash_a = 0, hash_b = 0;
    double t0 = now_sec();
    for (int frame = 1; frame <= frames; frame++) {
        cybiko_run_frame(a);
        cybiko_run_frame(b);
        radio_sim_bus_pump(&bus);

        /*
         * Take the pointer and the length before hashing. Passing vlen
         * as an argument to the same call that writes it hashes zero
         * bytes, because argument order is not fixed.
         */
        size_t vlen_a = 0, vlen_b = 0;
        const uint8_t *vram_a = cybiko_lcd_vram(a, &vlen_a);
        const uint8_t *vram_b = cybiko_lcd_vram(b, &vlen_b);
        hash_a = cybiko_crc32(vram_a, vlen_a);
        hash_b = cybiko_crc32(vram_b, vlen_b);
        ui_a = ui_hash(hash_a);
        ui_b = ui_hash(hash_b);
        if (ui_a && ui_b)
            saw_ui = true;
        if (!injected && radio_save_patched && key_phase == 51) {
            injected = true;
            printf("frame %d: typed \"%s\" on A (SCI2 writes %u DTC %u)\n",
                   frame, MARKER, cybiko_sci2_reg_writes, cybiko_sci2_dtc_runs);
        }
        if (key_phase == 51 && frame - phase_frame >= 48) {
            key_phase = 52;
            phase_frame = frame;
            printf("frame %d: B sending the stock line to cycle RX\n", frame);
        } else if (key_phase == 52 && frame - phase_frame >= 36) {
            key_phase = 53;
            dumps = 0;
            printf("frame %d: B stock line sent (SCI2 writes %u DTC %u)\n",
                   frame, cybiko_sci2_reg_writes, cybiko_sci2_dtc_runs);
        }
        if (ui_a && ui_b && !dumped_radio) {
            dumped_radio = true;
            welcome_frame = frame;
            key_phase = 1;
            print_radio_object(b, frame);
        }
        if (key_phase == 1 && nickname_hash(hash_b)) {
            key_phase = 2;
            phase_frame = frame;
            printf("frame %d: nickname\n", frame);
        } else if (key_phase == 3 && gender_hash(hash_b)) {
            key_phase = 4;
            phase_frame = frame;
            printf("frame %d: gender\n", frame);
        } else if (key_phase == 5 && hash_b == 0x566D62E5u) {
            key_phase = 14;
            phase_frame = frame;
            printf("frame %d: profile offer, focusing OK\n", frame);
        } else if (key_phase == 14 && frame - phase_frame >= 28) {
            key_phase = 15;
            phase_frame = frame;
        } else if (key_phase == 15 && frame - phase_frame >= 24) {
            key_phase = 16;
            phase_frame = frame;
            printf("frame %d: profile OK (SCI2 writes %u)\n",
                   frame, cybiko_sci2_reg_writes);
        } else if ((key_phase == 16 || key_phase == 5) && desktop_hash(hash_b) &&
                   !radio_save_patched) {
            dump_settings(cybiko_get_nvram(a, NULL), 0x200000u, "pre-patch A");
            dump_settings(cybiko_get_nvram(b, NULL), 0x200000u, "pre-patch B");
            bool pa = enable_radio_save(a);
            bool pb = enable_radio_save(b);
            printf("frame %d: patched settings.dat wireless bytes A=%d B=%d\n",
                   frame, pa, pb);
            dump_settings(cybiko_get_nvram(b, NULL), 0x200000u, "post-patch B");
            radio_save_patched = true;
            cybiko_reset(a);
            cybiko_reset(b);
            key_phase = 30;
            phase_frame = frame;
            printf("frame %d: reset both CPUs to reload settings\n", frame);
        } else if (key_phase == 30 && desktop_hash(hash_b)) {
            key_phase = 6;
            phase_frame = frame;
            printf("frame %d: desktop after settings patch (SCI2 writes %u)\n",
                   frame, cybiko_sci2_reg_writes);
        } else if (key_phase == 30 && hash_b == 0xFAEDD600u) {
            key_phase = 1;
            welcome_frame = frame;
            printf("frame %d: wizard still present after settings patch\n", frame);
        } else if ((key_phase == 16) && desktop_hash(hash_b)) {
            key_phase = 6;
            phase_frame = frame;
            printf("frame %d: desktop after profile\n", frame);
        } else if (key_phase == 5 && desktop_hash(hash_b)) {
            key_phase = 6;
            phase_frame = frame;
            printf("frame %d: desktop\n", frame);
        } else if (key_phase == 6 && frame - phase_frame >= 80) {
            key_phase = 7;
            printf("frame %d: Chat icon keys sent\n", frame);
        } else if (key_phase == 7 && hash_b == 0x98F1CD8Eu) {
            key_phase = 8;
            phase_frame = frame;
            printf("frame %d: chat list\n", frame);
        } else if (key_phase == 8 && frame - phase_frame >= 36) {
            key_phase = 9;
            printf("frame %d: opened Main room\n", frame);
        } else if (key_phase == 9 && hash_b == 0x3B807333u) {
            key_phase = 10;
            phase_frame = frame;
            printf("frame %d: room list\n", frame);
        } else if (key_phase == 10 && frame - phase_frame >= 36) {
            key_phase = 11;
            printf("frame %d: opened General\n", frame);
        } else if (key_phase == 11 && (hash_b == 0x1C9DC307u || hash_b == 0xD9075C44u)) {
            key_phase = 12;
            phase_frame = frame;
            dumps = 0;
            printf("frame %d: in General as deck (SCI2 writes %u DTC %u)\n",
                   frame, cybiko_sci2_reg_writes, cybiko_sci2_dtc_runs);
        } else if (key_phase == 12 && radio_save_patched &&
                   frame - phase_frame >= 40) {
            key_phase = 50;
            phase_frame = frame;
            printf("frame %d: typing %s on A\n", frame, MARKER);
        } else if (key_phase == 12 && !radio_save_patched &&
                   frame - phase_frame >= 40) {
            key_phase = 13;
            phase_frame = frame;
            printf("frame %d: confirmed Select all\n", frame);
        } else if (key_phase == 13 && frame - phase_frame >= 40) {
            key_phase = 17;
            phase_frame = frame;
            dumps = 0;
            printf("frame %d: sending the prefilled message\n", frame);
        } else if (key_phase == 17 && frame - phase_frame >= 30) {
            key_phase = 18;
            printf("frame %d: message sent (SCI2 writes %u DTC %u)\n",
                   frame, cybiko_sci2_reg_writes, cybiko_sci2_dtc_runs);
        } else if (key_phase == 18 && hash_b == 0x792CC52Bu) {
            key_phase = 19;
            phase_frame = frame;
            printf("frame %d: communications-off dialog\n", frame);
        } else if (key_phase == 19 && frame - phase_frame >= 30) {
            key_phase = 20;
            phase_frame = frame;
            printf("frame %d: leaving Chat for the desktop\n", frame);
        } else if (key_phase == 20 && desktop_hash(hash_b)) {
            key_phase = 21;
            phase_frame = frame;
            dumps = 0;
            printf("frame %d: desktop, opening System\n", frame);
        }
        if (key_phase == 22 && hash_b == 0x801019F2u) {
            key_phase = 23;
            phase_frame = frame;
            dumps = 0;
            printf("frame %d: Communications field focused\n", frame);
        } else if (key_phase == 23 && frame - phase_frame >= 110) {
            key_phase = 24;
            phase_frame = frame;
            printf("frame %d: Communications cycle keys sent\n", frame);
        } else if (key_phase == 24 && desktop_hash(hash_b)) {
            key_phase = 6;
            phase_frame = frame;
            dumps = 0;
            printf("frame %d: back at desktop, reopening Chat\n", frame);
        }
        clear_keys(&side_a);
        clear_keys(&side_b);
        if (key_phase == 1 || key_phase == 5) {
            int origin = key_phase == 1 ? welcome_frame : phase_frame;
            int since = frame - origin;
            bool down = (since % 40) < 8;
            set_key(&side_a, 4, 0x0008, down);
            set_key(&side_b, 4, 0x0008, down);
        } else if (key_phase == 2) {
            if (play_keys(&side_a, &side_b, k_nick_keys,
                          (int)(sizeof k_nick_keys / sizeof k_nick_keys[0]),
                          frame - phase_frame)) {
                key_phase = 3;
                printf("frame %d: nickname keys sent\n", frame);
            }
        } else if (key_phase == 4) {
            if (play_keys(&side_a, &side_b, k_gender_keys,
                          (int)(sizeof k_gender_keys / sizeof k_gender_keys[0]),
                          frame - phase_frame)) {
                key_phase = 5;
                phase_frame = frame;
                printf("frame %d: gender keys sent\n", frame);
            }
        } else if (key_phase == 6) {
            play_keys_timed(&side_a, &side_b, k_chat_icon,
                            (int)(sizeof k_chat_icon / sizeof k_chat_icon[0]),
                            frame - phase_frame, 28);
        } else if (key_phase == 12) {
            int since = frame - phase_frame;
            bool down = since >= 12 && since < 22;
            set_key(&side_a, 4, 0x0008, down);
            set_key(&side_b, 4, 0x0008, down);
        } else if (key_phase == 50) {
            if (play_keys_one(&side_a, k_chat_type,
                              (int)(sizeof k_chat_type / sizeof k_chat_type[0]),
                              frame - phase_frame)) {
                key_phase = 51;
                phase_frame = frame;
                dumps = 0;
                printf("frame %d: chat type keys sent\n", frame);
            }
        } else if (key_phase == 52) {
            int since = frame - phase_frame;
            bool down = since >= 4 && since < 14;
            set_key(&side_b, 4, 0x0008, down);
        } else if (key_phase == 17) {
            int since = frame - phase_frame;
            bool down = since >= 4 && since < 14;
            set_key(&side_a, 4, 0x0008, down);
            set_key(&side_b, 4, 0x0008, down);
        } else if (key_phase == 19) {
            int since = frame - phase_frame;
            bool down = since >= 4 && since < 14;
            set_key(&side_a, 4, 0x0008, down);
            set_key(&side_b, 4, 0x0008, down);
        } else if (key_phase == 20) {
            int since = frame - phase_frame;
            bool down = (since % 20) < 6;
            set_key(&side_a, 5, 0x0400, down);
            set_key(&side_b, 5, 0x0400, down);
        } else if (key_phase == 21) {
            if (play_keys_timed(&side_a, &side_b, k_system_keys,
                                (int)(sizeof k_system_keys / sizeof k_system_keys[0]),
                                frame - phase_frame, 36)) {
                key_phase = 22;
                phase_frame = frame;
                dumps = 0;
                printf("frame %d: System keys sent\n", frame);
            }
        } else if (key_phase == 22) {
            int since = frame - phase_frame;
            bool down = (since % 10) < 4;
            set_key(&side_a, 4, 0x0008, down);
            set_key(&side_b, 4, 0x0008, down);
        } else if (key_phase == 23) {
            int since = frame - phase_frame;
            int slot = since / 20;
            int pos = since % 20;
            bool down = pos < 8;
            /* Select opens an enum; Left/Right should then change it. */
            if (slot == 0 && down) {
                set_key(&side_a, 4, 0x0010, true);
                set_key(&side_b, 4, 0x0010, true);
            } else if (slot >= 1 && slot <= 3 && down) {
                set_key(&side_a, 6, 0x4000, true);
                set_key(&side_b, 6, 0x4000, true);
            } else if (slot == 4 && down) {
                set_key(&side_a, 4, 0x0008, true);
                set_key(&side_b, 4, 0x0008, true);
            }
        } else if (key_phase == 24) {
            int since = frame - phase_frame;
            bool down = (since % 20) < 6;
            set_key(&side_a, 5, 0x0400, down);
            set_key(&side_b, 5, 0x0400, down);
        } else if (key_phase == 14) {
            int since = frame - phase_frame;
            bool down = since >= 4 && since < 14;
            set_key(&side_a, 6, 0x4000, down);
            set_key(&side_b, 6, 0x4000, down);
        } else if (key_phase == 15) {
            int since = frame - phase_frame;
            bool down = since < 10;
            set_key(&side_a, 4, 0x0008, down);
            set_key(&side_b, 4, 0x0008, down);
        } else if (key_phase == 16) {
            int since = frame - phase_frame;
            bool down = (since % 10) < 4;
            if (since < 900) {
                set_key(&side_a, 4, 0x0008, down);
                set_key(&side_b, 4, 0x0008, down);
            } else {
                /* Esc: leave the form once Enter stops advancing it. */
                set_key(&side_a, 5, 0x0400, down);
                set_key(&side_b, 5, 0x0400, down);
            }
        } else if (key_phase == 8 || key_phase == 10) {
            int since = frame - phase_frame;
            bool down = since >= 8 && since < 18;
            set_key(&side_a, 4, 0x0008, down);
            set_key(&side_b, 4, 0x0008, down);
        }
        if (dumps < 8 && radio_save_patched && key_phase >= 50 && hash_a != prev_b) {
            char path[64];
            snprintf(path, sizeof path, "/tmp/cybiko-a-%d-%08X.pgm", frame, hash_a);
            dump_pgm(path, vram_a);
            printf("frame %d: wrote A %s\n", frame, path);
            dumps++;
        }
        if (dumps < 15 && radio_save_patched && key_phase >= 6 && hash_b != prev_b) {
            char path[64];
            snprintf(path, sizeof path, "/tmp/cybiko-p-%d-%08X.pgm", frame, hash_b);
            dump_pgm(path, vram_b);
            printf("frame %d: wrote %s\n", frame, path);
            dumps++;
        }
        if (hash_b != prev_b) {
            size_t rlen = 0;
            const uint8_t *ram = cybiko_get_nvram(b, &rlen);
            size_t off = 0x4B49FEu - 0x400000u;
            uint32_t obj = 0;
            if (ram && off + 4 <= rlen) {
                obj = ((uint32_t)ram[off] << 24) | ((uint32_t)ram[off + 1] << 16) |
                      ((uint32_t)ram[off + 2] << 8) | ram[off + 3];
            }
            if (obj != last_obj) {
                printf("frame %d: radio object now %08X (vram %08X)\n", frame, obj, hash_b);
                last_obj = obj;
            }
            prev_b = hash_b;
        }
        {
            size_t rlen = 0;
            const uint8_t *ram = cybiko_get_nvram(b, &rlen);
            size_t off = 0x4B49FEu - 0x400000u;
            int st = -1;
            if (ram && off + 4 <= rlen) {
                uint32_t obj = ((uint32_t)ram[off] << 24) | ((uint32_t)ram[off + 1] << 16) |
                               ((uint32_t)ram[off + 2] << 8) | ram[off + 3];
                if (obj >= 0x400000u && obj < 0x600000u) {
                    size_t so = (obj - 0x400000u) + 0x335Au;
                    if (so < rlen)
                        st = ram[so];
                }
            }
            if (st >= 0 && st < 16)
                state_hist[st]++;
            else
                state_other++;
            if (st == 1) {
                if (!state1_first)
                    state1_first = frame;
                state1_last = frame;
                if (injected && state1_after++ == 0)
                    printf("frame %d: radio state 1 after inject\n", frame);
            }
        }
        if (cop_a.stat_tx != last_tx_a) {
            last_tx_a = cop_a.stat_tx;
            printf("frame %d: A TX #%u len=%u type=%02X ch=%02X\n",
                   frame, last_tx_a, cop_a.last_tx_len,
                   cop_a.last_tx_len > 1 ? cop_a.last_tx[1] : 0,
                   cop_a.last_tx_len > 0 ? cop_a.last_tx[0] : 0);
            if (cop_a.last_tx_len >= 8)
                printf("  A TX head %02X %02X %02X %02X %02X %02X %02X %02X\n",
                       cop_a.last_tx[0], cop_a.last_tx[1], cop_a.last_tx[2],
                       cop_a.last_tx[3], cop_a.last_tx[4], cop_a.last_tx[5],
                       cop_a.last_tx[6], cop_a.last_tx[7]);
            if (cop_a.last_tx_len > 19) {
                char text[8];
                memset(text, 0, sizeof text);
                memcpy(text, cop_a.last_tx + 19, 5);
                printf("  A TX msg len=%u text=%.5s\n",
                       cop_a.last_tx[18], text);
            }
        }
        {
            int pb = cybiko_radio_coproc_pending(&cop_b);
            if (pb != last_pending_b) {
                size_t n = 0;
                const uint8_t *ram = cybiko_get_nvram(b, &n);
                bool hit = ram && contains(ram, n, MARKER);
                size_t on = 0;
                const uint8_t *chip = cybiko_onchip(b, &on);
                bool onchip = chip && contains(chip, on, MARKER);
                printf("frame %d: B pending %d -> %d  marker=%d onchip=%d  SCI2 %u DTC %u\n",
                       frame, last_pending_b, pb, hit, onchip,
                       cybiko_sci2_reg_writes, cybiko_sci2_dtc_runs);
                for (int i = 0; i < pb; i++) {
                    const cybiko_radio_frame_t *f = cybiko_radio_coproc_peek(&cop_b, i);
                    if (!f)
                        continue;
                    int has = 0;
                    for (int k = 0; k + (int)strlen(MARKER) <= f->len; k++) {
                        if (memcmp(f->data + k, MARKER, strlen(MARKER)) == 0) {
                            has = 1;
                            break;
                        }
                    }
                    printf("  q[%d] len=%u type=%02X ch=%02X marker=%d sender=%08X\n",
                           i, f->len, f->len > CYBIKO_RADIO_HDR + 1 ? f->data[CYBIKO_RADIO_HDR + 1] : 0,
                           f->len > CYBIKO_RADIO_HDR ? f->data[CYBIKO_RADIO_HDR] : 0, has, f->sender_id);
                }
                last_pending_b = pb;
            }
        }
        if (cybiko_sci2_reg_writes != sci2_seen) {
            sci2_seen = cybiko_sci2_reg_writes;
            if (injected)
                printf("frame %d: SCI2 writes now %u DTC %u\n",
                       frame, sci2_seen, cybiko_sci2_dtc_runs);
        }
        if (frame == frames) {
            dump_pgm("/tmp/cybiko-final.pgm", vram_b);
            dump_pgm("/tmp/cybiko-final-a.pgm", vram_a);
            printf("frame %d: final vram B %08X A %08X\n", frame, hash_b, hash_a);
        }
        if (key_phase >= 12 && frame % 120 == 0)
            print_radio_object(b, frame);
        if (frame % 60 == 0 || frame == 1) {
            printf("frame %4d  %.1fs  A pc=%06X %s vram=%08X %s  B pc=%06X %s vram=%08X %s  q=%d/%d\n",
                   frame, now_sec() - t0,
                   cybiko_cpu_pc(a), cybiko_cpu_halted(a) ? "halt" : "run",
                   hash_a, phase_name(hash_a),
                   cybiko_cpu_pc(b), cybiko_cpu_halted(b) ? "halt" : "run",
                   hash_b, phase_name(hash_b),
                   cybiko_radio_coproc_pending(&cop_a),
                   cybiko_radio_coproc_pending(&cop_b));
            fflush(stdout);
        }
    }

    if (side_a.len) {
        side_a.buf[side_a.len] = 0;
        printf("serial A: %s\n", side_a.buf);
    }
    if (side_b.len) {
        side_b.buf[side_b.len] = 0;
        printf("serial B: %s\n", side_b.buf);
    }

    size_t rlen = 0;
    const uint8_t *ram_b = cybiko_get_nvram(b, &rlen);
    bool in_ram = ram_b && contains(ram_b, rlen, MARKER);
    size_t alen = 0;
    const uint8_t *ram_a = cybiko_get_nvram(a, &alen);
    bool in_a = ram_a && contains(ram_a, alen, MARKER);
    bool hi_b = ram_b && contains(ram_b, rlen, "Hi, everybody!");
    size_t on = 0;
    const uint8_t *chip = cybiko_onchip(b, &on);
    bool onchip = chip && contains(chip, on, MARKER);
    printf("ui A=%d B=%d  saw_welcome=%d  injected=%d  \"%s\" in A RAM=%d B RAM=%d onchip=%d  hi-b=%d  B pending=%d rx=%d\n",
           ui_a, ui_b, saw_ui, injected, MARKER, in_a, in_ram, onchip, hi_b,
           cybiko_radio_coproc_pending(&cop_b),
           cybiko_radio_coproc_rx_count(&cop_b));
    printf("radio state 1 frames %d-%d, %d after inject\n",
           state1_first, state1_last, state1_after);
    printf("SCI2 reg writes %u  DTC runs %u\n",
           cybiko_sci2_reg_writes, cybiko_sci2_dtc_runs);
    printf("radio state counts:");
    for (int i = 0; i < 16; i++) {
        if (state_hist[i])
            printf(" %d=%d", i, state_hist[i]);
    }
    if (state_other)
        printf(" other=%d", state_other);
    printf("\n");
    if (ram_b && rlen >= CFS_IMAGE_SIZE) {
        cfs_image_t *img = malloc(sizeof *img);
        if (img) {
            memcpy(img->data, ram_b, CFS_IMAGE_SIZE);
            size_t n = 0;
            if (cfs_read_file(img, "settings.dat", NULL, 0, &n) && n > 0) {
                uint8_t *body = malloc(n);
                if (body && cfs_read_file(img, "settings.dat", body, n, &n)) {
                    printf("settings.dat %zu bytes  b12=%02X b13=%02X b41=%02X\n",
                           n,
                           n > 12 ? body[12] : 0,
                           n > 13 ? body[13] : 0,
                           n > 41 ? body[41] : 0);
                }
                free(body);
            } else {
                printf("settings.dat not readable\n");
            }
            free(img);
        }
    }

    cybiko_destroy(a);
    cybiko_destroy(b);
    free(boot);
    free(flash);
    if (!(saw_ui && in_ram))
        return 1;
    printf("both sides reached the UI and B's RAM holds the scripted chat\n");
    return 0;
}
