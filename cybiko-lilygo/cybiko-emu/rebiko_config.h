#ifndef REBIKO_CONFIG_H
#define REBIKO_CONFIG_H

/*
 * ReBiko Pocket settings: one struct, stored as one NVS blob.
 *
 * Blob layout (little-endian), format version 1:
 *   0  "RBKO"
 *   4  u8  format version
 *   5  u16 payload length
 *   7  payload:
 *        u8 cpu_mode, light, volume, audio_on, scale, screen_sleep
 *        u8 override count, then per override:
 *        u8 phys, mod, action kind, action id
 *   .. u32 CRC-32 of everything before it
 * A newer reader keeps reading old blobs: fields are only ever appended
 * to the payload, and a missing tail takes its defaults.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rebiko_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RB_CONFIG_VERSION 1

/* Stored as a number: new modes go at the end */
typedef enum { RB_CPU_DYNAMIC, RB_CPU_HIGH, RB_CPU_LOW, RB_CPU_MODES } rb_cpu_mode_t;
typedef enum { RB_SCALE_2X, RB_SCALE_1X, RB_SCALES } rb_scale_t;

#define RB_LIGHT_LEVELS 4    /* 4/4, 3/4, 2/4, 1/4 */
#define RB_VOLUME_MAX   10

typedef struct {
    uint8_t cpu_mode;       /* rb_cpu_mode_t */
    uint8_t light;          /* 0 = brightest */
    uint8_t volume;         /* 0..10 */
    uint8_t audio_on;
    uint8_t scale;          /* rb_scale_t */
    uint8_t screen_sleep;   /* 1: screen off while CyOS sleeps */
    rb_bindings_t keys;
} rb_config_t;

#define RB_CONFIG_BLOB_MAX (7 + 7 + 4 * RB_MAX_OVERRIDES + 4)

typedef enum {
    RB_CFG_OK,
    RB_CFG_MISSING,    /* nothing stored: defaults */
    RB_CFG_CORRUPT,    /* bad magic, length or CRC: defaults */
    RB_CFG_REPAIRED,   /* read, but values out of range were reset */
} rb_cfg_status_t;

void rb_config_defaults(rb_config_t *c);
/* Clamp every field and drop bad key overrides. True if anything changed. */
bool rb_config_validate(rb_config_t *c);
size_t rb_config_pack(const rb_config_t *c, uint8_t *buf, size_t cap);
/* Always leaves a usable config in *c */
rb_cfg_status_t rb_config_unpack(rb_config_t *c, const uint8_t *buf, size_t len);
bool rb_config_equal(const rb_config_t *a, const rb_config_t *b);

uint32_t rb_crc32(const uint8_t *p, size_t n);

/* ESP32 clock for the mode. Dynamic drops to 160 MHz only while the
 * screen is off for CyOS's sleep: awake, CyOS's redraw bursts need 240.
 * Always low stays at 160 for battery: busy screens and games then run
 * behind real time now and then, and their sound breaks up. */
int rb_cpu_mhz(int cpu_mode, bool screen_on);

#ifdef __cplusplus
}
#endif

#endif
