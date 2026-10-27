/*
 * PCF8593 Real-Time Clock emulation with I2C interface.
 *
 * Connected to CPU Port F on the Cybiko Xtreme:
 *   Write: bit 1 (0x02) = SCL, bit 6 (0x40) = SDA (inverted)
 *   Read:  bit 6 (0x40) = SDA output
 *
 * I2C address: 0xA2 (write) / 0xA3 (read)
 *
 * Internal registers are BCD format.
 */
#include "core/rtc.h"
#include <string.h>
#include <time.h>

/* I2C addresses */
#define I2C_ADDR_WRITE 0xA2
#define I2C_ADDR_READ  0xA3

static uint8_t to_bcd(int val) {
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

static int from_bcd(uint8_t bcd) {
    return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

static uint64_t get_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Days in month m (1-12); the chip's year is year % 4, and 0 is a leap year
 * from 2000 to 2099 */
static int month_days(int m, int year4) {
    static const uint8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 31;
    return m == 2 && year4 == 0 ? 29 : DAYS[m - 1];
}

static void advance_seconds(rtc_t *rtc) {
    int sec = from_bcd(rtc->data[2]) + 1;
    if (sec < 60) { rtc->data[2] = to_bcd(sec); return; }
    rtc->data[2] = 0;

    int min = from_bcd(rtc->data[3]) + 1;
    if (min < 60) { rtc->data[3] = to_bcd(min); return; }
    rtc->data[3] = 0;

    int hour = from_bcd(rtc->data[4]) + 1;
    if (hour < 24) { rtc->data[4] = to_bcd(hour); return; }
    rtc->data[4] = 0;

    int day = from_bcd(rtc->data[5] & 0x3F) + 1;
    uint8_t year_bits = rtc->data[5] & 0xC0;
    int month = from_bcd(rtc->data[6] & 0x1F);
    if (day <= month_days(month, year_bits >> 6)) { rtc->data[5] = year_bits | to_bcd(day); return; }
    rtc->data[5] = year_bits | to_bcd(1);

    month++;
    if (month <= 12) { rtc->data[6] = (uint8_t)((rtc->data[6] & 0xE0) | to_bcd(month)); return; }
    rtc->data[6] = (uint8_t)((rtc->data[6] & 0xE0) | to_bcd(1));

    int year = ((rtc->data[5] >> 6) & 3) + 1;
    rtc->data[5] = (uint8_t)(((year % 4) << 6) | (rtc->data[5] & 0x3F));
}

void rtc_init(rtc_t *rtc) {
    memset(rtc, 0, sizeof(*rtc));

    /* Initialize with current system time */
    time_t now_t = time(NULL);
    struct tm *tm = localtime(&now_t);

    rtc->data[0] = 0x00;  /* Control: counting enabled */
    rtc->data[1] = 0x00;  /* Hundredths */
    rtc->data[2] = to_bcd(tm->tm_sec);
    rtc->data[3] = to_bcd(tm->tm_min);
    rtc->data[4] = to_bcd(tm->tm_hour);
    rtc->data[5] = (uint8_t)(((tm->tm_year % 4) << 6) | to_bcd(tm->tm_mday));
    rtc->data[6] = to_bcd(tm->tm_mon + 1);  /* tm_mon is 0-based */

    rtc->pin_scl = 1;
    rtc->pin_sda = 1;
    rtc->inp     = 1;
    rtc->mode    = RTC_MODE_RECV;
    rtc->active  = false;

    rtc->last_tick_ns = get_monotonic_ns();
    rtc->frac_ns = 0;
}

void rtc_tick(rtc_t *rtc) {
    /* Only advance if counting is enabled (bit 7 of control = 0) */
    if (rtc->data[0] & 0x80) return;

    uint64_t now = get_monotonic_ns();
    rtc->frac_ns += now - rtc->last_tick_ns;
    rtc->last_tick_ns = now;

    /* Advance seconds */
    while (rtc->frac_ns >= 1000000000ULL) {
        rtc->frac_ns -= 1000000000ULL;
        advance_seconds(rtc);
    }
}

bool rtc_sda_r(const rtc_t *rtc) {
    return rtc->inp != 0;
}

static void clear_buffer_rx(rtc_t *rtc) {
    memset(rtc->data_recv, 0, sizeof(rtc->data_recv));
    rtc->data_recv_index = 0;
}

void rtc_scl_w(rtc_t *rtc, bool high) {
    int state = high ? 1 : 0;

    /* Process on rising edge of SCL while active */
    if (rtc->active && rtc->pin_scl == 0 && state != 0) {
        switch (rtc->mode) {
        case RTC_MODE_RECV:
            /* Release SDA at start of each new byte (was low for ACK) */
            if (rtc->bits == 0) {
                rtc->inp = 1;
            }
            /* HOST -> RTC: clock in a bit */
            if (rtc->pin_sda != 0) {
                rtc->data_recv[rtc->data_recv_index] |= (0x80 >> rtc->bits);
            }
            rtc->bits++;
            /* After 8 data bits + 1 ACK = bit 9 */
            if (rtc->bits > 8) {
                /* ACK: pull SDA low to acknowledge */
                rtc->inp = 0;
                uint8_t received = rtc->data_recv[rtc->data_recv_index];

                /* First byte 0xA3 = switch to read/send mode */
                if (rtc->data_recv[0] == I2C_ADDR_READ && rtc->data_recv_index == 0) {
                    rtc->mode = RTC_MODE_SEND;
                }
                /* First byte 0xA2 + second byte = set register position */
                if (rtc->data_recv[0] == I2C_ADDR_WRITE && rtc->data_recv_index == 1) {
                    rtc->pos = rtc->data_recv[1] & 0x0F;
                }
                /* 0xA2 + pos + data bytes = write registers */
                if (rtc->data_recv[0] == I2C_ADDR_WRITE && rtc->data_recv_index >= 2) {
                    int rtc_pos = (rtc->data_recv[1] + (rtc->data_recv_index - 2)) & 0x0F;
                    rtc->data[rtc_pos] = received;
                }

                rtc->bits = 0;
                rtc->data_recv_index++;
            }
            break;

        case RTC_MODE_SEND:
            /* RTC -> HOST: clock out a bit */
            rtc->inp = (rtc->data[rtc->pos] >> (7 - rtc->bits)) & 1;
            rtc->bits++;
            /* After 8 data bits + ACK */
            if (rtc->bits > 8) {
                /* Check master ACK/NACK */
                if (rtc->pin_sda != 0) {
                    /* Master NACK = end of read */
                    rtc->mode = RTC_MODE_RECV;
                    rtc->inp = 1;
                    clear_buffer_rx(rtc);
                }
                rtc->bits = 0;
                rtc->pos = (rtc->pos + 1) & 0x0F;
            }
            break;
        }
    }

    rtc->pin_scl = state;
}

void rtc_sda_w(rtc_t *rtc, bool high) {
    int state = high ? 1 : 0;

    /* Check for START/STOP conditions while SCL is high */
    if (rtc->pin_scl != 0) {
        /* START: SDA high -> low while SCL high */
        if (state == 0 && rtc->pin_sda != 0) {
            rtc->active = true;
            rtc->bits = 0;
            rtc->data_recv_index = 0;
            clear_buffer_rx(rtc);
        }
        /* STOP: SDA low -> high while SCL high */
        if (state != 0 && rtc->pin_sda == 0) {
            rtc->active = false;
            rtc->inp = 1;
        }
    }

    rtc->pin_sda = state;
}
