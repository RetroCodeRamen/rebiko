/*
 * Cybiko emulator orchestrator.
 *
 * Ties together CPU, address bus, LCD, timers, RTC, speaker, and keyboard
 * into a single runnable emulator driven by a platform HAL.
 */
#include "emulator.h"
#include "h8s_cpu.h"
#include "address_bus.h"
#include "hd66421.h"
#include "timer8.h"
#include "timer16.h"
#include "rtc.h"
#include "speaker.h"
#include "keyboard.h"
#include "cfs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * On ESP32 with PSRAM, large calloc goes to PSRAM where every access
 * has 10-20x the latency of internal SRAM.  Force the emulator struct
 * (CPU registers, timers, bus state) into internal SRAM for speed.
 */
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "xtensa/core-macros.h"
static void *emu_calloc_internal(size_t n, size_t sz) {
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = calloc(n, sz);  /* fallback if internal is full */
    return p;
}
#else
#define emu_calloc_internal(n, sz) calloc(n, sz)
#endif

struct cybiko_emu {
    cybiko_hal_t hal;

    h8s_cpu_t     cpu;
    address_bus_t bus;
    hd66421_t     lcd;
    timer8_t      timer8[2];
    timer16_t     timer16[6];
    rtc_t         rtc;
    speaker_t     speaker;
    keyboard_t    keyboard;

#if CYBIKO_HAS_SPI_FLASH
    uint8_t *spi_flash;        /* AT45_SIZE, the Classic's CyOS and files */
    uint16_t prev_keys[CYBIKO_KEYBOARD_COLUMNS];
    int      key_irq_retries;  /* extra IRQ2 if CyOS missed the edge */
#endif

    bool     running;
    uint64_t total_steps;
    uint64_t sleep_cycles;
    uint32_t frame_count;

#ifdef CYBIKO_PROFILE
    uint32_t pc_samples[CYBIKO_PC_SAMPLES];
    int      pc_sample_count;
    uint32_t pc_sample_tick;
#endif
};

/* Timer16 channel 1 TIOCB1 output compare → speaker (Port 1 bit 3) */
static void timer16_ch1_tiocb_cb(void *ctx, int level) {
    cybiko_emu_t *emu = ctx;
    speaker_set_level(&emu->speaker, level);
}

static void init_peripherals(cybiko_emu_t *emu) {
    hd66421_init(&emu->lcd);

    timer8_init(&emu->timer8[0], 0, &emu->cpu);
    timer8_init(&emu->timer8[1], 1, &emu->cpu);

    /* Timer16 channels: (channel, tgrCount, baseVector, cpu) */
    timer16_init(&emu->timer16[0], 0, 4, 32, &emu->cpu);
    timer16_init(&emu->timer16[1], 1, 2, 40, &emu->cpu);
    timer16_init(&emu->timer16[2], 2, 2, 44, &emu->cpu);
    timer16_init(&emu->timer16[3], 3, 4, 48, &emu->cpu);
    timer16_init(&emu->timer16[4], 4, 2, 56, &emu->cpu);
    timer16_init(&emu->timer16[5], 5, 2, 60, &emu->cpu);

    emu->timer16[1].output_b_cb = timer16_ch1_tiocb_cb;
    emu->timer16[1].output_b_ctx = emu;

    speaker_init(&emu->speaker, CYBIKO_CLOCK_HZ);
    keyboard_init(&emu->keyboard, CYBIKO_KEYBOARD_SCAN_COLS);
}

/* ---------- lifecycle ---------- */

cybiko_emu_t *cybiko_create(const cybiko_hal_t *hal) {
    cybiko_emu_t *emu = emu_calloc_internal(1, sizeof(cybiko_emu_t));
    if (!emu) return NULL;

    /* Copy the HAL callbacks */
    emu->hal = *hal;

    /* Initialize the bus itself (must come before memory_init - zeroes struct) */
    bus_init(&emu->bus);

    /* Initialize memory regions in the bus */
    memory_init(&emu->bus.boot_rom,     CYBIKO_BOOT_ROM_SIZE, false);
    memory_init(&emu->bus.external_ram, CYBIKO_EXT_RAM_SIZE,  true);
#if CYBIKO_HAS_FLASH_MAP
    memory_init(&emu->bus.flash_rom,    CYBIKO_FLASH_SIZE,    false);
#endif
    memory_init(&emu->bus.on_chip_ram,  CYBIKO_ON_CHIP_SIZE,  true);
#if CYBIKO_HAS_SPI_FLASH
    emu->spi_flash = malloc(AT45_SIZE);
    if (!emu->spi_flash) { free(emu); return NULL; }
    memset(emu->spi_flash, 0xFF, AT45_SIZE);
    bus_load_spi_flash(&emu->bus, emu->spi_flash);
#endif

#if defined(ESP_PLATFORM)
    {
        void *fast_on = emu_calloc_internal(1, CYBIKO_ON_CHIP_SIZE);
        if (fast_on) {
            free(emu->bus.on_chip_ram.data);
            emu->bus.on_chip_ram.data = fast_on;
        }
        void *fast_boot = emu_calloc_internal(1, CYBIKO_BOOT_ROM_SIZE);
        if (fast_boot) {
            free(emu->bus.boot_rom.data);
            emu->bus.boot_rom.data = fast_boot;
        }
    }
#endif

    /* Wire peripherals into bus */
    emu->bus.lcd       = &emu->lcd;
    emu->bus.timer8[0] = &emu->timer8[0];
    emu->bus.timer8[1] = &emu->timer8[1];
    /* Channels past CYBIKO_TIMER16_CHANNELS stay unwired: the bus routes
     * nothing to them and TSTR cannot start them. */
    for (int i = 0; i < CYBIKO_TIMER16_CHANNELS; i++) {
        emu->bus.timer16[i] = &emu->timer16[i];
    }
    emu->bus.rtc      = &emu->rtc;
    emu->bus.speaker  = &emu->speaker;
    emu->bus.keyboard = &emu->keyboard;

    init_peripherals(emu);

    cybiko_rtc_init(&emu->rtc);

    /* Initialize CPU (needs bus already wired) */
    h8s_cpu_init(&emu->cpu, &emu->bus);

    /* Wire CPU back into bus (for interrupt delivery, etc.) */
    emu->bus.cpu = &emu->cpu;

    emu->running = true;

    return emu;
}

void cybiko_destroy(cybiko_emu_t *emu) {
    if (!emu) return;
    memory_free(&emu->bus.boot_rom);
    memory_free(&emu->bus.external_ram);
    memory_free(&emu->bus.flash_rom);
    memory_free(&emu->bus.on_chip_ram);
#if CYBIKO_HAS_SPI_FLASH
    free(emu->spi_flash);
#endif
    free(emu);
}

/* ---------- ROM / NVRAM loading ---------- */

bool cybiko_load_boot_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len) {
    memory_load(&emu->bus.boot_rom, data, len, 0);
    return true;
}

bool cybiko_load_flash_rom(cybiko_emu_t *emu, const uint8_t *data, size_t len) {
#if CYBIKO_HAS_SPI_FLASH
    /* V1: the image is the AT45DB041 contents (flash_v1246.bin, 540672 bytes) */
    if (len > AT45_SIZE) len = AT45_SIZE;
    memcpy(emu->spi_flash, data, len);
    bus_load_spi_flash(&emu->bus, emu->spi_flash);
#else
    memory_load(&emu->bus.flash_rom, data, len, 0);
#endif
    return true;
}

bool cybiko_load_nvram(cybiko_emu_t *emu, const uint8_t *data, size_t len) {
    memory_load(&emu->bus.external_ram, data, len, 0);
    return true;
}

/* ---------- reset ---------- */

void cybiko_reset(cybiko_emu_t *emu) {
    h8s_cpu_reset(&emu->cpu);
}

void cybiko_reboot(cybiko_emu_t *emu) {
    if (!emu) return;
    if (emu->bus.on_chip_ram.data)
        memset(emu->bus.on_chip_ram.data, 0, emu->bus.on_chip_ram.size);
    if (emu->bus.external_ram.data)
        memset(emu->bus.external_ram.data, 0, emu->bus.external_ram.size);
    memset(emu->bus.sci_output_pos, 0, sizeof emu->bus.sci_output_pos);
#if CYBIKO_HAS_SPI_FLASH
    memset(emu->prev_keys, 0, sizeof emu->prev_keys);
    emu->key_irq_retries = 0;
    if (emu->bus.sci0.bound) {
        emu->bus.sci0.scr = 0;
        emu->bus.sci0.tdre = true;
        emu->bus.sci0.rdrf = false;
        emu->bus.sci0.rdr = 0;
        emu->bus.sci0.txi_delay = 0;
        emu->bus.sci0.async_countdown = 0;
        emu->bus.sci0.radio_up = false;
        emu->bus.sci0.tx_frames = 0;
        emu->bus.sci0.rx_injected = 0;
    }
#endif
    init_peripherals(emu);
    h8s_cpu_reset(&emu->cpu);
    emu->running = true;
    emu->total_steps = 0;
    emu->sleep_cycles = 0;
    emu->frame_count = 0;
}

/* ---------- main frame loop ---------- */

/*
 * EMU_TIMER_STRIDE: Number of timer ticks per CPU step.
 * Real H8S instructions average 2-3 clock cycles. By running the CPU
 * every N timer ticks instead of every tick, we correctly model the
 * instruction-to-clock ratio and reduce CPU decode overhead on slow hosts.
 * Default 1 = original behavior (1 instruction per clock, matches Java/web).
 */
#ifndef EMU_TIMER_STRIDE
#define EMU_TIMER_STRIDE 1
#endif

/* Batch size for SLEEP fast-forward (cycles). Larger = faster idle,
 * but timer events may be coarser-grained. 256 is well within the
 * typical prescaler range and keeps audio transitions accurate enough. */
#ifndef EMU_SLEEP_BATCH
#define EMU_SLEEP_BATCH 256
#endif

/*
 * Advance all active timers by 'cycles' master clock ticks.
 * Uses batch arithmetic: add to prescale_counter, then drain events
 * via while loop. This is equivalent to calling prescale_tick()
 * one-at-a-time but processes N cycles in one call.
 */
static void tick_timers_batch(cybiko_emu_t *emu, int cycles, int frame_cycle) {
    /* Timer8[0] */
    if (emu->timer8[0].cached_divisor > 0) {
        emu->timer8[0].prescale_counter += cycles;
        while (emu->timer8[0].prescale_counter >= emu->timer8[0].cached_divisor) {
            emu->timer8[0].prescale_counter -= emu->timer8[0].cached_divisor;
            timer8_event(&emu->timer8[0]);
            if (!emu->timer8[0].cached_divisor) break;
        }
    }
    /* Timer8[1] */
    if (emu->timer8[1].cached_divisor > 0) {
        emu->timer8[1].prescale_counter += cycles;
        while (emu->timer8[1].prescale_counter >= emu->timer8[1].cached_divisor) {
            emu->timer8[1].prescale_counter -= emu->timer8[1].cached_divisor;
            timer8_event(&emu->timer8[1]);
            if (!emu->timer8[1].cached_divisor) break;
        }
    }
    /*
     * Timer16 channels. timer16_event never changes the divisor, so the
     * event count for the batch is known up front, and timer16_advance
     * only runs the full event on ticks that can match or overflow.
     * Channel 1 at phi/1 was ~5 events per guest instruction before this.
     * The speaker channel stamps every transition with this batch's
     * frame_cycle, as the per-event loop did.
     */
    emu->speaker.frame_cycle = frame_cycle;
    for (int i = 0; i < 6; i++) {
        timer16_t *t = &emu->timer16[i];
        if (t->cached_divisor > 0) {
            t->prescale_counter += cycles;
            if (t->prescale_counter >= t->cached_divisor) {
                uint32_t n = (uint32_t)(t->prescale_counter / t->cached_divisor);
                t->prescale_counter -= (int)n * t->cached_divisor;
                timer16_advance(t, n);
            }
        }
    }
}

#ifdef CYBIKO_SPLIT_PROFILE
/* Host cycles in the active path: [0] timers, [1] CPU, [2] instructions */
uint32_t cybiko_split[3];
#endif

void cybiko_run_frame(cybiko_emu_t *emu) {
    /* Poll keyboard state from the HAL */
    if (emu->hal.keyboard_poll) {
        emu->hal.keyboard_poll(emu->hal.ctx,
                               emu->keyboard.columns,
                               CYBIKO_KEYBOARD_COLUMNS);
    }
#ifdef CYBIKO_KEY_IRQ_VECTOR
    /* V1 scans the matrix from IRQ2. Radio SCAN can eat the press-edge
     * frame; without a retry CyOS never looks again and the key is lost. */
    {
        bool edge = false, down = false;
        for (int i = 0; i < CYBIKO_KEYBOARD_SCAN_COLS; i++) {
            if (emu->keyboard.columns[i] & ~emu->prev_keys[i])
                edge = true;
            if (emu->keyboard.columns[i])
                down = true;
            emu->prev_keys[i] = emu->keyboard.columns[i];
        }
        if (edge) {
            h8s_cpu_request_interrupt(&emu->cpu, CYBIKO_KEY_IRQ_VECTOR);
            emu->key_irq_retries = 3;
        } else if (down && emu->key_irq_retries > 0) {
            h8s_cpu_request_interrupt(&emu->cpu, CYBIKO_KEY_IRQ_VECTOR);
            emu->key_irq_retries--;
        } else if (!down) {
            emu->key_irq_retries = 0;
        }
    }
#endif
    /* Begin speaker frame (record start level, reset transitions) */
    speaker_begin_frame(&emu->speaker);

    int cycle = 0;

    while (cycle < CYBIKO_CYCLES_PER_FRAME) {
        /*
         * SLEEP fast-forward: when the CPU is halted (SLEEP instruction),
         * timers keep running but no instructions execute. Instead of
         * iterating one cycle at a time, advance timers in large batches
         * until an interrupt fires and wakes the CPU.
         * Post-boot this skips 60-90% of cycles (CyOS idle loop).
         */
        if (emu->cpu.halted) {
            while (cycle < CYBIKO_CYCLES_PER_FRAME && emu->cpu.halted) {
                if (emu->cpu.pending_irq_count > 0) {
                    /* Interrupt pending — wake CPU via step */
                    h8s_cpu_step(&emu->cpu);
                    break;
                }
                int remaining = CYBIKO_CYCLES_PER_FRAME - cycle;
                int batch = (remaining > EMU_SLEEP_BATCH) ? EMU_SLEEP_BATCH : remaining;
                tick_timers_batch(emu, batch, cycle);
                bus_tick_sci2(&emu->bus, batch);
#if CYBIKO_HAS_SPI_FLASH
                bus_tick_dma_completion(&emu->bus);
#endif
                cycle += batch;
                emu->sleep_cycles += (uint64_t)batch;
            }
            continue;
        }

        /*
         * Active CPU path: advance timers by 8*STRIDE cycles, then run
         * eight CPU steps. Same instruction-to-timer ratio as the older
         * pair of steps; fewer trips through the timer walk.
         */
#ifdef CYBIKO_SPLIT_PROFILE
        uint32_t c0 = XTHAL_GET_CCOUNT();
#endif
        tick_timers_batch(emu, EMU_TIMER_STRIDE * 8, cycle);
        bus_tick_sci2(&emu->bus, EMU_TIMER_STRIDE * 8);
        cycle += EMU_TIMER_STRIDE * 8;
#ifdef CYBIKO_SPLIT_PROFILE
        uint32_t c1 = XTHAL_GET_CCOUNT();
#endif
        h8s_cpu_run(&emu->cpu, 8);
#ifdef CYBIKO_SPLIT_PROFILE
        uint32_t c2 = XTHAL_GET_CCOUNT();
        cybiko_split[0] += c1 - c0;
        cybiko_split[1] += c2 - c1;
        cybiko_split[2] += 8;
#endif
#if CYBIKO_HAS_SPI_FLASH
        bus_tick_dma_completion(&emu->bus);
#endif
#ifdef CYBIKO_PROFILE
        if ((++emu->pc_sample_tick & 63) == 0 &&
            emu->pc_sample_count < CYBIKO_PC_SAMPLES)
            emu->pc_samples[emu->pc_sample_count++] = emu->cpu.pc;
#endif
    }

    emu->total_steps += CYBIKO_CYCLES_PER_FRAME;

    /* Render the frame via HAL */
    if (emu->hal.render_frame) {
        const uint8_t *pixels = hd66421_render(&emu->lcd);
        emu->hal.render_frame(emu->hal.ctx, pixels,
                              CYBIKO_LCD_WIDTH, CYBIKO_LCD_HEIGHT);
    }

    /* Generate audio samples for this frame */
    if (emu->hal.audio_output) {
        uint8_t audio_buf[8192];
        int samples = speaker_generate_samples(&emu->speaker,
                                               CYBIKO_CYCLES_PER_FRAME,
                                               audio_buf, sizeof(audio_buf));
        if (samples > 0) {
            emu->hal.audio_output(emu->hal.ctx, audio_buf, samples);
        }
    }

    /* Tick RTC once per frame */
    bus_tick_rtc(&emu->bus);

#if CYBIKO_HAS_SPI_FLASH
    if (emu->bus.sci0.bound)
        cybiko_sci0_frame(&emu->bus.sci0);
#endif

    /* Drain serial output buffers to the HAL */
    if (emu->hal.serial_output) {
        for (int ch = 0; ch < 3; ch++) {
            for (int i = 0; i < emu->bus.sci_output_pos[ch]; i++) {
                emu->hal.serial_output(emu->hal.ctx,
                                       (uint8_t)emu->bus.sci_output[ch][i]);
            }
            emu->bus.sci_output_pos[ch] = 0;
        }
    }

    emu->frame_count++;
}

/* ---------- queries ---------- */

bool cybiko_is_running(const cybiko_emu_t *emu) {
    return emu->running && !emu->cpu.halted;
}

void cybiko_attach_radio(cybiko_emu_t *emu, struct cybiko_radio_coproc *coproc) {
    cybiko_attach_radio_id(emu, coproc, 0);
}

void cybiko_attach_radio_id(cybiko_emu_t *emu, struct cybiko_radio_coproc *coproc,
                            uint32_t radio_id) {
    if (!emu) return;
    bus_attach_radio(&emu->bus, coproc, radio_id);
}

bool cybiko_v1_radio_status(const cybiko_emu_t *emu, cybiko_v1_radio_status_t *out) {
#if CYBIKO_HAS_SPI_FLASH
    const cybiko_sci0_v1_t *s = &emu->bus.sci0;
    out->radio_up = s->radio_up;
    out->tx_frames = s->tx_frames;
    out->rx_injected = s->rx_injected;
    return s->bound;
#else
    (void)emu; (void)out;
    return false;
#endif
}

void cybiko_get_stats(const cybiko_emu_t *emu, cybiko_stats_t *out) {
    out->instructions = emu->cpu.cycle_count;
    out->sleep_cycles = emu->sleep_cycles;
    out->frames = emu->frame_count;
}

int cybiko_take_pc_samples(cybiko_emu_t *emu, uint32_t *out, int max) {
#ifdef CYBIKO_PROFILE
    int n = emu->pc_sample_count < max ? emu->pc_sample_count : max;
    memcpy(out, emu->pc_samples, (size_t)n * sizeof(uint32_t));
    emu->pc_sample_count = 0;
    return n;
#else
    (void)emu; (void)out; (void)max;
    return 0;
#endif
}

const uint8_t *cybiko_get_nvram(const cybiko_emu_t *emu, size_t *len) {
    if (len) *len = emu->bus.external_ram.size;
    return memory_raw((memory_t *)&emu->bus.external_ram);
}

uint32_t cybiko_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool cybiko_patch_cyid_image(uint8_t *flash, size_t len, uint32_t cyid) {
    const size_t block = 0x7F800;
    const size_t id_at = 0x7F818;
    if (!flash || id_at + 4 > len || block + 2048 > len)
        return false;
    flash[id_at]     = (uint8_t)(cyid >> 24);
    flash[id_at + 1] = (uint8_t)(cyid >> 16);
    flash[id_at + 2] = (uint8_t)(cyid >> 8);
    flash[id_at + 3] = (uint8_t)cyid;
    uint32_t sum = cybiko_crc32(flash + block, 2044);
    flash[block + 2044] = (uint8_t)(sum >> 24);
    flash[block + 2045] = (uint8_t)(sum >> 16);
    flash[block + 2046] = (uint8_t)(sum >> 8);
    flash[block + 2047] = (uint8_t)sum;
    return true;
}

const uint8_t *cybiko_get_vram(const cybiko_emu_t *emu) {
    return emu->lcd.vram;
}

uint32_t cybiko_get_pc(const cybiko_emu_t *emu) {
    return emu->cpu.pc;
}

bool cybiko_lcd_on(const cybiko_emu_t *emu) {
    return (emu->lcd.regs[0] & 0x40) != 0;   /* control 1, DISP */
}

bool cybiko_is_halted(const cybiko_emu_t *emu) {
    return emu->cpu.halted;
}

uint8_t cybiko_get_ccr(const cybiko_emu_t *emu) {
    return emu->cpu.ccr;
}

uint32_t cybiko_get_reg(const cybiko_emu_t *emu, int n) {
    return (n >= 0 && n < 8) ? emu->cpu.er[n] : 0;
}

uint8_t cybiko_peek8(cybiko_emu_t *emu, uint32_t address) {
    return bus_read8(&emu->bus, address);
}

void cybiko_poke8(cybiko_emu_t *emu, uint32_t address, uint8_t value) {
    bus_write8(&emu->bus, address, value);
}

void cybiko_debug_run(cybiko_emu_t *emu, uint32_t pc, int n) {
    emu->cpu.pc = pc & 0xFFFFFF;
    emu->cpu.halted = false;
    h8s_cpu_run(&emu->cpu, n);
}

const uint8_t *cybiko_get_spi_flash(const cybiko_emu_t *emu, size_t *len, uint32_t *writes) {
#if CYBIKO_HAS_SPI_FLASH
    if (len) *len = AT45_SIZE;
    if (writes) *writes = emu->bus.spi_flash.writes;
    return emu->spi_flash;
#else
    (void)emu;
    if (len) *len = 0;
    if (writes) *writes = 0;
    return NULL;
#endif
}

void cybiko_set_adc(cybiko_emu_t *emu, int channel, uint16_t value) {
#if CYBIKO_HAS_SPI_FLASH
    if (channel >= 0 && channel < 4)
        emu->bus.adc[channel] = value & 0x3FF;
#else
    (void)emu; (void)channel; (void)value;
#endif
}

bool cybiko_patch_cyid(cybiko_emu_t *emu, uint32_t cyid) {
    if (!emu) return false;
    return cybiko_patch_cyid_image(memory_raw(&emu->bus.flash_rom),
                                   emu->bus.flash_rom.size, cyid);
}
