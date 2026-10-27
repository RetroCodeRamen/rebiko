/*
 * Cybiko address bus - memory-mapped I/O router.
 *
 * Ported from AddressBus.java. Routes CPU reads/writes to the correct
 * peripheral based on the 24-bit address. Region bounds come from
 * machine.h (CYBIKO_MACHINE_XT by default, CYBIKO_MACHINE_V1).
 *
 * XT Memory map:
 *   0x000000-0x03FFFF  Boot ROM (32KB mirrored)
 *   0x100000-0x100001  LCD controller (HD66421)
 *   0x200000-0x200003  USB controller (stub)
 *   0x400000-0x5FFFFF  External RAM (2MB)
 *   0x600000-0x7FFFFF  Flash ROM (512KB mirrored)
 *   0xE00000-0xEFFFFF  Keyboard matrix
 *   0xFFDC00-0xFFFFFF  On-chip RAM & I/O registers
 *
 * V1 (Classic) memory map:
 *   0x000000-0x007FFF  Boot ROM (no mirror)
 *   0x200000-0x27FFFF  External RAM (512KB)
 *   0x600000-0x600001  LCD controller (HD66421)
 *   0xE00000-0xEFFFFF  Keyboard matrix
 *   0xFFEC00-0xFFFFFF  On-chip RAM & I/O registers
 *   CyOS is on AT45DB041 SPI flash, not in the address map.
 */
#include "core/address_bus.h"
#include "core/h8s_cpu.h"
#include "core/machine.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define BUS_HOT IRAM_ATTR
#else
#define BUS_HOT
#endif

/* --- Address region enumeration --- */
typedef enum {
    REGION_BOOT_ROM,
    REGION_LCD,
    REGION_USB,
    REGION_EXT_RAM,
    REGION_FLASH,
    REGION_KEYBOARD,
    REGION_ON_CHIP,
    REGION_UNMAPPED
} bus_region_t;

/* --- SSR bit definitions --- */
#define SSR_TDRE 0x80
#define SSR_RDRF 0x40
#define SSR_TEND 0x04

/* --- Memory map constants (per machine, from machine.h) --- */
#define BOOT_ROM_MIRROR_END    CYBIKO_BOOT_ROM_MIRROR_END
#define LCD_BASE               CYBIKO_LCD_BASE
#define LCD_END                CYBIKO_LCD_END
#define USB_BASE               0x200000
#define USB_END                0x200003
#define EXT_RAM_BASE           CYBIKO_EXT_RAM_BASE
#define EXT_RAM_END            CYBIKO_EXT_RAM_END
#define EXT_RAM_SIZE           CYBIKO_EXT_RAM_SIZE
#if CYBIKO_HAS_FLASH_MAP
#define FLASH_BASE             CYBIKO_FLASH_BASE
#define FLASH_END              CYBIKO_FLASH_END
#define FLASH_SIZE             CYBIKO_FLASH_SIZE
#endif
#define KEYBOARD_BASE          0xE00000
#define KEYBOARD_END           0xEFFFFF
#define ON_CHIP_RAM_BASE       CYBIKO_ON_CHIP_BASE

/* --- Forward declarations --- */
static uint8_t  read_on_chip8(address_bus_t *bus, uint32_t address);
static uint16_t read_on_chip16(address_bus_t *bus, uint32_t address);
static void     write_on_chip8(address_bus_t *bus, uint32_t address, uint8_t value);
static void     write_on_chip16(address_bus_t *bus, uint32_t address, uint16_t value);
#if CYBIKO_HAS_DMA
static void     execute_dma_transfer(address_bus_t *bus, int channel);
#endif
#if CYBIKO_HAS_SPI_FLASH
static void     execute_sci1_dtc(address_bus_t *bus, uint8_t scr);
#endif

/* --- Address decoding --- */

static bus_region_t decode_region(uint32_t address)
{
    if (address <= BOOT_ROM_MIRROR_END)
        return REGION_BOOT_ROM;

    if (address >= LCD_BASE && address <= LCD_END)
        return REGION_LCD;

#if CYBIKO_HAS_USB
    /* XT only. On V1 this range is the start of external RAM. */
    if (address >= USB_BASE && address <= USB_END)
        return REGION_USB;
#endif

    if (address >= EXT_RAM_BASE && address <= EXT_RAM_END)
        return REGION_EXT_RAM;

#if CYBIKO_HAS_FLASH_MAP
    if (address >= FLASH_BASE && address <= FLASH_END)
        return REGION_FLASH;
#endif

    if (address >= KEYBOARD_BASE && address <= KEYBOARD_END)
        return REGION_KEYBOARD;

    if (address >= ON_CHIP_RAM_BASE && address <= 0xFFFFFF)
        return REGION_ON_CHIP;

    return REGION_UNMAPPED;
}

#if CYBIKO_HAS_FLASH_MAP
static uint32_t flash_offset(uint32_t address)
{
    return (address - FLASH_BASE) & (FLASH_SIZE - 1);
}
#endif

static uint32_t on_chip_offset(uint32_t address)
{
    return address - ON_CHIP_RAM_BASE;
}

static void log_unmapped(address_bus_t *bus, const char *op, uint32_t address)
{
    if (bus->unmapped_log_count < 200) {
        uint32_t pc = bus->cpu ? bus->cpu->pc : 0;
        fprintf(stderr, "Bus: unmapped %s at 0x%06X (PC=0x%06X)\n", op, address, pc);
        bus->unmapped_log_count++;
        if (bus->unmapped_log_count == 200) {
            fprintf(stderr, "Bus: suppressing further unmapped warnings (200 reached)\n");
        }
    }
}

/* --- Timer16 routing helpers --- */

static int route_timer16_read8(address_bus_t *bus, uint32_t address)
{
    if (address >= 0xFFFEA0 && address <= 0xFFFEAB && bus->timer16[5])
        return timer16_read8(bus->timer16[5], address - 0xFFFEA0);
    if (address >= 0xFFFE90 && address <= 0xFFFE9B && bus->timer16[4])
        return timer16_read8(bus->timer16[4], address - 0xFFFE90);
    if (address >= 0xFFFE80 && address <= 0xFFFE8F && bus->timer16[3])
        return timer16_read8(bus->timer16[3], address - 0xFFFE80);
    if (address >= 0xFFFFD0 && address <= 0xFFFFDF && bus->timer16[0])
        return timer16_read8(bus->timer16[0], address - 0xFFFFD0);
    if (address >= 0xFFFFE0 && address <= 0xFFFFEB && bus->timer16[1])
        return timer16_read8(bus->timer16[1], address - 0xFFFFE0);
    if (address >= 0xFFFFF0 && address <= 0xFFFFFB && bus->timer16[2])
        return timer16_read8(bus->timer16[2], address - 0xFFFFF0);
    return -1;
}

static int route_timer16_read16(address_bus_t *bus, uint32_t address)
{
    if (address >= 0xFFFEA0 && address <= 0xFFFEAB && bus->timer16[5])
        return timer16_read16(bus->timer16[5], address - 0xFFFEA0);
    if (address >= 0xFFFE90 && address <= 0xFFFE9B && bus->timer16[4])
        return timer16_read16(bus->timer16[4], address - 0xFFFE90);
    if (address >= 0xFFFE80 && address <= 0xFFFE8F && bus->timer16[3])
        return timer16_read16(bus->timer16[3], address - 0xFFFE80);
    if (address >= 0xFFFFD0 && address <= 0xFFFFDF && bus->timer16[0])
        return timer16_read16(bus->timer16[0], address - 0xFFFFD0);
    if (address >= 0xFFFFE0 && address <= 0xFFFFEB && bus->timer16[1])
        return timer16_read16(bus->timer16[1], address - 0xFFFFE0);
    if (address >= 0xFFFFF0 && address <= 0xFFFFFB && bus->timer16[2])
        return timer16_read16(bus->timer16[2], address - 0xFFFFF0);
    return -1;
}

static bool route_timer16_write8(address_bus_t *bus, uint32_t address, uint8_t value)
{
    if (address >= 0xFFFEA0 && address <= 0xFFFEAB && bus->timer16[5]) {
        timer16_write8(bus->timer16[5], address - 0xFFFEA0, value); return true;
    }
    if (address >= 0xFFFE90 && address <= 0xFFFE9B && bus->timer16[4]) {
        timer16_write8(bus->timer16[4], address - 0xFFFE90, value); return true;
    }
    if (address >= 0xFFFE80 && address <= 0xFFFE8F && bus->timer16[3]) {
        timer16_write8(bus->timer16[3], address - 0xFFFE80, value); return true;
    }
    if (address >= 0xFFFFD0 && address <= 0xFFFFDF && bus->timer16[0]) {
        timer16_write8(bus->timer16[0], address - 0xFFFFD0, value); return true;
    }
    if (address >= 0xFFFFE0 && address <= 0xFFFFEB && bus->timer16[1]) {
        timer16_write8(bus->timer16[1], address - 0xFFFFE0, value); return true;
    }
    if (address >= 0xFFFFF0 && address <= 0xFFFFFB && bus->timer16[2]) {
        timer16_write8(bus->timer16[2], address - 0xFFFFF0, value); return true;
    }
    return false;
}

static bool route_timer16_write16(address_bus_t *bus, uint32_t address, uint16_t value)
{
    if (address >= 0xFFFEA0 && address <= 0xFFFEAB && bus->timer16[5]) {
        timer16_write16(bus->timer16[5], address - 0xFFFEA0, value); return true;
    }
    if (address >= 0xFFFE90 && address <= 0xFFFE9B && bus->timer16[4]) {
        timer16_write16(bus->timer16[4], address - 0xFFFE90, value); return true;
    }
    if (address >= 0xFFFE80 && address <= 0xFFFE8F && bus->timer16[3]) {
        timer16_write16(bus->timer16[3], address - 0xFFFE80, value); return true;
    }
    if (address >= 0xFFFFD0 && address <= 0xFFFFDF && bus->timer16[0]) {
        timer16_write16(bus->timer16[0], address - 0xFFFFD0, value); return true;
    }
    if (address >= 0xFFFFE0 && address <= 0xFFFFEB && bus->timer16[1]) {
        timer16_write16(bus->timer16[1], address - 0xFFFFE0, value); return true;
    }
    if (address >= 0xFFFFF0 && address <= 0xFFFFFB && bus->timer16[2]) {
        timer16_write16(bus->timer16[2], address - 0xFFFFF0, value); return true;
    }
    return false;
}

/* --- Timer8 routing helpers --- */

static uint8_t read_timer8(address_bus_t *bus, int bus_off)
{
    timer8_t *ch = (bus_off % 2 == 0) ? bus->timer8[0] : bus->timer8[1];
    int reg_off = bus_off & ~1;
    return ch ? timer8_read(ch, reg_off) : 0;
}

static void write_timer8(address_bus_t *bus, int bus_off, uint8_t value)
{
    timer8_t *ch = (bus_off % 2 == 0) ? bus->timer8[0] : bus->timer8[1];
    int reg_off = bus_off & ~1;
    if (ch) timer8_write(ch, reg_off, value);
}

/* --- Init / Free --- */

void bus_init(address_bus_t *bus)
{
    memset(bus, 0, sizeof(*bus));
    bus->adcr = 0x7E;
    cybiko_sci2_init(&bus->sci2);
#if CYBIKO_HAS_SPI_FLASH
    at45_init(&bus->spi_flash, NULL);
    cybiko_uart_init(&bus->uart);
    bus->adc[0] = CYBIKO_ADC_DEFAULT_OTHER;
    bus->adc[1] = CYBIKO_ADC_DEFAULT_CH1;
    bus->adc[2] = CYBIKO_ADC_DEFAULT_CH2;
    bus->adc[3] = CYBIKO_ADC_DEFAULT_OTHER;
#endif
}

void bus_free(address_bus_t *bus)
{
    memory_free(&bus->boot_rom);
    memory_free(&bus->external_ram);
    memory_free(&bus->flash_rom);
    memory_free(&bus->on_chip_ram);
}

/* --- Read operations --- */

/*
 * Fast-path reads: CyOS runs from external RAM (0x400000-0x5FFFFF), so
 * instruction fetches and most data reads hit ext RAM.  Stack/locals are
 * in on-chip RAM (0xFFDC00-0xFFFDFF).  These two regions account for the
 * vast majority of bus reads, so we check them first with direct array
 * access—no decode_region(), no switch, no bounds check, no modulo.
 */

uint8_t BUS_HOT bus_read8(address_bus_t *bus, uint32_t address)
{
    address &= 0xFFFFFF;

    /* Fast path: external RAM (code + data) */
    if (address >= EXT_RAM_BASE && address <= EXT_RAM_END) {
        uint32_t off = address & (EXT_RAM_SIZE - 1);
        return bus->external_ram.data[off];
    }
    /* Fast path: on-chip RAM below I/O (stack/locals) */
    if (address >= ON_CHIP_RAM_BASE && address < CYBIKO_ON_CHIP_IO) {
        return bus->on_chip_ram.data[address - ON_CHIP_RAM_BASE];
    }

    /* Slow path */
    switch (decode_region(address)) {
    case REGION_BOOT_ROM:
        return memory_read8(&bus->boot_rom, address & 0x7FFF);
    case REGION_LCD:
        return bus->lcd ? hd66421_read8(bus->lcd, address & 1) : 0;
    case REGION_USB:
        return 0;
    case REGION_EXT_RAM:
        return bus->external_ram.data[address & (EXT_RAM_SIZE - 1)];
    case REGION_FLASH:
#if CYBIKO_HAS_FLASH_MAP
        return memory_read8(&bus->flash_rom, flash_offset(address));
#else
        return 0;
#endif
    case REGION_KEYBOARD: {
        if (!bus->keyboard) return 0xFF;
        uint16_t kb_val = keyboard_read(bus->keyboard, address & ~1);
        return (address & 1) == 0 ? (kb_val >> 8) & 0xFF : kb_val & 0xFF;
    }
    case REGION_ON_CHIP:
        return read_on_chip8(bus, address);
    case REGION_UNMAPPED:
        log_unmapped(bus, "read8", address);
        return 0;
    }
    return 0;
}

uint16_t BUS_HOT bus_read16(address_bus_t *bus, uint32_t address)
{
    address &= 0xFFFFFF;

    /* Fast path: external RAM (instruction fetch + data) */
    if (address >= EXT_RAM_BASE && address <= EXT_RAM_END) {
        uint32_t off = address & (EXT_RAM_SIZE - 1);
        return ((uint16_t)bus->external_ram.data[off] << 8) |
               bus->external_ram.data[off + 1];
    }
    /* Fast path: on-chip RAM below I/O (stack/locals) */
    if (address >= ON_CHIP_RAM_BASE && address < CYBIKO_ON_CHIP_IO) {
        uint32_t off = address - ON_CHIP_RAM_BASE;
        return ((uint16_t)bus->on_chip_ram.data[off] << 8) |
               bus->on_chip_ram.data[off + 1];
    }

    /* Slow path */
    switch (decode_region(address)) {
    case REGION_BOOT_ROM:
        return memory_read16(&bus->boot_rom, address & 0x7FFF);
    case REGION_LCD:
        if (!bus->lcd) return 0;
        return (hd66421_read8(bus->lcd, 0) << 8) | hd66421_read8(bus->lcd, 1);
    case REGION_USB:
        return 0;
    case REGION_EXT_RAM:
        return ((uint16_t)bus->external_ram.data[address & (EXT_RAM_SIZE - 1)] << 8) |
               bus->external_ram.data[(address & (EXT_RAM_SIZE - 1)) + 1];
    case REGION_FLASH:
#if CYBIKO_HAS_FLASH_MAP
        return memory_read16(&bus->flash_rom, flash_offset(address));
#else
        return 0;
#endif
    case REGION_KEYBOARD:
        if (!bus->keyboard) return 0xFFFF;
        return keyboard_read(bus->keyboard, address);
    case REGION_ON_CHIP:
        return read_on_chip16(bus, address);
    case REGION_UNMAPPED:
        log_unmapped(bus, "read16", address);
        return 0;
    }
    return 0;
}

uint32_t BUS_HOT bus_read32(address_bus_t *bus, uint32_t address)
{
    return ((uint32_t)bus_read16(bus, address) << 16) | bus_read16(bus, address + 2);
}

/* --- Write operations --- */

void BUS_HOT bus_write8(address_bus_t *bus, uint32_t address, uint8_t value)
{
    address &= 0xFFFFFF;

    /* Fast path: external RAM (data writes) */
    if (address >= EXT_RAM_BASE && address <= EXT_RAM_END) {
        bus->external_ram.data[address & (EXT_RAM_SIZE - 1)] = value;
        return;
    }
    /* Fast path: on-chip RAM below I/O (stack writes) */
    if (address >= ON_CHIP_RAM_BASE && address < CYBIKO_ON_CHIP_IO) {
        bus->on_chip_ram.data[address - ON_CHIP_RAM_BASE] = value;
        return;
    }

    /* Slow path */
    switch (decode_region(address)) {
    case REGION_BOOT_ROM:
        break;
    case REGION_LCD:
        if (bus->lcd) hd66421_write8(bus->lcd, address & 1, value);
        break;
    case REGION_USB:
        break;
    case REGION_EXT_RAM:
        bus->external_ram.data[address & (EXT_RAM_SIZE - 1)] = value;
        break;
    case REGION_FLASH:
#if CYBIKO_HAS_FLASH_MAP
        memory_write8(&bus->flash_rom, flash_offset(address), value);
#endif
        break;
    case REGION_KEYBOARD:
        break;
    case REGION_ON_CHIP:
        write_on_chip8(bus, address, value);
        break;
    case REGION_UNMAPPED:
        log_unmapped(bus, "write8", address);
        break;
    }
}

void BUS_HOT bus_write16(address_bus_t *bus, uint32_t address, uint16_t value)
{
    address &= 0xFFFFFF;

    /* Fast path: external RAM */
    if (address >= EXT_RAM_BASE && address <= EXT_RAM_END) {
        uint32_t off = address & (EXT_RAM_SIZE - 1);
        bus->external_ram.data[off]     = (uint8_t)(value >> 8);
        bus->external_ram.data[off + 1] = (uint8_t)(value);
        return;
    }
    /* Fast path: on-chip RAM below I/O */
    if (address >= ON_CHIP_RAM_BASE && address < CYBIKO_ON_CHIP_IO) {
        uint32_t off = address - ON_CHIP_RAM_BASE;
        bus->on_chip_ram.data[off]     = (uint8_t)(value >> 8);
        bus->on_chip_ram.data[off + 1] = (uint8_t)(value);
        return;
    }

    /* Slow path */
    switch (decode_region(address)) {
    case REGION_BOOT_ROM:
        break;
    case REGION_LCD:
        if (bus->lcd) {
            hd66421_write8(bus->lcd, 0, value >> 8);
            hd66421_write8(bus->lcd, 1, value & 0xFF);
        }
        break;
    case REGION_USB:
        break;
    case REGION_EXT_RAM: {
        uint32_t off2 = address & (EXT_RAM_SIZE - 1);
        bus->external_ram.data[off2]     = (uint8_t)(value >> 8);
        bus->external_ram.data[off2 + 1] = (uint8_t)(value);
        break;
    }
    case REGION_FLASH:
#if CYBIKO_HAS_FLASH_MAP
        memory_write16(&bus->flash_rom, flash_offset(address), value);
#endif
        break;
    case REGION_KEYBOARD:
        break;
    case REGION_ON_CHIP:
        write_on_chip16(bus, address, value);
        break;
    case REGION_UNMAPPED:
        log_unmapped(bus, "write16", address);
        break;
    }
}

void BUS_HOT bus_write32(address_bus_t *bus, uint32_t address, uint32_t value)
{
    bus_write16(bus, address, (uint16_t)(value >> 16));
    bus_write16(bus, address + 2, (uint16_t)(value & 0xFFFF));
}

/* --- On-chip 8-bit read --- */

static uint8_t read_on_chip8(address_bus_t *bus, uint32_t address)
{
    /* Timer16 channels (non-contiguous, check first) */
    int t16 = route_timer16_read8(bus, address);
    if (t16 >= 0) return (uint8_t)t16;

    /* Timer8 Channel 0/1: 0xFFFFB0-0xFFFFB9 */
    if (address >= 0xFFFFB0 && address <= 0xFFFFB9) {
        return read_timer8(bus, address - 0xFFFFB0);
    }

    /* TSTR - Timer Start Register */
    if (address == 0xFFFFC0) return bus->tstr;
    /* TSYR stub */
    if (address == 0xFFFFC1) return 0;

    /* IER / ISR */
    if (address == 0xFFFF2E) return bus->ier;
    if (address == 0xFFFF2F) return bus->isr;

#if CYBIKO_HAS_SPI_FLASH
    if (bus->uart.bound) {
        uint8_t uart_val;
        if (cybiko_uart_read(&bus->uart, address, &uart_val))
            return uart_val;
    }
#endif
    /* SCI2 SCR/SSR/RDR come from the co-processor once one is attached.
     * Until then SCI2 SSR stays transmit-ready, which is what CyOS boot sees. */
    if (bus->sci2.bound) {
        uint8_t sci_val;
        if (cybiko_sci2_read(&bus->sci2, address, &sci_val))
            return sci_val;
    }

#if CYBIKO_HAS_SPI_FLASH
    /* SCI1 is the SPI flash link: RDRF once a byte has been clocked */
    if (address == 0xFFFF84)
        return SSR_TDRE | SSR_TEND | (bus->sci1_rdrf ? SSR_RDRF : 0);
    if (address == 0xFFFF85) {
        bus->sci1_rdrf = false;
        return bus->sci1_rdr;
    }
    /* ICR (H8S/2241 interrupt control) */
    if (address >= 0xFFFEC0 && address <= 0xFFFEC2)
        return bus->icr[address - 0xFFFEC0];
#endif

#if CYBIKO_HAS_SPI_FLASH
    if (bus->sci0.bound) {
        uint8_t sci_val;
        if (cybiko_sci0_read(&bus->sci0, address, &sci_val))
            return sci_val;
    }
#endif

    /* SCI SSR registers - always report transmit ready */
    if (address == 0xFFFF7C) return SSR_TDRE | SSR_TEND;  /* SCI0 SSR */
    if (address == 0xFFFF84) return SSR_TDRE | SSR_TEND;  /* SCI1 SSR */
    if (address == 0xFFFF85) return 0;                     /* SCI1 RDR */
    if (address == 0xFFFF8C) return SSR_TDRE | SSR_TEND;  /* SCI2 SSR */

#if CYBIKO_HAS_DMA
    /* DMA registers (XT) */
    if (address >= 0xFFFEE0 && address <= 0xFFFEFF) {
        return bus->dma_regs[address - 0xFFFEE0];
    }
    if (address >= 0xFFFF00 && address <= 0xFFFF07) {
        return bus->dma_ctrl[address - 0xFFFF00];
    }
#endif

    /* System control stubs */
    if (address == 0xFFFF38) return 0;  /* SBYCR */
    if (address == 0xFFFF39) return 0;  /* SYSCR */
    if (address == 0xFFFF3B) return 0;  /* MDCR */

    /* Port Input Data Registers (0xFFFF50-0xFFFF5E) */
    if (address >= 0xFFFF50 && address <= 0xFFFF5E) {
        /* Power status: XT Port A (0xFFFF59), V1 Port 1 (0xFFFF50) */
        if (address == CYBIKO_POWER_PORT_IN) return CYBIKO_POWER_VALUE;
        /* Port F input (0xFFFF5E) - RTC SDA */
        if (address == 0xFFFF5E) {
            if (bus->rtc && rtc_sda_r(bus->rtc))
                return 0xFF;
            else
                return 0xFF & ~CYBIKO_RTC_SDA_BIT;
        }
        return memory_read8(&bus->on_chip_ram, on_chip_offset(address));
    }

    /* Port Data Registers (0xFFFF60-0xFFFF6F) */
    if (address >= 0xFFFF60 && address <= 0xFFFF6F) {
#ifdef CYBIKO_POWER_PORT_DR
        /* Port A DR (0xFFFF69) - power status for XT */
        if (address == CYBIKO_POWER_PORT_DR) return CYBIKO_POWER_VALUE;
#endif
        /* Port F DR (0xFFFF6E) - RTC SDA */
        if (address == 0xFFFF6E) {
            if (bus->rtc && rtc_sda_r(bus->rtc))
                return 0xFF;
            else
                return 0xFF & ~CYBIKO_RTC_SDA_BIT;
        }
        return memory_read8(&bus->on_chip_ram, on_chip_offset(address));
    }

    /* Watchdog stub */
    if (address >= 0xFFFFBC && address <= 0xFFFFBF) return 0;

    /* ADC registers (0xFFFF90-0xFFFF99) */
    if (address >= 0xFFFF90 && address <= 0xFFFF99) {
        /* ADC data registers (0xFFFF90-0xFFFF97) */
        if (address <= 0xFFFF97) {
#if CYBIKO_HAS_SPI_FLASH
            /* V1: 10-bit result, left-aligned: ADDRn high = v >> 2,
             * low = (v & 3) << 6. Channels 1 and 2 are the battery. */
            uint16_t v = bus->adc[(address - 0xFFFF90) / 2];
            return (address % 2 == 0) ? (uint8_t)(v >> 2) : (uint8_t)((v & 3) << 6);
#else
            /* XT: even=0xCC, odd=0x00 */
            return (address % 2 == 0) ? 0xCC : 0x00;
#endif
        }
        if (address == 0xFFFF98) return bus->adcsr;
        return bus->adcr;  /* 0xFFFF99 */
    }

    /* Default: on-chip RAM fallthrough */
    return memory_read8(&bus->on_chip_ram, on_chip_offset(address));
}

/* --- On-chip 16-bit read --- */

static uint16_t read_on_chip16(address_bus_t *bus, uint32_t address)
{
    /* Timer16 routing first */
    int t16 = route_timer16_read16(bus, address);
    if (t16 >= 0) return (uint16_t)t16;

    /* I/O register region: compose from two 8-bit reads */
    if (address >= CYBIKO_ON_CHIP_IO) {
        return (read_on_chip8(bus, address) << 8) | read_on_chip8(bus, address + 1);
    }

    /* Below the I/O block: direct on-chip RAM read */
    return memory_read16(&bus->on_chip_ram, on_chip_offset(address));
}

/* --- On-chip 8-bit write --- */

static void write_on_chip8(address_bus_t *bus, uint32_t address, uint8_t value)
{
    /* Timer16 routing (check first, non-contiguous addresses) */
    if (route_timer16_write8(bus, address, value)) return;

    /* Timer8 Channel 0/1: 0xFFFFB0-0xFFFFB9 */
    if (address >= 0xFFFFB0 && address <= 0xFFFFB9) {
        write_timer8(bus, address - 0xFFFFB0, value);
        return;
    }

    /* TSTR - Timer Start Register (0xFFFFC0) */
    if (address == 0xFFFFC0) {
        bus->tstr = value & 0xFF;
        if (bus->timer16[0]) timer16_set_enabled(bus->timer16[0], (bus->tstr & 0x01) != 0);
        if (bus->timer16[1]) timer16_set_enabled(bus->timer16[1], (bus->tstr & 0x02) != 0);
        if (bus->timer16[2]) timer16_set_enabled(bus->timer16[2], (bus->tstr & 0x04) != 0);
        if (bus->timer16[3]) timer16_set_enabled(bus->timer16[3], (bus->tstr & 0x08) != 0);
        if (bus->timer16[4]) timer16_set_enabled(bus->timer16[4], (bus->tstr & 0x10) != 0);
        if (bus->timer16[5]) timer16_set_enabled(bus->timer16[5], (bus->tstr & 0x20) != 0);
        return;
    }

    /* IER */
    if (address == 0xFFFF2E) { bus->ier = value & 0xFF; return; }
    /* ISR - write-0-to-clear */
    if (address == 0xFFFF2F) { bus->isr &= value; return; }

    /* SCI registers (0xFFFF78-0xFFFF8E) */
    if (address >= 0xFFFF78 && address <= 0xFFFF8E) {
        int channel, reg;
        if (address >= 0xFFFF88) {
            channel = 2; reg = address - 0xFFFF88;
        } else if (address >= 0xFFFF80) {
            channel = 1; reg = address - 0xFFFF80;
        } else {
            channel = 0; reg = address - 0xFFFF78;
        }
        /* TDR - Transmit Data Register (reg offset 3). On V1, SCI1 is
         * the SPI flash link and SCI0 is the radio UART once bound;
         * neither is a console. Dumping radio frames to USB CDC made
         * every key wait on the host draining the port. */
        if (reg == 3) {
            bool echo = true;
#if CYBIKO_HAS_SPI_FLASH
            if (channel == 1 || (channel == 0 && bus->sci0.bound))
                echo = false;
#endif
            if (echo) {
                char c = (char)(value & 0x7F);
                if (c >= 0x20 || c == '\n') {
                    /* printable or newline */
                } else {
                    c = '.';
                }
                if (bus->sci_output_pos[channel] < 255) {
                    bus->sci_output[channel][bus->sci_output_pos[channel]++] = c;
                    bus->sci_output[channel][bus->sci_output_pos[channel]] = '\0';
                }
            }
        }
        if (channel == 2 && bus->sci2.bound)
            cybiko_sci2_on_reg(&bus->sci2, reg, value);
#if CYBIKO_HAS_SPI_FLASH
        if (channel == 0 && bus->sci0.bound)
            cybiko_sci0_on_reg(&bus->sci0, reg, value);
        if (channel == 2 && bus->uart.bound)
            cybiko_uart_on_reg(&bus->uart, reg, value);
#endif
#if CYBIKO_HAS_SPI_FLASH
        if (channel == 1 && reg == 3) {          /* TDR: clock one byte */
            bus->sci1_rdr = at45_transfer(&bus->spi_flash, value);
            bus->sci1_rdrf = true;
        } else if (channel == 1 && reg == 2) {   /* SCR: may start a DTC run */
            execute_sci1_dtc(bus, value);
        }
#endif
        /* Store to on-chip RAM regardless */
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

    /* Port DDR registers (0xFFFEB0-0xFFFEBF) */
    if (address >= 0xFFFEB0 && address <= 0xFFFEBF) {
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
#ifdef CYBIKO_RTC_SDA_VIA_DDR
        /* Port F DDR: output (bit set) pulls SDA low, input releases it */
        if (address == 0xFFFEBE && bus->rtc)
            rtc_sda_w(bus->rtc, (value & CYBIKO_RTC_SDA_BIT) == 0);
#endif
        return;
    }
#if CYBIKO_HAS_SPI_FLASH
    /* ICR */
    if (address >= 0xFFFEC0 && address <= 0xFFFEC2) {
        bus->icr[address - 0xFFFEC0] = value;
        return;
    }
#endif

    /* Port 1 write (0xFFFF60) - bit 3 drives speaker */
    if (address == 0xFFFF60) {
        int level = (value & 0x08) != 0 ? 1 : 0;
        if (bus->speaker) {
            speaker_set_level(bus->speaker, level);
        }
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

#if CYBIKO_HAS_SPI_FLASH
    /* Port 3 DR (0xFFFF62) bit 4 = flash CS, active low, and only driven
     * while Port 3 DDR bit 4 makes it an output */
    if (address == 0xFFFF62) {
        uint8_t ddr = memory_read8(&bus->on_chip_ram, on_chip_offset(0xFFFEB2));
        at45_cs(&bus->spi_flash, (ddr & 0x10) && !(value & 0x10));
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }
#endif

    /* Port F write (0xFFFF6E) - I2C RTC bit-banging */
    if (address == 0xFFFF6E) {
        if (bus->rtc) {
            rtc_scl_w(bus->rtc, (value & 0x02) != 0);
#ifndef CYBIKO_RTC_SDA_VIA_DDR
            /* XT: SDA is inverted - writing the bit = pull SDA low */
            rtc_sda_w(bus->rtc, (value & CYBIKO_RTC_SDA_BIT) == 0);
#endif
        }
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

    /* Port data register writes (0xFFFF60-0xFFFF6F) - catch remaining ports */
    if (address >= 0xFFFF60 && address <= 0xFFFF6F) {
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        return;
    }

#if CYBIKO_HAS_DMA
    /* DMA registers (XT) */
    if (address >= 0xFFFEE0 && address <= 0xFFFEFF) {
        bus->dma_regs[address - 0xFFFEE0] = value & 0xFF;
        return;
    }
    if (address >= 0xFFFF00 && address <= 0xFFFF07) {
        int idx = address - 0xFFFF00;
        uint8_t old_val = bus->dma_ctrl[idx];
        bus->dma_ctrl[idx] = value & 0xFF;

        /* DMA trigger edge detection on index 7 */
        if (idx == 7) {
            bool ch0_triggered = false;
            /* Channel 0: bit 4 (0x10) or bit 5 (0x20) rising edge */
            if ((value & 0x10) && !(old_val & 0x10)) {
                execute_dma_transfer(bus, 0);
                ch0_triggered = true;
            }
            if (!ch0_triggered && (value & 0x20) && !(old_val & 0x20)) {
                execute_dma_transfer(bus, 0);
            }
            bool ch1_triggered = false;
            /* Channel 1: bit 6 (0x40) or bit 7 (0x80) rising edge */
            if ((value & 0x40) && !(old_val & 0x40)) {
                execute_dma_transfer(bus, 1);
                ch1_triggered = true;
            }
            if (!ch1_triggered && (value & 0x80) && !(old_val & 0x80)) {
                execute_dma_transfer(bus, 1);
            }
        }
        return;
    }
#endif

    /* System control, watchdog stubs */
    if (address == 0xFFFF38 || address == 0xFFFF39) return;
    if (address >= 0xFFFFBC && address <= 0xFFFFBF) return;

    /* ADC registers */
    if (address >= 0xFFFF90 && address <= 0xFFFF99) {
        if (address == 0xFFFF98) {
#if CYBIKO_HAS_SPI_FLASH
            /* V1, as MAME: ADST converts at once (ADF set, ADST clear) and
             * raises ADI (vector 28) if ADIE; ADF clears only on a 0 write */
            if (value & 0x20) {
                bus->adcsr = (value & 0x5F) | 0x80;
                if ((value & 0x40) && bus->cpu)
                    h8s_cpu_request_interrupt(bus->cpu, 28);
            } else {
                bus->adcsr = (value & 0x7F) | (bus->adcsr & value & 0x80);
            }
#else
            if (value & 0x20) {
                bus->adcsr = (value & 0x7F) | 0x80;  /* auto-set ADF */
            } else {
                bus->adcsr = value & 0x7F;
            }
#endif
        } else if (address == 0xFFFF99) {
            bus->adcr = value & 0xFF;
        }
        return;
    }

    /* DTCERF. Store first so executeSci2Dtc reads the new bits.
     * The handler clears its own bit with a raw poke, then this write's
     * value (not the poked register) decides whether 0x03 is queued. */
    if (address == CYBIKO_SCI2_DTCERF && bus->sci2.bound) {
        memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
        cybiko_sci2_on_dtcerf(&bus->sci2, value);
        return;
    }

    /* Default: write to on-chip RAM */
    memory_write8(&bus->on_chip_ram, on_chip_offset(address), value);
}

/* --- On-chip 16-bit write --- */

static void write_on_chip16(address_bus_t *bus, uint32_t address, uint16_t value)
{
    /* Timer16 routing first */
    if (route_timer16_write16(bus, address, value)) return;

    /* I/O register region: split into two 8-bit writes */
    if (address >= CYBIKO_ON_CHIP_IO) {
        write_on_chip8(bus, address, (value >> 8) & 0xFF);
        write_on_chip8(bus, address + 1, value & 0xFF);
        return;
    }

    /* Below the I/O block: direct on-chip RAM write */
    memory_write16(&bus->on_chip_ram, on_chip_offset(address), value);
}

#if CYBIKO_HAS_DMA
/* --- DMA transfer (XT) --- */

static void execute_dma_transfer(address_bus_t *bus, int channel)
{
    int base = channel * 16;
    uint32_t src_addr = ((uint32_t)bus->dma_regs[base] << 24)
                      | ((uint32_t)bus->dma_regs[base + 1] << 16)
                      | ((uint32_t)bus->dma_regs[base + 2] << 8)
                      | bus->dma_regs[base + 3];
    uint32_t dst_addr = ((uint32_t)bus->dma_regs[base + 8] << 24)
                      | ((uint32_t)bus->dma_regs[base + 9] << 16)
                      | ((uint32_t)bus->dma_regs[base + 10] << 8)
                      | bus->dma_regs[base + 11];
    int count = (bus->dma_regs[base + 6] << 8) | bus->dma_regs[base + 7];

    uint8_t dmacr_h = bus->dma_ctrl[2 + channel * 2];
    uint8_t dmacr_l = bus->dma_ctrl[3 + channel * 2];
    bool mode16    = (dmacr_h & 0x80) != 0;  /* Bit 15: transfer size      */
    bool src_dec   = (dmacr_h & 0x40) != 0;  /* Bit 14: source decrement   */
    bool src_inc   = (dmacr_h & 0x20) != 0;  /* Bit 13: source inc enable  */
    bool dst_dec   = (dmacr_l & 0x40) != 0;  /* Bit  6: dest decrement     */
    bool dst_inc   = (dmacr_l & 0x20) != 0;  /* Bit  5: dest inc enable    */

    int step = mode16 ? 2 : 1;
    int src_step = src_inc ? (src_dec ? -step : step) : 0;
    int dst_step = dst_inc ? (dst_dec ? -step : step) : 0;

    src_addr &= 0xFFFFFF;
    dst_addr &= 0xFFFFFF;

    if (count == 0) {
        if (channel == 1) bus->dma_ctrl[7] &= ~0xC0;
        else bus->dma_ctrl[7] &= ~0x30;
        return;
    }

    bus->in_dma = true;
    if (mode16) {
        for (int i = 0; i < count; i++) {
            uint16_t val = bus_read16(bus, src_addr);
            bus_write16(bus, dst_addr, val);
            src_addr = (src_addr + src_step) & 0xFFFFFF;
            dst_addr = (dst_addr + dst_step) & 0xFFFFFF;
        }
    } else {
        for (int i = 0; i < count; i++) {
            uint8_t val = bus_read8(bus, src_addr);
            bus_write8(bus, dst_addr, val);
            src_addr = (src_addr + src_step) & 0xFFFFFF;
            dst_addr = (dst_addr + dst_step) & 0xFFFFFF;
        }
    }

    bus->in_dma = false;

    /* Clear trigger bits and count */
    if (channel == 1) bus->dma_ctrl[7] &= ~0xC0;
    else bus->dma_ctrl[7] &= ~0x30;
    bus->dma_regs[base + 6] = 0;
    bus->dma_regs[base + 7] = 0;
}
#endif

#if CYBIKO_HAS_SPI_FLASH
/*
 * V1 SCI1 DTC. CyOS fills the register block at 0xFFFBD0, sets DTCER bit 1
 * and then writes SCR; the whole transfer runs here on that write, as in
 * AddressBus.executeSci1Dtc. Block: MRA, SAR (3 bytes), DAR (4 bytes, top
 * byte 0), CRA (16-bit count). MRA 0x20 = receive into DAR, 0x80 = send
 * from SAR. Completion is SCI1 RXI (vector 85), deferred one CPU batch so
 * it lands after CyOS has re-enabled interrupts.
 */
#define DTC_SCI1_BLOCK 0xFFFBD0
#ifndef CYBIKO_DTC_DELAY
#define CYBIKO_DTC_DELAY 1   /* CPU batches before the completion interrupt */
#endif
#define DTCER_A        0xFFFF34

static void execute_sci1_dtc(address_bus_t *bus, uint8_t scr)
{
    bool rx = (scr & 0x50) == 0x50;   /* RE + RIE */
    bool tx = (scr & 0xA0) == 0xA0;   /* TE + TIE */
    if (!rx && !tx)
        return;
    uint8_t dtcer = memory_read8(&bus->on_chip_ram, on_chip_offset(DTCER_A));
    if (!(dtcer & 0x02))
        return;

    const uint8_t *b = bus->on_chip_ram.data + on_chip_offset(DTC_SCI1_BLOCK);
    uint8_t  mra = b[0];
    uint32_t sar = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    uint32_t dar = ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    unsigned count = ((unsigned)b[8] << 8) | b[9];
    if (count == 0)
        return;

    if (mra == 0x20 && rx) {
        for (unsigned i = 0; i < count; i++)
            bus_write8(bus, dar + i, at45_transfer(&bus->spi_flash, 0xFF));
    } else if (mra == 0x80 && tx) {
        for (unsigned i = 0; i < count; i++)
            bus->sci1_rdr = at45_transfer(&bus->spi_flash, bus_read8(bus, sar + i));
        bus->sci1_rdrf = true;
    } else {
        return;
    }

    memory_write8(&bus->on_chip_ram, on_chip_offset(DTC_SCI1_BLOCK + 8), 0);
    memory_write8(&bus->on_chip_ram, on_chip_offset(DTC_SCI1_BLOCK + 9), 0);
    memory_write8(&bus->on_chip_ram, on_chip_offset(DTCER_A), dtcer & ~0x02);
    bus->dma_completion_vector = 85;
    bus->dma_completion_delay = CYBIKO_DTC_DELAY;
}

void bus_load_spi_flash(address_bus_t *bus, uint8_t *data)
{
    at45_init(&bus->spi_flash, data);
}
#endif

/* --- Tick functions --- */

void bus_tick_dma_completion(address_bus_t *bus)
{
    if (bus->dma_completion_delay > 0) {
        bus->dma_completion_delay--;
        if (bus->dma_completion_delay == 0 && bus->cpu) {
            h8s_cpu_request_interrupt(bus->cpu, bus->dma_completion_vector);
        }
    }
}

void bus_tick_rtc(address_bus_t *bus)
{
    if (bus->rtc) {
        rtc_tick(bus->rtc);
    }
}

static uint8_t sci2_mem_read(void *ctx, uint32_t address)
{
    return bus_read8((address_bus_t *)ctx, address);
}

static void sci2_mem_poke(void *ctx, uint32_t address, uint8_t value)
{
    address_bus_t *bus = ctx;
    if (address >= EXT_RAM_BASE && address <= EXT_RAM_END) {
        memory_write8(&bus->external_ram, address - EXT_RAM_BASE, value);
        return;
    }
    if (address >= ON_CHIP_RAM_BASE)
        memory_write8(&bus->on_chip_ram, address - ON_CHIP_RAM_BASE, value);
}

static void sci2_raise(void *ctx, int vector)
{
    address_bus_t *bus = ctx;
    if (bus->cpu)
        h8s_cpu_request_interrupt(bus->cpu, vector);
}

void bus_attach_radio(address_bus_t *bus, cybiko_radio_coproc_t *coproc, uint32_t radio_id)
{
#if CYBIKO_HAS_SPI_FLASH
    /* The Classic's radio is on SCI0; its SCI2 is the serial port */
    cybiko_sci0_bind(&bus->sci0, coproc, sci2_mem_read, sci2_mem_poke, sci2_raise, bus,
                     radio_id);
#else
    (void)radio_id;
    cybiko_sci2_bind(&bus->sci2, coproc, sci2_mem_read, sci2_mem_poke, sci2_raise, bus);
#endif
}

void bus_tick_sci2(address_bus_t *bus, int cycles)
{
#if CYBIKO_HAS_SPI_FLASH
    cybiko_sci0_advance(&bus->sci0, cycles);
    cybiko_uart_advance(&bus->uart, cycles);
#else
    if (!bus->sci2.bound || cycles <= 0)
        return;
    cybiko_sci2_advance(&bus->sci2, cycles);
#endif
}
