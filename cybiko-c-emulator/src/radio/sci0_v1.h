#ifndef CYBIKO_SCI0_V1_H
#define CYBIKO_SCI0_V1_H

/*
 * Cybiko Classic (V1) SCI0, interrupt driven, to the radio co-processor.
 *
 * Behavior follows AddressBus.java (MIT, Copyright 2026 Dan Berkowitz):
 * tickSci0 and the SCI0 SCR write. CyOS sends a poll/scan command and the
 * frame byte by byte from TXI0; clearing TIE ends the frame, and the
 * co-processor answers 0x03, an indicator and 50/200 bytes through RXI0.
 *
 * CyOS 1.2.46 schedules its own radio. Posting a job (0x214A6C) or
 * finishing one calls the tick at 0x214760 with a random back-off; the
 * tick loads TMR1 and enables its overflow interrupt, and that interrupt
 * calls the tick again, which starts TXI0. Java and this file used to
 * fake the tick with a poll/scan driver because TMR1's OVI never fired
 * (timer8 raised OVI only on a 0 -> 1 OVF edge, and OVF stayed set). The
 * driver guessed Chat from connObj bytes and was the cause of lost and
 * echoed messages. With TMR1 fixed, only two things are left here:
 *  - async RX: the co-processor hands CyOS a received frame while its
 *    radio is idle (state 1), as the AVR does;
 *  - identity: the CyID cache (0x21F9DC in 1.2.46) is set to radio_id,
 *    because both ends usually boot the same flash.bin.
 * Both use CyOS addresses; sci0_v1.c has them for 1.2.46 and 1.2.57.
 *
 * CyOS 1.2.57 (Cybiko's System Pack) drives the radio the Xtreme's way:
 * frames go out and come in by DTC (DTCERE bits 2 and 3, register blocks
 * at 0xFFFBC4 and 0xFFFBB8), and after a sent frame the co-processor's
 * 0x03 and frame indicator follow CyOS clearing the TXI0 DTC bit. The
 * first DTC transfer switches this file to that behaviour (sci2_xt.c's)
 * until the next reboot.
 */

#include "radio_coproc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYBIKO_SCI0_SMR 0xFFFF78u
#define CYBIKO_SCI0_SCR 0xFFFF7Au
#define CYBIKO_SCI0_TDR 0xFFFF7Bu
#define CYBIKO_SCI0_SSR 0xFFFF7Cu
#define CYBIKO_SCI0_RDR 0xFFFF7Du

#define CYBIKO_SCI0_DTCERE 0xFFFF34u
#define CYBIKO_SCI0_RXI_VECTOR 81
#define CYBIKO_SCI0_TXI_VECTOR 82

typedef uint8_t (*cybiko_sci0_read_fn)(void *ctx, uint32_t address);
typedef void (*cybiko_sci0_poke_fn)(void *ctx, uint32_t address, uint8_t value);
typedef void (*cybiko_sci0_irq_fn)(void *ctx, int vector);

typedef struct cybiko_sci0_v1 {
    struct cybiko_radio_coproc *coproc;
    bool bound;
    uint8_t scr;
    bool tdre;
    bool rdrf;
    uint8_t rdr;
    int txi_delay;          /* cycles until TDRE */
    int async_countdown;    /* cycles until the next async check */
    bool radio_up;          /* CyOS has finished its first frame */
    uint32_t radio_id;      /* 0 = keep the ROM CyID */
    int layout;             /* CyOS version found in RAM, -1 = none yet */
    bool dtc_mode;          /* CyOS moves frames by DTC (1.2.57) */
    int pending_indicator;  /* after a DTC frame: sent once TXI0's DTC bit clears */
    cybiko_sci0_read_fn read8;
    cybiko_sci0_poke_fn poke8;
    cybiko_sci0_irq_fn irq;
    void *ctx;
    /* counters */
    uint32_t tx_frames;
    uint32_t rx_injected;
} cybiko_sci0_v1_t;

void cybiko_sci0_init(cybiko_sci0_v1_t *s);
void cybiko_sci0_bind(cybiko_sci0_v1_t *s, struct cybiko_radio_coproc *coproc,
                      cybiko_sci0_read_fn read8, cybiko_sci0_poke_fn poke8,
                      cybiko_sci0_irq_fn irq, void *ctx, uint32_t radio_id);

/* SCR, SSR and RDR reads once bound. False: not an SCI0 register. */
bool cybiko_sci0_read(cybiko_sci0_v1_t *s, uint32_t address, uint8_t *out);
/* Guest write to SCI0 register reg (0 = SMR .. 6 = SCMR). */
void cybiko_sci0_on_reg(cybiko_sci0_v1_t *s, int reg, uint8_t value);
/* Advance by guest cycles. */
void cybiko_sci0_advance(cybiko_sci0_v1_t *s, int cycles);
/* Once per frame: keeps the CyID set. */
void cybiko_sci0_frame(cybiko_sci0_v1_t *s);
/* Guest write to DTCERE (stored first). */
void cybiko_sci0_on_dtcer(cybiko_sci0_v1_t *s, uint8_t value);
/* Back to 1.2.46's byte-by-byte radio, for a reboot. */
void cybiko_sci0_reset_mode(cybiko_sci0_v1_t *s);

#ifdef __cplusplus
}
#endif

#endif
