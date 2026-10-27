#ifndef CYBIKO_RADIO_LINK_H
#define CYBIKO_RADIO_LINK_H

/*
 * PHY framing for one guest payload.
 *
 * Current CyOS frames fit in a single 255-byte SX1262 packet. The codec
 * still fragments, so a smaller MTU does not require a guest change.
 * CRC failures and duplicate message ids are dropped. There is no ACK
 * and no retry: CyOS retransmits on its own, and a local AVR 0x03 is not
 * a substitute for a packet the peer never got.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYBIKO_LINK_MAGIC        0xC7
/* 2: the guest payload keeps CyOS's 8-byte address header. A version 1
 * peer is dropped, not misread. */
#define CYBIKO_LINK_VERSION      2
#define CYBIKO_LINK_HDR          16
#define CYBIKO_LINK_MAX_PHYS     255
#define CYBIKO_LINK_MAX_PAYLOAD  240
#define CYBIKO_LINK_MAX_FRAGS    8
#define CYBIKO_LINK_SLOTS        4
#define CYBIKO_LINK_DUP_SENDERS  8
#define CYBIKO_LINK_DUP_WINDOW   16

typedef struct {
    bool     active;
    uint32_t sender;
    uint16_t msg_id;
    uint8_t  channel;
    uint8_t  frag_count;
    uint8_t  got;
    uint16_t total_len;
    uint16_t chunk_len[CYBIKO_LINK_MAX_FRAGS];
    uint8_t  chunks[CYBIKO_LINK_MAX_FRAGS][CYBIKO_LINK_MAX_PHYS - CYBIKO_LINK_HDR];
} cybiko_link_slot_t;

typedef struct {
    bool     used;
    uint32_t sender;
    uint16_t ids[CYBIKO_LINK_DUP_WINDOW];
    uint8_t  count;
} cybiko_link_dup_t;

typedef struct {
    cybiko_link_slot_t slots[CYBIKO_LINK_SLOTS];
    cybiko_link_dup_t  dups[CYBIKO_LINK_DUP_SENDERS];
    uint32_t stat_ok;
    uint32_t stat_crc;
    uint32_t stat_dup;
    uint32_t stat_drop;
    uint32_t stat_malformed;
} cybiko_link_decoder_t;

uint16_t cybiko_link_crc16(const uint8_t *data, size_t len);

/* Writes one PHY packet per fragment. Returns the fragment count, or -1. */
int cybiko_link_fragment(uint16_t msg_id, uint32_t sender_id, uint8_t channel,
                         const uint8_t *payload, size_t len, size_t phy_mtu,
                         uint8_t out[][CYBIKO_LINK_MAX_PHYS],
                         uint16_t *out_len, int max_out);

/*
 * Feed one PHY packet.
 *  1  a full guest payload was written to out
 *  0  stored, ignored duplicate, or no free reassembly slot
 * -1  malformed or bad CRC
 */
int cybiko_link_push(cybiko_link_decoder_t *dec,
                     const uint8_t *pkt, size_t len,
                     uint8_t *out, size_t out_cap, size_t *out_len,
                     uint8_t *channel, uint32_t *sender_id);

/* Round-trip, duplicate, and bad-CRC vector. 0 = pass. */
int cybiko_link_self_check(void);

#ifdef __cplusplus
}
#endif

#endif
