#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <driver/i2s.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
#include <algorithm>

#include "tdeck_pins.h"

extern "C" {
#include "core/emulator.h"
#include "core/cfs.h"
#include "radio_link.h"
#include "radio_coproc.h"
}

#include "tdeck_keymap.h"
#include "radio_sx1262.h"

#include <stdarg.h>
#include <stdio.h>

extern "C" void cybiko_radio_log(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Serial.print(buf);
}

#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/usb_serial_jtag_struct.h"
#endif

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

static TFT_eSPI tft;
static uint16_t *disp_buf;  // 320x200 RGB565 in PSRAM
static uint32_t blit_count = 0;
static unsigned long emu_started_ms = 0;

// Backlight. The T-Deck's driver has 16 levels set by pulses on the BL pin:
// high is full, and each low-high pulse steps one level down, wrapping.
// Held low for 3 ms it turns off. $b steps through LIGHT_LEVELS, kept in NVS.
static const uint8_t LIGHT_LEVELS[] = { 16, 12, 8, 4 };
#define LIGHT_COUNT (sizeof LIGHT_LEVELS)
#define LIGHT_DEFAULT 1   // 12 of 16
static uint8_t light_idx = LIGHT_DEFAULT;
static uint8_t bl_level;  // what the driver is at now; 0 = off

static void backlight_set(uint8_t level) {
    if (level == bl_level) return;
    if (level == 0) {
        digitalWrite(PIN_TFT_BL, LOW);
        delay(3);
        bl_level = 0;
        return;
    }
    if (bl_level == 0) {
        digitalWrite(PIN_TFT_BL, HIGH);
        bl_level = 16;
        delay(1);   // let the driver start before counting pulses
    }
    int pulses = (16 + bl_level - level) % 16;
    for (int i = 0; i < pulses; i++) {
        digitalWrite(PIN_TFT_BL, LOW);
        delayMicroseconds(2);
        digitalWrite(PIN_TFT_BL, HIGH);
        delayMicroseconds(2);
    }
    bl_level = level;
}

static void light_load(void) {
    Preferences p;
    p.begin("cybiko", true);
    light_idx = p.getUChar("light", LIGHT_DEFAULT);
    p.end();
    if (light_idx >= LIGHT_COUNT) light_idx = LIGHT_DEFAULT;
}

// Screen sleep: when CyOS turns its LCD off (after 5 idle minutes), the
// backlight goes off, nothing is drawn, and the CPU drops to 160 MHz. A
// key wakes CyOS, which turns its LCD back on, and the screen follows.
// 160 MHz runs a sleeping CyOS ~46% busy. It does not suit the awake
// desktop: CyOS redraws in bursts of 22-45 ms frames there, close to the
// whole frame budget on average, so the 67 ms audio queue would run dry.
#define SCREEN_OFF_FRAMES 60   // CyOS blanks for a moment while booting
#define AWAKE_MHZ  240
#define ASLEEP_MHZ 160
static bool screen_on = true;
static int lcd_off_frames;

// Loading phase: only every 20th frame is drawn, and silent frames are
// fast-forwarded (hal_audio_output). It ends when CyOS has idled (asleep
// >= 60% for three 60-frame windows in a row; boot dips quiet for a second
// at a time, the settled desktop is ~67%), on the first key press, or after
// 3 minutes. The keyboard is read throughout: ignoring it here left the
// Classic's desktop looking frozen for up to two minutes.
static bool boot_done;

static bool boot_unpacking(void) {
    if (boot_done || emu_started_ms == 0)
        return false;
    if (millis() - emu_started_ms >= 180000ul)
        boot_done = true;
    return !boot_done;
}

static void check_boot_done(const cybiko_stats_t *st) {
    static uint64_t last_sleep;
    static uint32_t last_frames;
    if (boot_done) return;
    uint32_t frames = st->frames - last_frames;
    if (frames < 60) return;
    uint64_t slept = st->sleep_cycles - last_sleep;
    last_sleep = st->sleep_cycles;
    last_frames = st->frames;
    static int idle_windows;
    if (slept * 10 >= (uint64_t)frames * CYBIKO_CYCLES_PER_FRAME * 6)
        idle_windows++;
    else
        idle_windows = 0;
    if (idle_windows >= 3) {
        boot_done = true;
        Serial.printf("CyOS idle: loading phase over at %lu ms\n",
                      (unsigned long)(millis() - emu_started_ms));
    }
}

// Precomputed grayscale -> RGB565 lookup (256 entries)
static uint16_t gray_lut[256];

static void init_gray_lut(void) {
    for (int i = 0; i < 256; i++) {
        // Proper grayscale: keep R=B in lock-step, G = R*2 for balanced gray
        uint16_t v5 = (i >> 3) & 0x1F;   // 5-bit for R and B
        uint16_t v6 = v5 << 1;            // 6-bit for G (exact 2x ratio)
        gray_lut[i] = (v5 << 11) | (v6 << 5) | v5;
    }
}

static uint32_t render_us;  // time inside hal_render_frame, reset per report

static void render_frame_body(const uint8_t *pixels, int w, int h);

static void hal_render_frame(void *ctx, const uint8_t *pixels, int w, int h) {
    (void)ctx;
    uint32_t t0 = micros();
    render_frame_body(pixels, w, h);
    render_us += micros() - t0;
}

// The SX1262 shares SPI with the panel. Radio transfers wait until the
// previous frame's DMA has finished; the driver retries on a later poll.
static bool tdeck_bus_free(void) {
    return !tft.dmaBusy();
}

// Last frame sent to the panel. CyOS redraws rarely, and an unchanged
// frame costs a 128 KB upscale and a ~13 ms DMA for nothing.
static uint8_t shown_frame[160 * 100];
static bool shown_valid = false;

static void render_frame_body(const uint8_t *pixels, int w, int h) {
    /* The 320x200 DMA blit is ~13 ms. If it is still running, skip this
     * frame; shown_frame is not updated, so a change is drawn next time. */
    if (!screen_on || tft.dmaBusy())
        return;
    if (boot_unpacking() && (++blit_count % 20) != 1)
        return;
    size_t n = (size_t)w * h;
    if (n > sizeof shown_frame)
        n = sizeof shown_frame;
    if (shown_valid && memcmp(shown_frame, pixels, n) == 0)
        return;
    memcpy(shown_frame, pixels, n);
    shown_valid = true;

    // 2x nearest-neighbor upscale: 160x100 -> 320x200
    for (int y = 0; y < h; y++) {
        const uint8_t *src_row = pixels + y * w;
        uint16_t *dst_row0 = disp_buf + (y * 2) * SCALED_W;
        uint16_t *dst_row1 = dst_row0 + SCALED_W;
        for (int x = 0; x < w; x++) {
            uint16_t c = gray_lut[src_row[x]];
            dst_row0[x * 2]     = c;
            dst_row0[x * 2 + 1] = c;
            dst_row1[x * 2]     = c;
            dst_row1[x * 2 + 1] = c;
        }
    }
    tft.startWrite();
    tft.pushImageDMA(0, BORDER_TOP, SCALED_W, SCALED_H, disp_buf);
    tft.endWrite();
}

// ---------------------------------------------------------------------------
// Keyboard - ASCII to Cybiko matrix conversion
// ---------------------------------------------------------------------------

// Synthetic key release tracking. CyOS 1.2.46 misses a key held under
// 10 frames now and then (tools/v1_chat.c: 8 frames dropped about one key
// in four, 10 and up dropped none). Typed keys are taps, one at a time:
// a second tap of the same key during the first would only lengthen it,
// so "ll" came out as "l", and it needs 10 frames up before it goes down
// again (other keys need only 3). The C3 keyboard only keeps one unread byte, so
// I2C is drained on its own task.
#define MAX_ACTIVE_KEYS 16
#define KEY_HOLD_FRAMES 12
#define KEY_GAP_FRAMES  3     // all keys up between two taps
#define KEY_REPEAT_GAP_FRAMES 10  // ...of the same key: 3 or 6 lost the second
#define KEY_HOLD_STICKY (-1)  // stays down until release_key()
#define KB_QUEUE_LEN    32
#define TAP_QUEUE_LEN   32

static uint16_t key_matrix[CYBIKO_KEYBOARD_COLUMNS];

static struct {
    int      col;
    uint16_t mask;
    int      frames_left;
} active_keys[MAX_ACTIVE_KEYS];
static int active_key_count = 0;

// Fn+digit two-phase state machine
static struct {
    int      letter_col;
    uint16_t letter_mask;
    int      delay_frames;
} pending_fn;
static bool fn_pending = false;

// Typed keys waiting their turn, each a key plus Shift or Fn
static tdeck_key_event_t tap_queue[TAP_QUEUE_LEN];
static int tap_head, tap_count;
static int tap_gap = KEY_REPEAT_GAP_FRAMES;   // frames since every pulsed key went up
static tdeck_key_event_t last_tap;

static int gap_before(const tdeck_key_event_t *ev) {
    return ev->col == last_tap.col && ev->mask == last_tap.mask
           ? KEY_REPEAT_GAP_FRAMES : KEY_GAP_FRAMES;
}

static void press_key(int col, uint16_t mask) {
    for (int i = 0; i < active_key_count; i++) {
        if (active_keys[i].col == col && active_keys[i].mask == mask) {
            if (active_keys[i].frames_left != KEY_HOLD_STICKY)
                active_keys[i].frames_left = KEY_HOLD_FRAMES;
            key_matrix[col] |= mask;
            return;
        }
    }
    if (active_key_count >= MAX_ACTIVE_KEYS) {
        /* A burst used to OR the 9th key into the matrix with no release
         * slot, so it stuck until reboot. Drop the oldest pulsed key. */
        int victim = 0;
        for (int i = 0; i < active_key_count; i++) {
            if (active_keys[i].frames_left != KEY_HOLD_STICKY) {
                victim = i;
                break;
            }
        }
        key_matrix[active_keys[victim].col] &= ~active_keys[victim].mask;
        active_keys[victim] = active_keys[active_key_count - 1];
        active_key_count--;
    }
    active_keys[active_key_count].col = col;
    active_keys[active_key_count].mask = mask;
    active_keys[active_key_count].frames_left = KEY_HOLD_FRAMES;
    active_key_count++;
    key_matrix[col] |= mask;
}

static void hold_key(int col, uint16_t mask) {
    for (int i = 0; i < active_key_count; i++) {
        if (active_keys[i].col == col && active_keys[i].mask == mask) {
            active_keys[i].frames_left = KEY_HOLD_STICKY;
            key_matrix[col] |= mask;
            return;
        }
    }
    if (active_key_count < MAX_ACTIVE_KEYS) {
        active_keys[active_key_count].col = col;
        active_keys[active_key_count].mask = mask;
        active_keys[active_key_count].frames_left = KEY_HOLD_STICKY;
        active_key_count++;
    }
    key_matrix[col] |= mask;
}

static void release_key(int col, uint16_t mask) {
    key_matrix[col] &= ~mask;
    for (int i = 0; i < active_key_count; i++) {
        if (active_keys[i].col == col && active_keys[i].mask == mask) {
            active_keys[i] = active_keys[active_key_count - 1];
            active_key_count--;
            return;
        }
    }
}

static bool pulsed_key_down(void) {
    for (int i = 0; i < active_key_count; i++)
        if (active_keys[i].frames_left != KEY_HOLD_STICKY)
            return true;
    return fn_pending;
}

static void press_tap(const tdeck_key_event_t *ev) {
    if (ev->needs_fn) {
        press_key(ev->col, ev->mask);
        pending_fn.letter_col = ev->fn_col;
        pending_fn.letter_mask = ev->fn_mask;
        pending_fn.delay_frames = 3;
        fn_pending = true;
    } else {
        if (ev->needs_shift)
            press_key(TDECK_SHIFT_COL, TDECK_SHIFT_MASK);
        press_key(ev->col, ev->mask);
    }
    last_tap = *ev;
    tap_gap = 0;
}

static void queue_tap(const tdeck_key_event_t *ev) {
    if (tap_count == 0 && !pulsed_key_down() && tap_gap >= gap_before(ev)) {
        press_tap(ev);
        return;
    }
    if (tap_count == TAP_QUEUE_LEN)
        return;   // typing far ahead of the guest; drop the newest
    tap_queue[(tap_head + tap_count) % TAP_QUEUE_LEN] = *ev;
    tap_count++;
}

static void keyboard_tick_frame(void) {
    for (int i = 0; i < active_key_count; ) {
        if (active_keys[i].frames_left == KEY_HOLD_STICKY) {
            i++;
            continue;
        }
        active_keys[i].frames_left--;
        if (active_keys[i].frames_left <= 0) {
            key_matrix[active_keys[i].col] &= ~active_keys[i].mask;
            active_keys[i] = active_keys[active_key_count - 1];
            active_key_count--;
        } else {
            i++;
        }
    }
    if (fn_pending) {
        pending_fn.delay_frames--;
        if (pending_fn.delay_frames <= 0) {
            press_key(pending_fn.letter_col, pending_fn.letter_mask);
            fn_pending = false;
        }
    }
    if (pulsed_key_down()) {
        tap_gap = 0;
        return;
    }
    if (tap_gap < KEY_REPEAT_GAP_FRAMES)
        tap_gap++;
    if (tap_count > 0 && tap_gap >= gap_before(&tap_queue[tap_head])) {
        press_tap(&tap_queue[tap_head]);
        tap_head = (tap_head + 1) % TAP_QUEUE_LEN;
        tap_count--;
    }
}

static void process_ascii_key(char c) {
    tdeck_key_event_t ev;
    if (!tdeck_map_ascii(c, &ev)) return;
    queue_tap(&ev);
}

// Trackball pulse counting via interrupts.
// The trackball generates brief LOW pulses per step — too short to
// reliably catch by polling at ~20fps.  ISRs count every pulse, then
// poll_input() converts accumulated counts into synthetic held presses.
#define TRACKBALL_CLICK_DEBOUNCE_MS 30
#define TRACKBALL_ESC_MS            600
static volatile int tb_up_count    = 0;
static volatile int tb_down_count  = 0;
static volatile int tb_left_count  = 0;
static volatile int tb_right_count = 0;
static volatile uint32_t tb_click_down_ms;
static volatile uint8_t  tb_click_isr_down;

static void IRAM_ATTR tb_up_isr()    { tb_up_count++; }
static void IRAM_ATTR tb_down_isr()  { tb_down_count++; }
static void IRAM_ATTR tb_left_isr()  { tb_left_count++; }
static void IRAM_ATTR tb_right_isr() { tb_right_count++; }

static void IRAM_ATTR tb_click_isr() {
    uint32_t now = millis();
    if (!now) now = 1;
    if (!digitalRead(PIN_TRACKBALL_CLICK)) {
        if (!tb_click_isr_down) {
            tb_click_isr_down = 1;
            tb_click_down_ms = now;
        }
    } else {
        tb_click_isr_down = 0;
    }
}

static void init_trackball_interrupts(void) {
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_UP),    tb_up_isr,    FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_DOWN),  tb_down_isr,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_LEFT),  tb_left_isr,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_RIGHT), tb_right_isr, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_CLICK), tb_click_isr, CHANGE);
}

#ifdef CYBIKO_SPLIT_PROFILE
extern "C" uint32_t cybiko_split[3];
#endif
static void volume_step(int by);
static void volume_done(void);
static void volume_note(char *buf, size_t n);

static void press_special(tdeck_special_key_t k) {
    tdeck_key_event_t ev = {};
    if (tdeck_map_special(k, &ev.col, &ev.mask))
        queue_tap(&ev);
}

// The T-Deck has no Esc, F1-F7, Help, Select, Insert or Delete. $ is a
// command key: $ then a letter sends one of them ($x = Esc). $p and $m
// turn the volume up and down, and start volume mode: more p and m keep
// stepping it until 2 s pass without one. Any other key ends volume mode
// and is typed as usual.
#define VOLUME_MODE_MS 2000
static bool cmd_pending;
static unsigned long volume_until;   // volume mode ends at this millis(); 0 = off
static char chrome_top[40] = "ReBiko Pocket";

/* $l: a range test. The bottom line shows what this radio hears, and a
 * ping goes out every second on the ping channel (CyOS ignores it) for
 * modem_tool on the PC to count. Green: hearing everything; yellow: some
 * lost; red: nothing heard for 5 s. */
#define LINK_PING_MS 1000
static bool link_view;
static uint32_t link_ping_at, link_ping_seq;
static uint32_t link_rx_seen, link_rx_at;

static void draw_link_line(void) {
    const cybiko_lora_t *lk = cybiko_sx1262_link();
    char line[48];
    uint16_t color = TFT_RED;
    if (!lk) {
        snprintf(line, sizeof line, "radio down");
    } else {
        const cybiko_sx1262_stats_t *rs = cybiko_sx1262_phy_stats();
        uint32_t heard = lk->stats.rx_msgs, lost = lk->stats.rx_lost;
        uint32_t ago = link_rx_at ? (millis() - link_rx_at) / 1000 : 0;
        float pct = heard + lost ? 100.0f * lost / (heard + lost) : 0.0f;
        snprintf(line, sizeof line, "rx %lu lost %lu %.1f%%  %ddBm snr %d  %lus",
                 (unsigned long)heard, (unsigned long)lost, pct,
                 rs->last_rssi_dbm, rs->last_snr_db, (unsigned long)ago);
        if (link_rx_at && ago < 5)
            color = lost ? TFT_YELLOW : TFT_GREEN;
    }
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(line, 4, BORDER_BOTTOM + 2, 2);
}

static void link_tick(uint32_t now) {
    const cybiko_lora_t *lk = cybiko_sx1262_link();
    if (!link_view || (long)(now - link_ping_at) < 0)
        return;
    link_ping_at = now + LINK_PING_MS;
    if (lk && lk->stats.rx_msgs != link_rx_seen) {
        link_rx_seen = lk->stats.rx_msgs;
        link_rx_at = now;
    }
    if (cybiko_radio_transport_t *t = cybiko_sx1262_transport()) {
        char msg[32];
        int n = snprintf(msg, sizeof msg, "PING %lu from T-Deck", (unsigned long)link_ping_seq++);
        t->send_packet(t, (const uint8_t *)msg, (size_t)n, CYBIKO_LORA_PING_CHANNEL);
    }
    if (!screen_on)
        return;
    tft.dmaWait();
    tft.fillRect(0, BORDER_BOTTOM, SCALED_W, 240 - BORDER_BOTTOM, TFT_BLACK);
    draw_link_line();
}

static void draw_chrome(void) {
    if (!screen_on)
        return;
    tft.dmaWait();
    tft.fillRect(0, 0, SCALED_W, BORDER_TOP, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    if (chrome_top[0])
        tft.drawString(chrome_top, 4, 2, 2);
    if (volume_until) {
        char note[16];
        volume_note(note, sizeof note);
        tft.fillRect(196, 0, SCALED_W - 196, BORDER_TOP, TFT_BLACK);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString(note, 200, 2, 2);
    }
    if (cmd_pending || volume_until) {
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("$", 300, 2, 2);
    }
    tft.fillRect(0, BORDER_BOTTOM, SCALED_W, 240 - BORDER_BOTTOM, TFT_BLACK);
    if (link_view && !cmd_pending) {
        draw_link_line();
        return;
    }
    tft.setTextColor(cmd_pending ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(cmd_pending ? "$x Esc  $h Help  $q-u F1-7  $p/m Vol" :
                                 "$ then x=Esc h=Help q-u=F1-7 p/m=Vol",
                   4, BORDER_BOTTOM + 2, 2);
}

static void show_cmd(bool on) {
    (void)on;
    draw_chrome();
}

static void handle_key(char c) {
    boot_done = true;   // someone is using it: back to real time
    if (volume_until) {
        if (c == 'p' || c == 'P') { volume_step(+1); return; }
        if (c == 'm' || c == 'M') { volume_step(-1); return; }
        volume_done();   // then type the key as usual
    }
    if (cmd_pending) {
        cmd_pending = false;
        show_cmd(false);
        switch (c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) {
        case 'x': press_special(TDECK_KEY_ESC); break;
        case 'h': press_special(TDECK_KEY_HELP); break;
        case 's': press_special(TDECK_KEY_SELECT); break;
        case 'i': press_special(TDECK_KEY_INSERT); break;
        case 'd': press_special(TDECK_KEY_DELETE); break;
        case 'g': press_special(TDECK_KEY_TAB); break;
        case 'q': press_special(TDECK_KEY_F1); break;
        case 'w': press_special(TDECK_KEY_F2); break;
        case 'e': press_special(TDECK_KEY_F3); break;
        case 'r': press_special(TDECK_KEY_F4); break;
        case 't': press_special(TDECK_KEY_F5); break;
        case 'y': press_special(TDECK_KEY_F6); break;
        case 'u': press_special(TDECK_KEY_F7); break;
        case 'p': volume_step(+1); break;
        case 'm': volume_step(-1); break;
        case 'b': {
            light_idx = (light_idx + 1) % LIGHT_COUNT;
            backlight_set(LIGHT_LEVELS[light_idx]);
            Preferences p;
            p.begin("cybiko", false);
            p.putUChar("light", light_idx);
            p.end();
            snprintf(chrome_top, sizeof chrome_top, "backlight %u/%u",
                     (unsigned)(LIGHT_COUNT - light_idx), (unsigned)LIGHT_COUNT);
            draw_chrome();
            break;
        }
        case 'l':
            link_view = !link_view;
            link_ping_at = millis();
            draw_chrome();
            break;
        case '$': process_ascii_key('$'); break;
        default: break;   // anything else cancels
        }
        return;
    }
    if (c == '$') {
        cmd_pending = true;
        show_cmd(true);
    } else {
        process_ascii_key(c);
    }
}

static QueueHandle_t kb_queue;

static int keyboard_drain_i2c(void) {
    int got = 0;
    for (int i = 0; i < 8; i++) {
        int n = Wire.requestFrom(KB_I2C_ADDR, (int)1);
        if (n <= 0 || Wire.available() <= 0)
            break;
        uint8_t c = (uint8_t)Wire.read();
        if (c == 0x00 || c == 0xFF)
            break;
        if (kb_queue)
            xQueueSend(kb_queue, &c, 0);
        else
            handle_key((char)c);
        got++;
    }
    return got;
}

/* The C3 keyboard holds one unread byte. Polling it only once per emulator
 * frame dropped keys whenever radio or a DMA blit stretched a frame past
 * the next press. This task keeps the I2C FIFO empty. */
static void keyboard_task(void *arg) {
    (void)arg;
    for (;;) {
        int got;
        do {
            got = keyboard_drain_i2c();
        } while (got >= 8);
        bool pending = digitalRead(PIN_KB_INT) == LOW;
        vTaskDelay(pdMS_TO_TICKS(pending ? 2 : 5));
    }
}

static void poll_input(void) {
    if (!kb_queue)
        keyboard_drain_i2c();
    uint8_t queued;
    while (kb_queue && xQueueReceive(kb_queue, &queued, 0) == pdTRUE) {
        static int kb_log = 0;
#ifdef CYBIKO_KEYLOG
        Serial.printf("KEY 0x%02X %3u '%c'\n", queued, queued,
                      queued >= 0x20 && queued < 0x7F ? queued : '.');
#else
        if (kb_log < 16) {
            Serial.printf("kb %02X\n", queued);
            kb_log++;
        }
#endif
        handle_key((char)queued);
    }

    // Keys from the USB serial port, for scripted tests: ASCII as from the
    // T-Deck keyboard, and 0x11-0x14 for trackball up/down/left/right.
    while (Serial.available() > 0) {
        int c = Serial.read();
        switch (c) {
        case 0x11: press_key(TDECK_PAD_UP_COL, TDECK_PAD_UP_MASK); break;
        case 0x12: press_key(TDECK_PAD_DOWN_COL, TDECK_PAD_DOWN_MASK); break;
        case 0x13: press_key(TDECK_PAD_LEFT_COL, TDECK_PAD_LEFT_MASK); break;
        case 0x14: press_key(TDECK_PAD_RIGHT_COL, TDECK_PAD_RIGHT_MASK); break;
        default:   if (c > 0) handle_key((char)c); break;
        }
    }

    // Trackball: read and clear pulse counters (atomic via noInterrupts)
    noInterrupts();
    int up    = tb_up_count;    tb_up_count    = 0;
    int down  = tb_down_count;  tb_down_count  = 0;
    int left  = tb_left_count;  tb_left_count  = 0;
    int right = tb_right_count; tb_right_count = 0;
    interrupts();

    // Convert pulses into synthetic held key presses
    if (up > 0)    press_key(TDECK_PAD_UP_COL, TDECK_PAD_UP_MASK);
    if (down > 0)  press_key(TDECK_PAD_DOWN_COL, TDECK_PAD_DOWN_MASK);
    if (left > 0)  press_key(TDECK_PAD_LEFT_COL, TDECK_PAD_LEFT_MASK);
    if (right > 0) press_key(TDECK_PAD_RIGHT_COL, TDECK_PAD_RIGHT_MASK);
    if (up | down | left | right)
        boot_done = true;

    /* A click shorter than 600 ms is Enter, the key CyOS uses to leave
     * the welcome pages and to open an icon (Select does not launch).
     * Holding the ball down that long is Esc, which the T-Deck keyboard
     * lacks. Enter is sent on release so a hold does not also open
     * something. Esc stays down for the rest of the hold: a 4-frame pulse
     * was too short for CyOS 1.2.46's scan, and bounce on GPIO 0 turned a
     * hold into Enter. The click ISR timestamps the press so a slow frame
     * cannot restart the hold timer. */
    static uint32_t click_since;
    static uint32_t click_up_since;
    static bool click_down;
    static bool esc_held;
    bool raw = !digitalRead(PIN_TRACKBALL_CLICK);
    uint32_t now = millis();
    if (!now) now = 1;
    noInterrupts();
    uint32_t isr_down_ms = tb_click_down_ms;
    interrupts();
    if (raw != click_down) {
        if (!click_up_since)
            click_up_since = now;
        if (now - click_up_since < TRACKBALL_CLICK_DEBOUNCE_MS)
            raw = click_down;
        else
            click_up_since = 0;
    } else {
        click_up_since = 0;
    }
    if (raw && !click_down) {
        click_down = true;
        click_since = isr_down_ms ? isr_down_ms : now;
        if (now - click_since > 2000)
            click_since = now;
        esc_held = false;
        boot_done = true;
    } else if (!raw && click_down) {
        click_down = false;
        if (esc_held) {
            int col; uint16_t mask;
            if (tdeck_map_special(TDECK_KEY_ESC, &col, &mask))
                release_key(col, mask);
            esc_held = false;
        } else if (click_since && now - click_since >= TRACKBALL_CLICK_DEBOUNCE_MS) {
            press_key(TDECK_ENTER_COL, TDECK_ENTER_MASK);
        }
        click_since = 0;
    }
    if (click_down && !esc_held && click_since && now - click_since >= TRACKBALL_ESC_MS) {
        int col; uint16_t mask;
        if (tdeck_map_special(TDECK_KEY_ESC, &col, &mask)) {
            hold_key(col, mask);
            esc_held = true;
            Serial.println("trackball Esc");
        }
    }
}

static void hal_keyboard_poll(void *ctx, uint16_t *matrix, int num_cols) {
    (void)ctx;
    int cols = (num_cols < CYBIKO_KEYBOARD_COLUMNS) ? num_cols : CYBIKO_KEYBOARD_COLUMNS;
    memcpy(matrix, key_matrix, cols * sizeof(uint16_t));
}

// ---------------------------------------------------------------------------
// Audio - I2S output
// ---------------------------------------------------------------------------

#define I2S_PORT       I2S_NUM_0
#define I2S_SAMPLE_RATE 48000
// Each frame writes 800 samples (16.7 ms) and waits for room, so the I2S
// clock paces the emulator (see pace_frame). The queue is what absorbs a
// run of slow frames (CyOS redrawing on every key press); its length is
// also how far sound lags the screen. 8 x 400-sample DMA buffers ≈ 67 ms.
#ifndef CYBIKO_AUDIO_BUFS
#define CYBIKO_AUDIO_BUFS 8    // ~67 ms: 18 buffers made keys feel 150 ms late
#endif
#define I2S_DMA_BUF_COUNT CYBIKO_AUDIO_BUFS
#define I2S_DMA_BUF_LEN   400

static bool i2s_initialized = false;
static QueueHandle_t i2s_events;
static uint32_t i2s_underruns;  // DMA ran dry: an audible gap

// The T-Deck has no volume knob. $p and $m step the level up and down,
// from 0 (off) to 10; the level is kept in NVS. Gain is 256 for
// full scale, which was too loud on the T-Deck's amplifier.
//   level  0  1  2  3   4   5   6   7   8   9   10
//   %      0  3  6  9  12  18  25  37  50  75  100
static const uint16_t VOLUME_Q8[] = { 0, 8, 15, 23, 31, 46, 64, 95, 128, 192, 256 };
#define VOLUME_MAX (sizeof VOLUME_Q8 / sizeof VOLUME_Q8[0] - 1)
#define VOLUME_DEFAULT 8   // 50%
static uint8_t volume_idx = VOLUME_DEFAULT;
static uint8_t volume_saved = VOLUME_DEFAULT;

static void volume_load(void) {
    Preferences p;
    p.begin("cybiko", true);
    // "vol" held an index into the old 5-step table; this is a new key.
    volume_idx = p.getUChar("level", VOLUME_DEFAULT);
    p.end();
    if (volume_idx > VOLUME_MAX) volume_idx = VOLUME_DEFAULT;
    volume_saved = volume_idx;
}

static void volume_note(char *buf, size_t n) {
    if (volume_idx)
        snprintf(buf, n, "volume %u", volume_idx);
    else
        snprintf(buf, n, "volume off");
}

// Takes effect at once; NVS is written once volume mode ends.
static void volume_step(int by) {
    int v = volume_idx + by;
    volume_idx = v < 0 ? 0 : v > (int)VOLUME_MAX ? VOLUME_MAX : v;
    volume_until = millis() + VOLUME_MODE_MS;
    if (!volume_until) volume_until = 1;   // 0 means off
    draw_chrome();
    char note[16];
    volume_note(note, sizeof note);
    Serial.printf("%s\n", note);
}

static void volume_done(void) {
    volume_until = 0;
    if (volume_idx != volume_saved) {
        Preferences p;
        p.begin("cybiko", false);
        p.putUChar("level", volume_idx);
        p.end();
        volume_saved = volume_idx;
    }
    draw_chrome();
}

static void init_i2s(void) {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = I2S_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = I2S_DMA_BUF_COUNT;
    cfg.dma_buf_len = I2S_DMA_BUF_LEN;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;

    if (i2s_driver_install(I2S_PORT, &cfg, 8, &i2s_events) != ESP_OK) {
        Serial.println("I2S driver install failed");
        return;
    }

    i2s_pin_config_t pins = {};
    pins.bck_io_num = PIN_I2S_BCK;
    pins.ws_io_num = PIN_I2S_WS;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
        Serial.println("I2S set pin failed");
        return;
    }

    i2s_initialized = true;
    volume_load();
}

static void count_i2s_underruns(void) {
    i2s_event_t ev;
    while (i2s_events && xQueueReceive(i2s_events, &ev, 0) == pdTRUE)
        if (ev.type == I2S_EVENT_TX_Q_OVF)
            i2s_underruns++;
}

static int16_t i2s_conv_buf[1024];
static uint32_t audio_wait_us;  // time blocked in i2s_write, this frame
static bool boot_unpacking(void);

static void hal_audio_output(void *ctx, const uint8_t *samples, int count) {
    (void)ctx;
    if (!i2s_initialized || count <= 0) return;

    int n = (count > 1024) ? 1024 : count;
    int gain = VOLUME_Q8[volume_idx];   // 256 = the old full level
    bool silent = true;
    for (int i = 0; i < n; i++) {
        silent &= samples[i] == 128;
        i2s_conv_buf[i] = (int16_t)(((int)samples[i] - 128) * gain);
    }

    // Boot fast-forward: while CyOS is still loading, a silent frame does
    // not wait for the audio clock, so boot runs as fast as the T-Deck can
    // emulate instead of at 60 Hz. Silence that does not fit is dropped.
    // A frame with sound waits as usual, so boot sounds stay intact.
    if (silent && boot_unpacking()) {
        size_t written = 0;
        i2s_write(I2S_PORT, i2s_conv_buf, n * sizeof(int16_t), &written, 0);
        return;
    }

    // Wait for room rather than drop samples. The I2S clock and
    // esp_timer drift apart, and a non-blocking write lost a chunk every
    // time the 42 ms queue filled, which was the choppy sound. Blocking
    // here makes the I2S clock the frame clock. The timeout only guards
    // against a stalled driver.
    size_t bytes_written = 0;
    uint32_t t0 = micros();
    i2s_write(I2S_PORT, i2s_conv_buf, n * sizeof(int16_t), &bytes_written,
              pdMS_TO_TICKS(100));
    audio_wait_us += micros() - t0;
}

// ---------------------------------------------------------------------------
// Serial output
// ---------------------------------------------------------------------------

static void hal_serial_output(void *ctx, uint8_t byte) {
    (void)ctx;
    /* USB console only. The T-Deck radio is the SX1262 on SPI, not this. */
    Serial.write(byte);
}

// ---------------------------------------------------------------------------
// SD card file loading
// ---------------------------------------------------------------------------

// One folder per machine, so the Classic and the Xtreme never share ROMs
// or saves. The Classic saves its AT45DB041 image (CyOS plus the file
// system); the Xtreme saves its 2 MB RAM, whose front is the file system.
#ifdef CYBIKO_MACHINE_V1
#define SD_DIR     "/cybiko-v1"
#define SAVE_NAME  "save.flash"
#else
#define SD_DIR     "/cybiko"
#define SAVE_NAME  "save.nvram"
#endif
#define SD_PATH(name) SD_DIR "/" name

static uint8_t *load_sd_file(const char *path, size_t *out_size) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("Failed to open %s\n", path);
        return NULL;
    }
    size_t sz = f.size();
    uint8_t *buf = (uint8_t *)ps_malloc(sz);
    if (!buf) {
        Serial.printf("Failed to allocate %u bytes for %s\n", (unsigned)sz, path);
        f.close();
        return NULL;
    }
    size_t rd = f.read(buf, sz);
    f.close();
    if (rd != sz) {
        Serial.printf("Short read on %s: %u/%u\n", path, (unsigned)rd, (unsigned)sz);
        free(buf);
        return NULL;
    }
    *out_size = sz;
    Serial.printf("Loaded %s (%u bytes)\n", path, (unsigned)sz);
    return buf;
}

// NVRAM save — the Xtreme every 5 minutes, the Classic 10 s after CyOS's
// last flash write; both skip unchanged data
static uint32_t last_nvram_hash = 0;

static uint32_t simple_hash(const uint8_t *data, size_t len) {
    // FNV-1a hash on a sample of the data (every 256th byte for speed)
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i += 256) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

#ifndef CYBIKO_MACHINE_V1
static bool ends_with_app(const char *name) {
    size_t n = strlen(name);
    return n >= 4 && strcmp(name + n - 4, ".app") == 0;
}

/* Copy /cybiko/apps/*.app into the CFS image at the front of NVRAM.
 * A name already stored is left alone, so a reboot does not add it again.
 * Returns the buffer setup() should load. The caller frees it. */
static uint8_t *inject_sd_apps(uint8_t *nvram, size_t *nvram_size) {
    File dir = SD.open(SD_PATH("apps"));
    if (!dir)
        return nvram;
    if (!dir.isDirectory()) {
        dir.close();
        return nvram;
    }

    cfs_image_t *img = (cfs_image_t *)ps_malloc(sizeof(cfs_image_t));
    if (!img) {
        Serial.println("apps: no room for the filesystem image");
        dir.close();
        return nvram;
    }

    bool existing = nvram && *nvram_size >= CFS_IMAGE_SIZE;
    if (existing)
        memcpy(img->data, nvram, CFS_IMAGE_SIZE);
    else
        cfs_format(img);

    int added = 0;
    File entry = dir.openNextFile();
    while (entry) {
        const char *full = entry.name();
        const char *base = strrchr(full, '/');
        base = base ? base + 1 : full;
        bool take = !entry.isDirectory() && base[0] != '.' && ends_with_app(base);
        if (take && strlen(base) > 58) {
            Serial.printf("App name too long: %s\n", base);
            take = false;
        }
        if (take && cfs_has_file(img, base)) {
            Serial.printf("App already stored: %s\n", base);
            take = false;
        }
        if (take) {
            size_t sz = entry.size();
            uint8_t *buf = (uint8_t *)ps_malloc(sz > 0 ? sz : 1);
            size_t rd = buf ? entry.read(buf, sz) : 0;
            if (buf && rd == sz && cfs_add_file(img, base, buf, sz)) {
                Serial.printf("Added app %s (%u bytes)\n", base, (unsigned)sz);
                added++;
            } else {
                Serial.printf("Could not add app %s\n", base);
            }
            free(buf);
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    if (added == 0) {
        free(img);
        return nvram;
    }
    if (existing) {
        memcpy(nvram, img->data, CFS_IMAGE_SIZE);
        free(img);
        Serial.printf("Stored %d new app(s) into the existing save\n", added);
        return nvram;
    }

    uint8_t *fresh = (uint8_t *)ps_malloc(CFS_IMAGE_SIZE);
    if (!fresh) {
        Serial.println("apps: could not allocate a new save image");
        free(img);
        return nvram;
    }
    memcpy(fresh, img->data, CFS_IMAGE_SIZE);
    free(img);
    free(nvram);
    *nvram_size = CFS_IMAGE_SIZE;
    Serial.printf("Stored %d app(s) into a new filesystem image\n", added);
    return fresh;
}
#endif

static void save_nvram(cybiko_emu_t *emu) {
    size_t len = 0;
#ifdef CYBIKO_MACHINE_V1
    // Skip unless CyOS programmed a flash page since the last save. A
    // sampled hash would miss a 264-byte page.
    uint32_t writes = 0;
    const uint8_t *data = cybiko_get_spi_flash(emu, &len, &writes);
    if (!data || len == 0) return;
    if (writes == last_nvram_hash) return;
    last_nvram_hash = writes;
#else
    const uint8_t *data = cybiko_get_nvram(emu, &len);
    if (!data || len == 0) return;

    // Skip save if data hasn't changed
    uint32_t hash = simple_hash(data, len);
    if (hash == last_nvram_hash) return;
    last_nvram_hash = hash;
#endif

    // The card shares SPI with the panel: never start while a frame's DMA
    // is still running. Write a temp file and only replace the save once
    // every byte is on the card. Opening save.nvram for writing truncates
    // it, and an interrupted 2 MB write used to leave a short or empty file.
    tft.dmaWait();
    uint32_t t0 = millis();
    File f = SD.open(SD_PATH("save.tmp"), FILE_WRITE);
    if (!f) {
        Serial.println("NVRAM save failed: cannot open save.tmp");
        last_nvram_hash = 0;
        return;
    }
    // 4 KB at a time through an internal buffer, so a short write shows
    // where it stopped.
    static uint8_t chunk[4096];
    size_t wrote = 0;
    while (wrote < len) {
        size_t n = len - wrote < sizeof chunk ? len - wrote : sizeof chunk;
        memcpy(chunk, data + wrote, n);
        size_t w = f.write(chunk, n);
        wrote += w;
        if (w != n)
            break;
    }
    f.close();
    if (wrote != len) {
        Serial.printf("NVRAM save failed: %u of %u bytes, old save kept\n",
                      (unsigned)wrote, (unsigned)len);
        SD.remove(SD_PATH("save.tmp"));
        last_nvram_hash = 0;  // try again next interval
        return;
    }
    SD.remove(SD_PATH(SAVE_NAME));
    if (!SD.rename(SD_PATH("save.tmp"), SD_PATH(SAVE_NAME))) {
        Serial.println("NVRAM save failed: rename, data is in save.tmp");
        last_nvram_hash = 0;
        return;
    }
    Serial.printf("NVRAM saved (%u bytes, %lu ms)\n", (unsigned)len,
                  (unsigned long)(millis() - t0));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static cybiko_emu_t *emu = NULL;
static uint32_t frame_count = 0;
static unsigned long last_nvram_save_ms = 0;
static unsigned long last_fps_report = 0;
static uint32_t frame_us = 0;
static cybiko_stats_t last_stats = {};
#ifndef NVRAM_SAVE_INTERVAL_MS
#define NVRAM_SAVE_INTERVAL_MS 300000  // 5 minutes
#endif
#ifdef CYBIKO_MACHINE_V1
#define FLASH_SAVE_QUIET_MS 10000       // after CyOS's last page write
static uint32_t flash_writes_seen;
#endif
#define FPS_REPORT_INTERVAL_MS 5000

/* T-Deck LiPo → CyOS 1.2.46 ADC ch1/ch2. USB is the charger path CyOS
 * already idles on. Unplugged uses ch1 = ch2 = 341 + 36..38 (the only
 * on-battery window that still idles). CyOS blinks the pack icon at
 * level ≤ 42, so a healthy pack hides that icon by patching the two
 * CMP.W #42 sites; around 10% the compare is restored.
 *
 * Charging is USB 5V, not a PC host. GPIO4 measures the +5V rail (Q4),
 * so a wall brick reads ~4.6–5.0 V and a pack reads the cell. SOF still
 * marks a host so unplug-from-PC is instant; a charger has no SOF.
 *
 * Do not use (bool)Serial: on a self-powered S3, unplugging the cable
 * never raises BUS_RESET, so Serial stays true. SOF frames stop. */
#define BAT_USB_MV_ON   4450
#define BAT_USB_MV_OFF  4300
/* 1S LiPo: 3.60 V is about 10%. Hysteresis so the icon does not chatter. */
#define BAT_LOW_PCT_ON  10
#define BAT_LOW_PCT_OFF 14
static uint32_t bat_mv;
static bool bat_usb;
static bool bat_sof;
static bool bat_low;
static int bat_pct = 100;

static uint16_t emu_peek16(uint32_t a) {
    return ((uint16_t)cybiko_peek8(emu, a) << 8) | cybiko_peek8(emu, a + 1);
}

static uint32_t emu_peek32(uint32_t a) {
    return ((uint32_t)emu_peek16(a) << 16) | emu_peek16(a + 2);
}

static void emu_poke16(uint32_t a, uint16_t v) {
    cybiko_poke8(emu, a, (uint8_t)(v >> 8));
    cybiko_poke8(emu, a + 1, (uint8_t)v);
}

static bool ram_ptr(uint32_t p, uint32_t need) {
    return p >= 0x200000u && p + need < 0x280000u;
}

/* CyOS 1.2.46 smooths ADC 7/8 + 1/8. From charger to pack that takes
 * ~30 s, so write the battery object when the cable state changes. */
static void v1_sync_battery_ram(uint16_t ch1, uint16_t ch2) {
#ifdef CYBIKO_MACHINE_V1
    uint32_t obj = emu_peek32(0x227FD0u);
    if (ram_ptr(obj, 0x20)) {
        emu_poke16(obj + 0x1C, ch1);
        emu_poke16(obj + 0x1E, ch2);
    }
    uint32_t tab = emu_peek32(0x227FCCu);
    if (ram_ptr(tab, 0x0C)) {
        emu_poke16(tab + 6, ch1);
        emu_poke16(tab + 8, ch2);
    }
#else
    (void)ch1;
    (void)ch2;
#endif
}

static bool usb_host_via_sof(void) {
#if CONFIG_IDF_TARGET_ESP32S3
    if (USB_SERIAL_JTAG.int_raw.sof_int_raw) {
        USB_SERIAL_JTAG.int_clr.val = (1u << 1);   /* SOF, write-1-to-clear */
        return true;
    }
    return false;
#else
    return (bool)Serial;
#endif
}

/* LiPo tops out ~4.2 V. USB 5V through the Schottky sits ~4.6–5.0 V. */
static bool usb_5v_rail(uint32_t mv) {
    if (mv >= BAT_USB_MV_ON)
        return true;
    if (mv < BAT_USB_MV_OFF)
        return false;
    return bat_usb;
}

static const char *bat_src_tag(void) {
    if (bat_sof)
        return "USB";
    if (bat_usb)
        return "chg";
    if (bat_low)
        return "low";
    return "bat";
}

/* CyOS 1.2.46 only draws the blinking pack icon at level ≤ 42. That is
 * also the only on-battery window that still idles, so a healthy pack
 * cannot use a real level. Patch the two CMP.W #42 checks to 0 and the
 * icon stays off; restore 42 when the T-Deck is around 10%. */
static bool v1_set_lowbat_icon(bool show) {
#ifdef CYBIKO_MACHINE_V1
    static const uint32_t addrs[] = { 0x25B760u, 0x25B7CEu };
    uint16_t lim = show ? 42 : 0;
    int ok = 0;
    for (unsigned i = 0; i < sizeof addrs / sizeof addrs[0]; i++) {
        uint32_t a = addrs[i];
        if (cybiko_peek8(emu, a) != 0x79 || cybiko_peek8(emu, a + 1) != 0x20)
            continue;
        emu_poke16(a + 2, lim);
        ok++;
    }
    return ok == 2;
#else
    (void)show;
    return true;
#endif
}

static int lipo_pct(int mv) {
    static const int pts[][2] = {
        {3400, 0}, {3500, 5}, {3600, 10}, {3700, 20},
        {3760, 30}, {3810, 40}, {3860, 50}, {3910, 60},
        {3970, 70}, {4050, 80}, {4150, 100},
    };
    const int n = (int)(sizeof pts / sizeof pts[0]);
    if (mv <= pts[0][0]) return pts[0][1];
    if (mv >= pts[n - 1][0]) return pts[n - 1][1];
    for (int i = 1; i < n; i++) {
        if (mv <= pts[i][0]) {
            int m0 = pts[i - 1][0], p0 = pts[i - 1][1];
            int m1 = pts[i][0], p1 = pts[i][1];
            return p0 + (mv - m0) * (p1 - p0) / (m1 - m0);
        }
    }
    return 100;
}

static void tdeck_battery_apply(bool force_ram) {
#ifdef CYBIKO_MACHINE_V1
    if (!emu) return;
    /* CyOS 1.2.46 stops idling — and Chat stops sending — on real pack
     * ADC (PLAN-V1). Keep Java's charger readings in the guest so an
     * unplugged T-Deck still talks LoRa. The chrome bar uses the LiPo. */
    uint16_t ch1 = 0x0300, ch2 = 0x0100;
    cybiko_set_adc(emu, 1, ch1);
    cybiko_set_adc(emu, 2, ch2);
    v1_set_lowbat_icon(false);
    if (force_ram)
        v1_sync_battery_ram(ch1, ch2);
#else
    (void)force_ram;
#endif
}

static void tdeck_battery_poll(void) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++)
        sum += analogReadMilliVolts(PIN_BAT_ADC);
    bat_mv = (sum / 4) * (uint32_t)BAT_ADC_DIV;
    bat_pct = lipo_pct((int)bat_mv);
    if (bat_pct <= BAT_LOW_PCT_ON)
        bat_low = true;
    else if (bat_pct >= BAT_LOW_PCT_OFF)
        bat_low = false;
    tdeck_battery_apply(false);
}

#ifdef CYBIKO_DIAG_SPI
// Shared-bus check: an SD write and an SX1262 status read, run before and
// after TFT_eSPI takes the SPI host for DMA.
static void diag_spi(const char *when) {
    static uint8_t buf[4096];
    memset(buf, 0xA5, sizeof buf);
    File f = SD.open("/cybiko/diag.tmp", FILE_WRITE);
    size_t wrote = 0;
    if (f) {
        for (int i = 0; i < 16; i++) {
            size_t w = f.write(buf, sizeof buf);
            wrote += w;
            if (w != sizeof buf) break;
        }
        f.close();
    }
    SD.remove("/cybiko/diag.tmp");
    Serial.printf("DIAG %s: SD wrote %u/65536, radio status %d\n", when,
                  (unsigned)wrote, cybiko_sx1262_phy_probe());
}
#endif

void setup() {
    Serial.begin(115200);
    // Never wait on the USB host. HWCDC switches to a 100 ms timeout once a
    // host has been seen; after unplugging, or with the cable in and no
    // terminal reading, every write then stalled until it timed out. CyOS
    // console output goes out one byte at a time, so the emulator froze
    // and looked crashed. Unread output is dropped instead.
    Serial.setTxTimeoutMs(0);
    Serial.printf("Reset reason %d\n", (int)esp_reset_reason());

    int radio_check = cybiko_link_self_check();
    if (radio_check == 0)
        Serial.println("Radio codec self-check ok");
    else
        Serial.printf("Radio codec self-check failed: %d\n", radio_check);

    // Enable peripheral power
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);

    // Deselect all SPI devices
    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    pinMode(PIN_RADIO_CS, OUTPUT);
    digitalWrite(PIN_RADIO_CS, HIGH);
    pinMode(PIN_RADIO_RST, OUTPUT);
    digitalWrite(PIN_RADIO_RST, HIGH);
    pinMode(PIN_RADIO_BUSY, INPUT);
    pinMode(PIN_RADIO_DIO1, INPUT);

    // Init SPI bus
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);

    // Init I2C with keyboard startup delay. Keep the default 100 kHz: 800 kHz
    // timed out (error 263) on this C3 keyboard and dropped the loop to ~1 fps.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    delay(500);
    Wire.setTimeOut(50);
    pinMode(PIN_KB_INT, INPUT_PULLUP);
    kb_queue = xQueueCreate(KB_QUEUE_LEN, sizeof(uint8_t));
    if (kb_queue)
        xTaskCreatePinnedToCore(keyboard_task, "kb", 3072, NULL, 2, NULL, 1);
    else
        Serial.println("keyboard queue failed; polling from the emu loop");

    // Init TFT
    tft.init();
    tft.setRotation(1);  // Landscape
    tft.setSwapBytes(true);  // ESP32 is little-endian, ST7789 expects big-endian
    tft.fillScreen(TFT_BLACK);
    pinMode(PIN_TFT_BL, OUTPUT);
    light_load();
    backlight_set(LIGHT_LEVELS[light_idx]);

    // Display buffer: internal DMA-capable SRAM if a 128 KB block is free,
    // so the per-frame upscale does not stream through the PSRAM cache that
    // the guest's 2 MB RAM and flash image depend on. PSRAM otherwise.
    Serial.printf("Internal heap: %u free, largest DMA block %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    disp_buf = (uint16_t *)heap_caps_malloc(SCALED_W * SCALED_H * sizeof(uint16_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    Serial.printf("Display buffer in %s\n", disp_buf ? "internal SRAM" : "PSRAM");
    if (!disp_buf)
        disp_buf = (uint16_t *)ps_malloc(SCALED_W * SCALED_H * sizeof(uint16_t));
    if (!disp_buf) {
        Serial.println("FATAL: Failed to allocate display buffer in PSRAM");
        while (1) delay(1000);
    }
    memset(disp_buf, 0, SCALED_W * SCALED_H * sizeof(uint16_t));
    init_gray_lut();

    int radio_rc = cybiko_sx1262_begin(tdeck_bus_free);
    Serial.printf("SX1262 begin %d status=0x%02X errors=0x%04X %s\n",
                  radio_rc, cybiko_sx1262_chip_status(),
                  cybiko_sx1262_device_errors(), cybiko_sx1262_last_error());
    tft.setTextColor(radio_rc == 0 ? TFT_GREEN : TFT_RED, TFT_BLACK);
    char radio_line[48];
    snprintf(radio_line, sizeof radio_line, "SX1262 %s  st=%02X err=%04X",
             cybiko_sx1262_last_error(), cybiko_sx1262_chip_status(),
             cybiko_sx1262_device_errors());
    tft.drawString(radio_line, 10, 10, 2);

    // Init SD card
#ifndef CYBIKO_SD_HZ
#define CYBIKO_SD_HZ 20000000U  /* 800 kHz made writes stop after 12 KB */
#endif
    if (!SD.begin(PIN_SD_CS, SPI, CYBIKO_SD_HZ)) {
        Serial.println("FATAL: SD card init failed");
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("SD card failed", 10, 30, 2);
        tft.drawString("Need " SD_PATH("boot.bin"), 10, 50, 2);
        tft.drawString("and " SD_PATH("flash.bin"), 10, 70, 2);
        while (1) delay(1000);
    }
    Serial.printf("SD card initialized: %llu MB, %llu MB used\n",
                  (unsigned long long)(SD.totalBytes() >> 20),
                  (unsigned long long)(SD.usedBytes() >> 20));

    // Init I2S audio
    init_i2s();

    // Init trackball GPIOs + interrupts for pulse counting
    pinMode(PIN_TRACKBALL_UP, INPUT_PULLUP);
    pinMode(PIN_TRACKBALL_DOWN, INPUT_PULLUP);
    pinMode(PIN_TRACKBALL_LEFT, INPUT_PULLUP);
    pinMode(PIN_TRACKBALL_RIGHT, INPUT_PULLUP);
    pinMode(PIN_TRACKBALL_CLICK, INPUT_PULLUP);
    init_trackball_interrupts();

    // Init keyboard matrix
    memset(key_matrix, 0, sizeof(key_matrix));

    // Load ROMs from SD
    size_t boot_size = 0, flash_size = 0;
    uint8_t *boot_data = load_sd_file(SD_PATH("boot.bin"), &boot_size);
#ifdef CYBIKO_MACHINE_V1
    // The Classic's save is the whole flash chip; flash.bin is only the
    // factory image used until the first save. Delete save.flash to reset.
    uint8_t *flash_data = NULL;
    if (SD.exists(SD_PATH(SAVE_NAME)))
        flash_data = load_sd_file(SD_PATH(SAVE_NAME), &flash_size);
    if (!flash_data)
        flash_data = load_sd_file(SD_PATH("flash.bin"), &flash_size);
#else
    uint8_t *flash_data = load_sd_file(SD_PATH("flash.bin"), &flash_size);
#endif

    if (!boot_data || !flash_data) {
        Serial.println("FATAL: Missing boot.bin or flash.bin on SD card");
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Missing ROM files!", 10, 10, 2);
        tft.drawString("Need " SD_PATH("boot.bin"), 10, 30, 2);
        tft.drawString("and " SD_PATH("flash.bin"), 10, 50, 2);
        while (1) delay(1000);
    }

    // Create emulator
    cybiko_hal_t hal = {};
    hal.render_frame = hal_render_frame;
    hal.audio_output = hal_audio_output;
    hal.keyboard_poll = hal_keyboard_poll;
    hal.serial_output = hal_serial_output;
    hal.ctx = NULL;

    emu = cybiko_create(&hal);
    if (!emu) {
        Serial.println("FATAL: cybiko_create failed");
        while (1) delay(1000);
    }

#ifdef CYBIKO_PSRAM_BENCH
    Serial.printf("BENCH cpu %lu MHz, emu struct %s at %p, ext RAM %s\n",
                  (unsigned long)getCpuFrequencyMhz(),
                  esp_ptr_internal(emu) ? "internal" : "PSRAM", (void *)emu,
                  esp_ptr_internal(cybiko_get_nvram(emu, NULL)) ? "internal" : "PSRAM");
    {
        volatile uint32_t x = 0;
        uint32_t c0 = ESP.getCycleCount();
        for (int i = 0; i < 10000; i++) x += i;
        Serial.printf("BENCH plain C loop: %lu cycles/iteration\n",
                      (unsigned long)((ESP.getCycleCount() - c0) / 10000));
    }
    // Fixed cost per instruction: straight-line NOPs and MOV.B #imm,R0L,
    // 256 of each from on-chip RAM and from external RAM.
    {
        struct { const char *name; uint32_t at; uint8_t b0, b1; } st[] = {
            { "nop   internal", 0xFFF000, 0x00, 0x00 },
            { "nop   psram   ", 0x200000, 0x00, 0x00 },
            { "movb  internal", 0xFFF000, 0xF8, 0x12 },
            { "movb  psram   ", 0x200000, 0xF8, 0x12 },
        };
        for (auto &t : st) {
            for (int i = 0; i < 256; i++) {
                cybiko_poke8(emu, t.at + 2 * i, t.b0);
                cybiko_poke8(emu, t.at + 2 * i + 1, t.b1);
            }
            uint32_t best = 0xFFFFFFFF;
            for (int rep = 0; rep < 20; rep++) {
                uint32_t c0 = ESP.getCycleCount();
                cybiko_debug_run(emu, t.at, 256);
                uint32_t c = ESP.getCycleCount() - c0;
                if (c < best) best = c;
            }
            Serial.printf("BENCH %s %lu cycles/insn\n", t.name, (unsigned long)(best / 256));
        }
    }
    // Interpreter cost with guest code and data in internal SRAM (on-chip
    // RAM) against PSRAM (external RAM), sequential and 4 KB-strided reads.
    {
        static const uint8_t seq[] = {      // loop: 6 instructions
            0x01,0x00,0x69,0x12,            // MOV.L @ER1, ER2
            0x0A,0xA3,                      // ADD.L ER2, ER3
            0x0B,0x91,                      // ADDS #4, ER1
            0x1B,0x54,                      // DEC.W #1, R4
            0x46,0xF6 };                    // BNE loop
        static const uint8_t strided[] = {
            0x01,0x00,0x69,0x12,            // MOV.L @ER1, ER2
            0x0A,0xA3,                      // ADD.L ER2, ER3
            0x7A,0x11,0x00,0x00,0x10,0x00,  // ADD.L #0x1000, ER1
            0x1B,0x54,                      // DEC.W #1, R4
            0x46,0xF2 };                    // BNE loop
        struct { const char *name; uint32_t code, data; const uint8_t *prog; size_t len; } runs[] = {
            { "internal seq ", 0xFFF000, 0xFFF100, seq, sizeof seq },
            { "psram seq    ", 0x200000, 0x210000, seq, sizeof seq },
            { "psram strided", 0x200000, 0x210000, strided, sizeof strided },
        };
        for (auto &r : runs) {
            for (size_t i = 0; i < r.len; i++) cybiko_poke8(emu, r.code + i, r.prog[i]);
            // MOV.L #data, ER1 ; MOV.W #count, R4 set through a prologue
            uint8_t pro[] = { 0x7A,0x01,(uint8_t)(r.data>>24),(uint8_t)(r.data>>16),(uint8_t)(r.data>>8),(uint8_t)r.data,
                              0x79,0x04,0x00,0x40 };   // R4 = 64 iterations
            uint32_t pro_at = r.code - 0x20;
            for (size_t i = 0; i < sizeof pro; i++) cybiko_poke8(emu, pro_at + i, pro[i]);
            // jump from the prologue into the loop: BRA d8
            cybiko_poke8(emu, pro_at + 10, 0x40);
            cybiko_poke8(emu, pro_at + 11, (uint8_t)(r.code - (pro_at + 12)));
            uint32_t best = 0xFFFFFFFF;
            for (int rep = 0; rep < 20; rep++) {
                uint32_t c0 = ESP.getCycleCount();
                cybiko_debug_run(emu, pro_at, 3 + 64 * 5);
                uint32_t c = ESP.getCycleCount() - c0;
                if (c < best) best = c;
            }
            Serial.printf("BENCH %s %lu cycles/insn (best of 20)\n", r.name,
                          (unsigned long)(best / (3 + 64 * 5)));
            uint32_t total = 0;
            for (int rep = 0; rep < 20; rep++) {
                uint32_t c0 = ESP.getCycleCount();
                cybiko_debug_run(emu, pro_at, 3 + 64 * 5);
                total += ESP.getCycleCount() - c0;
            }
            Serial.printf("BENCH %s %lu cycles/insn (mean)\n", r.name,
                          (unsigned long)(total / 20 / (3 + 64 * 5)));
        }
    }
#endif
    cybiko_load_boot_rom(emu, boot_data, boot_size);
    cybiko_load_flash_rom(emu, flash_data, flash_size);
#ifndef CYBIKO_MACHINE_V1
    if (cybiko_sx1262_transport() && cybiko_patch_cyid(emu, cybiko_sx1262_device_id()))
        Serial.printf("CyID %08lX\n", (unsigned long)cybiko_sx1262_device_id());
#endif
    free(boot_data);
    free(flash_data);

#ifdef CYBIKO_MACHINE_V1
    {
        uint32_t writes = 0;
        cybiko_get_spi_flash(emu, NULL, &writes);
        last_nvram_hash = writes;
    }
    // Classic radio (SCI0) over LoRa. The board's MAC-derived id is both
    // the guest CyID (patched into RAM once CyOS loads) and the LoRa link
    // sender, so every T-Deck on the air is a different Cybiko.
    static cybiko_radio_coproc_t radio_cop;
    if (cybiko_sx1262_transport()) {
        cybiko_radio_coproc_init(&radio_cop);
        cybiko_radio_coproc_set_transport(&radio_cop, cybiko_sx1262_transport());
        cybiko_attach_radio_id(emu, &radio_cop, cybiko_sx1262_device_id());
        Serial.printf("Classic radio on LoRa, CyID %08lX, TX %s\n",
                      (unsigned long)cybiko_sx1262_device_id(),
                      cybiko_sx1262_link()->tx_enabled ? "on" : "off");
    }
#else
    // Load NVRAM if present, then add any new /cybiko/apps/*.app files.
    size_t nvram_size = 0;
    uint8_t *nvram_data = load_sd_file(SD_PATH(SAVE_NAME), &nvram_size);
    nvram_data = inject_sd_apps(nvram_data, &nvram_size);
    if (nvram_data) {
        cybiko_load_nvram(emu, nvram_data, nvram_size);
        // Set initial hash so we don't immediately re-save
        last_nvram_hash = simple_hash(nvram_data, nvram_size);
        free(nvram_data);
    }

    // Guest radio over the SX1262. Without CYBIKO_RADIO_TX the transport
    // refuses sends and only receives. The Classic's radio is on SCI0,
    // which is not emulated yet (PLAN-V1.md step 7).
    static cybiko_radio_coproc_t radio_cop;
    if (cybiko_sx1262_transport()) {
        cybiko_radio_coproc_init(&radio_cop);
        cybiko_radio_coproc_set_transport(&radio_cop, cybiko_sx1262_transport());
        cybiko_attach_radio(emu, &radio_cop);
        Serial.printf("Radio co-processor attached, TX %s\n",
                      cybiko_sx1262_link()->tx_enabled ? "on" : "off");
    }
#endif

    tdeck_battery_poll();
    cybiko_reset(emu);

    // Init DMA after SD card operations are complete (shared SPI bus)
#ifdef CYBIKO_DIAG_SPI
    diag_spi("before initDMA");
#endif
    tft.initDMA();
#ifdef CYBIKO_DIAG_SPI
    diag_spi("after initDMA");
    static uint16_t diag_px[320 * 20];
    tft.startWrite();
    tft.pushImageDMA(0, 0, 320, 20, diag_px);
    tft.endWrite();
    tft.dmaWait();
    diag_spi("after one DMA frame");
#endif

    last_nvram_save_ms = millis();
    last_fps_report = millis();
    emu_started_ms = millis();
    snprintf(chrome_top, sizeof chrome_top, "Loading CyOS...");
    draw_chrome();
    Serial.println("ReBiko Pocket started");
}

static void screen_follow_cyos(void) {
    if (cybiko_lcd_on(emu)) {
        lcd_off_frames = 0;
        if (screen_on) return;
        setCpuFrequencyMhz(AWAKE_MHZ);
        screen_on = true;
        shown_valid = false;      // redraw the whole frame
        backlight_set(LIGHT_LEVELS[light_idx]);
        draw_chrome();
        Serial.println("screen on");
        return;
    }
    if (!screen_on || ++lcd_off_frames < SCREEN_OFF_FRAMES) return;
    screen_on = false;
    backlight_set(0);
    setCpuFrequencyMhz(ASLEEP_MHZ);
    Serial.println("screen off: CyOS is asleep");
}

// Hold the guest to the real Cybiko's 60 Hz. Each frame is a fixed slice
// of guest time (RTC ticks once per frame, keys hold for 4 frames, audio
// is generated per frame), so running idle CyOS at 260 fps made all of
// that 4x too fast. Frames that run late are not made up in a burst.
static uint32_t pace_wait_us;  // time spent waiting, reset per report

static void pace_frame(void) {
    // With audio running, hal_audio_output already held this frame to the
    // I2S rate; a second clock here would fight it.
    if (i2s_initialized)
        return;
    static int64_t next_us3;  // deadline in 1/3 us: 60 Hz = 50000 per frame
    int64_t now3 = esp_timer_get_time() * 3;
    if (!next_us3)
        next_us3 = now3;
    next_us3 += 50000;
    int64_t ahead3 = next_us3 - now3;
    if (ahead3 <= 0) {
        if (ahead3 < -3 * 50000)
            next_us3 = now3;  // more than 3 frames behind: resync
        return;
    }
    uint32_t ahead_us = (uint32_t)(ahead3 / 3);
    pace_wait_us += ahead_us;
    if (ahead_us > 2000)
        vTaskDelay(pdMS_TO_TICKS((ahead_us - 1000) / 1000));
    while (esp_timer_get_time() * 3 < next_us3) {
    }
}

#ifdef CYBIKO_PROFILE
// Top guest PCs (16-byte buckets) since the last report.
static void report_pc_samples(void) {
    static uint32_t pcs[CYBIKO_PC_SAMPLES];
    int n = cybiko_take_pc_samples(emu, pcs, CYBIKO_PC_SAMPLES);
    for (int i = 0; i < n; i++) pcs[i] &= ~0xFu;
    std::sort(pcs, pcs + n);
    uint32_t top_pc[8] = {}; int top_n[8] = {};
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && pcs[j] == pcs[i]) j++;
        int c = j - i;
        for (int k = 0; k < 8; k++) {
            if (c > top_n[k]) {
                memmove(&top_pc[k + 1], &top_pc[k], (7 - k) * sizeof top_pc[0]);
                memmove(&top_n[k + 1], &top_n[k], (7 - k) * sizeof top_n[0]);
                top_pc[k] = pcs[i]; top_n[k] = c;
                break;
            }
        }
        i = j;
    }
    Serial.printf("  pc samples %d:", n);
    for (int k = 0; k < 8 && top_n[k]; k++)
        Serial.printf(" %06lX:%d", (unsigned long)top_pc[k], top_n[k]);
    Serial.println();
}
#endif

void loop() {
    // Poll input. Tick after the guest has seen this frame's keys: ticking
    // first cut every synthetic hold by a frame, and Esc never lasted long
    // enough for CyOS to scan it.
    poll_input();

    uint32_t t0 = micros();
    audio_wait_us = 0;
    /* Radio before the guest so a slow Reed-Solomon poll frame cannot
     * sit on an SX1262 packet for a whole timeslice. If DIO1 is high the
     * chip has a packet; wait out display DMA so we actually read it. */
    if (digitalRead(PIN_RADIO_DIO1) == HIGH && tft.dmaBusy())
        tft.dmaWait();
    cybiko_sx1262_pump();
    cybiko_run_frame(emu);
    // Time blocked on the audio queue is pacing, not emulation
    frame_us += micros() - t0 - audio_wait_us;
    pace_wait_us += audio_wait_us;
    keyboard_tick_frame();
    count_i2s_underruns();

    if (digitalRead(PIN_RADIO_DIO1) == HIGH && tft.dmaBusy())
        tft.dmaWait();
    cybiko_sx1262_pump();
    {
        bool sof = usb_host_via_sof();
        if (sof != bat_sof || (frame_count % 60) == 0) {
            tdeck_battery_poll();
            bool usb = sof || usb_5v_rail(bat_mv);
            bool changed = (usb != bat_usb) || (sof != bat_sof);
            bat_sof = sof;
            bat_usb = usb;
            if (changed)
                tdeck_battery_apply(true);
        } else {
            v1_set_lowbat_icon(!bat_usb && bat_low);
        }
    }
    if (!boot_done) {
        cybiko_stats_t bst;
        cybiko_get_stats(emu, &bst);
        check_boot_done(&bst);
    } else {
        screen_follow_cyos();
    }

    frame_count++;

    // FPS reporting every 5 seconds
    unsigned long now = millis();
    if (now - last_fps_report >= FPS_REPORT_INTERVAL_MS) {
        float elapsed_sec = (now - last_fps_report) / 1000.0f;
        float fps = frame_count / elapsed_sec;
        float speed_pct = (fps / 60.0f) * 100.0f;
        // fps alone hides guest speed: with EMU_TIMER_STRIDE each frame
        // carries a fixed cycle budget. Report guest instructions/s and
        // host cycles per guest instruction for the time spent outside
        // the display callback.
        cybiko_stats_t st;
        cybiko_get_stats(emu, &st);
        uint64_t insns = st.instructions - last_stats.instructions;
        uint64_t slept = st.sleep_cycles - last_stats.sleep_cycles;
        uint32_t frames = st.frames - last_stats.frames;
        uint32_t cpu_us = frame_us > render_us ? frame_us - render_us : 0;
        float kips = insns / elapsed_sec / 1000.0f;
        float cyc_per_insn = insns ? (cpu_us * (float)getCpuFrequencyMhz()) / insns : 0.0f;
        float sleep_pct = frames ? 100.0f * slept /
            ((float)frames * CYBIKO_CYCLES_PER_FRAME) : 0.0f;
        Serial.printf("%.1f fps (%.0f%%)  %.0f kIPS  %.0f cyc/insn  "
                      "sleep %.0f%%  core %lu ms  render %lu ms  idle %lu ms  "
                      "audio gaps %lu  bat %lu mV %s %d%%  "
                      "cpu %lu MHz\n",
                      fps, speed_pct, kips, cyc_per_insn, sleep_pct,
                      (unsigned long)(cpu_us / 1000),
                      (unsigned long)(render_us / 1000),
                      (unsigned long)(pace_wait_us / 1000),
                      (unsigned long)i2s_underruns,
                      (unsigned long)bat_mv, bat_src_tag(),
                      bat_pct, (unsigned long)getCpuFrequencyMhz());
        i2s_underruns = 0;
        pace_wait_us = 0;
#ifdef CYBIKO_SPLIT_PROFILE
        {
            if (cybiko_split[2])
                Serial.printf("  split: timers %lu cyc/insn, cpu %lu cyc/insn over %lu insns\n",
                              (unsigned long)(cybiko_split[0] / cybiko_split[2]),
                              (unsigned long)(cybiko_split[1] / cybiko_split[2]),
                              (unsigned long)cybiko_split[2]);
            cybiko_split[0] = cybiko_split[1] = cybiko_split[2] = 0;
        }
#endif
#ifdef CYBIKO_MACHINE_V1
        {
            uint32_t fw = 0;
            cybiko_get_spi_flash(emu, NULL, &fw);
            cybiko_v1_radio_status_t rs;
            if (cybiko_v1_radio_status(emu, &rs))
                Serial.printf("  flash pages written %lu  classic radio: %s "
                              "sent %lu delivered %lu\n", (unsigned long)fw,
                              rs.radio_up ? "up" : "-",
                              (unsigned long)rs.tx_frames, (unsigned long)rs.rx_injected);
            else
                Serial.printf("  flash pages written %lu\n", (unsigned long)fw);
        }
#endif
        if (const cybiko_lora_t *lk = cybiko_sx1262_link()) {
            const cybiko_sx1262_stats_t *rs = cybiko_sx1262_phy_stats();
            Serial.printf("  radio rx %lu (crc %lu, link bad %lu, msgs %lu, lost %lu) "
                          "tx %lu done %lu rejected %lu waited %lu  rssi %d snr %d  bus waits %lu\n",
                          (unsigned long)rs->rx_ok, (unsigned long)rs->rx_crc,
                          (unsigned long)lk->stats.rx_bad, (unsigned long)lk->stats.rx_msgs,
                          (unsigned long)lk->stats.rx_lost,
                          (unsigned long)lk->stats.tx_pkts, (unsigned long)rs->tx_done,
                          (unsigned long)lk->stats.tx_rejected,
                          (unsigned long)rs->tx_deferred,
                          rs->last_rssi_dbm, rs->last_snr_db,
                          (unsigned long)rs->bus_waits);
        }
#ifdef CYBIKO_PROFILE
        report_pc_samples();
#endif
        last_stats = st;
        frame_us = 0;
        render_us = 0;
        snprintf(chrome_top, sizeof chrome_top, "%.0f fps  %.2fV %s %d%%",
                 fps, bat_mv / 1000.0f, bat_src_tag(), bat_pct);
        draw_chrome();
        frame_count = 0;
        last_fps_report = now;
    }

    if (volume_until && (long)(now - volume_until) >= 0)
        volume_done();
    link_tick(now);

#ifdef CYBIKO_MACHINE_V1
    // Save the flash image once CyOS has stopped writing it for 10 s, so a
    // changed setting is on the card soon after, not up to 5 minutes later.
    // A failed save is retried 10 s on.
    {
        size_t len;
        uint32_t writes = 0;
        cybiko_get_spi_flash(emu, &len, &writes);
        if (writes != flash_writes_seen) {
            flash_writes_seen = writes;
            last_nvram_save_ms = now;
        }
        if (writes != last_nvram_hash && now - last_nvram_save_ms >= FLASH_SAVE_QUIET_MS) {
            save_nvram(emu);
            last_nvram_save_ms = now;
        }
    }
#else
    // Auto-save NVRAM every 5 minutes
    if (now - last_nvram_save_ms >= NVRAM_SAVE_INTERVAL_MS) {
        save_nvram(emu);
        last_nvram_save_ms = now;
    }
#endif

    pace_frame();
}
