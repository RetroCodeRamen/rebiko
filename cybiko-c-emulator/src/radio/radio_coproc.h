#ifndef CYBIKO_RADIO_COPROC_H
#define CYBIKO_RADIO_COPROC_H

/*
 * Emulated Cybiko AVR radio co-processor.
 *
 * Ported in behavior from RadioCoProcessor.java (MIT, Copyright 2026
 * Dan Berkowitz). The guest still talks SCI. This object is the point
 * where those bytes become transport payloads and come back.
 *
 * complete_tx_dtc does not wait. The Java object waits 15 ms so a UDP
 * peer can answer inside one exchange. LoRa airtime is hundreds of
 * milliseconds; blocking here would stall the frame loop and still miss
 * that window. The host pumps the transport, then try_async_delivery()
 * injects a frame once the guest radio is idle.
 *
 * A queued 0x03 is the local AVR accepting a command. It is not a
 * statement that another radio received the packet.
 *
 * A frame keeps its 8-byte address header on the air: the destination
 * CyID (FF FF FF FF for everyone), then the sender's. The real AVR sends
 * every byte as CyOS gave it and filters nothing, so CyOS on each
 * receiver decides whether a frame is for it.
 *
 * The Classic's replies follow its AVR firmware (avr19c6.bin in CyOS
 * 1.2.57's System.a): nothing for an 01 command, only 0x03 when a frame
 * has gone out, and a 0x32/0xC8 frame start only when a frame arrives.
 * The Xtreme's AVR firmware is not available; it keeps the Java replies.
 */

#include "radio_transport.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYBIKO_RADIO_MAX_PAYLOAD 240
#define CYBIKO_RADIO_RX_CAP      4
#define CYBIKO_RADIO_RXQ         512
#define CYBIKO_RADIO_IND_SMALL   0x32
#define CYBIKO_RADIO_IND_LARGE   0xC8
#define CYBIKO_RADIO_ACK         0x03
#define CYBIKO_RADIO_HDR         8      /* destination CyID, sender CyID */

/* The frame CyOS last started: 0x30 a small (50-byte) one, 0xCF a large
 * (200-byte) one. The Java emulator called them poll and scan. */
typedef enum {
    CYBIKO_RADIO_FRAME_NONE = 0,
    CYBIKO_RADIO_FRAME_SMALL = 1,
    CYBIKO_RADIO_FRAME_LARGE = 2
} cybiko_radio_frame_kind_t;

typedef struct {
    uint8_t  data[CYBIKO_RADIO_MAX_PAYLOAD];
    uint16_t len;
    uint32_t sender_id;
} cybiko_radio_frame_t;

typedef struct cybiko_radio_coproc {
    cybiko_radio_transport_t *transport;
    bool v1_mode;
    bool classic;                /* replies as the Classic's AVR firmware */
    bool initialized;
    bool v1_accum;
    bool rx_frame_ready;
    bool async_rx_pending;
    uint8_t current_channel;
    bool channel_set;            /* CyOS has tuned; frames on other channels are dropped */
    cybiko_radio_frame_kind_t last_frame;

    uint8_t cmd[3];
    int cmd_pos;

    uint8_t v1_buf[210];
    int v1_len;

    cybiko_radio_frame_t frames[CYBIKO_RADIO_RX_CAP];
    int q_head;
    int q_count;

    uint8_t rxq[CYBIKO_RADIO_RXQ];
    int rxq_head;
    int rxq_tail;
    int rxq_count;

    uint32_t stat_tx;
    uint32_t stat_tx_drop;
    uint32_t stat_rx;
    uint32_t stat_overflow;
    uint32_t stat_off_channel;
    uint16_t last_tx_len;
    uint8_t  last_tx[48];
} cybiko_radio_coproc_t;

void cybiko_radio_coproc_init(cybiko_radio_coproc_t *c);
void cybiko_radio_coproc_set_transport(cybiko_radio_coproc_t *c, cybiko_radio_transport_t *t);
void cybiko_radio_coproc_set_v1(cybiko_radio_coproc_t *c, bool v1);
void cybiko_radio_coproc_set_classic(cybiko_radio_coproc_t *c, bool classic);

void cybiko_radio_coproc_receive(cybiko_radio_coproc_t *c, uint8_t value);
void cybiko_radio_coproc_handle_transmit(cybiko_radio_coproc_t *c,
                                        const uint8_t *data, size_t len);
/* The frame-start indicator to follow 0x03, or -1 (Classic) when no
 * frame is waiting */
int  cybiko_radio_coproc_complete_tx_dtc(cybiko_radio_coproc_t *c);
void cybiko_radio_coproc_prepare_rx(cybiko_radio_coproc_t *c, int count);
int  cybiko_radio_coproc_try_async(cybiko_radio_coproc_t *c);
void cybiko_radio_coproc_v1_tx_complete(cybiko_radio_coproc_t *c);

void cybiko_radio_coproc_queue_response(cybiko_radio_coproc_t *c, uint8_t value);
int  cybiko_radio_coproc_read(cybiko_radio_coproc_t *c);
int  cybiko_radio_coproc_rx_count(const cybiko_radio_coproc_t *c);
int  cybiko_radio_coproc_pending(const cybiko_radio_coproc_t *c);
const cybiko_radio_frame_t *cybiko_radio_coproc_peek(const cybiko_radio_coproc_t *c, int index);

#ifdef __cplusplus
}
#endif

#endif
