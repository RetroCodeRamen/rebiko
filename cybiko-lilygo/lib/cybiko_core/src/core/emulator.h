#ifndef CYBIKO_EMULATOR_H
#define CYBIKO_EMULATOR_H
#include "types.h"
#include "machine.h"
#include "../hal/hal.h"

typedef struct cybiko_emu cybiko_emu_t;

cybiko_emu_t *cybiko_create(const cybiko_hal_t *hal);
void          cybiko_destroy(cybiko_emu_t *emu);
bool cybiko_load_boot_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len);
bool cybiko_load_flash_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len);
bool cybiko_load_nvram(cybiko_emu_t *emu, const uint8_t *data, size_t len);
void cybiko_reset(cybiko_emu_t *emu);
/* Cold boot: RAM and peripherals, CPU reset. Flash / NVRAM image stay. */
void cybiko_reboot(cybiko_emu_t *emu);
void cybiko_run_frame(cybiko_emu_t *emu);
struct cybiko_radio_coproc;
void cybiko_attach_radio(cybiko_emu_t *emu, struct cybiko_radio_coproc *coproc);
/* radio_id: the guest CyID and link sender id. The Classic patches it into
 * RAM after CyOS loads (its CyID is inside the compressed flash). */
void cybiko_attach_radio_id(cybiko_emu_t *emu, struct cybiko_radio_coproc *coproc,
                            uint32_t radio_id);

/* The Classic's serial port (SCI2, where CyberLoad talked to CyOS). tx
 * gets each byte CyOS sends; cybiko_serial_put queues bytes for CyOS and
 * returns how many fit. No-ops on the Xtreme, whose SCI2 is the radio. */
void cybiko_attach_serial(cybiko_emu_t *emu, void (*tx)(void *user, uint8_t byte), void *user);
int  cybiko_serial_put(cybiko_emu_t *emu, const uint8_t *data, int n);

/* Classic radio progress, for logs and tests. False on the Xtreme. */
typedef struct {
    bool radio_up;          /* CyOS has finished its first radio frame */
    uint32_t tx_frames;     /* frames CyOS finished sending */
    uint32_t rx_injected;   /* received frames handed to CyOS */
} cybiko_v1_radio_status_t;
bool cybiko_v1_radio_status(const cybiko_emu_t *emu, cybiko_v1_radio_status_t *out);
bool cybiko_is_running(const cybiko_emu_t *emu);
const uint8_t *cybiko_get_nvram(const cybiko_emu_t *emu, size_t *len);

/* Running totals since create. Guest instructions executed, and guest
 * cycles skipped by the SLEEP fast-forward. */
typedef struct {
    uint64_t instructions;
    uint64_t sleep_cycles;
    uint32_t frames;
} cybiko_stats_t;
void cybiko_get_stats(const cybiko_emu_t *emu, cybiko_stats_t *out);

/* Built with -DCYBIKO_PROFILE: the frame loop samples the guest PC every
 * 64 CPU batches into a ring. Returns the sample count (<= max). */
#define CYBIKO_PC_SAMPLES 2048
int cybiko_take_pc_samples(cybiko_emu_t *emu, uint32_t *out, int max);

/* ISO-HDLC CRC-32, the same value java.util.zip.CRC32 produces. */
uint32_t cybiko_crc32(const uint8_t *data, size_t len);

/*
 * XT CyID patch. Flash offset 0x7F818, big-endian, then CRC-32 of the
 * 2044-byte config block at 0x7F800 stored at 0x7FFFC. This matches
 * CybikoEmulator.patchCyId. Returns false when the image is shorter
 * than the Xtreme config block.
 */
bool cybiko_patch_cyid_image(uint8_t *flash, size_t len, uint32_t cyid);
bool cybiko_patch_cyid(cybiko_emu_t *emu, uint32_t cyid);

/* Debug views: the 4000-byte HD66421 VRAM (Java's STATUS vram= CRC is over
 * this), the guest PC, and whether the CPU is in SLEEP. */
const uint8_t *cybiko_get_vram(const cybiko_emu_t *emu);
uint32_t cybiko_get_pc(const cybiko_emu_t *emu);
bool cybiko_is_halted(const cybiko_emu_t *emu);
/* HD66421 DISP bit. Classic CyOS clears it when it sleeps (5 idle minutes)
 * and sets it again on a key. */
bool cybiko_lcd_on(const cybiko_emu_t *emu);
/* Changes whenever the rendered LCD picture may have changed */
uint32_t cybiko_lcd_serial(const cybiko_emu_t *emu);
/* ERn, and a guest byte read (through the bus: do not peek I/O registers
 * with side effects). For host debugging tools. */
uint32_t cybiko_get_reg(const cybiko_emu_t *emu, int n);
uint8_t  cybiko_get_ccr(const cybiko_emu_t *emu);
uint8_t cybiko_peek8(cybiko_emu_t *emu, uint32_t address);
void cybiko_poke8(cybiko_emu_t *emu, uint32_t address, uint8_t value);
/* Run n instructions from pc, no timers. For benchmarks before reset. */
void cybiko_debug_run(cybiko_emu_t *emu, uint32_t pc, int n);

/* Cybiko Classic: the AT45DB041 image (CyOS plus the file system), which
 * is what a V1 save holds. writes counts pages programmed since load.
 * NULL on the Xtreme. */
const uint8_t *cybiko_get_spi_flash(const cybiko_emu_t *emu, size_t *len, uint32_t *writes);
/* The Classic's flash pages CyOS has programmed since the last call, one
 * bit a page (bit p&7 of byte p>>3), copied to bits and cleared. Returns
 * how many; 0 on the Xtreme. A save can rewrite just these pages. */
int cybiko_take_spi_flash_dirty(cybiko_emu_t *emu, uint8_t *bits, size_t n);

struct tm;
/* The guest's clock chip. take_written: true once after the guest set
 * the time (CyOS Settings). get: the year is the one nearest year_hint
 * with the chip's year % 4. set: from the host, e.g. a time broadcast. */
bool cybiko_rtc_take_written(cybiko_emu_t *emu);
void cybiko_rtc_get(const cybiko_emu_t *emu, struct tm *out, int year_hint);
void cybiko_rtc_set(cybiko_emu_t *emu, const struct tm *tm);

/* Cybiko Classic ADC result, 10-bit, channels 0-3. Channels 1 and 2 feed
 * CyOS's battery gauge. No effect on the Xtreme. */
void cybiko_set_adc(cybiko_emu_t *emu, int channel, uint16_t value);

#define CYBIKO_LCD_WIDTH   160
#define CYBIKO_LCD_HEIGHT  100
#define CYBIKO_FPS         60
#define CYBIKO_CYCLES_PER_FRAME (CYBIKO_CLOCK_HZ / CYBIKO_FPS)
#define CYBIKO_KEYBOARD_COLUMNS 15  /* total matrix entries (HAL copies all) */
/* CYBIKO_CLOCK_HZ and CYBIKO_KEYBOARD_SCAN_COLS come from machine.h */

#endif
