#include "uart_v1.h"

#include <string.h>

/* 57600 baud, 10 bits a byte, at the Classic's 11.0592 MHz */
#define BYTE_CYCLES 1920

void cybiko_uart_init(cybiko_uart_v1_t *u) {
    memset(u, 0, sizeof *u);
    u->tdre = true;
}

void cybiko_uart_bind(cybiko_uart_v1_t *u, cybiko_uart_tx_fn tx, void *tx_user,
                      cybiko_uart_irq_fn irq, void *irq_ctx) {
    u->bound = tx != NULL;
    u->tx = tx;
    u->tx_user = tx_user;
    u->irq = irq;
    u->irq_ctx = irq_ctx;
}

static void uart_irq(cybiko_uart_v1_t *u, int vector) {
    if (u->irq)
        u->irq(u->irq_ctx, vector);
}

bool cybiko_uart_read(cybiko_uart_v1_t *u, uint32_t address, uint8_t *out) {
    if (!u->bound)
        return false;
    switch (address) {
    case CYBIKO_UART_SCR:
        *out = u->scr;
        return true;
    case CYBIKO_UART_SSR:
        /* TDRE, RDRF, TEND */
        *out = (uint8_t)((u->tdre ? 0x84 : 0) | (u->rdrf ? 0x40 : 0));
        return true;
    case CYBIKO_UART_RDR:
        *out = u->rdr;
        return true;
    }
    return false;
}

void cybiko_uart_on_reg(cybiko_uart_v1_t *u, int reg, uint8_t value) {
    if (!u->bound)
        return;
    if (reg == 2) {                     /* SCR */
        uint8_t old = u->scr;
        u->scr = value;
        if ((value & 0x40) && !(old & 0x40) && u->rdrf)
            uart_irq(u, CYBIKO_UART_RXI_VECTOR);
        /* TIE on with TDR free: TXI2 at once, as the chip does */
        if ((value & 0x80) && !(old & 0x80) && u->tdre && u->txi_delay == 0)
            uart_irq(u, CYBIKO_UART_TXI_VECTOR);
    } else if (reg == 3) {              /* TDR */
        if (u->tx)
            u->tx(u->tx_user, value);
        u->tx_bytes++;
        u->tdre = false;
        u->txi_delay = BYTE_CYCLES;
    } else if (reg == 4) {              /* SSR: writing 0 clears RDRF */
        if (!(value & 0x40))
            u->rdrf = false;
    }
}

int cybiko_uart_rx_pending(const cybiko_uart_v1_t *u) {
    return (int)(u->rx_head - u->rx_tail);
}

int cybiko_uart_put(cybiko_uart_v1_t *u, const uint8_t *data, int n) {
    int room = CYBIKO_UART_RX_SIZE - cybiko_uart_rx_pending(u);
    if (n > room)
        n = room;
    for (int i = 0; i < n; i++)
        u->rx[u->rx_head++ & (CYBIKO_UART_RX_SIZE - 1)] = data[i];
    return n;
}

void cybiko_uart_advance(cybiko_uart_v1_t *u, int cycles) {
    if (!u->bound || cycles <= 0)
        return;
    if (u->txi_delay > 0) {
        u->txi_delay -= cycles;
        if (u->txi_delay <= 0) {
            u->txi_delay = 0;
            u->tdre = true;
            if (u->scr & 0x80)
                uart_irq(u, CYBIKO_UART_TXI_VECTOR);
        }
    }
    if (u->rx_delay > 0)
        u->rx_delay -= cycles;
    /* The next byte waits for the receiver (RE) and for CyOS to take the
     * last one, so nothing is overrun */
    if (u->rx_delay <= 0 && !u->rdrf && (u->scr & 0x10) && cybiko_uart_rx_pending(u) > 0) {
        u->rdr = u->rx[u->rx_tail++ & (CYBIKO_UART_RX_SIZE - 1)];
        u->rdrf = true;
        u->rx_bytes++;
        u->rx_delay = BYTE_CYCLES;
        if (u->scr & 0x40)
            uart_irq(u, CYBIKO_UART_RXI_VECTOR);
    }
}
