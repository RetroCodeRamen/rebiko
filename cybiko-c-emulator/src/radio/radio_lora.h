#ifndef CYBIKO_RADIO_LORA_H
#define CYBIKO_RADIO_LORA_H

/*
 * cybiko_radio_transport_t over one packet radio (radio_phy.h).
 *
 * send_packet fragments the guest payload with the link codec and queues
 * the PHY packets; it never waits. pump() polls the PHY, starts the next
 * queued packet once the previous one has left the air, and hands every
 * reassembled payload to the listener with the sender's CyID from the
 * link header. A full queue rejects the send (-1) rather than pretending
 * it went out. There is no ACK and no retry: CyOS retransmits itself.
 */

#include "radio_link.h"
#include "radio_phy.h"
#include "radio_transport.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Three CyOS retransmits of a two-fragment frame, plus a beacon. */
#define CYBIKO_LORA_TXQ 8

typedef struct {
    uint32_t tx_msgs;       /* guest payloads queued for the air */
    uint32_t tx_pkts;       /* PHY packets handed to tx() */
    uint32_t tx_rejected;   /* send_packet refused: queue full or too long */
    uint32_t rx_pkts;       /* PHY packets read */
    uint32_t rx_msgs;       /* payloads delivered to the listener */
    uint32_t rx_bad;        /* malformed or bad link CRC */
    uint32_t rx_lost;       /* gaps in a sender's message ids: never heard */
} cybiko_lora_stats_t;

/* Range-test pings go out on a channel CyOS never tunes, so every
 * co-processor drops them and only the link counters see them. */
#define CYBIKO_LORA_PING_CHANNEL 0

/* Senders whose next message id is tracked for rx_lost. */
#define CYBIKO_LORA_PEERS 4
/* A larger jump is a restart (ids are seeded at random), not loss. */
#define CYBIKO_LORA_MAX_GAP 64

typedef struct {
    cybiko_radio_transport_t base;
    cybiko_radio_phy_t *phy;
    uint16_t next_msg_id;
    uint8_t channel;
    bool tx_enabled;
    cybiko_radio_on_packet_fn fn;
    void *fn_user;
    cybiko_link_decoder_t dec;
    struct {
        uint16_t len;
        uint8_t pkt[CYBIKO_LINK_MAX_PHYS];
    } txq[CYBIKO_LORA_TXQ];
    int txq_head, txq_count;
    uint32_t pumps;
    cybiko_lora_stats_t stats;
    struct {
        uint32_t sender;
        uint16_t last_id;
        bool used;
    } peers[CYBIKO_LORA_PEERS];
    int peer_next;          /* slot to reuse when the table is full */
} cybiko_lora_t;

/* tx_enabled false: sends are rejected and counted, receive still works.
 * Keep it false until the operator has chosen a legal frequency. */
void cybiko_lora_init(cybiko_lora_t *t, cybiko_radio_phy_t *phy,
                      uint32_t device_id, bool tx_enabled);
void cybiko_lora_set_tx_enabled(cybiko_lora_t *t, bool on);
/* Start the link message ids somewhere new. Receivers drop ids they have
 * just seen from a sender, so a node that restarts at id 1 would lose its
 * first messages to a peer that is still up. Pass something random. */
void cybiko_lora_seed_msg_id(cybiko_lora_t *t, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif
