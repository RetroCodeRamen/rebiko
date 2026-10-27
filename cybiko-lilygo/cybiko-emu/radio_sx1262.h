#ifndef CYBIKO_RADIO_SX1262_H
#define CYBIKO_RADIO_SX1262_H

/*
 * Original T-Deck SX1262 as a guest radio transport. The chip driver is
 * the shared sx1262_phy (cybiko-c-emulator/src/radio), the framing is
 * radio_lora. This file only knows the T-Deck pins and that the SPI bus
 * is shared with the ST7789 and the SD card.
 */

#include "radio_transport.h"
#include "radio_lora.h"
#include "sx1262_phy.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = chip configured and listening. bus_free returns false while the
 * display DMA owns the SPI bus. Transmit is off unless the build sets
 * CYBIKO_RADIO_TX=1 (see radio_lora.h). */
int cybiko_sx1262_begin(bool (*bus_free)(void));

/* Guest CyID and link sender id, from the MAC. Valid after begin. */
uint32_t cybiko_sx1262_device_id(void);
uint8_t  cybiko_sx1262_chip_status(void);
uint16_t cybiko_sx1262_device_errors(void);
const char *cybiko_sx1262_last_error(void);

/* NULL until begin succeeded. */
cybiko_radio_transport_t *cybiko_sx1262_transport(void);
const cybiko_lora_t *cybiko_sx1262_link(void);
/* ReBiko's own messages on CYBIKO_LORA_PING_CHANNEL (see radio_lora.h) */
void cybiko_sx1262_on_control(void (*fn)(void *, const uint8_t *, size_t, uint32_t), void *user);

/* Once per frame from the main loop, never from the CPU loop. */
void cybiko_sx1262_pump(void);

#ifdef __cplusplus
}
#endif

#endif
