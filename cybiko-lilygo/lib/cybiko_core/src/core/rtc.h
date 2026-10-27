#ifndef CYBIKO_RTC_H
#define CYBIKO_RTC_H
#include "types.h"

typedef enum { RTC_MODE_RECV, RTC_MODE_SEND } rtc_mode_t;

typedef struct {
    uint8_t    data[16];
    bool       active;
    rtc_mode_t mode;
    int        pin_scl, pin_sda, inp;
    int        bits, pos, data_recv_index;
    uint8_t    data_recv[50];
    uint64_t   last_tick_ns, frac_ns;
    bool       time_touched;   /* registers 1-6 written in this transfer */
    bool       time_written;   /* the guest set the time (not its boot reset) */
} rtc_t;

void cybiko_rtc_init(rtc_t *rtc);
void rtc_scl_w(rtc_t *rtc, bool high);
void rtc_sda_w(rtc_t *rtc, bool high);
bool rtc_sda_r(const rtc_t *rtc);
void rtc_tick(rtc_t *rtc);

struct tm;
/* The clock as a struct tm. The chip keeps only year % 4; the year is the
 * one nearest year_hint with that remainder. */
void rtc_get_tm(const rtc_t *rtc, struct tm *out, int year_hint);
/* Set the clock (seconds and up), keeping its control register */
void rtc_set_tm(rtc_t *rtc, const struct tm *tm);

#endif
