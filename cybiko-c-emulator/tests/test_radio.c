#include "acutest.h"
#include "radio/radio_coproc.h"
#include "radio/radio_link.h"
#include "radio/radio_sim.h"

#include <string.h>

static radio_sim_bus_t bus;
static radio_sim_node_t node[3];
static cybiko_radio_coproc_t cpu[3];

static void setup_n(int n, size_t mtu) {
    radio_sim_bus_init(&bus);
    for (int i = 0; i < n; i++) {
        uint32_t id = 0x10000001u + (uint32_t)i;
        radio_sim_node_init(&node[i], &bus, id, mtu);
        radio_sim_bus_add(&bus, &node[i]);
        cybiko_radio_coproc_init(&cpu[i]);
        cybiko_radio_coproc_set_transport(&cpu[i], &node[i].base);
    }
}

static void beacon(uint8_t *p) {
    memset(p, 0, 42);
    p[0] = 0xC2;
    p[1] = 0x01;
    memcpy(p + 12, "Cybiko", 6);
}

static void chat(uint8_t *p, const char *msg) {
    memset(p, 0, 192);
    p[0] = 0xC4;
    p[1] = 0x20;
    p[18] = (uint8_t)strlen(msg);
    memcpy(p + 19, msg, strlen(msg));
}

static size_t wrap_xt(uint8_t *dst, uint32_t cyid, const uint8_t *payload, size_t plen) {
    size_t n = 8 + plen + 1;
    memset(dst, 0, n);
    memset(dst, 0xFF, 4);
    dst[4] = (uint8_t)(cyid >> 24);
    dst[5] = (uint8_t)(cyid >> 16);
    dst[6] = (uint8_t)(cyid >> 8);
    dst[7] = (uint8_t)cyid;
    memcpy(dst + 8, payload, plen);
    return n;
}

static void test_self_check(void) {
    TEST_CHECK(cybiko_link_self_check() == 0);
}

static void test_channel_command_and_short_drop(void) {
    setup_n(2, 255);
    cybiko_radio_coproc_receive(&cpu[0], 0x01);
    cybiko_radio_coproc_receive(&cpu[0], 0x02);
    cybiko_radio_coproc_receive(&cpu[0], 0x04);
    TEST_CHECK(cpu[0].initialized);
    TEST_CHECK(cpu[0].current_channel == 4);
    TEST_CHECK(node[0].listen_channel == 4);
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[0]) == 0x03);

    uint8_t cmd[4] = {0x01, 0x03, 0x00, 0x00};
    cybiko_radio_coproc_handle_transmit(&cpu[0], cmd, sizeof cmd);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 0);
    TEST_CHECK(cpu[0].stat_tx == 0);
}

static void test_tuned_channel_only(void) {
    setup_n(2, 255);
    uint8_t payload[192], raw[210];
    chat(payload, "main");
    size_t n = wrap_xt(raw, 0x10000001u, payload, 192);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);   /* not tuned yet */

    cybiko_radio_coproc_receive(&cpu[1], 0x01);
    cybiko_radio_coproc_receive(&cpu[1], 0x02);
    cybiko_radio_coproc_receive(&cpu[1], 0x08);               /* Computers */
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);     /* MAIN, ch 4 */
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);
    TEST_CHECK(cpu[1].stat_off_channel == 1);

    payload[0] = 0xC8;
    n = wrap_xt(raw, 0x10000001u, payload, 192);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 2);
}

static void test_beacon_and_chat_roundtrip(void) {
    setup_n(2, 64); /* forces the 192-byte chat across several PHY packets */
    uint8_t payload[192];
    uint8_t raw[210];
    beacon(payload);
    size_t n = wrap_xt(raw, 0x10000001u, payload, 42);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);

    chat(payload, "hello");
    n = wrap_xt(raw, 0x10000001u, payload, 192);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);

    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[0]) == 0);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 2);
    const cybiko_radio_frame_t *b = cybiko_radio_coproc_peek(&cpu[1], 0);
    const cybiko_radio_frame_t *m = cybiko_radio_coproc_peek(&cpu[1], 1);
    /* frames keep CyOS's address header: 8 bytes, then the payload */
    TEST_CHECK(b && b->len == 50 && b->data[9] == 0x01);
    TEST_CHECK(b->sender_id == 0x10000001u);
    TEST_CHECK(m && m->len == 200 && m->data[9] == 0x20);
    TEST_CHECK(memcmp(m->data + 8 + 19, "hello", 5) == 0);

    /* Poll must not consume the chat sitting behind... beacon is first and small.
     * Deliver the beacon, then hold the chat on a following poll. */
    cybiko_radio_coproc_receive(&cpu[1], 0x30);
    cybiko_radio_coproc_receive(&cpu[1], 0x00);
    TEST_CHECK(cybiko_radio_coproc_complete_tx_dtc(&cpu[1]) == 0x32);
    cybiko_radio_coproc_prepare_rx(&cpu[1], 50);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&cpu[1]) == 50);
    uint8_t got[50];
    for (int i = 0; i < 50; i++)
        got[i] = (uint8_t)cybiko_radio_coproc_read(&cpu[1]);
    TEST_CHECK(got[0] == 0xFF && got[1] == 0xFF && got[2] == 0xFF && got[3] == 0xFF);
    TEST_CHECK(got[4] == 0x10 && got[5] == 0x00 && got[6] == 0x00 && got[7] == 0x01);
    TEST_CHECK(got[8] == 0xC2 && got[9] == 0x01);
    TEST_CHECK(memcmp(got + 20, "Cybiko", 6) == 0);

    TEST_CHECK(cybiko_radio_coproc_complete_tx_dtc(&cpu[1]) == 0x32);
    cybiko_radio_coproc_prepare_rx(&cpu[1], 50);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);
    for (int i = 0; i < 50; i++)
        TEST_CHECK(cybiko_radio_coproc_read(&cpu[1]) == 0x00);

    cybiko_radio_coproc_receive(&cpu[1], 0xCF);
    cybiko_radio_coproc_receive(&cpu[1], 0x00);
    TEST_CHECK(cybiko_radio_coproc_complete_tx_dtc(&cpu[1]) == 0xC8);
    cybiko_radio_coproc_prepare_rx(&cpu[1], 200);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 0);
    uint8_t big[8];
    for (int i = 0; i < 8; i++)
        big[i] = (uint8_t)cybiko_radio_coproc_read(&cpu[1]);
    TEST_CHECK(big[7] == 0x01);
    uint8_t type = 0, lenb = 0;
    char text[8] = {0};
    for (int i = 8; i < 200; i++) {
        uint8_t by = (uint8_t)cybiko_radio_coproc_read(&cpu[1]);
        if (i == 9) type = by;
        if (i == 26) lenb = by;
        if (i >= 27 && i < 32) text[i - 27] = (char)by;
    }
    TEST_CHECK(type == 0x20);
    TEST_CHECK(lenb == 5);
    TEST_CHECK(memcmp(text, "hello", 5) == 0);
}

static void test_opaque_type_and_null_and_cap(void) {
    setup_n(2, 64);
    uint8_t payload[60];
    memset(payload, 0x5A, sizeof payload);
    payload[0] = 0xC2;
    payload[1] = 0x99;
    uint8_t raw[80];
    size_t n = wrap_xt(raw, 0x10000001u, payload, sizeof payload);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);
    const cybiko_radio_frame_t *f = cybiko_radio_coproc_peek(&cpu[1], 0);
    TEST_CHECK(f && f->len == 68 && f->data[9] == 0x99);

    /* prepare without a ready flag must not eat the frame */
    cybiko_radio_coproc_prepare_rx(&cpu[1], 50);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[1]) == 0x00);

    for (int i = 0; i < 5; i++) {
        payload[2] = (uint8_t)i;
        n = wrap_xt(raw, 0x10000001u, payload, 42);
        /* first slot already holds the 60-byte frame; these are beacons */
        uint8_t b[42];
        beacon(b);
        b[2] = (uint8_t)i;
        n = wrap_xt(raw, 0x10000001u, b, 42);
        cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    }
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 4);
    /* oldest was the 60-byte frame, then beacon 0. Both gone. Head is beacon 1. */
    f = cybiko_radio_coproc_peek(&cpu[1], 0);
    TEST_CHECK(f && f->len == 50 && f->data[10] == 1);
}

static void test_loss_dup_latency_peers(void) {
    setup_n(3, 255);
    bus.drop_every = 1;
    uint8_t payload[42], raw[64];
    beacon(payload);
    size_t n = wrap_xt(raw, 0x10000001u, payload, 42);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 0);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[2]) == 0);
    TEST_CHECK(bus.stat_loss >= 1);

    bus.drop_every = 0;
    bus.duplicate = true;
    bus.seq = 0;
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[2]) == 1);
    TEST_CHECK(node[1].dec.stat_dup >= 1);

    bus.duplicate = false;
    bus.hold_pumps = 2;
    bus.seq = 0;
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 2);
}

static void test_async_and_v1_paths(void) {
    setup_n(2, 255);
    uint8_t payload[192], raw[210];
    chat(payload, "yo");
    size_t n = wrap_xt(raw, 0x10000001u, payload, 192);
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);

    TEST_CHECK(cybiko_radio_coproc_try_async(&cpu[1]) == 0xC8);
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[1]) == 0xC8);
    cybiko_radio_coproc_prepare_rx(&cpu[1], 200);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 0);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&cpu[1]) == 200);

    /* V1: empty exchange reports the large/null indicator and does not block. */
    cybiko_radio_coproc_set_v1(&cpu[0], true);
    cybiko_radio_coproc_receive(&cpu[0], 0x30);
    cybiko_radio_coproc_receive(&cpu[0], 0x00);
    cybiko_radio_coproc_v1_tx_complete(&cpu[0]);
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[0]) == 0x03);
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[0]) == 0xC8);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&cpu[0]) == 200);
}

static void test_bad_crc_injected(void) {
    setup_n(1, 255);
    uint8_t payload[42];
    beacon(payload);
    uint8_t pkts[CYBIKO_LINK_MAX_FRAGS][CYBIKO_LINK_MAX_PHYS];
    uint16_t lens[CYBIKO_LINK_MAX_FRAGS];
    int nf = cybiko_link_fragment(3, 0xABCDu, 2, payload, 42, 255, pkts, lens, CYBIKO_LINK_MAX_FRAGS);
    TEST_CHECK(nf == 1);
    pkts[0][20] ^= 0xFF;
    TEST_CHECK(radio_sim_inject(&node[0], pkts[0], lens[0], 0) == 0);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[0]) == 0);
    TEST_CHECK(node[0].dec.stat_crc >= 1);
}

static size_t v2_beacon(void *user, uint8_t *dst, size_t cap) {
    (void)user;
    if (cap < 42) return 0;
    beacon(dst);
    return 42;
}

static void test_v2_beacon_uses_small_indicator(void) {
    setup_n(2, 255);
    cybiko_radio_coproc_set_v2(&cpu[0], true);
    cybiko_radio_coproc_set_v2_beacon(&cpu[0], v2_beacon, 0);
    cybiko_radio_coproc_receive(&cpu[0], 0x30);
    cybiko_radio_coproc_receive(&cpu[0], 0x00);
    /* No peer frame yet: ACK, large/null indicator, 200 zero bytes. */
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[0]) == 0x03);
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[0]) == 0xC8);

    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);

    cybiko_radio_coproc_receive(&cpu[1], 0x01);
    cybiko_radio_coproc_receive(&cpu[1], 0x04);
    cybiko_radio_coproc_receive(&cpu[1], 0x00);
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[1]) == 0x03);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&cpu[1]) == 0);
    TEST_CHECK(cybiko_radio_coproc_pending(&cpu[1]) == 1);
}

static void test_address_header_kept(void) {
    setup_n(3, 255);
    uint8_t payload[42], raw[64];
    beacon(payload);
    size_t n = wrap_xt(raw, 0x10000001u, payload, 42);
    raw[0] = 0x10; raw[1] = 0x00; raw[2] = 0x00; raw[3] = 0x02;   /* to B only */
    cybiko_radio_coproc_handle_transmit(&cpu[0], raw, n);
    radio_sim_bus_pump(&bus);
    /* the AVR filters nothing: C hears it too, still addressed to B */
    for (int k = 1; k <= 2; k++) {
        TEST_CHECK(cybiko_radio_coproc_try_async(&cpu[k]) == 0x32);
        TEST_CHECK(cybiko_radio_coproc_read(&cpu[k]) == 0x32);
        cybiko_radio_coproc_prepare_rx(&cpu[k], 50);
        uint8_t got[50];
        for (int i = 0; i < 50; i++)
            got[i] = (uint8_t)cybiko_radio_coproc_read(&cpu[k]);
        TEST_CHECK(memcmp(got, raw, 50) == 0);
    }
}

static void test_classic_avr_replies(void) {
    setup_n(2, 255);
    cybiko_radio_coproc_set_classic(&cpu[0], true);
    cybiko_radio_coproc_set_v1(&cpu[0], true);
    /* no reply to an 01 command */
    cybiko_radio_coproc_receive(&cpu[0], 0x01);
    cybiko_radio_coproc_receive(&cpu[0], 0x02);
    cybiko_radio_coproc_receive(&cpu[0], 0x04);
    TEST_CHECK(cpu[0].current_channel == 4);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&cpu[0]) == 0);
    /* a frame with nothing heard: 0x03 only, no empty frame after it */
    cybiko_radio_coproc_receive(&cpu[0], 0x30);
    cybiko_radio_coproc_receive(&cpu[0], 0x00);
    cybiko_radio_coproc_v1_tx_complete(&cpu[0]);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&cpu[0]) == 1);
    TEST_CHECK(cybiko_radio_coproc_read(&cpu[0]) == 0x03);
    TEST_CHECK(cybiko_radio_coproc_complete_tx_dtc(&cpu[0]) == -1);
    /* with a frame waiting, its start follows */
    uint8_t payload[42], raw[64];
    beacon(payload);
    payload[0] = 0xC4;   /* channel 4, as tuned */
    size_t n = wrap_xt(raw, 0x10000002u, payload, 42);
    cybiko_radio_coproc_handle_transmit(&cpu[1], raw, n);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_complete_tx_dtc(&cpu[0]) == 0x32);
}

TEST_LIST = {
    { "link self-check", test_self_check },
    { "channel command and short frame drop", test_channel_command_and_short_drop },
    { "only the tuned channel is heard", test_tuned_channel_only },
    { "beacon and chat across a small MTU", test_beacon_and_chat_roundtrip },
    { "opaque type, null read, queue cap", test_opaque_type_and_null_and_cap },
    { "loss, duplicate, latency, three peers", test_loss_dup_latency_peers },
    { "async delivery and V1 null exchange", test_async_and_v1_paths },
    { "bad CRC is not delivered", test_bad_crc_injected },
    { "V2 beacon and init clears the UART queue", test_v2_beacon_uses_small_indicator },
    { "the address header crosses unchanged", test_address_header_kept },
    { "Classic AVR replies", test_classic_avr_replies },
    { NULL, NULL }
};
