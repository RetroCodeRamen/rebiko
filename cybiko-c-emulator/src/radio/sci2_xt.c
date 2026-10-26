#include "sci2_xt.h"

#include <string.h>

uint32_t cybiko_sci2_reg_writes;
uint32_t cybiko_sci2_dtc_runs;

#define SCI2_RADIO_OBJ   0x4B49FEu
#define SCI2_STATE_OFF   0x335Au
#define SCI2_DTC_MAX     256

void cybiko_sci2_init(cybiko_sci2_xt_t *s) {
    memset(s, 0, sizeof *s);
    s->pending_indicator = -1;
    s->tdre = true;
}

void cybiko_sci2_bind(cybiko_sci2_xt_t *s, struct cybiko_radio_coproc *coproc,
                      cybiko_sci2_read_fn read8, cybiko_sci2_poke_fn poke8,
                      cybiko_sci2_irq_fn irq, void *ctx) {
    cybiko_sci2_init(s);
    s->coproc = coproc;
    s->bound = coproc != NULL;
    s->read8 = read8;
    s->poke8 = poke8;
    s->irq = irq;
    s->ctx = ctx;
}

static void raise_irq(cybiko_sci2_xt_t *s, int vector) {
    if (s->irq)
        s->irq(s->ctx, vector);
}

static uint32_t read32(cybiko_sci2_xt_t *s, uint32_t address) {
    return ((uint32_t)s->read8(s->ctx, address) << 24) |
           ((uint32_t)s->read8(s->ctx, address + 1) << 16) |
           ((uint32_t)s->read8(s->ctx, address + 2) << 8) |
           (uint32_t)s->read8(s->ctx, address + 3);
}

static void execute(cybiko_sci2_xt_t *s, uint8_t scr) {
    bool receive = (scr & 0x50) == 0x50;
    bool transmit = (scr & 0xA0) == 0xA0;
    if (!receive && !transmit)
        return;

    uint8_t dtcerf = s->read8(s->ctx, CYBIKO_SCI2_DTCERF);
    bool rxi = (dtcerf & 0x80) != 0;
    bool txi = (dtcerf & 0x40) != 0;
    if (!rxi && !txi)
        return;

    uint32_t base;
    if (txi && transmit)
        base = CYBIKO_SCI2_TX_DTC;
    else if (rxi && receive)
        base = CYBIKO_SCI2_RX_DTC;
    else
        return;

    uint8_t mra = s->read8(s->ctx, base);
    uint32_t sar = ((uint32_t)s->read8(s->ctx, base + 1) << 16) |
                   ((uint32_t)s->read8(s->ctx, base + 2) << 8) |
                   (uint32_t)s->read8(s->ctx, base + 3);
    uint32_t dar = ((uint32_t)s->read8(s->ctx, base + 4) << 24) |
                   ((uint32_t)s->read8(s->ctx, base + 5) << 16) |
                   ((uint32_t)s->read8(s->ctx, base + 6) << 8) |
                   (uint32_t)s->read8(s->ctx, base + 7);
    int count = ((int)s->read8(s->ctx, base + 8) << 8) |
                (int)s->read8(s->ctx, base + 9);
    if (count == 0)
        return;

    if (txi && transmit) {
        int n = count;
        if (n > SCI2_DTC_MAX)
            n = SCI2_DTC_MAX;
        uint8_t buf[SCI2_DTC_MAX];
        for (int i = 0; i < n; i++)
            buf[i] = s->read8(s->ctx, sar + (uint32_t)i);
        cybiko_radio_coproc_handle_transmit(s->coproc, buf, (size_t)n);
        /* Frames of 9 bytes or fewer are AVR commands. complete_tx_dtc
         * would drop a frame that is already queued. */
        if (count <= 9)
            s->pending_indicator = CYBIKO_RADIO_IND_SMALL;
        else
            s->pending_indicator = cybiko_radio_coproc_complete_tx_dtc(s->coproc);
        raise_irq(s, CYBIKO_SCI2_TXI_VECTOR);
    } else {
        bool step = (mra & 0x30) != 0;
        int n = count;
        if (n > CYBIKO_RADIO_RXQ)
            n = CYBIKO_RADIO_RXQ;
        cybiko_radio_coproc_prepare_rx(s->coproc, n);
        for (int i = 0; i < n; i++) {
            int b = cybiko_radio_coproc_rx_count(s->coproc) > 0
                        ? cybiko_radio_coproc_read(s->coproc)
                        : 0xFF;
            uint32_t dest = step ? (dar + (uint32_t)i) : dar;
            s->poke8(s->ctx, dest, (uint8_t)b);
        }
        cybiko_radio_coproc_queue_response(s->coproc, 0xFF);
    }

    s->poke8(s->ctx, base + 8, 0);
    s->poke8(s->ctx, base + 9, 0);
    s->poke8(s->ctx, CYBIKO_SCI2_DTCERF, (uint8_t)(dtcerf & (txi ? ~0x40 : ~0x80)));
}

bool cybiko_sci2_read(cybiko_sci2_xt_t *s, uint32_t address, uint8_t *out) {
    if (!s->bound)
        return false;
    if (address == CYBIKO_SCI2_SCR) {
        *out = s->scr;
        return true;
    }
    if (address == CYBIKO_SCI2_SSR) {
        uint8_t ssr = CYBIKO_SCI2_SSR_TEND;
        if (s->tdre)
            ssr |= CYBIKO_SCI2_SSR_TDRE;
        if (s->rdrf)
            ssr |= CYBIKO_SCI2_SSR_RDRF;
        *out = ssr;
        return true;
    }
    if (address == CYBIKO_SCI2_RDR) {
        s->rdrf = false;
        *out = s->rdr;
        return true;
    }
    return false;
}

void cybiko_sci2_on_reg(cybiko_sci2_xt_t *s, int reg, uint8_t value) {
    if (!s->bound)
        return;
    cybiko_sci2_reg_writes++;
    if (reg == 3) {
        cybiko_radio_coproc_receive(s->coproc, value);
        s->tdre = false;
        s->txi_delay = CYBIKO_SCI2_TXI_DELAY;
        return;
    }
    if (reg != 2)
        return;

    execute(s, value);
    uint8_t old = s->scr;
    s->scr = value;
    /* RXI is level: RDRF and RIE. A byte that arrived while RIE was off
     * raises it as soon as RIE goes on (as sci0_v1 does). Without this, an
     * indicator queued before CyOS's radio was ready sat in RDR for good,
     * every later response queued behind it, and the Xtreme never took
     * another frame (seen over LoRa, 26 Oct 2026). */
    if ((value & 0x40) && !(old & 0x40) && s->rdrf)
        raise_irq(s, CYBIKO_SCI2_RXI_VECTOR);
    bool tie = (s->scr & 0x80) != 0;
    bool tie_was_off = (old & 0x80) == 0;
    if (tie && s->tdre && (tie_was_off || s->txi_delay == 0))
        s->txi_delay = CYBIKO_SCI2_TXI_DELAY;
}

void cybiko_sci2_on_dtcerf(cybiko_sci2_xt_t *s, uint8_t value) {
    if (!s->bound)
        return;
    if ((value & 0xC0) != 0) {
        cybiko_sci2_dtc_runs++;
        execute(s, s->scr);
    }
    if (s->pending_indicator >= 0 && (value & 0x40) == 0) {
        cybiko_radio_coproc_queue_response(s->coproc, CYBIKO_RADIO_ACK);
        cybiko_radio_coproc_queue_response(s->coproc, (uint8_t)s->pending_indicator);
        s->pending_indicator = -1;
    }
}

void cybiko_sci2_tick(cybiko_sci2_xt_t *s) {
    if (!s->bound)
        return;
    if (s->txi_delay > 0) {
        s->txi_delay--;
        if (s->txi_delay == 0) {
            s->tdre = true;
            if ((s->scr & 0x80) != 0)
                raise_irq(s, CYBIKO_SCI2_TXI_VECTOR);
        }
    }
    if (!s->rdrf && cybiko_radio_coproc_rx_count(s->coproc) > 0) {
        s->rdr = (uint8_t)cybiko_radio_coproc_read(s->coproc);
        s->rdrf = true;
        if ((s->scr & 0x40) != 0)
            raise_irq(s, CYBIKO_SCI2_RXI_VECTOR);
    }
    if (s->async_countdown > 0) {
        s->async_countdown--;
        return;
    }
    if (s->rdrf || cybiko_radio_coproc_rx_count(s->coproc) != 0)
        return;
    if (s->pending_indicator >= 0 || cybiko_radio_coproc_pending(s->coproc) <= 0)
        return;
    s->async_countdown = CYBIKO_SCI2_ASYNC_INTERVAL;
    uint32_t obj = read32(s, SCI2_RADIO_OBJ);
    if (obj < 0x400000u || obj >= 0x600000u)
        return;
    if (s->read8(s->ctx, obj + SCI2_STATE_OFF) == 1)
        cybiko_radio_coproc_try_async(s->coproc);
}

static bool rx_load_due(const cybiko_sci2_xt_t *s) {
    return !s->rdrf && cybiko_radio_coproc_rx_count(s->coproc) > 0;
}

/*
 * Same result as n calls to cybiko_sci2_tick. The guest CPU does not run
 * inside one call, and raise_irq only queues a vector, so RDRF, the RX
 * queue, and the frame queue cannot change except in a tick that loads
 * RDR or runs the async check. Every other tick only counts down
 * txi_delay and async_countdown, so those are skipped in bulk. Once the
 * async check fails with async_countdown at 0 it fails the same way for
 * the rest of the call, which is the idle case: all that is left then is
 * the TXI countdown. Before this, an attached co-processor cost one full
 * tick per guest cycle (307200 a frame) and made the host core ~6x slower.
 */
void cybiko_sci2_advance(cybiko_sci2_xt_t *s, int n) {
    if (!s->bound)
        return;
    if (n == 1) {  /* the Linux core ticks one cycle at a time */
        cybiko_sci2_tick(s);
        return;
    }
    while (n > 0) {
        if (rx_load_due(s) || s->async_countdown == 0) {
            cybiko_sci2_tick(s);
            n--;
            if (s->async_countdown == 0 && !rx_load_due(s))
                break;  /* stable idle; only TXI below */
            continue;
        }
        int k = n;
        if (s->async_countdown + 1 < k)
            k = s->async_countdown + 1;
        if (s->txi_delay > 0 && s->txi_delay < k)
            k = s->txi_delay;
        /* k-1 ticks with no event, then one full tick */
        if (s->txi_delay > 0)
            s->txi_delay -= k - 1;
        s->async_countdown -= k - 1;
        cybiko_sci2_tick(s);
        n -= k;
    }
    if (n > 0 && s->txi_delay > 0) {
        if (n >= s->txi_delay) {
            s->txi_delay = 1;
            cybiko_sci2_tick(s);  /* fires TXI; async check fails again */
        } else {
            s->txi_delay -= n;
        }
    }
}
