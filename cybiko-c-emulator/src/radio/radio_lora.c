#include "radio_lora.h"
#include "radio_log.h"

#include <string.h>
#ifndef ARDUINO
#include <stdio.h>
#include <stdarg.h>
void cybiko_radio_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
#endif

static void radio_snip(char *dst, size_t cap, const uint8_t *d, size_t n) {
    size_t o = 0, i;
    if (!dst || cap == 0) return;
    i = n > 19 ? 19 : 0;   /* Chat text starts at 19 */
    for (; i < n && o + 1 < cap && o < 20; i++) {
        unsigned char c = d[i];
        dst[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    dst[o] = 0;
}

static int lora_start(cybiko_radio_transport_t *self) {
    (void)self;
    return 0;
}

static void lora_close(cybiko_radio_transport_t *self) { (void)self; }

static void lora_set_channel(cybiko_radio_transport_t *self, uint8_t channel) {
    cybiko_lora_t *t = self->impl;
    t->channel = channel;
}

static void lora_set_listener(cybiko_radio_transport_t *self,
                              cybiko_radio_on_packet_fn fn, void *user) {
    cybiko_lora_t *t = self->impl;
    t->fn = fn;
    t->fn_user = user;
}

static int lora_send(cybiko_radio_transport_t *self,
                     const uint8_t *data, size_t len, uint8_t channel) {
    cybiko_lora_t *t = self->impl;
    if (!t->tx_enabled) {
        t->stats.tx_rejected++;
        return -1;
    }
    {
        char snip[24];
        radio_snip(snip, sizeof snip, data, len);
        cybiko_radio_log("tx type=%02X len=%zu %s\n",
                         len > 1 ? data[1] : 0, len, snip);
    }
    uint8_t pkts[CYBIKO_LINK_MAX_FRAGS][CYBIKO_LINK_MAX_PHYS];
    uint16_t lens[CYBIKO_LINK_MAX_FRAGS];
    int nf = cybiko_link_fragment(t->next_msg_id, self->device_id, channel,
                                  data, len, t->phy->mtu, pkts, lens,
                                  CYBIKO_LINK_MAX_FRAGS);
    /* All fragments or none: half a message only wastes airtime. */
    if (nf < 0 || nf > CYBIKO_LORA_TXQ - t->txq_count) {
        t->stats.tx_rejected++;
        return -1;
    }
    t->next_msg_id++;
    for (int i = 0; i < nf; i++) {
        int slot = (t->txq_head + t->txq_count) % CYBIKO_LORA_TXQ;
        t->txq[slot].len = lens[i];
        memcpy(t->txq[slot].pkt, pkts[i], lens[i]);
        t->txq_count++;
    }
    t->stats.tx_msgs++;
    return 0;
}

/* Each sender numbers its messages 1, 2, 3, ... so a gap is a message
 * this radio never completed (lost or corrupted on the air). */
static void count_lost(cybiko_lora_t *t, uint32_t sender, uint16_t id) {
    int i;
    for (i = 0; i < CYBIKO_LORA_PEERS; i++)
        if (t->peers[i].used && t->peers[i].sender == sender)
            break;
    if (i == CYBIKO_LORA_PEERS) {
        i = t->peer_next;
        t->peer_next = (t->peer_next + 1) % CYBIKO_LORA_PEERS;
        t->peers[i].used = true;
        t->peers[i].sender = sender;
    } else {
        uint16_t gap = (uint16_t)(id - t->peers[i].last_id);
        if (gap > 1 && gap <= CYBIKO_LORA_MAX_GAP)
            t->stats.rx_lost += gap - 1u;
    }
    t->peers[i].last_id = id;
}

static void lora_pump(cybiko_radio_transport_t *self) {
    cybiko_lora_t *t = self->impl;
    cybiko_radio_phy_t *phy = t->phy;
    t->pumps++;
    if (phy->poll)
        phy->poll(phy);

    uint8_t pkt[CYBIKO_LINK_MAX_PHYS];
    uint8_t out[CYBIKO_LINK_MAX_PAYLOAD];
    size_t len;
    while (phy->rx(phy, pkt, sizeof pkt, &len) == 1) {
        t->stats.rx_pkts++;
        size_t out_len = 0;
        uint8_t channel = 0;
        uint32_t sender = 0;
        int rc = cybiko_link_push(&t->dec, pkt, len, out, sizeof out,
                                  &out_len, &channel, &sender);
        if (rc < 0) {
            t->stats.rx_bad++;
        } else if (rc == 1) {
            {
                char snip[24];
                radio_snip(snip, sizeof snip, out, out_len);
                cybiko_radio_log("rx type=%02X len=%zu from %08X %s\n",
                                 out_len > 1 ? out[1] : 0, out_len,
                                 (unsigned)sender, snip);
            }
            t->stats.rx_msgs++;
            count_lost(t, sender, (uint16_t)((pkt[3] << 8) | pkt[4]));
            if (t->fn)
                t->fn(t->fn_user, out, out_len, channel, sender);
        }
    }

    if (t->txq_count > 0 && !phy->tx_busy(phy)) {
        if (phy->tx(phy, t->txq[t->txq_head].pkt, t->txq[t->txq_head].len) == 0) {
            t->txq_head = (t->txq_head + 1) % CYBIKO_LORA_TXQ;
            t->txq_count--;
            t->stats.tx_pkts++;
        }
    }
}

void cybiko_lora_init(cybiko_lora_t *t, cybiko_radio_phy_t *phy,
                      uint32_t device_id, bool tx_enabled) {
    memset(t, 0, sizeof *t);
    t->phy = phy;
    t->next_msg_id = 1;
    t->tx_enabled = tx_enabled;
    t->base.device_id = device_id;
    t->base.impl = t;
    t->base.start = lora_start;
    t->base.close = lora_close;
    t->base.send_packet = lora_send;
    t->base.set_channel = lora_set_channel;
    t->base.set_listener = lora_set_listener;
    t->base.pump = lora_pump;
}

void cybiko_lora_seed_msg_id(cybiko_lora_t *t, uint32_t seed) {
    t->next_msg_id = (uint16_t)(seed ^ (seed >> 16));
}

void cybiko_lora_set_tx_enabled(cybiko_lora_t *t, bool on) {
    t->tx_enabled = on;
}
