/*
 * Hitachi HD66421 LCD controller emulation.
 *
 * 160x100 pixel display, 2-bit grayscale (4 shades).
 * VRAM is 4000 bytes (4 pixels per byte, 2 bits each).
 *
 * Memory-mapped at 0x100000 (register index) and 0x100001 (register data).
 */
#include "core/hd66421.h"
#include <string.h>

/* Register indices */
#define REG_CONTROL1   0x00
#define REG_CONTROL2   0x01
#define REG_X_ADDR     0x02
#define REG_Y_ADDR     0x03
#define REG_RAM        0x04
#define REG_START_LINE 0x05
#define REG_PALETTE1   0x0C
#define REG_PALETTE2   0x0D
#define REG_PALETTE3   0x0E
#define REG_PALETTE4   0x0F
#define REG_CONTRAST   0x10

/* Control register 1 bits */
#define R0_DISP 0x40
#define R0_REV  0x04
#define R0_RMW  0x80

/* Control register 2 bits */
#define R1_INC  0x02  /* 0=Y increment, 1=X increment */

static int clamp_int(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

void hd66421_init(hd66421_t *lcd) {
    memset(lcd, 0, sizeof(*lcd));
    /* Default palette: white, light gray, dark gray, black */
    lcd->regs[REG_PALETTE1] = 0;
    lcd->regs[REG_PALETTE2] = 10;
    lcd->regs[REG_PALETTE3] = 20;
    lcd->regs[REG_PALETTE4] = 31;
    lcd->regs[REG_CONTRAST] = 0;
    lcd->dirty = true;
}

uint8_t hd66421_read8(hd66421_t *lcd, int offset) {
    if (offset == 0) {
        return lcd->reg_index;
    } else {
        if (lcd->reg_index == REG_RAM) {
            int addr = lcd->y_addr * (HD66421_WIDTH / 4) + lcd->x_addr;
            if (addr >= 0 && addr < HD66421_VRAM_SIZE) {
                return lcd->vram[addr];
            }
            return 0;
        }
        return lcd->regs[lcd->reg_index];
    }
}

void hd66421_write8(hd66421_t *lcd, int offset, uint8_t value) {
    if (offset == 0) {
        lcd->reg_index = value & 0x1F;
    } else {
        /* Store register value */
        /* only the registers hd66421_render reads change the picture;
         * X and Y are set before every write */
        if (lcd->regs[lcd->reg_index] != value
            && (lcd->reg_index == REG_CONTROL1 || lcd->reg_index == REG_CONTRAST
                || (lcd->reg_index >= REG_PALETTE1 && lcd->reg_index <= REG_PALETTE4)))
            lcd->dirty = true;
        lcd->regs[lcd->reg_index] = value;

        switch (lcd->reg_index) {
        case REG_X_ADDR:
            lcd->x_addr = value;
            break;
        case REG_Y_ADDR:
            lcd->y_addr = value;
            break;
        case REG_RAM: {
            int addr = lcd->y_addr * (HD66421_WIDTH / 4) + lcd->x_addr;
            if (addr >= 0 && addr < HD66421_VRAM_SIZE && lcd->vram[addr] != value) {
                lcd->vram[addr] = value;
                lcd->dirty = true;
            }
            /* Auto-increment based on INC bit */
            if (lcd->regs[REG_CONTROL2] & R1_INC) {
                lcd->x_addr++;
            } else {
                lcd->y_addr++;
            }
            /* X overflow wraps to next Y (always checked, per MAME) */
            if (lcd->x_addr >= HD66421_WIDTH / 4) {
                lcd->x_addr = 0;
                lcd->y_addr++;
            }
            /* Y overflow wraps to 0 (always checked, per MAME) */
            if (lcd->y_addr >= HD66421_HEIGHT) {
                lcd->y_addr = 0;
            }
            break;
        }
        default:
            break;
        }
    }
}

const uint8_t *hd66421_render(hd66421_t *lcd) {
    if (!lcd->dirty)
        return lcd->frame_buffer;
    lcd->dirty = false;
    lcd->serial++;
    /* If display not on, fill with white */
    if (!(lcd->regs[REG_CONTROL1] & R0_DISP)) {
        memset(lcd->frame_buffer, 255, HD66421_WIDTH * HD66421_HEIGHT);
        return lcd->frame_buffer;
    }

    /* Compute palette: 4 grayscale levels mapped to 0-255 */
    int contrast = lcd->regs[REG_CONTRAST];
    int palette[4];
    palette[0] = clamp_int(31 - (lcd->regs[REG_PALETTE1] - contrast + 3), 0, 31) * 255 / 31;
    palette[1] = clamp_int(31 - (lcd->regs[REG_PALETTE2] - contrast + 3), 0, 31) * 255 / 31;
    palette[2] = clamp_int(31 - (lcd->regs[REG_PALETTE3] - contrast + 3), 0, 31) * 255 / 31;
    palette[3] = clamp_int(31 - (lcd->regs[REG_PALETTE4] - contrast + 3), 0, 31) * 255 / 31;

    /* Render VRAM bottom-to-top (matching MAME update_screen) */
    int x = 0;
    int y = HD66421_HEIGHT - 1;
    for (int i = 0; i < HD66421_VRAM_SIZE; i++) {
        uint8_t data = lcd->vram[i];
        int base = y * HD66421_WIDTH + x;
        /* 4 pixels per byte: bits 7-6, 5-4, 3-2, 1-0 */
        lcd->frame_buffer[base]     = (uint8_t)palette[(data >> 6) & 3];
        lcd->frame_buffer[base + 1] = (uint8_t)palette[(data >> 4) & 3];
        lcd->frame_buffer[base + 2] = (uint8_t)palette[(data >> 2) & 3];
        lcd->frame_buffer[base + 3] = (uint8_t)palette[(data >> 0) & 3];
        x += 4;
        if (x >= HD66421_WIDTH) {
            x = 0;
            y--;
        }
    }

    return lcd->frame_buffer;
}
