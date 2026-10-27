#include "acutest.h"
#include "radio/radio_coproc.h"
#include "radio/radio_sim.h"
#include "radio/sci0_v1.h"

#include <string.h>

/* The Classic's SCI0 under CyOS 1.2.57, which moves frames by DTC. A flat
 * 16 MB array stands in for the bus; the DTC register blocks and DTCERE
 * sit at their real addresses. */

#define TX_DTC 0xFFFBC4u
#define RX_DTC 0xFFFBB8u
#define DTCERE_RXI0 0x08
#define DTCERE_TXI0 0x04
#define FRAME_AT 0x210000u
#define RX_AT    0x211000u

static uint8_t mem[0x1000000];
static int irqs[128];
static radio_sim_bus_t bus;
static radio_sim_node_t node[2];
static cybiko_radio_coproc_t cop[2];
static cybiko_sci0_v1_t sci;

static uint8_t rd(void *ctx, uint32_t a) { (void)ctx; return mem[a & 0xFFFFFF]; }
static void wr(void *ctx, uint32_t a, uint8_t v) { (void)ctx; mem[a & 0xFFFFFF] = v; }
static void irq(void *ctx, int vector) { (void)ctx; irqs[vector]++; }

static void setup(void) {
    memset(mem, 0, sizeof mem);
    memset(irqs, 0, sizeof irqs);
    radio_sim_bus_init(&bus);
    for (int i = 0; i < 2; i++) {
        radio_sim_node_init(&node[i], &bus, 0x4C000001u + (uint32_t)i, 255);
        radio_sim_bus_add(&bus, &node[i]);
        cybiko_radio_coproc_init(&cop[i]);
        cybiko_radio_coproc_set_transport(&cop[i], &node[i].base);
    }
    cybiko_sci0_bind(&sci, &cop[0], rd, wr, irq, NULL, 0x4C000001u);
}

static void dtc_block(uint32_t base, uint8_t mra, uint32_t sar, uint32_t dar, uint16_t count) {
    mem[base] = mra;
    mem[base + 1] = (uint8_t)(sar >> 16);
    mem[base + 2] = (uint8_t)(sar >> 8);
    mem[base + 3] = (uint8_t)sar;
    for (int i = 0; i < 4; i++)
        mem[base + 4 + i] = (uint8_t)(dar >> (24 - 8 * i));
    mem[base + 8] = (uint8_t)(count >> 8);
    mem[base + 9] = (uint8_t)count;
}

/* A 51-byte frame as CyOS 1.2.57 hands it to the DTC: address header,
 * channel 2 byte, type, data, and the XT-style trailing byte */
static void put_frame(uint32_t at, uint32_t dest, uint32_t src) {
    for (int i = 0; i < 4; i++) {
        mem[at + i] = (uint8_t)(dest >> (24 - 8 * i));
        mem[at + 4 + i] = (uint8_t)(src >> (24 - 8 * i));
    }
    mem[at + 8] = 0xC2;
    mem[at + 9] = 0x01;
    for (int i = 10; i < 51; i++)
        mem[at + i] = (uint8_t)i;
}

/* CyOS: TXI0's DTC on, SCR with TE and TIE, then the DTC bit off when done */
static void send_by_dtc(uint32_t at, uint16_t count) {
    dtc_block(TX_DTC, 0x20, at, 0xFFFF7Bu, count);
    mem[CYBIKO_SCI0_DTCERE] = DTCERE_TXI0;
    cybiko_sci0_on_reg(&sci, 2, 0xA0);
    cybiko_sci0_on_dtcer(&sci, mem[CYBIKO_SCI0_DTCERE] & ~DTCERE_TXI0);
}

static void test_frame_then_ack_only(void) {
    setup();
    put_frame(FRAME_AT, 0xFFFFFFFFu, 0x4C000001u);
    send_by_dtc(FRAME_AT, 51);
    TEST_CHECK(sci.dtc_mode);
    TEST_CHECK(irqs[CYBIKO_SCI0_TXI_VECTOR] == 1);
    TEST_CHECK(sci.tx_frames == 1);
    /* the AVR says 0x03 and nothing more: no frame was heard */
    TEST_CHECK(cybiko_radio_coproc_rx_count(&cop[0]) == 1);
    TEST_CHECK(cybiko_radio_coproc_read(&cop[0]) == 0x03);

    /* the peer gets all 50 bytes, header included, without the trailing one */
    radio_sim_bus_pump(&bus);
    const cybiko_radio_frame_t *f = cybiko_radio_coproc_peek(&cop[1], 0);
    TEST_CHECK(f && f->len == 50);
    TEST_CHECK(f && memcmp(f->data, &mem[FRAME_AT], 50) == 0);
}

static void test_command_gets_no_reply(void) {
    setup();
    mem[FRAME_AT] = 0x01; mem[FRAME_AT + 1] = 0x02; mem[FRAME_AT + 2] = 0x02;
    send_by_dtc(FRAME_AT, 3);
    TEST_CHECK(cybiko_radio_coproc_rx_count(&cop[0]) == 0);
    TEST_CHECK(sci.tx_frames == 0);
}

static void test_waiting_frame_follows_ack(void) {
    setup();
    put_frame(FRAME_AT, 0x4C000001u, 0x4C000002u);   /* B to A */
    uint8_t b[51];
    memcpy(b, &mem[FRAME_AT], 51);
    cybiko_radio_coproc_set_v1(&cop[1], false);
    cybiko_radio_coproc_handle_transmit(&cop[1], b, 51);
    radio_sim_bus_pump(&bus);
    TEST_CHECK(cybiko_radio_coproc_pending(&cop[0]) == 1);

    /* A sends a small frame; 0x03, then the start of B's frame */
    cybiko_radio_coproc_receive(&cop[0], 0x30);
    cybiko_radio_coproc_receive(&cop[0], 0x00);
    put_frame(FRAME_AT + 0x100, 0xFFFFFFFFu, 0x4C000001u);
    send_by_dtc(FRAME_AT + 0x100, 51);
    TEST_CHECK(cybiko_radio_coproc_read(&cop[0]) == 0x03);
    TEST_CHECK(cybiko_radio_coproc_read(&cop[0]) == 0x32);

    /* CyOS reads the 50 bytes by DTC: B's frame exactly as sent */
    dtc_block(RX_DTC, 0x20, 0xFFFF7Du, RX_AT, 50);
    mem[CYBIKO_SCI0_DTCERE] = DTCERE_RXI0;
    cybiko_sci0_on_reg(&sci, 2, 0x50);
    TEST_CHECK(memcmp(&mem[RX_AT], b, 50) == 0);
    TEST_CHECK(cybiko_radio_coproc_pending(&cop[0]) == 0);
    TEST_CHECK((mem[CYBIKO_SCI0_DTCERE] & DTCERE_RXI0) == 0);
}

TEST_LIST = {
    { "a frame by DTC, then 0x03 only", test_frame_then_ack_only },
    { "an AVR command by DTC gets no reply", test_command_gets_no_reply },
    { "a waiting frame follows 0x03 and arrives unchanged", test_waiting_frame_follows_ack },
    { NULL, NULL }
};
