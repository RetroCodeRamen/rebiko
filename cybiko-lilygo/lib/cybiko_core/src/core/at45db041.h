#ifndef CYBIKO_AT45DB041_H
#define CYBIKO_AT45DB041_H

/*
 * AT45DB041 SPI DataFlash, the Cybiko Classic's CyOS and file storage.
 * Port of AT45DB041Flash.java (MIT, Copyright 2026 Dan Berkowitz), which
 * follows MAME at45dbxx.cpp. The CPU reaches it through SCI1 in clocked
 * mode with chip select on Port 3 bit 4.
 *
 * Commands: 0x57 status, 0x52 main memory page read, 0x60 compare page
 * with buffer 1, 0x82 program a page through buffer 1 (committed when CS
 * goes high).
 *
 * The Classic shipped with the 512 KB AT45DB041; this is its 1 MB sibling,
 * the AT45DB081: the same 264-byte pages, twice as many, one more page
 * address bit, density code 4. CyOS 1.2.46 reads the code ("Detected
 * flash device model 4 [4091 blocks]") and uses all of it, so a full
 * Cylandia (166 KB) fits beside the stock files. A 512 KB image loads
 * into the first half; at45_format_free() makes the rest empty blocks.
 */

#include "types.h"

#define AT45_PAGES     4096
#define AT45_PAGE_SIZE 264
#define AT45_SIZE      (AT45_PAGES * AT45_PAGE_SIZE)   /* 1081344 */
#define AT45_DB041_SIZE 540672                         /* the factory chip */

typedef struct {
    uint8_t *data;                    /* AT45_SIZE bytes, owned by the emulator */
    uint8_t  buffer1[AT45_PAGE_SIZE];
    uint8_t  cmd[8];
    int      cmd_size;
    bool     output;                  /* serial output mode */
    bool     selected;
    uint8_t  status;
    /* Output source: a page (read) or the status byte */
    const uint8_t *io_page;           /* page start, NULL = status byte */
    int      io_pos;
    /* Program: write position in buffer1, -1 = not programming */
    int      prog_pos;
    uint32_t writes;                  /* pages committed since load, for save */
} at45db041_t;

void    at45_init(at45db041_t *f, uint8_t *data);
/* CS active low: selected = true when the pin is driven low. */
void    at45_cs(at45db041_t *f, bool selected);
/* One full-duplex byte. Returns MISO. */
uint8_t at45_transfer(at45db041_t *f, uint8_t mosi);

/* Write pages first..AT45_PAGES-1 as empty file blocks, checksummed as
 * CyOS checks them */
void at45_format_free(uint8_t *data, int first);

#endif
