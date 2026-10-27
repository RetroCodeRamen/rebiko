#include "radio_coproc.h"

#include <string.h>

void cybiko_radio_coproc_init(cybiko_radio_coproc_t *c) {
    memset(c, 0, sizeof *c);
}

static void rxq_clear(cybiko_radio_coproc_t *c) {
    c->rxq_head = 0;
    c->rxq_tail = 0;
    c->rxq_count = 0;
}

static void rxq_push(cybiko_radio_coproc_t *c, uint8_t value) {
    if (c->rxq_count >= CYBIKO_RADIO_RXQ) {
        c->stat_overflow++;
        return;
    }
    c->rxq[c->rxq_tail] = value;
    c->rxq_tail = (c->rxq_tail + 1) % CYBIKO_RADIO_RXQ;
    c->rxq_count++;
}

void cybiko_radio_coproc_queue_response(cybiko_radio_coproc_t *c, uint8_t value) {
    rxq_push(c, value);
}

int cybiko_radio_coproc_read(cybiko_radio_coproc_t *c) {
    if (c->rxq_count == 0) return 0xFF;
    uint8_t v = c->rxq[c->rxq_head];
    c->rxq_head = (c->rxq_head + 1) % CYBIKO_RADIO_RXQ;
    c->rxq_count--;
    return v;
}

int cybiko_radio_coproc_rx_count(const cybiko_radio_coproc_t *c) {
    return c->rxq_count;
}

int cybiko_radio_coproc_pending(const cybiko_radio_coproc_t *c) {
    return c->q_count;
}

const cybiko_radio_frame_t *cybiko_radio_coproc_peek(const cybiko_radio_coproc_t *c, int index) {
    if (index < 0 || index >= c->q_count) return NULL;
    return &c->frames[(c->q_head + index) % CYBIKO_RADIO_RX_CAP];
}

static void queue_frame(cybiko_radio_coproc_t *c, const uint8_t *data, size_t len, uint32_t sender) {
    if (!data || len == 0 || len > CYBIKO_RADIO_MAX_PAYLOAD) {
        c->stat_tx_drop++;
        return;
    }
    while (c->q_count >= CYBIKO_RADIO_RX_CAP) {
        c->q_head = (c->q_head + 1) % CYBIKO_RADIO_RX_CAP;
        c->q_count--;
        c->rx_frame_ready = false;
        c->async_rx_pending = false;
        c->stat_overflow++;
    }
    int tail = (c->q_head + c->q_count) % CYBIKO_RADIO_RX_CAP;
    cybiko_radio_frame_t *f = &c->frames[tail];
    memcpy(f->data, data, len);
    f->len = (uint16_t)len;
    f->sender_id = sender;
    c->q_count++;
    c->stat_rx++;
}

static cybiko_radio_frame_t *peek_frame(cybiko_radio_coproc_t *c) {
    if (c->q_count == 0) return NULL;
    return &c->frames[c->q_head];
}

static void pop_frame(cybiko_radio_coproc_t *c) {
    if (c->q_count == 0) return;
    c->q_head = (c->q_head + 1) % CYBIKO_RADIO_RX_CAP;
    c->q_count--;
}

/* A real RF2915 hears only the channel it is tuned to. Each Chat room is
 * its own channel (MAIN 4, Romantic 6, Computers 8, Sports 10, Games 52,
 * Music 54; the desktop is 2), so without this a room shows messages
 * posted in another. Until CyOS tunes, everything is let through. */
static void on_packet(void *user, const uint8_t *data, size_t len,
                      uint8_t channel, uint32_t sender_id) {
    cybiko_radio_coproc_t *c = (cybiko_radio_coproc_t *)user;
    if (c->channel_set && channel != c->current_channel) {
        c->stat_off_channel++;
        return;
    }
    queue_frame(c, data, len, sender_id);
}

void cybiko_radio_coproc_set_transport(cybiko_radio_coproc_t *c, cybiko_radio_transport_t *t) {
    c->transport = t;
    if (t && t->set_listener)
        t->set_listener(t, on_packet, c);
}

void cybiko_radio_coproc_set_v1(cybiko_radio_coproc_t *c, bool v1) { c->v1_mode = v1; }
void cybiko_radio_coproc_set_v2(cybiko_radio_coproc_t *c, bool v2) { c->v2_mode = v2; }
void cybiko_radio_coproc_set_classic(cybiko_radio_coproc_t *c, bool classic) { c->classic = classic; }
void cybiko_radio_coproc_set_v2_beacon(cybiko_radio_coproc_t *c,
                                      cybiko_radio_beacon_fn fn, void *user) {
    c->beacon_fn = fn;
    c->beacon_user = user;
}

static void send_payload(cybiko_radio_coproc_t *c, const uint8_t *payload, size_t len, uint8_t channel) {
    if (!c->transport || !c->transport->send_packet) {
        c->stat_tx_drop++;
        return;
    }
    int rc = c->transport->send_packet(c->transport, payload, len, channel);
    if (rc == 0) {
        c->stat_tx++;
        c->last_tx_len = (uint16_t)len;
        memset(c->last_tx, 0, sizeof c->last_tx);
        memcpy(c->last_tx, payload, len < sizeof c->last_tx ? len : sizeof c->last_tx);
    } else {
        c->stat_tx_drop++;
    }
}

void cybiko_radio_coproc_handle_transmit(cybiko_radio_coproc_t *c,
                                        const uint8_t *data, size_t len) {
    if (!c || !data) return;
    /* Address header plus the XT trailing byte. Shorter buffers are AVR
     * commands (for example 01 03 00 00) and must not hit the air. */
    if (len <= CYBIKO_RADIO_HDR + 1) {
        c->stat_tx_drop++;
        return;
    }
    size_t end = c->v1_mode ? len : len - 1;
    if (end > CYBIKO_RADIO_MAX_PAYLOAD) end = CYBIKO_RADIO_MAX_PAYLOAD;
    uint8_t channel = data[CYBIKO_RADIO_HDR] & 0x3F;
    send_payload(c, data, end, channel);
}

void cybiko_radio_coproc_prepare_rx(cybiko_radio_coproc_t *c, int count) {
    if (count < 0) count = 0;
    if (count > CYBIKO_RADIO_RXQ) count = CYBIKO_RADIO_RXQ;

    bool take = c->rx_frame_ready || c->async_rx_pending;
    c->rx_frame_ready = false;
    c->async_rx_pending = false;

    if (take && c->q_count > 0) {
        /* the frame as it was sent, address header included */
        cybiko_radio_frame_t *f = peek_frame(c);
        pop_frame(c);
        int n = f->len < count ? f->len : count;
        for (int i = 0; i < n; i++)
            rxq_push(c, f->data[i]);
        for (int i = n; i < count; i++)
            rxq_push(c, 0xFF);
    } else {
        for (int i = 0; i < count; i++)
            rxq_push(c, 0x00);
    }
}

int cybiko_radio_coproc_complete_tx_dtc(cybiko_radio_coproc_t *c) {
    c->v1_accum = false;
    c->rx_frame_ready = false;
    cybiko_radio_frame_t *next = peek_frame(c);
    bool held = next && c->last_command == CYBIKO_RADIO_CMD_POLL && next->len > 50;
    if (c->classic && (!next || held))
        return -1;   /* the AVR says only 0x03 */
    if (!next || held)
        return CYBIKO_RADIO_IND_SMALL;
    c->rx_frame_ready = true;
    return (next->len > 50) ? CYBIKO_RADIO_IND_LARGE : CYBIKO_RADIO_IND_SMALL;
}

int cybiko_radio_coproc_try_async(cybiko_radio_coproc_t *c) {
    cybiko_radio_frame_t *next = peek_frame(c);
    if (!next) return -1;
    int indicator = (next->len > 50) ? CYBIKO_RADIO_IND_LARGE : CYBIKO_RADIO_IND_SMALL;
    c->async_rx_pending = true;
    rxq_push(c, (uint8_t)indicator);
    return indicator;
}

static void v2_queue_response(cybiko_radio_coproc_t *c) {
    rxq_push(c, CYBIKO_RADIO_ACK);
    c->rx_frame_ready = false;
    int indicator;
    if (c->q_count > 0) {
        c->rx_frame_ready = true;
        /* V2 treats 0xC8 as "discard". Real data always uses the 50-byte window. */
        indicator = CYBIKO_RADIO_IND_SMALL;
    } else {
        indicator = CYBIKO_RADIO_IND_LARGE;
    }
    rxq_push(c, (uint8_t)indicator);
    cybiko_radio_coproc_prepare_rx(c, indicator == CYBIKO_RADIO_IND_LARGE ? 200 : 50);
}

void cybiko_radio_coproc_v1_tx_complete(cybiko_radio_coproc_t *c) {
    if (!c->v1_accum) return;
    c->v1_accum = false;
    if (c->v1_len > 0)
        cybiko_radio_coproc_handle_transmit(c, c->v1_buf, (size_t)c->v1_len);

    rxq_push(c, CYBIKO_RADIO_ACK);
    c->rx_frame_ready = false;
    int indicator;
    cybiko_radio_frame_t *next = peek_frame(c);
    if (c->classic && (!next || (c->last_command == CYBIKO_RADIO_CMD_POLL && next->len > 50)))
        return;   /* the AVR says only 0x03 */
    if (!next) {
        indicator = CYBIKO_RADIO_IND_LARGE;
    } else if (c->last_command == CYBIKO_RADIO_CMD_POLL && next->len > 50) {
        indicator = CYBIKO_RADIO_IND_LARGE;
    } else {
        indicator = (next->len > 50) ? CYBIKO_RADIO_IND_LARGE : CYBIKO_RADIO_IND_SMALL;
        c->rx_frame_ready = true;
    }
    rxq_push(c, (uint8_t)indicator);
    cybiko_radio_coproc_prepare_rx(c, indicator == CYBIKO_RADIO_IND_LARGE ? 200 : 50);
}

/* The Classic's AVR answers no 01 command; the Java object acks each */
static void process_command3(cybiko_radio_coproc_t *c, uint8_t cmd, uint8_t param) {
    switch (cmd) {
    case 0x04:
        rxq_clear(c);
        c->initialized = true;
        break;
    case 0x02:
        c->current_channel = param;
        c->channel_set = true;
        if (c->transport && c->transport->set_channel)
            c->transport->set_channel(c->transport, param);
        c->initialized = true;
        break;
    default:
        break;
    }
    if (!c->classic)
        rxq_push(c, CYBIKO_RADIO_ACK);
}

static void process_command2(cybiko_radio_coproc_t *c, uint8_t header) {
    if (header == 0x30) c->last_command = CYBIKO_RADIO_CMD_POLL;
    else if (header == 0xCF) c->last_command = CYBIKO_RADIO_CMD_SCAN;

    if (c->v1_mode && !c->v2_mode) {
        c->v1_accum = true;
        c->v1_len = 0;
    }
    if (c->v2_mode && c->beacon_fn) {
        uint8_t beacon[CYBIKO_RADIO_MAX_PAYLOAD];
        size_t n = c->beacon_fn(c->beacon_user, beacon, sizeof beacon);
        if (n > 0) {
            uint8_t ch = beacon[0] & 0x3F;
            send_payload(c, beacon, n, ch);
        }
    }
    if (c->v2_mode)
        v2_queue_response(c);
}

void cybiko_radio_coproc_receive(cybiko_radio_coproc_t *c, uint8_t value) {
    if (c->v1_accum) {
        if (c->v1_len < (int)sizeof c->v1_buf)
            c->v1_buf[c->v1_len++] = value;
        return;
    }
    if (c->cmd_pos >= 3) c->cmd_pos = 0;
    c->cmd[c->cmd_pos++] = value;
    int need = (c->cmd[0] == 0x01) ? 3 : 2;
    if (c->cmd_pos >= need) {
        if (need == 3)
            process_command3(c, c->cmd[1], c->cmd[2]);
        else
            process_command2(c, c->cmd[0]);
        c->cmd_pos = 0;
    }
}
