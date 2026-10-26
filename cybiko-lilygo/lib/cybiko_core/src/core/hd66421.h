#ifndef CYBIKO_HD66421_H
#define CYBIKO_HD66421_H
#include "types.h"

#define HD66421_WIDTH  160
#define HD66421_HEIGHT 100
#define HD66421_VRAM_SIZE 4000

typedef struct {
    uint8_t regs[32];
    uint8_t vram[HD66421_VRAM_SIZE];
    uint8_t frame_buffer[HD66421_WIDTH * HD66421_HEIGHT];
    uint8_t reg_index;
    uint8_t x_addr;
    uint8_t y_addr;
    /* The picture may differ from frame_buffer. Set only when a write
     * changes VRAM or a register: CyOS rewrites the same bytes hundreds of
     * times a frame while the picture changes a few times a second. */
    bool dirty;
    uint32_t serial;    /* counts rebuilds of frame_buffer */
} hd66421_t;

void    hd66421_init(hd66421_t *lcd);
uint8_t hd66421_read8(hd66421_t *lcd, int offset);
void    hd66421_write8(hd66421_t *lcd, int offset, uint8_t value);
const uint8_t *hd66421_render(hd66421_t *lcd);

#endif
