#ifndef CYBIKO_ADDRESS_BUS_H
#define CYBIKO_ADDRESS_BUS_H

#include "types.h"
#include "machine.h"
#include "memory.h"
#include "hd66421.h"
#include "timer8.h"
#include "timer16.h"
#include "rtc.h"
#include "speaker.h"
#include "keyboard.h"
#include "sci2_xt.h"
#if CYBIKO_HAS_SPI_FLASH
#include "at45db041.h"
#include "sci0_v1.h"
#include "uart_v1.h"
#endif

/* Forward declare CPU */
typedef struct h8s_cpu h8s_cpu_t;

typedef struct address_bus {
    /* Memory regions */
    memory_t  boot_rom;       /* 32KB, read-only */
    memory_t  external_ram;   /* 2MB, writable */
    memory_t  flash_rom;      /* 512KB, read-only */
    memory_t  on_chip_ram;    /* 10KB (0x2400 bytes), writable */

    /* Peripherals (pointers, owned by emulator) */
    hd66421_t  *lcd;
    timer8_t   *timer8[2];
    timer16_t  *timer16[6];
    rtc_t      *rtc;
    speaker_t  *speaker;
    keyboard_t *keyboard;
    h8s_cpu_t  *cpu;

    /* I/O state */
    uint8_t tstr;            /* Timer start register (0xFFFFC0) */
    uint8_t ier;             /* Interrupt enable register (0xFFFF2E) */
    uint8_t isr;             /* Interrupt status register (0xFFFF2F) */
    uint8_t adcsr;           /* ADC status register */
    uint8_t adcr;            /* ADC control register */

    /* DMA registers (XT) */
    uint8_t dma_regs[32];    /* 0xFFFEE0-0xFFFEFF */
    uint8_t dma_ctrl[8];     /* 0xFFFF00-0xFFFF07 */

    /* Serial output buffers */
    char    sci_output[3][256];
    int     sci_output_pos[3];

    /* DMA completion tracking */
    int     dma_completion_delay;
    int     dma_completion_vector;

    /* Debug log counters */
    int     unmapped_log_count;
    int     io_fallthrough_log;
    int     dma_debug_log;
    bool    in_dma;  /* true during DMA transfers */

    /* SCI2 radio DTC. Idle until bus_attach_radio(). */
    cybiko_sci2_xt_t sci2;

#if CYBIKO_HAS_SPI_FLASH
    /* V1: AT45DB041 on SCI1, bulk transfers by the DTC */
    at45db041_t spi_flash;
    uint8_t  sci1_rdr;
    bool     sci1_rdrf;
    uint8_t  icr[3];          /* 0xFFFEC0-0xFFFEC2 */
    uint16_t adc[4];          /* 10-bit ADC results, channels 0-3 */
    /* V1 radio: SCI0 to the co-processor. Idle until bus_attach_radio(). */
    cybiko_sci0_v1_t sci0;
    /* V1 serial port: SCI2. Idle until cybiko_attach_serial(). */
    cybiko_uart_v1_t uart;
#endif
} address_bus_t;

void     bus_init(address_bus_t *bus);
void     bus_free(address_bus_t *bus);

uint8_t  bus_read8(address_bus_t *bus, uint32_t address);
uint16_t bus_read16(address_bus_t *bus, uint32_t address);
uint32_t bus_read32(address_bus_t *bus, uint32_t address);
void     bus_write8(address_bus_t *bus, uint32_t address, uint8_t value);
void     bus_write16(address_bus_t *bus, uint32_t address, uint16_t value);
void     bus_write32(address_bus_t *bus, uint32_t address, uint32_t value);

/* Counts down a deferred DTC/DMA completion interrupt. Call once per CPU
 * batch; the delay is in calls. */
void     bus_tick_dma_completion(address_bus_t *bus);
void     bus_tick_rtc(address_bus_t *bus);
#if CYBIKO_HAS_SPI_FLASH
/* data: AT45_SIZE bytes owned by the caller */
void     bus_load_spi_flash(address_bus_t *bus, uint8_t *data);
#endif

/* Attach the radio co-processor: SCI2 + DTC on the XT, SCI0 on V1.
 * radio_id is the guest CyID (V1 patches it in RAM; 0 keeps the ROM's). */
void     bus_attach_radio(address_bus_t *bus, struct cybiko_radio_coproc *coproc,
                          uint32_t radio_id);
/* Advance SCI2 by guest cycles. No work when no co-processor is attached. */
void     bus_tick_sci2(address_bus_t *bus, int cycles);

#endif
