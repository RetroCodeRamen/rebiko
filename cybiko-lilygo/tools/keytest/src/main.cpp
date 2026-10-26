/*
 * T-Deck keyboard and trackball test. No emulator.
 *
 * Polls the keyboard controller (I2C 0x55) every 2 ms and prints every
 * byte it returns, the time since the previous key, and the KB_INT line.
 * Trackball steps and the click are printed too. The screen shows the
 * last key large and a history list.
 *
 * Serial lines:
 *   KEY 0x71 113 'q'  t=12345 ms  gap=480 ms  int=1
 *   BALL up 3 | BALL click down | BALL click up
 *
 * Built with -DKEYTEST_RAW (env keytest-raw) it puts the keyboard in raw
 * mode (command 0x03): 5 bytes per poll, one per column, bit n = row n,
 * set while held. Each change prints
 *   RAW 00 40 00 00 00  down c1r6  t=12345
 */
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>

#include "tdeck_pins.h"

static TFT_eSPI tft;

#ifdef KEYTEST_RAW
#define KEYTEST_MODE "raw"
#else
#define KEYTEST_MODE "key"
#endif

static volatile int tb_up, tb_down, tb_left, tb_right;
static void IRAM_ATTR isr_up(void)    { tb_up++; }
static void IRAM_ATTR isr_down(void)  { tb_down++; }
static void IRAM_ATTR isr_left(void)  { tb_left++; }
static void IRAM_ATTR isr_right(void) { tb_right++; }

#define HISTORY 9
static char history[HISTORY][40];
static int hist_count;
static uint32_t key_count;

static void add_history(const char *line) {
    if (hist_count == HISTORY) {
        memmove(history[0], history[1], sizeof history[0] * (HISTORY - 1));
        hist_count--;
    }
    strncpy(history[hist_count], line, sizeof history[0] - 1);
    history[hist_count][sizeof history[0] - 1] = 0;
    hist_count++;
}

static void draw(const char *big) {
    tft.fillRect(0, 30, 320, 210, TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(big, 10, 34, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    for (int i = 0; i < hist_count; i++)
        tft.drawString(history[i], 10, 72 + i * 18, 2);
}

static const char *name_of(uint8_t c, char *buf, size_t n) {
    switch (c) {
    case 0x08: return "Backspace";
    case 0x0D: return "Enter";
    case 0x20: return "Space";
    case 0x1B: return "Esc";
    }
    if (c >= 0x21 && c < 0x7F) {
        snprintf(buf, n, "'%c'", c);
        return buf;
    }
    snprintf(buf, n, "code %u", c);
    return buf;
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    pinMode(PIN_RADIO_CS, OUTPUT);
    digitalWrite(PIN_RADIO_CS, HIGH);

    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    delay(500);  // keyboard controller start-up

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("KEY TEST  (no emulator)", 10, 6, 2);

    pinMode(PIN_KB_INT, INPUT);
    pinMode(PIN_TRACKBALL_UP, INPUT_PULLUP);
    pinMode(PIN_TRACKBALL_DOWN, INPUT_PULLUP);
    pinMode(PIN_TRACKBALL_LEFT, INPUT_PULLUP);
    pinMode(PIN_TRACKBALL_RIGHT, INPUT_PULLUP);
    pinMode(PIN_TRACKBALL_CLICK, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_UP), isr_up, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_DOWN), isr_down, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_LEFT), isr_left, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TRACKBALL_RIGHT), isr_right, FALLING);

    Wire.beginTransmission(KB_I2C_ADDR);
    int ack = Wire.endTransmission();
    Serial.printf("KEYTEST start, keyboard at 0x%02X %s\n", KB_I2C_ADDR,
                  ack == 0 ? "answers" : "does NOT answer");
    // The controller keeps its mode across an ESP32 reset, and ReBiko
    // Pocket leaves it in raw mode: set the mode this test wants.
    Wire.beginTransmission(KB_I2C_ADDR);
#ifdef KEYTEST_RAW
    Wire.write(0x03);
#else
    Wire.write(0x04);
#endif
    Serial.printf("%s mode command %s\n", KEYTEST_MODE, Wire.endTransmission() == 0 ? "sent" : "failed");
    draw(ack == 0 ? "Press a key" : "Keyboard not found");
}

#ifdef KEYTEST_RAW
static void poll_raw(uint32_t now) {
    static uint8_t last[5];
    uint8_t cur[5] = {};
    int n = Wire.requestFrom(KB_I2C_ADDR, (int)5);
    for (int i = 0; i < 5 && Wire.available(); i++) cur[i] = Wire.read();
    if (n != 5 || memcmp(cur, last, 5) == 0) return;
    char line[64];
    int len = snprintf(line, sizeof line, "RAW %02X %02X %02X %02X %02X ",
                       cur[0], cur[1], cur[2], cur[3], cur[4]);
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 8; r++) {
            uint8_t bit = 1u << r;
            if ((cur[c] ^ last[c]) & bit)
                len += snprintf(line + len, sizeof line - len, " %s c%dr%d",
                                (cur[c] & bit) ? "down" : "up", c, r);
        }
    memcpy(last, cur, 5);
    Serial.printf("%s  t=%lu\n", line, (unsigned long)now);
    add_history(line + 4);
    draw(line + 4);
}
#endif

void loop() {
    static uint32_t last_key_ms, last_poll;
    static int last_int = -1;
    static bool click_down;
    uint32_t now = millis();

    int kint = digitalRead(PIN_KB_INT);
    if (kint != last_int) {
        if (last_int >= 0)
            Serial.printf("INT %d  t=%lu\n", kint, (unsigned long)now);
        last_int = kint;
    }

#ifdef KEYTEST_RAW
    if (now - last_poll >= 10) {
        last_poll = now;
        poll_raw(now);
    }
    if (0) {
#else
    if (now - last_poll >= 2) {
#endif
        last_poll = now;
        if (Wire.requestFrom(KB_I2C_ADDR, (int)1) == 1) {
            uint8_t c = Wire.read();
            if (c) {
                char nb[16], line[48];
                key_count++;
                uint32_t gap = last_key_ms ? now - last_key_ms : 0;
                last_key_ms = now;
                Serial.printf("KEY 0x%02X %3u '%c'  t=%lu ms  gap=%lu ms  int=%d\n", c, c,
                              c >= 0x20 && c < 0x7F ? c : '.', (unsigned long)now,
                              (unsigned long)gap, kint);
                snprintf(line, sizeof line, "#%lu  0x%02X  %s", (unsigned long)key_count, c,
                         name_of(c, nb, sizeof nb));
                add_history(line);
                draw(name_of(c, nb, sizeof nb));
            }
        }
    }

    noInterrupts();
    int u = tb_up, d = tb_down, l = tb_left, r = tb_right;
    tb_up = tb_down = tb_left = tb_right = 0;
    interrupts();
    const char *dirs[4] = { "up", "down", "left", "right" };
    int counts[4] = { u, d, l, r };
    for (int i = 0; i < 4; i++) {
        if (counts[i]) {
            char line[40];
            Serial.printf("BALL %s %d  t=%lu\n", dirs[i], counts[i], (unsigned long)now);
            snprintf(line, sizeof line, "ball %s x%d", dirs[i], counts[i]);
            add_history(line);
            draw(line);
        }
    }
    bool click = digitalRead(PIN_TRACKBALL_CLICK) == LOW;
    if (click != click_down) {
        click_down = click;
        Serial.printf("BALL click %s  t=%lu\n", click ? "down" : "up", (unsigned long)now);
        if (click) {
            add_history("ball click");
            draw("ball click");
        }
    }
}
