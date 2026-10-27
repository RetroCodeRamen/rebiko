#include "tdeck_keymap.h"

typedef struct {
    char ascii;
    int col;
    uint16_t mask;
    bool needs_fn;
    int fn_col;
    uint16_t fn_mask;
    bool shift;         /* Shift and this key */
} tdeck_ascii_key_t;

#ifdef CYBIKO_MACHINE_V1
/* Classic: every character here is one key, or Shift and one key; no Fn. Backspace is the
 * BkSp key (A.6 bit 2), which erases before the cursor; Del (A.0 bit 2)
 * deletes forward and does nothing at the end of a line. The Java
 * emulator sent Backspace to Del. */
static const tdeck_ascii_key_t ASCII_MAP[] = {
    { 'q', 0, 0x10, false, 0, 0 }, { 'a', 0, 0x20, false, 0, 0 },
    { '`', 0, 0x40, false, 0, 0 }, { '\b', 6, 0x04, false, 0, 0 },
    { 0x1B, 0, 0x02, false, 0, 0 },
    { '2', 1, 0x08, false, 0, 0 }, { 'w', 1, 0x10, false, 0, 0 },
    { 's', 1, 0x20, false, 0, 0 }, { 'z', 1, 0x40, false, 0, 0 },
    { ' ', 2, 0x04, false, 0, 0 }, { '3', 2, 0x08, false, 0, 0 },
    { 'e', 2, 0x10, false, 0, 0 }, { 'd', 2, 0x20, false, 0, 0 },
    { 'x', 2, 0x40, false, 0, 0 },
    { '1', 3, 0x02, false, 0, 0 }, { '\t', 3, 0x04, false, 0, 0 },
    { '4', 3, 0x08, false, 0, 0 }, { 'r', 3, 0x10, false, 0, 0 },
    { 'f', 3, 0x20, false, 0, 0 }, { 'c', 3, 0x40, false, 0, 0 },
    { '[', 3, 0x80, false, 0, 0 },
    { '5', 4, 0x08, false, 0, 0 }, { 't', 4, 0x10, false, 0, 0 },
    { 'g', 4, 0x20, false, 0, 0 }, { 'v', 4, 0x40, false, 0, 0 },
    { ']', 4, 0x80, false, 0, 0 },
    { ';', 5, 0x02, false, 0, 0 }, { '\n', 5, 0x04, false, 0, 0 },
    { '\r', 5, 0x04, false, 0, 0 }, { '6', 5, 0x08, false, 0, 0 },
    { 'y', 5, 0x10, false, 0, 0 }, { 'h', 5, 0x20, false, 0, 0 },
    { 'b', 5, 0x40, false, 0, 0 }, { '\\', 5, 0x80, false, 0, 0 },
    { '/', 6, 0x02, false, 0, 0 }, { '7', 6, 0x08, false, 0, 0 },
    { 'u', 6, 0x10, false, 0, 0 }, { 'j', 6, 0x20, false, 0, 0 },
    { 'n', 6, 0x40, false, 0, 0 },
    { '-', 7, 0x01, false, 0, 0 }, { '.', 7, 0x02, false, 0, 0 },
    { '0', 7, 0x04, false, 0, 0 }, { '8', 7, 0x08, false, 0, 0 },
    { 'i', 7, 0x10, false, 0, 0 }, { 'k', 7, 0x20, false, 0, 0 },
    { 'm', 7, 0x40, false, 0, 0 },
    { '\'', 8, 0x01, false, 0, 0 }, { '=', 8, 0x02, false, 0, 0 },
    { '9', 8, 0x04, false, 0, 0 }, { 'p', 8, 0x08, false, 0, 0 },
    { 'o', 8, 0x10, false, 0, 0 }, { 'l', 8, 0x20, false, 0, 0 },
    { ',', 8, 0x40, false, 0, 0 },
    /* Shift layer, as on a US PC keyboard (T-Deck $ then $ is Shift+4) */
    { '!', 3, 0x02, false, 0, 0, true }, { '@', 1, 0x08, false, 0, 0, true },
    { '#', 2, 0x08, false, 0, 0, true }, { '$', 3, 0x08, false, 0, 0, true },
    { '%', 4, 0x08, false, 0, 0, true }, { '^', 5, 0x08, false, 0, 0, true },
    { '&', 6, 0x08, false, 0, 0, true }, { '*', 7, 0x08, false, 0, 0, true },
    { '(', 8, 0x04, false, 0, 0, true }, { ')', 7, 0x04, false, 0, 0, true },
    { '_', 7, 0x01, false, 0, 0, true }, { '+', 8, 0x02, false, 0, 0, true },
    { ':', 5, 0x02, false, 0, 0, true }, { '"', 8, 0x01, false, 0, 0, true },
    { '?', 6, 0x02, false, 0, 0, true }, { '<', 8, 0x40, false, 0, 0, true },
    { '>', 7, 0x02, false, 0, 0, true }, { '{', 3, 0x80, false, 0, 0, true },
    { '}', 4, 0x80, false, 0, 0, true }, { '|', 5, 0x80, false, 0, 0, true },
    /* no ~: CyOS types Shift+` as ` (checked on the T-Deck, 27 Oct 2026) */
};
#else
static const tdeck_ascii_key_t ASCII_MAP[] = {
    { 'a', 3, 0x0004, false, 0, 0 },
    { 'b', 1, 0x0004, false, 0, 0 },
    { 'c', 2, 0x0200, false, 0, 0 },
    { 'd', 2, 0x0100, false, 0, 0 },
    { 'e', 3, 0x0080, false, 0, 0 },
    { 'f', 2, 0x1000, false, 0, 0 },
    { 'g', 1, 0x0002, false, 0, 0 },
    { 'h', 1, 0x0010, false, 0, 0 },
    { 'i', 0, 0x1000, false, 0, 0 },
    { 'j', 1, 0x0080, false, 0, 0 },
    { 'k', 0, 0x0800, false, 0, 0 },
    { 'l', 0, 0x4000, false, 0, 0 },
    { 'm', 0, 0x0100, false, 0, 0 },
    { 'n', 1, 0x0008, false, 0, 0 },
    { 'o', 0, 0x2000, false, 0, 0 },
    { 'p', 9, 0x0010, false, 0, 0 },
    { 'q', 3, 0x0002, false, 0, 0 },
    { 'r', 2, 0x2000, false, 0, 0 },
    { 's', 3, 0x0020, false, 0, 0 },
    { 't', 2, 0x4000, false, 0, 0 },
    { 'u', 1, 0x0040, false, 0, 0 },
    { 'v', 2, 0x0800, false, 0, 0 },
    { 'w', 3, 0x0040, false, 0, 0 },
    { 'x', 3, 0x0010, false, 0, 0 },
    { 'y', 1, 0x0020, false, 0, 0 },
    { 'z', 3, 0x0008, false, 0, 0 },
    { '\n', 4, 0x0008, false, 0, 0 },
    { '\r', 4, 0x0008, false, 0, 0 },
    { ' ',  4, 0x0040, false, 0, 0 },
    { '\b', 5, 0x0100, false, 0, 0 },
    { '\t', 5, 0x0080, false, 0, 0 },
    { 0x1B, 5, 0x0400, false, 0, 0 },
    { '.',  9, 0x0002, false, 0, 0 },
    { ',',  0, 0x0400, false, 0, 0 },
    { ';',  9, 0x0008, false, 0, 0 },
    { '1', TDECK_FN_COL, TDECK_FN_MASK, true, 3, 0x0002 },
    { '2', TDECK_FN_COL, TDECK_FN_MASK, true, 3, 0x0040 },
    { '3', TDECK_FN_COL, TDECK_FN_MASK, true, 3, 0x0080 },
    { '4', TDECK_FN_COL, TDECK_FN_MASK, true, 2, 0x2000 },
    { '5', TDECK_FN_COL, TDECK_FN_MASK, true, 2, 0x4000 },
    { '6', TDECK_FN_COL, TDECK_FN_MASK, true, 1, 0x0020 },
    { '7', TDECK_FN_COL, TDECK_FN_MASK, true, 1, 0x0040 },
    { '8', TDECK_FN_COL, TDECK_FN_MASK, true, 0, 0x1000 },
    { '9', TDECK_FN_COL, TDECK_FN_MASK, true, 0, 0x2000 },
    { '0', TDECK_FN_COL, TDECK_FN_MASK, true, 9, 0x0010 },
    /* The Xtreme's own symbol keys, and its Fn and Shift layers, for the
     * T-Deck's Sym characters (Java emulator docs/research/xt-keyboard.md) */
    { '!', 9, 0x0004, false, 0, 0 },
    { '(', 2, 0x0400, false, 0, 0 },
    { ')', 0, 0x0200, false, 0, 0 },
    { '#',  TDECK_FN_COL, TDECK_FN_MASK, true, 3, 0x0010 },   /* Fn+X */
    { '*',  TDECK_FN_COL, TDECK_FN_MASK, true, 1, 0x0002 },   /* Fn+G */
    { '@',  TDECK_FN_COL, TDECK_FN_MASK, true, 3, 0x0004 },   /* Fn+A */
    { '$',  TDECK_FN_COL, TDECK_FN_MASK, true, 2, 0x0100 },   /* Fn+D */
    { '/',  TDECK_FN_COL, TDECK_FN_MASK, true, 9, 0x0002 },   /* Fn+. */
    { '?',  TDECK_FN_COL, TDECK_FN_MASK, true, 9, 0x0004 },   /* Fn+! */
    { '_',  TDECK_FN_COL, TDECK_FN_MASK, true, 0, 0x0800 },   /* Fn+K */
    { ':',  TDECK_FN_COL, TDECK_FN_MASK, true, 9, 0x0008 },   /* Fn+; */
    { '+',  TDECK_FN_COL, TDECK_FN_MASK, true, 1, 0x0010 },   /* Fn+H */
    { '"',  TDECK_FN_COL, TDECK_FN_MASK, true, 3, 0x0008 },   /* Fn+Z */
    { '-',  TDECK_FN_COL, TDECK_FN_MASK, true, 1, 0x0080 },   /* Fn+J */
    { '\'', TDECK_FN_COL, TDECK_FN_MASK, true, 0, 0x0400 },  /* Fn+, */
    { '=',  TDECK_FN_COL, TDECK_FN_MASK, true, 0, 0x4000 },   /* Fn+L */
    { '[',  TDECK_FN_COL, TDECK_FN_MASK, true, 2, 0x0400 },   /* Fn+( */
    { ']',  TDECK_FN_COL, TDECK_FN_MASK, true, 0, 0x0200 },   /* Fn+) */
    { '\\', 9, 0x0002, false, 0, 0 },                        /* Shift+. */
};
#endif

bool tdeck_map_ascii(char c, tdeck_key_event_t *ev) {
    bool upper = (c >= 'A' && c <= 'Z');
    char lower = upper ? (char)(c - 'A' + 'a') : c;
    const tdeck_ascii_key_t *k = 0;
    for (unsigned i = 0; i < sizeof ASCII_MAP / sizeof ASCII_MAP[0]; i++) {
        if (ASCII_MAP[i].ascii == lower) {
            k = &ASCII_MAP[i];
            break;
        }
    }
    if (!k) {
        if (ev) ev->found = false;
        return false;
    }
    if (!ev) return true;
    ev->found = true;
    ev->needs_shift = (upper && !k->needs_fn) || k->shift;
#ifndef CYBIKO_MACHINE_V1
    if (k->ascii == '\\') ev->needs_shift = true;   /* Xtreme: Shift+. */
#endif
    ev->needs_fn = k->needs_fn;
    ev->col = k->col;
    ev->mask = k->mask;
    ev->fn_col = k->fn_col;
    ev->fn_mask = k->fn_mask;
    return true;
}

#ifdef CYBIKO_MACHINE_V1
/* Classic, from the Java V1 matrix (MAME A.0-A.8) */
static const struct { int col; uint16_t mask; } SPECIAL[TDECK_KEY_COUNT] = {
    [TDECK_KEY_ESC]    = { 0, 0x02 }, [TDECK_KEY_HELP]   = { 2, 0x80 },
    [TDECK_KEY_SELECT] = { 4, 0x04 }, [TDECK_KEY_INSERT] = { 1, 0x04 },
    [TDECK_KEY_DELETE] = { 0, 0x04 }, [TDECK_KEY_TAB]    = { 3, 0x04 },
    [TDECK_KEY_BKSP]   = { 6, 0x04 },
    [TDECK_KEY_F1] = { 6, 0x01 }, [TDECK_KEY_F2] = { 5, 0x01 },
    [TDECK_KEY_F3] = { 2, 0x02 }, [TDECK_KEY_F4] = { 3, 0x01 },
    [TDECK_KEY_F5] = { 2, 0x01 }, [TDECK_KEY_F6] = { 1, 0x01 },
    [TDECK_KEY_F7] = { 0, 0x01 },
};
#else
/* Xtreme, from docs/research/xt-keyboard.md in the Java emulator */
static const struct { int col; uint16_t mask; } SPECIAL[TDECK_KEY_COUNT] = {
    [TDECK_KEY_ESC]    = { 5, 0x0400 }, [TDECK_KEY_HELP]   = { 9, 0x0001 },
    [TDECK_KEY_SELECT] = { 4, 0x0010 }, [TDECK_KEY_INSERT] = { 5, 0x0200 },
    [TDECK_KEY_DELETE] = { 5, 0x0100 }, [TDECK_KEY_TAB]    = { 5, 0x0080 },
    [TDECK_KEY_BKSP]   = { 5, 0x0100 },   /* the Xtreme has Del only */
    [TDECK_KEY_F1] = { 6, 0x0001 }, [TDECK_KEY_F2] = { 5, 0x0001 },
    [TDECK_KEY_F3] = { 4, 0x0001 }, [TDECK_KEY_F4] = { 3, 0x0001 },
    [TDECK_KEY_F5] = { 2, 0x0001 }, [TDECK_KEY_F6] = { 1, 0x0001 },
    [TDECK_KEY_F7] = { 0, 0x0001 },
};
#endif

bool tdeck_map_special(tdeck_special_key_t key, int *col, uint16_t *mask) {
    if ((int)key < 0 || key >= TDECK_KEY_COUNT || SPECIAL[key].mask == 0)
        return false;
    *col = SPECIAL[key].col;
    *mask = SPECIAL[key].mask;
    return true;
}
