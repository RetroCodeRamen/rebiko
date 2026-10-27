/*
 * AT45DB041 SPI DataFlash. See at45db041.h.
 */
#include "core/at45db041.h"
#include <string.h>

#define CMD_STATUS  0x57
#define CMD_READ    0x52
#define CMD_COMPARE 0x60
#define CMD_PROGRAM 0x82

#define STATUS_READY     0x80
#define STATUS_MISMATCH  0x40
#define DEVICE_ID_BITS   0x20   /* AT45DB081 density code (4) in bits 5:3 */

void at45_init(at45db041_t *f, uint8_t *data) {
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->status = STATUS_READY;
    f->prog_pos = -1;
}

/* 11-bit page address and 9-bit byte address from the command bytes */
static int page_addr(const at45db041_t *f) {
    return ((f->cmd[1] & 0x1F) << 7) | ((f->cmd[2] & 0xFE) >> 1);
}

static int byte_addr(const at45db041_t *f) {
    return ((f->cmd[2] & 0x01) << 8) | f->cmd[3];
}

void at45_cs(at45db041_t *f, bool selected) {
    if (f->selected == selected)
        return;
    if (!selected) {
        /* CS high: a program command writes buffer 1 to its page */
        if (f->cmd_size >= 4 && f->cmd[0] == CMD_PROGRAM && f->data) {
            int page = page_addr(f);
            if (page < AT45_PAGES) {
                memcpy(f->data + page * AT45_PAGE_SIZE, f->buffer1, AT45_PAGE_SIZE);
                f->writes++;
                f->dirty[page >> 3] |= (uint8_t)(1u << (page & 7));
            }
        }
        f->cmd_size = 0;
        f->output = false;
        f->io_page = NULL;
        f->io_pos = 0;
        f->prog_pos = -1;
    }
    f->selected = selected;
}

uint8_t at45_transfer(at45db041_t *f, uint8_t mosi) {
    if (!f->selected)
        return 0xFF;   /* idle bus, MISO pulled high */

    if (f->output) {
        if (!f->io_page)
            return f->status;
        uint8_t v = f->io_page[f->io_pos];
        f->io_pos = (f->io_pos + 1) % AT45_PAGE_SIZE;   /* wraps within the page */
        return v;
    }

    if (f->cmd_size < 8) {
        f->cmd[f->cmd_size++] = mosi;
        switch (f->cmd[0]) {
        case CMD_STATUS:
            if (f->cmd_size == 1) {
                f->status = (f->status & 0xC7) | DEVICE_ID_BITS;
                f->output = true;
                f->io_page = NULL;
                f->cmd_size = 8;
            }
            break;
        case CMD_READ:
            /* 4 command bytes and 4 don't-care bytes, then page data */
            if (f->cmd_size == 8 && f->data) {
                int page = page_addr(f);
                if (page >= AT45_PAGES) page = AT45_PAGES - 1;
                f->io_page = f->data + page * AT45_PAGE_SIZE;
                f->io_pos = byte_addr(f) % AT45_PAGE_SIZE;
                f->output = true;
            }
            break;
        case CMD_COMPARE:
            if (f->cmd_size == 4 && f->data) {
                int page = page_addr(f);
                bool match = page < AT45_PAGES &&
                    memcmp(f->data + page * AT45_PAGE_SIZE, f->buffer1, AT45_PAGE_SIZE) == 0;
                if (match) f->status &= ~STATUS_MISMATCH;
                else       f->status |= STATUS_MISMATCH;
                f->cmd_size = 8;
            }
            break;
        case CMD_PROGRAM:
            if (f->cmd_size == 4) {
                memset(f->buffer1, 0xFF, AT45_PAGE_SIZE);
                f->prog_pos = byte_addr(f) % AT45_PAGE_SIZE;
                f->cmd_size = 8;   /* data follows; stay in input mode */
            }
            break;
        }
    } else if (f->prog_pos >= 0) {
        f->buffer1[f->prog_pos] = mosi;
        f->prog_pos = (f->prog_pos + 1) % AT45_PAGE_SIZE;
    }
    return 0;
}

/* A page is an 8-byte header (CRC-32 of the 254 bytes after it, big-endian;
 * a write count; a check word over the first six header bytes), 256 bytes
 * of block and a 2-byte trailer. An empty block has bit 7 of its first
 * byte clear. Checked against every empty page of flash_v1246.bin. */
static uint32_t at45_crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void at45_format_free(uint8_t *data, int first) {
    uint8_t page[AT45_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    page[8] = 0x7F;
    page[4] = page[5] = 0;
    uint32_t crc = at45_crc32(page + 8, AT45_PAGE_SIZE - 10);
    page[0] = (uint8_t)(crc >> 24); page[1] = (uint8_t)(crc >> 16);
    page[2] = (uint8_t)(crc >> 8);  page[3] = (uint8_t)crc;
    unsigned v = 0xAF17u ^ ((unsigned)page[0] << 8 | page[1])
               ^ ((unsigned)page[2] << 8 | page[3]) ^ ((unsigned)page[4] << 8 | page[5]);
    page[6] = (uint8_t)v;               /* byte-swapped */
    page[7] = (uint8_t)(v >> 8);
    for (int p = first; p < AT45_PAGES; p++)
        memcpy(data + (size_t)p * AT45_PAGE_SIZE, page, AT45_PAGE_SIZE);
}
