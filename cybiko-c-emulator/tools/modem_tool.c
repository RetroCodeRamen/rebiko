/*
 * Bench check for the Heltec modem, without CyOS.
 *
 *   modem_tool /dev/ttyUSB0 [--seconds N] [--ping MS] [--quiet]
 *
 * Opens the modem, prints its hello and status, and runs radio_lora over
 * it: every received payload is printed with its sender, and with --ping
 * a numbered "PING" payload is sent every MS milliseconds on the ping
 * channel, which CyOS ignores. Every 5 s it prints what was heard and
 * lost: run it here and walk with a T-Deck showing $l for a range test.
 */
#include "radio_lora.h"
#include "radio_serial_phy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void on_text(void *user, char type, const char *text) {
    (void)user;
    printf("modem %c: %s\n", type, text);
    fflush(stdout);
}

static void on_packet(void *user, const uint8_t *data, size_t len,
                      uint8_t channel, uint32_t sender) {
    cybiko_serial_phy_t *s = user;
    printf("rx from %08X ch %u len %zu rssi %d snr %d: ", (unsigned)sender, channel,
           len, s->stats.last_rssi_dbm, s->stats.last_snr_db);
    for (size_t i = 0; i < len && i < 48; i++)
        putchar(data[i] >= 0x20 && data[i] < 0x7F ? data[i] : '.');
    putchar('\n');
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /dev/ttyUSB0 [--seconds N] [--ping MS]\n", argv[0]);
        return 2;
    }
    int seconds = 30, ping_ms = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ping") && i + 1 < argc) ping_ms = atoi(argv[++i]);
    }

    static cybiko_serial_phy_t sp;
    if (cybiko_serial_phy_open(&sp, argv[1]) != 0) {
        perror(argv[1]);
        return 1;
    }
    sp.on_text = on_text;

    static cybiko_lora_t lt;
    cybiko_lora_init(&lt, &sp.phy, 0x00C0FFEE, ping_ms > 0);
    lt.base.set_listener(&lt.base, on_packet, &sp);

    uint64_t start = now_ms(), next_ping = start + 1500, next_status = start + 300;
    unsigned seq = 0;
    while (now_ms() - start < (uint64_t)seconds * 1000u) {
        lt.base.pump(&lt.base);
        uint64_t t = now_ms();
        if (t >= next_status) {
            cybiko_serial_phy_request_status(&sp);
            if (t > start + 1000) {
                unsigned heard = lt.stats.rx_msgs, lost = lt.stats.rx_lost;
                printf("link heard %u lost %u (%.1f%%) last rssi %d snr %d\n", heard, lost,
                       heard + lost ? 100.0 * lost / (heard + lost) : 0.0,
                       sp.stats.last_rssi_dbm, sp.stats.last_snr_db);
                fflush(stdout);
            }
            next_status = t + 5000;
        }
        if (ping_ms > 0 && t >= next_ping) {
            char msg[32];
            int n = snprintf(msg, sizeof msg, "PING %u from PC", seq++);
            int rc = lt.base.send_packet(&lt.base, (const uint8_t *)msg, (size_t)n,
                                         CYBIKO_LORA_PING_CHANNEL);
            printf("tx %s -> %d\n", msg, rc);
            fflush(stdout);
            next_ping = t + (uint64_t)ping_ms;
        }
        usleep(2000);
    }
    lt.base.pump(&lt.base);
    printf("summary: lora tx_msgs %u tx_pkts %u rejected %u rx_pkts %u rx_msgs %u rx_bad %u rx_lost %u | "
           "modem sent %u timeout %u refused %u lost %u rx %u\n",
           lt.stats.tx_msgs, lt.stats.tx_pkts, lt.stats.tx_rejected, lt.stats.rx_pkts,
           lt.stats.rx_msgs, lt.stats.rx_bad, lt.stats.rx_lost, sp.stats.tx_sent, sp.stats.tx_timeout,
           sp.stats.tx_refused, sp.stats.tx_lost, sp.stats.rx_pkts);
    cybiko_serial_phy_close(&sp);
    return 0;
}
