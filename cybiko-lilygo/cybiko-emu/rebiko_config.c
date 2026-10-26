#include "rebiko_config.h"

#include <string.h>

void rb_config_defaults(rb_config_t *c) {
    memset(c, 0, sizeof *c);
    c->cpu_mode = RB_CPU_DYNAMIC;
    c->light = 1;          /* 3/4 */
    c->volume = 8;         /* 50% */
    c->audio_on = 1;
    c->scale = RB_SCALE_2X;
    c->screen_sleep = 1;
}

bool rb_config_validate(rb_config_t *c) {
    rb_config_t d;
    bool changed = false;
    rb_config_defaults(&d);
#define FIX(field, ok) do { if (!(ok)) { c->field = d.field; changed = true; } } while (0)
    FIX(cpu_mode, c->cpu_mode < RB_CPU_MODES);
    FIX(light, c->light < RB_LIGHT_LEVELS);
    FIX(volume, c->volume <= RB_VOLUME_MAX);
    FIX(audio_on, c->audio_on <= 1);
    FIX(scale, c->scale < RB_SCALES);
    FIX(screen_sleep, c->screen_sleep <= 1);
#undef FIX
    if (rb_bindings_sanitize(&c->keys)) changed = true;
    return changed;
}

uint32_t rb_crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

size_t rb_config_pack(const rb_config_t *c, uint8_t *buf, size_t cap) {
    size_t payload = 7 + 4u * c->keys.count;
    size_t total = 7 + payload + 4;
    if (cap < total || c->keys.count > RB_MAX_OVERRIDES) return 0;
    uint8_t *p = buf;
    memcpy(p, "RBKO", 4); p += 4;
    *p++ = RB_CONFIG_VERSION;
    *p++ = (uint8_t)(payload & 0xFF);
    *p++ = (uint8_t)(payload >> 8);
    *p++ = c->cpu_mode; *p++ = c->light; *p++ = c->volume;
    *p++ = c->audio_on; *p++ = c->scale; *p++ = c->screen_sleep;
    *p++ = c->keys.count;
    for (int i = 0; i < c->keys.count; i++) {
        const rb_binding_t *b = &c->keys.ov[i];
        *p++ = b->chord.phys; *p++ = b->chord.mod;
        *p++ = b->action.kind; *p++ = b->action.id;
    }
    uint32_t crc = rb_crc32(buf, (size_t)(p - buf));
    for (int i = 0; i < 4; i++) *p++ = (uint8_t)(crc >> (8 * i));
    return total;
}

rb_cfg_status_t rb_config_unpack(rb_config_t *c, const uint8_t *buf, size_t len) {
    rb_config_defaults(c);
    if (!buf || len == 0) return RB_CFG_MISSING;
    if (len < 7 + 4 || memcmp(buf, "RBKO", 4) != 0 || buf[4] == 0)
        return RB_CFG_CORRUPT;
    size_t payload = buf[5] | ((size_t)buf[6] << 8);
    if (7 + payload + 4 != len) return RB_CFG_CORRUPT;
    uint32_t want = 0;
    for (int i = 0; i < 4; i++) want |= (uint32_t)buf[7 + payload + i] << (8 * i);
    if (rb_crc32(buf, 7 + payload) != want) return RB_CFG_CORRUPT;

    /* Fields in order; a shorter (older) payload leaves the rest at
     * their defaults, a longer (newer) one has fields this build skips. */
    const uint8_t *p = buf + 7, *end = buf + 7 + payload;
    uint8_t *fields[] = { &c->cpu_mode, &c->light, &c->volume,
                          &c->audio_on, &c->scale, &c->screen_sleep };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0] && p < end; i++)
        *fields[i] = *p++;
    if (p < end) {
        int n = *p++;
        if (n > RB_MAX_OVERRIDES || p + 4 * n > end) {
            rb_config_defaults(c);
            return RB_CFG_CORRUPT;
        }
        c->keys.count = (uint8_t)n;
        for (int i = 0; i < n; i++, p += 4) {
            c->keys.ov[i].chord.phys = p[0];
            c->keys.ov[i].chord.mod = p[1];
            c->keys.ov[i].action.kind = p[2];
            c->keys.ov[i].action.id = p[3];
        }
    }
    return rb_config_validate(c) ? RB_CFG_REPAIRED : RB_CFG_OK;
}

bool rb_config_equal(const rb_config_t *a, const rb_config_t *b) {
    uint8_t pa[RB_CONFIG_BLOB_MAX], pb[RB_CONFIG_BLOB_MAX];
    size_t na = rb_config_pack(a, pa, sizeof pa), nb = rb_config_pack(b, pb, sizeof pb);
    return na == nb && memcmp(pa, pb, na) == 0;
}

int rb_cpu_mhz(int cpu_mode, bool screen_on) {
    if (cpu_mode == RB_CPU_LOW) return 160;
    return cpu_mode == RB_CPU_DYNAMIC && !screen_on ? 160 : 240;
}
