#ifndef TDECK_KEYMAP_H
#define TDECK_KEYMAP_H

/*
 * T-Deck keyboard and trackball positions in the Cybiko matrix.
 * Xtreme (default): column 9 is P / period / semicolon; columns 10-14 are
 * not scanned. Classic (CYBIKO_MACHINE_V1): 9 columns of 8 bits with
 * dedicated number keys, from the Java emulator's V1 matrix (MAME A.0-A.8).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CYBIKO_MACHINE_V1

#define TDECK_SHIFT_COL   0
#define TDECK_SHIFT_MASK  0x0080
#define TDECK_FN_COL      1
#define TDECK_FN_MASK     0x0080

#define TDECK_PAD_UP_COL      1
#define TDECK_PAD_UP_MASK     0x0002
#define TDECK_PAD_RIGHT_COL   4
#define TDECK_PAD_RIGHT_MASK  0x0001
#define TDECK_PAD_DOWN_COL    4
#define TDECK_PAD_DOWN_MASK   0x0002
#define TDECK_PAD_LEFT_COL    0
#define TDECK_PAD_LEFT_MASK   0x0008
#define TDECK_ENTER_COL       5
#define TDECK_ENTER_MASK      0x0004
#define TDECK_SELECT_COL      4
#define TDECK_SELECT_MASK     0x0004

#else

#define TDECK_SHIFT_COL   8
#define TDECK_SHIFT_MASK  0x8000
#define TDECK_FN_COL      7
#define TDECK_FN_MASK     0x8000

#define TDECK_PAD_UP_COL      6
#define TDECK_PAD_UP_MASK     0x0800
#define TDECK_PAD_RIGHT_COL   6
#define TDECK_PAD_RIGHT_MASK  0x1000
#define TDECK_PAD_DOWN_COL    6
#define TDECK_PAD_DOWN_MASK   0x2000
#define TDECK_PAD_LEFT_COL    6
#define TDECK_PAD_LEFT_MASK   0x4000
#define TDECK_ENTER_COL       4
#define TDECK_ENTER_MASK      0x0008
#define TDECK_SELECT_COL      4
#define TDECK_SELECT_MASK     0x0010

#endif

typedef struct {
    bool found;
    bool needs_shift;
    bool needs_fn;
    int col;
    uint16_t mask;
    int fn_col;
    uint16_t fn_mask;
} tdeck_key_event_t;

/* ASCII from the T-Deck I2C keyboard. Upper case requests Shift. */
bool tdeck_map_ascii(char c, tdeck_key_event_t *ev);

/* Cybiko keys the T-Deck has no key for, reached through the $ command
 * key and the trackball. */
typedef enum {
    TDECK_KEY_ESC, TDECK_KEY_HELP, TDECK_KEY_SELECT, TDECK_KEY_INSERT,
    TDECK_KEY_DELETE, TDECK_KEY_TAB, TDECK_KEY_BKSP,
    TDECK_KEY_F1, TDECK_KEY_F2, TDECK_KEY_F3, TDECK_KEY_F4,
    TDECK_KEY_F5, TDECK_KEY_F6, TDECK_KEY_F7,
    TDECK_KEY_COUNT
} tdeck_special_key_t;

/* Matrix position of a special key. False if this machine lacks it. */
bool tdeck_map_special(tdeck_special_key_t key, int *col, uint16_t *mask);

#ifdef __cplusplus
}
#endif

#endif
