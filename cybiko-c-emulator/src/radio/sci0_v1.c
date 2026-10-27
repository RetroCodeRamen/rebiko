#include "sci0_v1.h"

#include <string.h>

/* Where CyOS keeps its radio object and CyID cache. 1.2.46 from
 * AddressBus.java; 1.2.57 (Cybiko's System Pack) found by matching the
 * code that uses them: the object's fields moved down 0x16, and the CyID
 * cache has the same five users. Each is told apart by its version
 * string; a CyOS not listed gets neither the async RX nor the CyID. */
typedef struct {
    uint32_t version_at;
    char version[7];
    uint32_t obj_ptr, state_off, cyid_ram;
} v1_layout_t;

static const v1_layout_t V1_LAYOUTS[] = {
    { 0x20364Au, "1.2.46", 0x21F216u, 0x3370u, 0x21F9DCu },
    { 0x203DECu, "1.2.57", 0x222514u, 0x335Au, 0x222CF4u },
};
#define V1_LAYOUT_COUNT (int)(sizeof V1_LAYOUTS / sizeof V1_LAYOUTS[0])

#define RAM_LO 0x200000u
#define RAM_HI 0x280000u

/* The DTC register blocks the boot ROM's DTC vector table points at */
#define V1_DTCERE_RXI0 0x08u
#define V1_DTCERE_TXI0 0x04u
#define V1_RX_DTC      0xFFFBB8u
#define V1_TX_DTC      0xFFFBC4u
#define V1_DTC_MAX     256

/* Java counts these in CPU steps at one instruction per cycle */
#define TXI_DELAY            32
#define ASYNC_INTERVAL       512

void cybiko_sci0_init(cybiko_sci0_v1_t *s) {
    memset(s, 0, sizeof *s);
    s->tdre = true;
    s->layout = -1;
    s->pending_indicator = -1;
}

void cybiko_sci0_bind(cybiko_sci0_v1_t *s, struct cybiko_radio_coproc *coproc,
                      cybiko_sci0_read_fn read8, cybiko_sci0_poke_fn poke8,
                      cybiko_sci0_irq_fn irq, void *ctx, uint32_t radio_id) {
    cybiko_sci0_init(s);
    s->coproc = coproc;
    s->bound = coproc != NULL;
    s->read8 = read8;
    s->poke8 = poke8;
    s->irq = irq;
    s->ctx = ctx;
    s->radio_id = radio_id;
    if (coproc)
        cybiko_radio_coproc_set_v1(coproc, true);
}

static void sci0_raise_irq(cybiko_sci0_v1_t *s, int vector) {
    if (s->irq)
        s->irq(s->ctx, vector);
}

static uint32_t sci0_read32(cybiko_sci0_v1_t *s, uint32_t a) {
    return ((uint32_t)s->read8(s->ctx, a) << 24) |
           ((uint32_t)s->read8(s->ctx, a + 1) << 16) |
           ((uint32_t)s->read8(s->ctx, a + 2) << 8) |
           (uint32_t)s->read8(s->ctx, a + 3);
}

static void sci0_poke32(cybiko_sci0_v1_t *s, uint32_t a, uint32_t v) {
    s->poke8(s->ctx, a, (uint8_t)(v >> 24));
    s->poke8(s->ctx, a + 1, (uint8_t)(v >> 16));
    s->poke8(s->ctx, a + 2, (uint8_t)(v >> 8));
    s->poke8(s->ctx, a + 3, (uint8_t)v);
}

static bool sci0_in_ram(uint32_t a) { return a >= RAM_LO && a < RAM_HI; }

static bool sci0_is_layout(cybiko_sci0_v1_t *s, int i) {
    const v1_layout_t *l = &V1_LAYOUTS[i];
    for (int k = 0; k < 6; k++)
        if (s->read8(s->ctx, l->version_at + (uint32_t)k) != (uint8_t)l->version[k])
            return false;
    return true;
}

/* The running CyOS's layout, or NULL. Checked again each time: a reboot
 * or a System Pack update can change it. */
static const v1_layout_t *sci0_layout(cybiko_sci0_v1_t *s) {
    if (s->layout >= 0 && sci0_is_layout(s, s->layout))
        return &V1_LAYOUTS[s->layout];
    s->layout = -1;
    for (int i = 0; i < V1_LAYOUT_COUNT; i++)
        if (sci0_is_layout(s, i)) {
            s->layout = i;
            return &V1_LAYOUTS[i];
        }
    return NULL;
}

static uint32_t sci0_radio_obj(cybiko_sci0_v1_t *s, const v1_layout_t *l) {
    if (!l) return 0;
    uint32_t obj = sci0_read32(s, l->obj_ptr);
    return sci0_in_ram(obj) && sci0_in_ram(obj + l->state_off) ? obj : 0;
}

bool cybiko_sci0_read(cybiko_sci0_v1_t *s, uint32_t address, uint8_t *out) {
    if (!s->bound)
        return false;
    switch (address) {
    case CYBIKO_SCI0_SCR:
        *out = s->scr;
        return true;
    case CYBIKO_SCI0_SSR:
        *out = (uint8_t)((s->tdre ? 0x84 : 0) | (s->rdrf ? 0x40 : 0));
        return true;
    case CYBIKO_SCI0_RDR:
        s->rdrf = false;
        *out = s->rdr;
        return true;
    }
    return false;
}

void cybiko_sci0_reset_mode(cybiko_sci0_v1_t *s) {
    s->dtc_mode = false;
    s->pending_indicator = -1;
    if (s->coproc)
        cybiko_radio_coproc_set_v1(s->coproc, true);
}

/* A DTC transfer the SCR write (or a DTCERE write) starts. As
 * sci2_xt.c's execute(): the frame goes to the co-processor at once and
 * TXI0 tells CyOS it is done; a receive fills the buffer from the
 * co-processor's queue. */
static void sci0_dtc(cybiko_sci0_v1_t *s, uint8_t scr) {
    bool receive = (scr & 0x50) == 0x50;
    bool transmit = (scr & 0xA0) == 0xA0;
    if (!receive && !transmit)
        return;
    uint8_t dtcer = s->read8(s->ctx, CYBIKO_SCI0_DTCERE);
    bool rxi = (dtcer & V1_DTCERE_RXI0) != 0;
    bool txi = (dtcer & V1_DTCERE_TXI0) != 0;
    uint32_t base;
    if (txi && transmit)
        base = V1_TX_DTC;
    else if (rxi && receive)
        base = V1_RX_DTC;
    else
        return;
    if (!s->dtc_mode) {
        s->dtc_mode = true;
        cybiko_radio_coproc_set_v1(s->coproc, false);   /* XT framing */
    }
    uint8_t mra = s->read8(s->ctx, base);
    uint32_t sar = ((uint32_t)s->read8(s->ctx, base + 1) << 16) |
                   ((uint32_t)s->read8(s->ctx, base + 2) << 8) |
                   (uint32_t)s->read8(s->ctx, base + 3);
    uint32_t dar = sci0_read32(s, base + 4);
    int count = ((int)s->read8(s->ctx, base + 8) << 8) | (int)s->read8(s->ctx, base + 9);
    if (count == 0)
        return;
    if (base == V1_TX_DTC) {
        /* the poll/scan command came byte by byte just before; its frame
         * is this transfer, not bytes still to come */
        s->coproc->v1_accum = false;
        int n = count > V1_DTC_MAX ? V1_DTC_MAX : count;
        uint8_t buf[V1_DTC_MAX];
        for (int i = 0; i < n; i++)
            buf[i] = s->read8(s->ctx, sar + (uint32_t)i);
        cybiko_radio_coproc_handle_transmit(s->coproc, buf, (size_t)n);
        /* 9 bytes or fewer is an AVR command, not a frame */
        if (count <= 9) {
            s->pending_indicator = CYBIKO_RADIO_IND_SMALL;
        } else {
            s->pending_indicator = cybiko_radio_coproc_complete_tx_dtc(s->coproc);
            s->tx_frames++;
            s->radio_up = true;
        }
        sci0_raise_irq(s, CYBIKO_SCI0_TXI_VECTOR);
    } else {
        bool step = (mra & 0x30) != 0;
        int n = count > CYBIKO_RADIO_RXQ ? CYBIKO_RADIO_RXQ : count;
        cybiko_radio_coproc_prepare_rx(s->coproc, n);
        for (int i = 0; i < n; i++) {
            int b = cybiko_radio_coproc_rx_count(s->coproc) > 0
                        ? cybiko_radio_coproc_read(s->coproc) : 0xFF;
            s->poke8(s->ctx, step ? dar + (uint32_t)i : dar, (uint8_t)b);
        }
        cybiko_radio_coproc_queue_response(s->coproc, 0xFF);
    }
    s->poke8(s->ctx, base + 8, 0);
    s->poke8(s->ctx, base + 9, 0);
    s->poke8(s->ctx, CYBIKO_SCI0_DTCERE,
             (uint8_t)(dtcer & ~(base == V1_TX_DTC ? V1_DTCERE_TXI0 : V1_DTCERE_RXI0)));
}

void cybiko_sci0_on_dtcer(cybiko_sci0_v1_t *s, uint8_t value) {
    if (!s->bound)
        return;
    if (value & (V1_DTCERE_RXI0 | V1_DTCERE_TXI0))
        sci0_dtc(s, s->scr);
    /* CyOS clears the TXI0 DTC bit once the frame is done; then come the
     * co-processor's 0x03 and the frame indicator, as on the Xtreme */
    if (s->pending_indicator >= 0 && !(value & V1_DTCERE_TXI0)) {
        cybiko_radio_coproc_queue_response(s->coproc, CYBIKO_RADIO_ACK);
        cybiko_radio_coproc_queue_response(s->coproc, (uint8_t)s->pending_indicator);
        s->pending_indicator = -1;
    }
}

static void sci0_write_scr(cybiko_sci0_v1_t *s, uint8_t value) {
    uint8_t old = s->scr;
    s->scr = value;
    if (value == 0)
        s->rdrf = false;
    /* RIE just enabled with a byte already waiting */
    if ((value & 0x40) && !(old & 0x40) && s->rdrf)
        sci0_raise_irq(s, CYBIKO_SCI0_RXI_VECTOR);
    /* TIE just enabled with TDRE set: first TXI0 after a byte time */
    if ((value & 0x80) && s->tdre && (!(old & 0x80) || s->txi_delay == 0))
        s->txi_delay = TXI_DELAY;
    /* TIE 1 -> 0: the TXI0 state machine finished the frame */
    if ((old & 0x80) && !(value & 0x80)) {
        if (s->coproc->v1_accum) {
            s->tx_frames++;
            s->radio_up = true;
        }
        cybiko_radio_coproc_v1_tx_complete(s->coproc);
    }
}

void cybiko_sci0_on_reg(cybiko_sci0_v1_t *s, int reg, uint8_t value) {
    if (!s->bound)
        return;
    if (reg == 3) {   /* TDR */
        cybiko_radio_coproc_receive(s->coproc, value);
        s->tdre = false;
        s->txi_delay = TXI_DELAY;
    } else if (reg == 2) {
        sci0_dtc(s, value);
        sci0_write_scr(s, value);
    }
}

static void sci0_async_delivery(cybiko_sci0_v1_t *s) {
    if (!s->radio_up || s->rdrf || cybiko_radio_coproc_rx_count(s->coproc) > 0 ||
        cybiko_radio_coproc_pending(s->coproc) <= 0)
        return;
    const v1_layout_t *l = sci0_layout(s);
    uint32_t obj = sci0_radio_obj(s, l);
    if (!obj || s->read8(s->ctx, obj + l->state_off) != 1)
        return;
    if (s->dtc_mode) {      /* the indicator only; the frame comes by DTC */
        if (s->pending_indicator < 0 && cybiko_radio_coproc_try_async(s->coproc) >= 0)
            s->rx_injected++;
        return;
    }
    int ind = cybiko_radio_coproc_try_async(s->coproc);
    if (ind >= 0) {
        cybiko_radio_coproc_prepare_rx(s->coproc, ind == CYBIKO_RADIO_IND_LARGE ? 200 : 50);
        s->rx_injected++;
    }
}

/*
 * Java ticks this once per CPU step. Here one call covers a batch of
 * cycles: the TXI countdown, one RDR load and the async check each happen
 * at most once per batch. A batch is eight instructions, short against
 * the guest's byte handling.
 */
void cybiko_sci0_advance(cybiko_sci0_v1_t *s, int cycles) {
    if (!s->bound || cycles <= 0)
        return;
    if (s->txi_delay > 0) {
        s->txi_delay -= cycles;
        if (s->txi_delay <= 0) {
            s->txi_delay = 0;
            s->tdre = true;
            if (s->scr & 0x80)
                sci0_raise_irq(s, CYBIKO_SCI0_TXI_VECTOR);
        }
    }
    if (!s->rdrf && cybiko_radio_coproc_rx_count(s->coproc) > 0) {
        s->rdr = (uint8_t)cybiko_radio_coproc_read(s->coproc);
        s->rdrf = true;
        if (s->scr & 0x40)
            sci0_raise_irq(s, CYBIKO_SCI0_RXI_VECTOR);
    }
    s->async_countdown -= cycles;
    if (s->async_countdown <= 0) {
        s->async_countdown = ASYNC_INTERVAL;
        sci0_async_delivery(s);
    }
}

void cybiko_sci0_frame(cybiko_sci0_v1_t *s) {
    if (!s->bound || !s->radio_id)
        return;
    /* Both ends usually boot the same flash.bin, so give this one its own
     * CyID. The boot loader uses that RAM before CyOS runs, so wait for
     * CyOS's radio object; CyOS may rewrite the cache, so keep it set. */
    const v1_layout_t *l = sci0_layout(s);
    if (!sci0_radio_obj(s, l))
        return;
    if (sci0_read32(s, l->cyid_ram) != s->radio_id)
        sci0_poke32(s, l->cyid_ram, s->radio_id);
}
