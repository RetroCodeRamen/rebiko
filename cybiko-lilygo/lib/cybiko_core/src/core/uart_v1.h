#ifndef CYBIKO_UART_V1_H
#define CYBIKO_UART_V1_H

/*
 * The Cybiko Classic's serial port: SCI2 at 0xFFFF88, 57600 8N1, the
 * port CyberLoad used. CyOS runs it from RXI2 and TXI2 (vectors 89, 90)
 * with SCR = 0xF0 at the desktop; the boot loader prints its log on it.
 *
 * Until a host attaches, the bus keeps its old SCI2 (always transmit
 * ready, nothing received). Once attached, each byte CyOS writes to TDR
 * goes to the host's tx callback, and bytes the host queues arrive one
 * per byte time through RDR with RXI2.
 */

#include <stdbool.h>
#include <stdint.h>

#define CYBIKO_UART_SMR 0xFFFF88u
#define CYBIKO_UART_SCR 0xFFFF8Au
#define CYBIKO_UART_SSR 0xFFFF8Cu
#define CYBIKO_UART_RDR 0xFFFF8Du

#define CYBIKO_UART_RXI_VECTOR 89
#define CYBIKO_UART_TXI_VECTOR 90

#define CYBIKO_UART_RX_SIZE 8192   /* power of two */

typedef void (*cybiko_uart_tx_fn)(void *user, uint8_t byte);
typedef void (*cybiko_uart_irq_fn)(void *ctx, int vector);

typedef struct cybiko_uart_v1 {
    bool bound;
    uint8_t scr;
    bool tdre;
    bool rdrf;
    uint8_t rdr;
    int txi_delay;      /* cycles until TDR is free again */
    int rx_delay;       /* cycles until the next queued byte may load */
    uint8_t rx[CYBIKO_UART_RX_SIZE];
    uint32_t rx_head, rx_tail;
    cybiko_uart_tx_fn tx;
    void *tx_user;
    cybiko_uart_irq_fn irq;
    void *irq_ctx;
    uint32_t tx_bytes, rx_bytes;
} cybiko_uart_v1_t;

void cybiko_uart_init(cybiko_uart_v1_t *u);
void cybiko_uart_bind(cybiko_uart_v1_t *u, cybiko_uart_tx_fn tx, void *tx_user,
                      cybiko_uart_irq_fn irq, void *irq_ctx);
/* SCR, SSR and RDR once bound. False: not one of those, or unbound. */
bool cybiko_uart_read(cybiko_uart_v1_t *u, uint32_t address, uint8_t *out);
/* Guest write to SCI2 register reg (0 = SMR .. 6 = SCMR). */
void cybiko_uart_on_reg(cybiko_uart_v1_t *u, int reg, uint8_t value);
void cybiko_uart_advance(cybiko_uart_v1_t *u, int cycles);
/* Host to Cybiko. Returns how many bytes fit. */
int  cybiko_uart_put(cybiko_uart_v1_t *u, const uint8_t *data, int n);
int  cybiko_uart_rx_pending(const cybiko_uart_v1_t *u);

#endif
