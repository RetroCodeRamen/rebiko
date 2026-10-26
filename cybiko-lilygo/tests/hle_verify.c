/* A run with every memset, fill and blit HLE call checked against the CPU
 * (core built with -DCYBIKO_HLE_VERIFY), Classic or Xtreme. tests/hle_verify.sh runs it.
 * Boots, idles on the desktop, then walks through a few apps by key. */
#include <stdio.h>
#include <stdlib.h>
#include "core/emulator.h"
extern uint32_t cybiko_hle_checks, cybiko_hle_mismatches;
static int frame; static uint16_t keys[16];
static void kb(void *c, uint16_t *m, int n) { (void)c; for (int i = 0; i < n; i++) m[i] = keys[i]; }
static uint8_t *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b; }
int main(int argc, char **argv) {
    size_t bn, fn; uint8_t *boot = slurp(argv[1], &bn), *fl = slurp(argv[2], &fn);
    cybiko_hal_t hal = {0}; hal.keyboard_poll = kb;
    cybiko_emu_t *e = cybiko_create(&hal);
    cybiko_load_boot_rom(e, boot, bn); cybiko_load_flash_rom(e, fl, fn);
#ifdef CYBIKO_MACHINE_V1
    cybiko_set_adc(e, 1, 377); cybiko_set_adc(e, 2, 377);
#endif
    cybiko_reset(e);
    /* walk: right, enter, down, esc and two letters, through a few apps */
#ifdef CYBIKO_MACHINE_V1
    enum { R = 4, RM = 0x01, D = 4, DM = 0x02, EN = 5, ENM = 0x04, ES = 0, ESM = 0x02,
           L1 = 2, L1M = 0x10, L2 = 3, L2M = 0x40 };
#else
    enum { R = 6, RM = 0x1000, D = 6, DM = 0x2000, EN = 4, ENM = 0x0008, ES = 5, ESM = 0x0400,
           L1 = 3, L1M = 0x0080, L2 = 2, L2M = 0x0200 };
#endif
    static const struct { int col, mask; } walk[] = {
        {R,RM},{R,RM},{EN,ENM},{D,DM},{D,DM},{EN,ENM},{ES,ESM},{ES,ESM},{R,RM},{EN,ENM},
        {L1,L1M},{L2,L2M},{ES,ESM},{ES,ESM},{R,RM},{R,RM},{EN,ENM},{D,DM},{ES,ESM},{ES,ESM} };
    int w = 0;
    for (frame = 0; frame < atoi(argv[3]); frame++) {
        int t = frame - 3800;
        if (t >= 0 && w < (int)(sizeof walk / sizeof walk[0])) {
            int ph = t % 150;
            keys[walk[w].col] = ph < 12 ? walk[w].mask : 0;
            if (ph == 149) w++;
        }
        cybiko_run_frame(e);
    }
    printf("HLE checks %u mismatches %u\n", (unsigned)cybiko_hle_checks, (unsigned)cybiko_hle_mismatches);
    return cybiko_hle_mismatches != 0;
}
