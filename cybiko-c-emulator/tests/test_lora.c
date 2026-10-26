#include "acutest.h"
#include "radio/radio_lora.h"

#include <string.h>

/*
 * Fake PHY pair. A packet accepted by tx() stays "on the air" for
 * `airtime` polls of the sender, then lands in the peer's inbox. One
 * packet in flight at a time, like an SX1262.
 */
#define FAKE_INBOX 16

typedef struct fake_phy {
    cybiko_radio_phy_t base;
    struct fake_phy *peer;
    int airtime;
    int busy_left;
    uint8_t air[CYBIKO_LINK_MAX_PHYS];
    size_t air_len;
    int tx_count;
    int max_in_flight;
    bool corrupt_next;
    struct { uint8_t pkt[CYBIKO_LINK_MAX_PHYS]; size_t len; } inbox[FAKE_INBOX];
    int in_head, in_count;
} fake_phy_t;

static int fake_tx(cybiko_radio_phy_t *self, const uint8_t *pkt, size_t len) {
    fake_phy_t *p = self->impl;
    if (p->busy_left > 0 || len > self->mtu) return -1;
    memcpy(p->air, pkt, len);
    p->air_len = len;
    if (p->corrupt_next) {
        p->air[len - 1] ^= 0x5A;
        p->corrupt_next = false;
    }
    p->busy_left = p->airtime;
    p->tx_count++;
    if (p->max_in_flight < 1) p->max_in_flight = 1;
    return 0;
}

static int fake_tx_busy(cybiko_radio_phy_t *self) {
    fake_phy_t *p = self->impl;
    return p->busy_left > 0;
}

static int fake_rx(cybiko_radio_phy_t *self, uint8_t *buf, size_t cap, size_t *len) {
    fake_phy_t *p = self->impl;
    if (p->in_count == 0) return 0;
    size_t n = p->inbox[p->in_head].len;
    if (n > cap) n = cap;
    memcpy(buf, p->inbox[p->in_head].pkt, n);
    *len = n;
    p->in_head = (p->in_head + 1) % FAKE_INBOX;
    p->in_count--;
    return 1;
}

static void fake_poll(cybiko_radio_phy_t *self) {
    fake_phy_t *p = self->impl;
    if (p->busy_left == 0) return;
    if (--p->busy_left > 0) return;
    fake_phy_t *d = p->peer;
    if (!d || d->in_count == FAKE_INBOX) return;
    int slot = (d->in_head + d->in_count) % FAKE_INBOX;
    memcpy(d->inbox[slot].pkt, p->air, p->air_len);
    d->inbox[slot].len = p->air_len;
    d->in_count++;
}

static fake_phy_t phy_a, phy_b;
static cybiko_lora_t lora_a, lora_b;

static struct {
    int count;
    uint8_t data[CYBIKO_LINK_MAX_PAYLOAD];
    size_t len;
    uint8_t channel;
    uint32_t sender;
} got;

static void on_packet(void *user, const uint8_t *data, size_t len,
                      uint8_t channel, uint32_t sender) {
    (void)user;
    got.count++;
    memcpy(got.data, data, len);
    got.len = len;
    got.channel = channel;
    got.sender = sender;
}

static void setup(size_t mtu, int airtime, bool tx_on) {
    memset(&phy_a, 0, sizeof phy_a);
    memset(&phy_b, 0, sizeof phy_b);
    memset(&got, 0, sizeof got);
    fake_phy_t *p[2] = { &phy_a, &phy_b };
    for (int i = 0; i < 2; i++) {
        p[i]->base.impl = p[i];
        p[i]->base.tx = fake_tx;
        p[i]->base.tx_busy = fake_tx_busy;
        p[i]->base.rx = fake_rx;
        p[i]->base.poll = fake_poll;
        p[i]->base.mtu = mtu;
        p[i]->airtime = airtime;
    }
    phy_a.peer = &phy_b;
    phy_b.peer = &phy_a;
    cybiko_lora_init(&lora_a, &phy_a.base, 0x10000001u, tx_on);
    cybiko_lora_init(&lora_b, &phy_b.base, 0x10000002u, tx_on);
    lora_b.base.set_listener(&lora_b.base, on_packet, NULL);
}

static void pump_both(int n) {
    for (int i = 0; i < n; i++) {
        lora_a.base.pump(&lora_a.base);
        lora_b.base.pump(&lora_b.base);
    }
}

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7);
}

static void test_chat_crosses(void) {
    setup(CYBIKO_LORA_PHY_MAX, 3, true);
    uint8_t chat[192];
    fill(chat, sizeof chat, 1);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, chat, sizeof chat, 4) == 0);
    pump_both(10);
    TEST_CHECK(got.count == 1);
    TEST_CHECK(got.len == sizeof chat && memcmp(got.data, chat, sizeof chat) == 0);
    TEST_CHECK(got.channel == 4);
    TEST_CHECK(got.sender == 0x10000001u);
    TEST_CHECK(lora_a.stats.tx_pkts == 1 && lora_b.stats.rx_msgs == 1);
}

static void test_tx_gate(void) {
    setup(CYBIKO_LORA_PHY_MAX, 1, false);
    uint8_t b[42];
    fill(b, sizeof b, 9);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, b, sizeof b, 2) == -1);
    pump_both(5);
    TEST_CHECK(phy_a.tx_count == 0);
    TEST_CHECK(lora_a.stats.tx_rejected == 1);
    /* Receive works with transmit off */
    cybiko_lora_set_tx_enabled(&lora_a, true);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, b, sizeof b, 2) == 0);
    pump_both(5);
    TEST_CHECK(got.count == 1);
}

static void test_fragments_one_at_a_time(void) {
    setup(64, 4, true);
    uint8_t chat[192];
    fill(chat, sizeof chat, 3);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, chat, sizeof chat, 2) == 0);
    TEST_CHECK(lora_a.txq_count == 4);  /* 192 / (64 - 16) */
    for (int i = 0; i < 40 && got.count == 0; i++) {
        pump_both(1);
        TEST_CHECK(phy_a.busy_left <= phy_a.airtime);
    }
    TEST_CHECK(got.count == 1);
    TEST_CHECK(memcmp(got.data, chat, sizeof chat) == 0);
    TEST_CHECK(phy_a.tx_count == 4);
}

static void test_queue_full_is_all_or_nothing(void) {
    setup(64, 1000, true);
    uint8_t chat[192];
    fill(chat, sizeof chat, 5);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, chat, sizeof chat, 2) == 0);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, chat, sizeof chat, 2) == 0);
    TEST_CHECK(lora_a.txq_count == 8);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, chat, sizeof chat, 2) == -1);
    TEST_CHECK(lora_a.txq_count == 8);
    TEST_CHECK(lora_a.stats.tx_rejected == 1);
    TEST_CHECK(lora_a.stats.tx_msgs == 2);
}

static void test_bad_packet_counted(void) {
    setup(CYBIKO_LORA_PHY_MAX, 1, true);
    uint8_t b[42];
    fill(b, sizeof b, 11);
    phy_a.corrupt_next = true;
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, b, sizeof b, 2) == 0);
    pump_both(5);
    TEST_CHECK(got.count == 0);
    TEST_CHECK(lora_b.stats.rx_bad == 1);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, b, sizeof b, 2) == 0);
    pump_both(5);
    TEST_CHECK(got.count == 1);
}

/* A node that restarts begins its message ids again. Its peer still
 * remembers the old ids and used to drop the new messages as repeats. */
static void test_restart_with_new_seed(void) {
    setup(CYBIKO_LORA_PHY_MAX, 1, true);
    uint8_t x[42];
    fill(x, sizeof x, 7);
    for (int i = 0; i < 3; i++) {
        TEST_CHECK(lora_a.base.send_packet(&lora_a.base, x, sizeof x, 2) == 0);
        pump_both(4);
    }
    TEST_CHECK(got.count == 3);
    cybiko_lora_init(&lora_a, &phy_a.base, 0x10000001u, true);   /* restart */
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, x, sizeof x, 2) == 0);
    pump_both(4);
    TEST_CHECK(got.count == 3);   /* id 1 again: dropped as a repeat */
    cybiko_lora_init(&lora_a, &phy_a.base, 0x10000001u, true);
    cybiko_lora_seed_msg_id(&lora_a, 0x5A5A1234u);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, x, sizeof x, 2) == 0);
    pump_both(4);
    TEST_CHECK(got.count == 4);
    TEST_CHECK(lora_b.stats.rx_lost == 0);   /* a restart is not loss */
}

static void test_lost_messages_counted(void) {
    setup(CYBIKO_LORA_PHY_MAX, 1, true);
    uint8_t x[42];
    fill(x, sizeof x, 13);
    for (int i = 0; i < 6; i++) {
        phy_a.corrupt_next = (i == 1 || i == 3 || i == 4);
        TEST_CHECK(lora_a.base.send_packet(&lora_a.base, x, sizeof x, 2) == 0);
        pump_both(4);
    }
    TEST_CHECK(got.count == 3);
    TEST_CHECK(lora_b.stats.rx_lost == 3);
}

static void test_both_directions(void) {
    setup(CYBIKO_LORA_PHY_MAX, 2, true);
    uint8_t x[42], y[42];
    fill(x, sizeof x, 20);
    fill(y, sizeof y, 40);
    TEST_CHECK(lora_a.base.send_packet(&lora_a.base, x, sizeof x, 2) == 0);
    TEST_CHECK(lora_b.base.send_packet(&lora_b.base, y, sizeof y, 2) == 0);
    pump_both(6);
    TEST_CHECK(got.count == 1 && memcmp(got.data, x, sizeof x) == 0);
    TEST_CHECK(lora_a.stats.rx_msgs == 1);
}

TEST_LIST = {
    { "192-byte chat crosses with sender and channel", test_chat_crosses },
    { "transmit gate rejects, receive still works", test_tx_gate },
    { "fragments go out one at a time", test_fragments_one_at_a_time },
    { "full queue rejects the whole message", test_queue_full_is_all_or_nothing },
    { "bad packet is counted, not delivered", test_bad_packet_counted },
    { "both directions at once", test_both_directions },
    { "restarted sender with a new seed is not a repeat", test_restart_with_new_seed },
    { "gaps in message ids are counted as lost", test_lost_messages_counted },
    { NULL, NULL }
};
