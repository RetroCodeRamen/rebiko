/*
 * Hitachi H8S/2323 CPU emulator core.
 * Ported from H8SCpu.java.
 */
#include "core/h8s_cpu.h"
#include <stdio.h>
#include <string.h>

#include "core/address_bus.h"
#include "core/machine.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#pragma GCC optimize ("O3")
#define H8S_HOT IRAM_ATTR
#else
#define H8S_HOT
#endif


/*
 * Inline fast-path bus access — avoids cross-TU function call for the
 * 95%+ of reads/writes that hit external RAM or on-chip RAM. Region
 * bounds come from machine.h as constants.
 */
#define EXT_RAM_HIT(a)  ((a) >= CYBIKO_EXT_RAM_BASE && (a) <= CYBIKO_EXT_RAM_END)
#define ON_CHIP_HIT(a)  ((a) >= CYBIKO_ON_CHIP_BASE && (a) < CYBIKO_ON_CHIP_IO)
#define EXT_RAM_OFF(a)  ((a) & (CYBIKO_EXT_RAM_SIZE - 1))
#define ON_CHIP_OFF(a)  ((a) - CYBIKO_ON_CHIP_BASE)
#define BOOT_OFF(a)     ((a) & (CYBIKO_BOOT_ROM_SIZE - 1))
#if CYBIKO_HAS_FLASH_MAP
#define FLASH_HIT(a)    ((a) >= CYBIKO_FLASH_BASE && (a) <= CYBIKO_FLASH_END)
#define FLASH_OFF(a)    ((a) & (CYBIKO_FLASH_SIZE - 1))
#endif

static inline uint8_t bus_read8_fast(address_bus_t *bus, uint32_t address) {
    address &= 0xFFFFFF;
    if (EXT_RAM_HIT(address))
        return bus->external_ram.data[EXT_RAM_OFF(address)];
    if (ON_CHIP_HIT(address))
        return bus->on_chip_ram.data[ON_CHIP_OFF(address)];
    if (address <= CYBIKO_BOOT_ROM_MIRROR_END)
        return bus->boot_rom.data[BOOT_OFF(address)];
#if CYBIKO_HAS_FLASH_MAP
    if (FLASH_HIT(address))
        return bus->flash_rom.data[FLASH_OFF(address)];
#endif
    return bus_read8(bus, address);
}
static inline uint16_t bus_read16_fast(address_bus_t *bus, uint32_t address) {
    address &= 0xFFFFFF;
    if (EXT_RAM_HIT(address)) {
        uint32_t off = EXT_RAM_OFF(address);
        return ((uint16_t)bus->external_ram.data[off] << 8) | bus->external_ram.data[off + 1];
    }
    if (ON_CHIP_HIT(address)) {
        uint32_t off = ON_CHIP_OFF(address);
        return ((uint16_t)bus->on_chip_ram.data[off] << 8) | bus->on_chip_ram.data[off + 1];
    }
    if (address <= CYBIKO_BOOT_ROM_MIRROR_END) {
        uint32_t off = BOOT_OFF(address);
        return ((uint16_t)bus->boot_rom.data[off] << 8) | bus->boot_rom.data[off + 1];
    }
#if CYBIKO_HAS_FLASH_MAP
    if (FLASH_HIT(address)) {
        uint32_t off = FLASH_OFF(address);
        return ((uint16_t)bus->flash_rom.data[off] << 8) | bus->flash_rom.data[off + 1];
    }
#endif
    return bus_read16(bus, address);
}
static inline void bus_write8_fast(address_bus_t *bus, uint32_t address, uint8_t value) {
    address &= 0xFFFFFF;
    if (EXT_RAM_HIT(address)) {
        bus->external_ram.data[EXT_RAM_OFF(address)] = value; return;
    }
    if (ON_CHIP_HIT(address)) {
        bus->on_chip_ram.data[ON_CHIP_OFF(address)] = value; return;
    }
    bus_write8(bus, address, value);
}
static inline void bus_write16_fast(address_bus_t *bus, uint32_t address, uint16_t value) {
    address &= 0xFFFFFF;
    if (EXT_RAM_HIT(address)) {
        uint32_t off = EXT_RAM_OFF(address);
        bus->external_ram.data[off] = (uint8_t)(value >> 8);
        bus->external_ram.data[off + 1] = (uint8_t)value; return;
    }
    if (ON_CHIP_HIT(address)) {
        uint32_t off = ON_CHIP_OFF(address);
        bus->on_chip_ram.data[off] = (uint8_t)(value >> 8);
        bus->on_chip_ram.data[off + 1] = (uint8_t)value; return;
    }
    bus_write16(bus, address, value);
}

/*
 * 32-bit accesses: MOV.L and the LDM/STM register saves on every call and
 * return are ~15% of CyOS instructions. bus_read32/bus_write32 went through
 * two calls into the full decoder each. Same result as two 16-bit accesses
 * at address and address+2; the whole longword must sit in one region,
 * otherwise this falls back to that pair.
 */
static inline uint32_t bus_read32_fast(address_bus_t *bus, uint32_t address) {
    address &= 0xFFFFFF;
    if (EXT_RAM_HIT(address) && EXT_RAM_HIT(address + 3)) {
        const uint8_t *p = bus->external_ram.data + EXT_RAM_OFF(address);
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    if (ON_CHIP_HIT(address) && ON_CHIP_HIT(address + 3)) {
        const uint8_t *p = bus->on_chip_ram.data + ON_CHIP_OFF(address);
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    return ((uint32_t)bus_read16_fast(bus, address) << 16) | bus_read16_fast(bus, address + 2);
}
static inline void bus_write32_fast(address_bus_t *bus, uint32_t address, uint32_t value) {
    address &= 0xFFFFFF;
    if (EXT_RAM_HIT(address) && EXT_RAM_HIT(address + 3)) {
        uint8_t *p = bus->external_ram.data + EXT_RAM_OFF(address);
        p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
        p[2] = (uint8_t)(value >> 8);  p[3] = (uint8_t)value;
        return;
    }
    if (ON_CHIP_HIT(address) && ON_CHIP_HIT(address + 3)) {
        uint8_t *p = bus->on_chip_ram.data + ON_CHIP_OFF(address);
        p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
        p[2] = (uint8_t)(value >> 8);  p[3] = (uint8_t)value;
        return;
    }
    bus_write16_fast(bus, address, (uint16_t)(value >> 16));
    bus_write16_fast(bus, address + 2, (uint16_t)value);
}

/* ---- CCR bit positions (for Java-style getFlag/setFlag using bit index) ---- */
#define BIT_C  0
#define BIT_V  1
#define BIT_Z  2
#define BIT_N  3
#define BIT_U  4
#define BIT_H  5
#define BIT_UI 6
#define BIT_I  7

/* ---- Trace macro ---- */
#ifdef CYBIKO_TRACE
#define TRACE(cpu, fmt, ...) do { if ((cpu)->tracing) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)
#else
#define TRACE(cpu, fmt, ...) ((void)0)
#endif

/* ---- Register access helpers ---- */
static inline uint32_t get_er(const h8s_cpu_t *cpu, int n) { return cpu->er[n]; }
static inline void set_er(h8s_cpu_t *cpu, int n, uint32_t v) { cpu->er[n] = v; }

static inline uint16_t get_e(const h8s_cpu_t *cpu, int n) { return (uint16_t)((cpu->er[n] >> 16) & 0xFFFF); }
static inline void set_e(h8s_cpu_t *cpu, int n, uint16_t v) {
    cpu->er[n] = (cpu->er[n] & 0x0000FFFF) | ((uint32_t)v << 16);
}

static inline uint16_t get_r(const h8s_cpu_t *cpu, int n) {
    if (n < 8) return (uint16_t)(cpu->er[n] & 0xFFFF);
    return (uint16_t)((cpu->er[n - 8] >> 16) & 0xFFFF);
}
static inline void set_r(h8s_cpu_t *cpu, int n, uint16_t v) {
    if (n < 8) cpu->er[n] = (cpu->er[n] & 0xFFFF0000u) | v;
    else { int i = n - 8; cpu->er[i] = (cpu->er[i] & 0x0000FFFFu) | ((uint32_t)v << 16); }
}

static inline uint8_t get_rh(const h8s_cpu_t *cpu, int n) { return (uint8_t)((cpu->er[n] >> 8) & 0xFF); }
static inline void set_rh(h8s_cpu_t *cpu, int n, uint8_t v) {
    cpu->er[n] = (cpu->er[n] & 0xFFFF00FFu) | ((uint32_t)v << 8);
}

static inline uint8_t get_rl(const h8s_cpu_t *cpu, int n) { return (uint8_t)(cpu->er[n] & 0xFF); }
static inline void set_rl(h8s_cpu_t *cpu, int n, uint8_t v) {
    cpu->er[n] = (cpu->er[n] & 0xFFFFFF00u) | v;
}

static inline uint8_t get_reg_b(const h8s_cpu_t *cpu, int n) {
    if (n < 8) return get_rh(cpu, n);
    return get_rl(cpu, n - 8);
}
static inline void set_reg_b(h8s_cpu_t *cpu, int n, uint8_t v) {
    if (n < 8) set_rh(cpu, n, v);
    else set_rl(cpu, n - 8, v);
}

/* ---- Flag helpers ---- */
static inline bool get_flag(const h8s_cpu_t *cpu, int bit) { return (cpu->ccr & (1 << bit)) != 0; }
static inline void set_flag(h8s_cpu_t *cpu, int bit, bool v) {
    if (v) cpu->ccr |= (uint8_t)(1 << bit);
    else cpu->ccr &= (uint8_t)~(1 << bit);
}

static inline void set_nz_b(h8s_cpu_t *cpu, int result) {
    cpu->ccr = (cpu->ccr & ~((1 << BIT_N) | (1 << BIT_Z)))
             | ((result & 0x80) ? (1 << BIT_N) : 0)
             | (((result & 0xFF) == 0) ? (1 << BIT_Z) : 0);
}
static inline void set_nz_w(h8s_cpu_t *cpu, int result) {
    cpu->ccr = (cpu->ccr & ~((1 << BIT_N) | (1 << BIT_Z)))
             | ((result & 0x8000) ? (1 << BIT_N) : 0)
             | (((result & 0xFFFF) == 0) ? (1 << BIT_Z) : 0);
}
static inline void set_nz_l(h8s_cpu_t *cpu, int32_t result) {
    cpu->ccr = (cpu->ccr & ~((1 << BIT_N) | (1 << BIT_Z)))
             | ((result < 0) ? (1 << BIT_N) : 0)
             | ((result == 0) ? (1 << BIT_Z) : 0);
}

/* Combined flag helpers for common instruction patterns (NZV in one write) */
static inline void set_flags_logic_b(h8s_cpu_t *cpu, int result) {
    cpu->ccr = (cpu->ccr & ~(CCR_N | CCR_Z | CCR_V))
             | ((result & 0x80) ? CCR_N : 0)
             | (((result & 0xFF) == 0) ? CCR_Z : 0);
}
static inline void set_flags_logic_w(h8s_cpu_t *cpu, int result) {
    cpu->ccr = (cpu->ccr & ~(CCR_N | CCR_Z | CCR_V))
             | ((result & 0x8000) ? CCR_N : 0)
             | (((result & 0xFFFF) == 0) ? CCR_Z : 0);
}
static inline void set_flags_logic_l(h8s_cpu_t *cpu, int32_t result) {
    cpu->ccr = (cpu->ccr & ~(CCR_N | CCR_Z | CCR_V))
             | ((result < 0) ? CCR_N : 0)
             | ((result == 0) ? CCR_Z : 0);
}

/* ---- Fetch helpers ---- */
static inline uint16_t fetch16(h8s_cpu_t *cpu) {
    uint16_t val = bus_read16_fast(cpu->bus, cpu->pc);
    cpu->pc = (cpu->pc + 2) & 0xFFFFFF;
    return val;
}
static inline uint32_t fetch32(h8s_cpu_t *cpu) {
    uint16_t hi = fetch16(cpu);
    uint16_t lo = fetch16(cpu);
    return ((uint32_t)hi << 16) | lo;
}

/* ---- Branch condition evaluator ---- */
static inline bool evaluate_condition(const h8s_cpu_t *cpu, int cond) {
    bool c = get_flag(cpu, BIT_C);
    bool z = get_flag(cpu, BIT_Z);
    bool n = get_flag(cpu, BIT_N);
    bool v = get_flag(cpu, BIT_V);
    switch (cond) {
        case 0x0: return true;
        case 0x1: return false;
        case 0x2: return !c && !z;
        case 0x3: return c || z;
        case 0x4: return !c;
        case 0x5: return c;
        case 0x6: return !z;
        case 0x7: return z;
        case 0x8: return !v;
        case 0x9: return v;
        case 0xA: return !n;
        case 0xB: return n;
        case 0xC: return !(n ^ v);
        case 0xD: return n ^ v;
        case 0xE: return !(z || (n ^ v));
        case 0xF: return z || (n ^ v);
        default: return false;
    }
}

/* ---- Unimplemented opcode handler ---- */
static void H8S_HOT unimplemented(h8s_cpu_t *cpu, int op, uint32_t addr) {
    fprintf(stderr, "Unimplemented opcode 0x%04X at PC=0x%06X\n", op, addr);
    (void)cpu;
}

/* ---- Shift helpers ---- */
static void H8S_HOT shift_b(h8s_cpu_t *cpu, int rd, bool left, bool logical, int count) {
    int val = get_reg_b(cpu, rd);
    for (int i = 0; i < count; i++) {
        if (left) {
            set_flag(cpu, BIT_C, (val & 0x80) != 0);
            int old = val;
            val = (val << 1) & 0xFF;
            if (!logical) set_flag(cpu, BIT_V, ((old ^ val) & 0x80) != 0);
        } else {
            set_flag(cpu, BIT_C, (val & 1) != 0);
            if (logical) val = (val >> 1) & 0x7F;
            else val = ((int8_t)val >> 1) & 0xFF;
        }
    }
    set_reg_b(cpu, rd, (uint8_t)val);
    set_nz_b(cpu, val);
    if (left && logical) set_flag(cpu, BIT_V, false);
    if (!left) set_flag(cpu, BIT_V, false);
}

static void H8S_HOT shift_w(h8s_cpu_t *cpu, int rd, bool left, bool logical, int count) {
    int val = get_r(cpu, rd);
    for (int i = 0; i < count; i++) {
        if (left) {
            set_flag(cpu, BIT_C, (val & 0x8000) != 0);
            int old = val;
            val = (val << 1) & 0xFFFF;
            if (!logical) set_flag(cpu, BIT_V, ((old ^ val) & 0x8000) != 0);
        } else {
            set_flag(cpu, BIT_C, (val & 1) != 0);
            if (logical) val = (val >> 1) & 0x7FFF;
            else val = ((int16_t)val >> 1) & 0xFFFF;
        }
    }
    set_r(cpu, rd, (uint16_t)val);
    set_nz_w(cpu, val);
    if (left && logical) set_flag(cpu, BIT_V, false);
    if (!left) set_flag(cpu, BIT_V, false);
}

static void H8S_HOT shift_l(h8s_cpu_t *cpu, int erd, bool left, bool logical, int count) {
    int32_t val = (int32_t)cpu->er[erd];
    for (int i = 0; i < count; i++) {
        if (left) {
            set_flag(cpu, BIT_C, (val & (int32_t)0x80000000) != 0);
            int32_t old = val;
            val = val << 1;
            if (!logical) set_flag(cpu, BIT_V, ((old ^ val) & (int32_t)0x80000000) != 0);
        } else {
            set_flag(cpu, BIT_C, (val & 1) != 0);
            if (logical) val = (int32_t)((uint32_t)val >> 1);
            else val = val >> 1;
        }
    }
    cpu->er[erd] = (uint32_t)val;
    set_nz_l(cpu, val);
    if (left && logical) set_flag(cpu, BIT_V, false);
    if (!left) set_flag(cpu, BIT_V, false);
}

/* ---- Rotate helpers ---- */
static void H8S_HOT rotate_b(h8s_cpu_t *cpu, int rd, bool left, bool through_carry, int count) {
    int val = get_reg_b(cpu, rd);
    for (int i = 0; i < count; i++) {
        if (left) {
            int msb = (val >> 7) & 1;
            if (through_carry) {
                int oldC = get_flag(cpu, BIT_C) ? 1 : 0;
                set_flag(cpu, BIT_C, msb != 0);
                val = ((val << 1) | oldC) & 0xFF;
            } else {
                set_flag(cpu, BIT_C, msb != 0);
                val = ((val << 1) | msb) & 0xFF;
            }
        } else {
            int lsb = val & 1;
            if (through_carry) {
                int oldC = get_flag(cpu, BIT_C) ? 0x80 : 0;
                set_flag(cpu, BIT_C, lsb != 0);
                val = ((val >> 1) | oldC) & 0xFF;
            } else {
                set_flag(cpu, BIT_C, lsb != 0);
                val = ((val >> 1) | (lsb << 7)) & 0xFF;
            }
        }
    }
    set_reg_b(cpu, rd, (uint8_t)val);
    set_nz_b(cpu, val);
    set_flag(cpu, BIT_V, false);
}

static void H8S_HOT rotate_w(h8s_cpu_t *cpu, int rd, bool left, bool through_carry, int count) {
    int val = get_r(cpu, rd);
    for (int i = 0; i < count; i++) {
        if (left) {
            int msb = (val >> 15) & 1;
            if (through_carry) {
                int oldC = get_flag(cpu, BIT_C) ? 1 : 0;
                set_flag(cpu, BIT_C, msb != 0);
                val = ((val << 1) | oldC) & 0xFFFF;
            } else {
                set_flag(cpu, BIT_C, msb != 0);
                val = ((val << 1) | msb) & 0xFFFF;
            }
        } else {
            int lsb = val & 1;
            if (through_carry) {
                int oldC = get_flag(cpu, BIT_C) ? 0x8000 : 0;
                set_flag(cpu, BIT_C, lsb != 0);
                val = ((val >> 1) | oldC) & 0xFFFF;
            } else {
                set_flag(cpu, BIT_C, lsb != 0);
                val = ((val >> 1) | (lsb << 15)) & 0xFFFF;
            }
        }
    }
    set_r(cpu, rd, (uint16_t)val);
    set_nz_w(cpu, val);
    set_flag(cpu, BIT_V, false);
}

static void H8S_HOT rotate_l(h8s_cpu_t *cpu, int erd, bool left, bool through_carry, int count) {
    uint32_t val = cpu->er[erd];
    for (int i = 0; i < count; i++) {
        if (left) {
            uint32_t msb = (val >> 31) & 1;
            if (through_carry) {
                uint32_t oldC = get_flag(cpu, BIT_C) ? 1u : 0u;
                set_flag(cpu, BIT_C, msb != 0);
                val = (val << 1) | oldC;
            } else {
                set_flag(cpu, BIT_C, msb != 0);
                val = (val << 1) | msb;
            }
        } else {
            uint32_t lsb = val & 1;
            if (through_carry) {
                uint32_t oldC = get_flag(cpu, BIT_C) ? 0x80000000u : 0u;
                set_flag(cpu, BIT_C, lsb != 0);
                val = (val >> 1) | oldC;
            } else {
                set_flag(cpu, BIT_C, lsb != 0);
                val = (val >> 1) | (lsb << 31);
            }
        }
    }
    cpu->er[erd] = val;
    set_nz_l(cpu, (int32_t)val);
    set_flag(cpu, BIT_V, false);
}

/* ---- Bit operation at memory address ---- */
static void H8S_HOT execute_bit_op_at_address(h8s_cpu_t *cpu, uint32_t addr, uint16_t bit_op) {
    int op_type = (bit_op >> 8) & 0xFF;
    int bit_num = (bit_op >> 4) & 0x07;
    int val = bus_read8_fast(cpu->bus, addr);

    switch (op_type) {
        case 0x70: bus_write8_fast(cpu->bus, addr, (uint8_t)(val | (1 << bit_num))); break;
        case 0x71: bus_write8_fast(cpu->bus, addr, (uint8_t)(val ^ (1 << bit_num))); break;
        case 0x72: bus_write8_fast(cpu->bus, addr, (uint8_t)(val & ~(1 << bit_num))); break;
        case 0x73: set_flag(cpu, BIT_Z, (val & (1 << bit_num)) == 0); break;
        case 0x74: if ((val & (1 << bit_num)) != 0) set_flag(cpu, BIT_C, true); break;
        case 0x75: if ((val & (1 << bit_num)) != 0) set_flag(cpu, BIT_C, !get_flag(cpu, BIT_C)); break;
        case 0x76: if ((val & (1 << bit_num)) == 0) set_flag(cpu, BIT_C, false); break;
        case 0x77: set_flag(cpu, BIT_C, (val & (1 << bit_num)) != 0); break;
        case 0x67: {
            if (get_flag(cpu, BIT_C)) val |= (1 << bit_num);
            else val &= ~(1 << bit_num);
            bus_write8_fast(cpu->bus, addr, (uint8_t)val);
            break;
        }
        case 0x60: {
            int rn = (bit_op >> 4) & 0xF;
            int bit = get_reg_b(cpu, rn) & 0x7;
            bus_write8_fast(cpu->bus, addr, (uint8_t)(val | (1 << bit)));
            break;
        }
        case 0x61: {
            int rn = (bit_op >> 4) & 0xF;
            int bit = get_reg_b(cpu, rn) & 0x7;
            bus_write8_fast(cpu->bus, addr, (uint8_t)(val ^ (1 << bit)));
            break;
        }
        case 0x62: {
            int rn = (bit_op >> 4) & 0xF;
            int bit = get_reg_b(cpu, rn) & 0x7;
            bus_write8_fast(cpu->bus, addr, (uint8_t)(val & ~(1 << bit)));
            break;
        }
        case 0x63: {
            int rn = (bit_op >> 4) & 0xF;
            int bit = get_reg_b(cpu, rn) & 0x7;
            set_flag(cpu, BIT_Z, (val & (1 << bit)) == 0);
            break;
        }
        default: unimplemented(cpu, bit_op, cpu->pc - 2); break;
    }
}

/* ======== Forward declarations for decode sub-functions ======== */
static inline void decode(h8s_cpu_t *cpu, uint16_t op);

/* ---- MOV.L with absolute address (6B_32) ---- */
static void H8S_HOT decode6B_32(h8s_cpu_t *cpu, uint16_t op2) {
    int lo2 = op2 & 0xFF;
    bool write = (lo2 & 0x80) != 0;
    bool addr24 = (lo2 & 0x20) != 0;
    int rd = lo2 & 0x7;
    uint32_t addr;
    if (addr24) addr = fetch32(cpu) & 0xFFFFFF;
    else { addr = (uint32_t)(int32_t)(int16_t)fetch16(cpu); addr &= 0xFFFFFF; }

    if (!write) {
        cpu->er[rd] = bus_read32_fast(cpu->bus, addr);
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_V, false);
    } else {
        bus_write32_fast(cpu->bus, addr, cpu->er[rd]);
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_V, false);
    }
}

/* ---- MULXS ---- */
static void H8S_HOT decode_mulxs(h8s_cpu_t *cpu, int hi2, uint16_t op2) {
    int rs = (op2 >> 4) & 0xF;
    int rd = op2 & 0xF;
    if (hi2 == 0x50) {
        int result = (int8_t)get_reg_b(cpu, rs) * (int8_t)get_reg_b(cpu, rd);
        set_r(cpu, rd, (uint16_t)(result & 0xFFFF));
        set_flag(cpu, BIT_N, (result & 0x8000) != 0);
        set_flag(cpu, BIT_Z, (result & 0xFFFF) == 0);
    } else {
        int32_t result = (int16_t)get_r(cpu, rs) * (int16_t)get_r(cpu, rd);
        cpu->er[rd & 0x7] = (uint32_t)result;
        set_flag(cpu, BIT_N, result < 0);
        set_flag(cpu, BIT_Z, result == 0);
    }
}

/* ---- DIVXS ---- */
static void H8S_HOT decode_divxs(h8s_cpu_t *cpu, int hi2, uint16_t op2) {
    int rs = (op2 >> 4) & 0xF;
    int rd = op2 & 0x7;
    if (hi2 == 0x51) {
        int16_t dividend = (int16_t)get_r(cpu, rd);
        int8_t divisor = (int8_t)get_reg_b(cpu, rs);
        if (divisor == 0) { set_flag(cpu, BIT_Z, true); }
        else {
            int quotient = dividend / divisor;
            int remainder = dividend % divisor;
            set_r(cpu, rd, (uint16_t)(((remainder & 0xFF) << 8) | (quotient & 0xFF)));
            set_flag(cpu, BIT_N, quotient < 0);
            set_flag(cpu, BIT_Z, quotient == 0);
        }
    } else {
        int32_t dividend = (int32_t)cpu->er[rd];
        int16_t divisor = (int16_t)get_r(cpu, rs);
        if (divisor == 0) { set_flag(cpu, BIT_Z, true); }
        else {
            int32_t quotient = dividend / divisor;
            int32_t remainder = dividend % divisor;
            cpu->er[rd] = (uint32_t)(((remainder & 0xFFFF) << 16) | (quotient & 0xFFFF));
            set_flag(cpu, BIT_N, quotient < 0);
            set_flag(cpu, BIT_Z, quotient == 0);
        }
    }
}

/* ---- decode0141: EXR operations ---- */
static void H8S_HOT decode0141(h8s_cpu_t *cpu, uint16_t op2) {
    int hi2 = (op2 >> 8) & 0xFF;
    cpu->irq_hold = true;   /* ORC/XORC/ANDC/LDC #imm, EXR */
    switch (hi2) {
        case 0x04: cpu->exr |= (op2 & 0xFF); break;
        case 0x05: cpu->exr ^= (op2 & 0xFF); break;
        case 0x06: cpu->exr &= (op2 & 0xFF); break;
        case 0x07: cpu->exr = (uint8_t)(op2 & 0xFF); break;
        default: unimplemented(cpu, 0x0141, cpu->pc - 2); break;
    }
}

/* ---- decode0140: LDC/STC CCR word-width ---- */
static void H8S_HOT decode0140(h8s_cpu_t *cpu, uint16_t op2) {
    int hi2 = (op2 >> 8) & 0xFF;
    if ((op2 & 0x80) == 0)
        cpu->irq_hold = true;   /* LDC @mem, CCR (bit 7 set = STC) */
    switch (hi2) {
        case 0x69: {
            int r = (op2 >> 4) & 0x7;
            if ((op2 & 0x80) == 0) cpu->ccr = (uint8_t)(bus_read16_fast(cpu->bus, cpu->er[r]) & 0xFF);
            else bus_write16_fast(cpu->bus, cpu->er[r], cpu->ccr);
            break;
        }
        case 0x6B: {
            bool write = (op2 & 0x80) != 0;
            bool addr24 = (op2 & 0x20) != 0;
            uint32_t addr;
            if (addr24) addr = fetch32(cpu) & 0xFFFFFF;
            else { addr = (uint32_t)(int32_t)(int16_t)fetch16(cpu); addr &= 0xFFFFFF; }
            if (!write) cpu->ccr = (uint8_t)(bus_read16_fast(cpu->bus, addr) & 0xFF);
            else bus_write16_fast(cpu->bus, addr, cpu->ccr);
            break;
        }
        case 0x6D: {
            int r = (op2 >> 4) & 0x7;
            if ((op2 & 0x80) == 0) {
                cpu->ccr = (uint8_t)(bus_read16_fast(cpu->bus, cpu->er[r]) & 0xFF);
                cpu->er[r] += 2;
            } else {
                cpu->er[r] -= 2;
                bus_write16_fast(cpu->bus, cpu->er[r], cpu->ccr);
            }
            break;
        }
        case 0x6F: {
            int r = (op2 >> 4) & 0x7;
            int16_t disp = (int16_t)fetch16(cpu);
            if ((op2 & 0x80) == 0) cpu->ccr = (uint8_t)(bus_read16_fast(cpu->bus, cpu->er[r] + disp) & 0xFF);
            else bus_write16_fast(cpu->bus, cpu->er[r] + disp, cpu->ccr);
            break;
        }
        default: unimplemented(cpu, 0x0140, cpu->pc - 2); break;
    }
}

/* ---- decode0100: MOV.L variants ---- */
static void H8S_HOT decode0100(h8s_cpu_t *cpu, uint16_t op2) {
    int hi2 = (op2 >> 8) & 0xFF;
    int lo2 = op2 & 0xFF;
    switch (hi2) {
        case 0x69: {
            int rh = (lo2 >> 4) & 0x7;
            int rl = lo2 & 0x7;
            if ((lo2 & 0x80) == 0) {
                cpu->er[rl] = bus_read32_fast(cpu->bus, cpu->er[rh]);
                set_flags_logic_l(cpu, (int32_t)cpu->er[rl]);
            } else {
                bus_write32_fast(cpu->bus, cpu->er[rh], cpu->er[rl]);
                set_flags_logic_l(cpu, (int32_t)cpu->er[rl]);
            }
            break;
        }
        case 0x6B: decode6B_32(cpu, op2); break;
        case 0x6D: {
            int rh = (lo2 >> 4) & 0x7;
            int rl = lo2 & 0x7;
            if ((lo2 & 0x80) == 0) {
                uint32_t val = bus_read32_fast(cpu->bus, cpu->er[rh]);
                cpu->er[rh] += 4;
                cpu->er[rl] = val;
                set_flags_logic_l(cpu, (int32_t)cpu->er[rl]);
            } else {
                uint32_t val = cpu->er[rl];
                cpu->er[rh] -= 4;
                bus_write32_fast(cpu->bus, cpu->er[rh], val);
                set_flags_logic_l(cpu, (int32_t)val);
            }
            break;
        }
        case 0x6F: {
            int rh = (lo2 >> 4) & 0x7;
            int rl = lo2 & 0x7;
            int16_t disp = (int16_t)fetch16(cpu);
            if ((lo2 & 0x80) == 0) {
                cpu->er[rl] = bus_read32_fast(cpu->bus, cpu->er[rh] + disp);
                set_flags_logic_l(cpu, (int32_t)cpu->er[rl]);
            } else {
                bus_write32_fast(cpu->bus, cpu->er[rh] + disp, cpu->er[rl]);
                set_flags_logic_l(cpu, (int32_t)cpu->er[rl]);
            }
            break;
        }
        case 0x78: {
            int r1 = (lo2 >> 4) & 0x7;
            uint16_t op3 = fetch16(cpu);
            int32_t disp = (int32_t)fetch32(cpu);
            int r2 = op3 & 0x7;
            if ((op3 & 0x0080) == 0) {
                cpu->er[r2] = bus_read32_fast(cpu->bus, cpu->er[r1] + disp);
                set_flags_logic_l(cpu, (int32_t)cpu->er[r2]);
            } else {
                bus_write32_fast(cpu->bus, cpu->er[r2] + disp, cpu->er[r1]);
                set_flags_logic_l(cpu, (int32_t)cpu->er[r1]);
            }
            break;
        }
        default: unimplemented(cpu, 0x0100, cpu->pc - 4); break;
    }
}

/* ---- decode01: prefix 0x01xx ---- */
static void H8S_HOT decode01(h8s_cpu_t *cpu, int lo) {
    switch (lo) {
        case 0x00: { uint16_t op2 = fetch16(cpu); decode0100(cpu, op2); break; }
        case 0x10: case 0x20: case 0x30: {
            int count = (lo >> 4);
            uint16_t op2 = fetch16(cpu);
            int rn = op2 & 0x7;
            if ((op2 & 0xFFF8) == 0x6D70) {
                for (int i = 0; i <= count; i++) {
                    cpu->er[rn - i] = bus_read32_fast(cpu->bus, cpu->er[7]);
                    cpu->er[7] += 4;
                }
            } else if ((op2 & 0xFFF8) == 0x6DF0) {
                for (int i = 0; i <= count; i++) {
                    cpu->er[7] -= 4;
                    bus_write32_fast(cpu->bus, cpu->er[7], cpu->er[rn + i]);
                }
            } else { unimplemented(cpu, 0x0100 | lo, cpu->pc - 4); }
            break;
        }
        case 0x40: { uint16_t op2 = fetch16(cpu); decode0140(cpu, op2); break; }
        case 0x41: { uint16_t op2 = fetch16(cpu); decode0141(cpu, op2); break; }
        case 0xC0: case 0xD0: case 0xF0: {
            uint16_t op2 = fetch16(cpu);
            int hi2 = (op2 >> 8) & 0xFF;
            if (hi2 == 0x6B) { decode6B_32(cpu, op2); }
            else if (hi2 == 0x50 || hi2 == 0x52) { decode_mulxs(cpu, hi2, op2); }
            else if (hi2 == 0x51 || hi2 == 0x53) { decode_divxs(cpu, hi2, op2); }
            else if (lo == 0xF0 && hi2 == 0x64) {
                int rs = (op2 >> 4) & 0x7; int rd = op2 & 0x7;
                cpu->er[rd] = cpu->er[rd] | cpu->er[rs];
                set_flags_logic_l(cpu, (int32_t)cpu->er[rd]);
            } else if (lo == 0xF0 && hi2 == 0x65) {
                int rs = (op2 >> 4) & 0x7; int rd = op2 & 0x7;
                cpu->er[rd] = cpu->er[rd] ^ cpu->er[rs];
                set_flags_logic_l(cpu, (int32_t)cpu->er[rd]);
            } else if (lo == 0xF0 && hi2 == 0x66) {
                int rs = (op2 >> 4) & 0x7; int rd = op2 & 0x7;
                cpu->er[rd] = cpu->er[rd] & cpu->er[rs];
                set_flags_logic_l(cpu, (int32_t)cpu->er[rd]);
            } else { unimplemented(cpu, 0x0100 | lo, cpu->pc - 4); }
            break;
        }
        case 0x80: cpu->halted = true; break;
        default: { uint16_t op2 = fetch16(cpu); (void)op2; unimplemented(cpu, 0x0100 | lo, cpu->pc - 4); break; }
    }
}

/* ---- decode0A: INC.B, ADD.L reg ---- */
static void H8S_HOT decode0A(h8s_cpu_t *cpu, int lo) {
    if ((lo & 0x80) != 0) {
        int rs = (lo >> 4) & 0x7; int rd = lo & 0x7;
        uint64_t s = cpu->er[rs]; uint64_t d = cpu->er[rd];
        uint64_t result = d + s;
        cpu->er[rd] = (uint32_t)result;
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_C, (result & 0x100000000ULL) != 0);
        set_flag(cpu, BIT_V, (int32_t)((d ^ result) & (s ^ result)) < 0);
        set_flag(cpu, BIT_H, ((int32_t)(d ^ s ^ result) & 0x10000000) != 0);
    } else if ((lo & 0xF0) == 0x00) {
        int rd = lo & 0xF;
        int val = get_reg_b(cpu, rd);
        int result = (val + 1) & 0xFF;
        set_reg_b(cpu, rd, (uint8_t)result);
        set_nz_b(cpu, result);
        set_flag(cpu, BIT_V, val == 0x7F);
    } else if ((lo & 0xF0) == 0x50) {
        int rd = lo & 0xF;
        int val = get_r(cpu, rd);
        int result = (val + 1) & 0xFFFF;
        set_r(cpu, rd, (uint16_t)result);
        set_nz_w(cpu, result);
        set_flag(cpu, BIT_V, val == 0x7FFF);
    } else if ((lo & 0xF0) == 0x70) {
        int rd = lo & 0x7;
        int32_t val = (int32_t)cpu->er[rd];
        cpu->er[rd] = (uint32_t)(val + 1);
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_V, val == 0x7FFFFFFF);
    } else { unimplemented(cpu, 0x0A00 | lo, cpu->pc - 2); }
}

/* ---- decode0B: ADDS, INC.W, INC.L ---- */
static void H8S_HOT decode0B(h8s_cpu_t *cpu, int lo) {
    switch (lo & 0xF0) {
        case 0x00: cpu->er[lo & 0x7] += 1; break;
        case 0x50: {
            int rd = lo & 0xF; int val = get_r(cpu, rd);
            int result = (val + 1) & 0xFFFF;
            set_r(cpu, rd, (uint16_t)result); set_nz_w(cpu, result);
            set_flag(cpu, BIT_V, val == 0x7FFF);
            break;
        }
        case 0x70: {
            int rd = lo & 0x7; int32_t val = (int32_t)cpu->er[rd];
            cpu->er[rd] = (uint32_t)(val + 1);
            set_nz_l(cpu, (int32_t)cpu->er[rd]);
            set_flag(cpu, BIT_V, val == 0x7FFFFFFF);
            break;
        }
        case 0x80: cpu->er[lo & 0x7] += 2; break;
        case 0x90: cpu->er[lo & 0x7] += 4; break;
        case 0xD0: {
            int rd = lo & 0xF; int val = get_r(cpu, rd);
            int result = (val + 2) & 0xFFFF;
            set_r(cpu, rd, (uint16_t)result); set_nz_w(cpu, result);
            break;
        }
        case 0xF0: {
            int rd = lo & 0x7; cpu->er[rd] += 2;
            set_nz_l(cpu, (int32_t)cpu->er[rd]);
            break;
        }
        default: unimplemented(cpu, 0x0B00 | lo, cpu->pc - 2); break;
    }
}

/* ---- decode0F: MOV.L reg, DAA ---- */
static void H8S_HOT decode0F(h8s_cpu_t *cpu, int lo) {
    if ((lo & 0x80) != 0) {
        int rs = (lo >> 4) & 0x7; int rd = lo & 0x7;
        cpu->er[rd] = cpu->er[rs];
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_V, false);
    } else if ((lo & 0xF0) == 0x00) {
        /* DAA stub */
    } else { unimplemented(cpu, 0x0F00 | lo, cpu->pc - 2); }
}

/* ---- decode17: NOT, EXTU, NEG, EXTS ---- */
static void H8S_HOT decode17(h8s_cpu_t *cpu, int lo) {
    int subop = (lo >> 4) & 0xF;
    int rd = lo & 0xF;
    int erd = lo & 0x7;
    switch (subop) {
        case 0x0: { int v = (~get_reg_b(cpu, rd)) & 0xFF; set_reg_b(cpu, rd, (uint8_t)v); set_flags_logic_b(cpu, v); break; }
        case 0x1: { int v = (~get_r(cpu, rd)) & 0xFFFF; set_r(cpu, rd, (uint16_t)v); set_flags_logic_w(cpu, v); break; }
        case 0x3: { cpu->er[erd] = ~cpu->er[erd]; set_flags_logic_l(cpu, (int32_t)cpu->er[erd]); break; }
        case 0x5: { set_r(cpu, rd, get_rl(cpu, rd)); set_flags_logic_w(cpu, get_r(cpu, rd)); break; }
        case 0x7: { cpu->er[erd] = get_r(cpu, erd); set_flags_logic_l(cpu, (int32_t)cpu->er[erd]); break; }
        case 0x8: {
            int v = get_reg_b(cpu, rd); int r = (-v) & 0xFF;
            set_reg_b(cpu, rd, (uint8_t)r); set_nz_b(cpu, r);
            set_flag(cpu, BIT_C, r != 0); set_flag(cpu, BIT_V, v == 0x80);
            set_flag(cpu, BIT_H, ((v ^ r) & 0x10) != 0);
            break;
        }
        case 0x9: {
            int v = get_r(cpu, rd); int r = (-v) & 0xFFFF;
            set_r(cpu, rd, (uint16_t)r); set_nz_w(cpu, r);
            set_flag(cpu, BIT_C, r != 0); set_flag(cpu, BIT_V, v == 0x8000);
            break;
        }
        case 0xB: {
            uint32_t v = cpu->er[erd]; cpu->er[erd] = (uint32_t)(-(int32_t)v);
            set_nz_l(cpu, (int32_t)cpu->er[erd]);
            set_flag(cpu, BIT_C, cpu->er[erd] != 0);
            set_flag(cpu, BIT_V, v == 0x80000000u);
            break;
        }
        case 0xD: { int v = (int8_t)get_rl(cpu, rd); set_r(cpu, rd, (uint16_t)(v & 0xFFFF)); set_flags_logic_w(cpu, v); break; }
        case 0xF: { int32_t v = (int16_t)get_r(cpu, erd); cpu->er[erd] = (uint32_t)v; set_flags_logic_l(cpu, v); break; }
        default: unimplemented(cpu, 0x1700 | lo, cpu->pc - 2); break;
    }
}

/* ---- Shift/rotate/extend group (0x10-0x17) ---- */
static void H8S_HOT decode10_17(h8s_cpu_t *cpu, int hi, int lo) {
    if (hi == 0x14) { /* OR.B reg */
        int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
        int result = get_reg_b(cpu, rd) | get_reg_b(cpu, rs);
        set_reg_b(cpu, rd, (uint8_t)result); set_flags_logic_b(cpu, result);
        return;
    }
    if (hi == 0x15) { /* XOR.B reg */
        int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
        int result = get_reg_b(cpu, rd) ^ get_reg_b(cpu, rs);
        set_reg_b(cpu, rd, (uint8_t)result); set_flags_logic_b(cpu, result);
        return;
    }
    if (hi == 0x16) { /* AND.B reg */
        int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
        int result = get_reg_b(cpu, rd) & get_reg_b(cpu, rs);
        set_reg_b(cpu, rd, (uint8_t)result); set_flags_logic_b(cpu, result);
        return;
    }
    if (hi == 0x17) { decode17(cpu, lo); return; }

    int subop = (lo >> 4) & 0xF;
    int rd = lo & 0xF;
    int erd = lo & 0x7;
    switch (hi) {
        case 0x10:
            switch (subop) {
                case 0x0: shift_b(cpu, rd, true, true, 1); break;
                case 0x1: shift_w(cpu, rd, true, true, 1); break;
                case 0x3: shift_l(cpu, erd, true, true, 1); break;
                case 0x4: shift_b(cpu, rd, true, true, 2); break;
                case 0x5: shift_w(cpu, rd, true, true, 2); break;
                case 0x7: shift_l(cpu, erd, true, true, 2); break;
                case 0x8: shift_b(cpu, rd, true, false, 1); break;
                case 0x9: shift_w(cpu, rd, true, false, 1); break;
                case 0xB: shift_l(cpu, erd, true, false, 1); break;
                case 0xC: shift_b(cpu, rd, true, false, 2); break;
                case 0xD: shift_w(cpu, rd, true, false, 2); break;
                case 0xF: shift_l(cpu, erd, true, false, 2); break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        case 0x11:
            switch (subop) {
                case 0x0: shift_b(cpu, rd, false, true, 1); break;
                case 0x1: shift_w(cpu, rd, false, true, 1); break;
                case 0x3: shift_l(cpu, erd, false, true, 1); break;
                case 0x4: shift_b(cpu, rd, false, true, 2); break;
                case 0x5: shift_w(cpu, rd, false, true, 2); break;
                case 0x7: shift_l(cpu, erd, false, true, 2); break;
                case 0x8: shift_b(cpu, rd, false, false, 1); break;
                case 0x9: shift_w(cpu, rd, false, false, 1); break;
                case 0xB: shift_l(cpu, erd, false, false, 1); break;
                case 0xC: shift_b(cpu, rd, false, false, 2); break;
                case 0xD: shift_w(cpu, rd, false, false, 2); break;
                case 0xF: shift_l(cpu, erd, false, false, 2); break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        case 0x12:
            switch (subop) {
                case 0x0: rotate_b(cpu, rd, true, true, 1); break;
                case 0x1: rotate_w(cpu, rd, true, true, 1); break;
                case 0x3: rotate_l(cpu, erd, true, true, 1); break;
                case 0x4: rotate_b(cpu, rd, true, true, 2); break;
                case 0x5: rotate_w(cpu, rd, true, true, 2); break;
                case 0x7: rotate_l(cpu, erd, true, true, 2); break;
                case 0x8: rotate_b(cpu, rd, true, false, 1); break;
                case 0x9: rotate_w(cpu, rd, true, false, 1); break;
                case 0xB: rotate_l(cpu, erd, true, false, 1); break;
                case 0xC: rotate_b(cpu, rd, true, false, 2); break;
                case 0xD: rotate_w(cpu, rd, true, false, 2); break;
                case 0xF: rotate_l(cpu, erd, true, false, 2); break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        case 0x13:
            switch (subop) {
                case 0x0: rotate_b(cpu, rd, false, true, 1); break;
                case 0x1: rotate_w(cpu, rd, false, true, 1); break;
                case 0x3: rotate_l(cpu, erd, false, true, 1); break;
                case 0x4: rotate_b(cpu, rd, false, true, 2); break;
                case 0x5: rotate_w(cpu, rd, false, true, 2); break;
                case 0x7: rotate_l(cpu, erd, false, true, 2); break;
                case 0x8: rotate_b(cpu, rd, false, false, 1); break;
                case 0x9: rotate_w(cpu, rd, false, false, 1); break;
                case 0xB: rotate_l(cpu, erd, false, false, 1); break;
                case 0xC: rotate_b(cpu, rd, false, false, 2); break;
                case 0xD: rotate_w(cpu, rd, false, false, 2); break;
                case 0xF: rotate_l(cpu, erd, false, false, 2); break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
    }
}

/* ---- decode1A: DEC.B, SUB.L reg ---- */
static void H8S_HOT decode1A(h8s_cpu_t *cpu, int lo) {
    if ((lo & 0x80) != 0) {
        int rs = (lo >> 4) & 0x7; int rd = lo & 0x7;
        uint64_t s = cpu->er[rs]; uint64_t d = cpu->er[rd];
        uint64_t result = d - s;
        cpu->er[rd] = (uint32_t)result;
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_C, (result & 0x100000000ULL) != 0);
        set_flag(cpu, BIT_V, (int32_t)((d ^ s) & (d ^ result)) < 0);
        set_flag(cpu, BIT_H, ((int32_t)(d ^ s ^ result) & 0x10000000) != 0);
    } else if ((lo & 0xF0) == 0x00) {
        int rd = lo & 0xF;
        int val = get_reg_b(cpu, rd);
        int result = (val - 1) & 0xFF;
        set_reg_b(cpu, rd, (uint8_t)result); set_nz_b(cpu, result);
        set_flag(cpu, BIT_V, val == 0x80);
    } else if ((lo & 0xF0) == 0x50) {
        int rd = lo & 0xF;
        int val = get_r(cpu, rd);
        int result = (val - 1) & 0xFFFF;
        set_r(cpu, rd, (uint16_t)result); set_nz_w(cpu, result);
        set_flag(cpu, BIT_V, val == 0x8000);
    } else if ((lo & 0xF0) == 0x70) {
        int rd = lo & 0x7;
        int32_t val = (int32_t)cpu->er[rd];
        cpu->er[rd] = (uint32_t)(val - 1);
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_V, (uint32_t)val == 0x80000000u);
    } else { unimplemented(cpu, 0x1A00 | lo, cpu->pc - 2); }
}

/* ---- decode1B: SUBS, DEC.W, DEC.L ---- */
static void H8S_HOT decode1B(h8s_cpu_t *cpu, int lo) {
    switch (lo & 0xF0) {
        case 0x00: cpu->er[lo & 0x7] -= 1; break;
        case 0x50: {
            int rd = lo & 0xF; int val = get_r(cpu, rd);
            int result = (val - 1) & 0xFFFF;
            set_r(cpu, rd, (uint16_t)result); set_nz_w(cpu, result);
            set_flag(cpu, BIT_V, val == 0x8000);
            break;
        }
        case 0x70: {
            int rd = lo & 0x7; int32_t val = (int32_t)cpu->er[rd];
            cpu->er[rd] = (uint32_t)(val - 1);
            set_nz_l(cpu, (int32_t)cpu->er[rd]);
            set_flag(cpu, BIT_V, (uint32_t)val == 0x80000000u);
            break;
        }
        case 0x80: cpu->er[lo & 0x7] -= 2; break;
        case 0x90: cpu->er[lo & 0x7] -= 4; break;
        case 0xD0: {
            int rd = lo & 0xF; int val = get_r(cpu, rd);
            int result = (val - 2) & 0xFFFF;
            set_r(cpu, rd, (uint16_t)result); set_nz_w(cpu, result);
            set_flag(cpu, BIT_V, val == 0x8000 || val == 0x8001);
            break;
        }
        case 0xF0: {
            int rd = lo & 0x7; cpu->er[rd] -= 2;
            set_nz_l(cpu, (int32_t)cpu->er[rd]);
            break;
        }
        default: unimplemented(cpu, 0x1B00 | lo, cpu->pc - 2); break;
    }
}

/* ---- decode1F: CMP.L reg, DAS ---- */
static void H8S_HOT decode1F(h8s_cpu_t *cpu, int lo) {
    if ((lo & 0x80) != 0) {
        int rs = (lo >> 4) & 0x7; int rd = lo & 0x7;
        uint64_t s = cpu->er[rs]; uint64_t d = cpu->er[rd];
        uint64_t result = d - s;
        set_nz_l(cpu, (int32_t)result);
        set_flag(cpu, BIT_C, (result & 0x100000000ULL) != 0);
        set_flag(cpu, BIT_V, (int32_t)((d ^ s) & (d ^ result)) < 0);
        set_flag(cpu, BIT_H, ((int32_t)(d ^ s ^ result) & 0x10000000) != 0);
    } else if ((lo & 0xF0) == 0x00) {
        /* DAS stub */
    } else { unimplemented(cpu, 0x1F00 | lo, cpu->pc - 2); }
}

/* ---- decode6A: MOV.B absolute / compound bit ---- */
static void H8S_HOT decode6A(h8s_cpu_t *cpu, int lo) {
    int topNibble = (lo >> 4) & 0xF;
    if (topNibble == 1 || topNibble == 3) {
        bool addr32 = (topNibble == 3);
        uint32_t addr;
        if (addr32) addr = fetch32(cpu) & 0xFFFFFF;
        else { addr = (uint32_t)(int32_t)(int16_t)fetch16(cpu); addr &= 0xFFFFFF; }
        uint16_t bitOp = fetch16(cpu);
        execute_bit_op_at_address(cpu, addr, bitOp);
        return;
    }
    bool write = (lo & 0x80) != 0;
    bool addr24 = (lo & 0x20) != 0;
    int rd = lo & 0xF;
    uint32_t addr;
    if (addr24) addr = fetch32(cpu) & 0xFFFFFF;
    else { addr = (uint32_t)(int32_t)(int16_t)fetch16(cpu); addr &= 0xFFFFFF; }
    if (!write) {
        int val = bus_read8_fast(cpu->bus, addr);
        set_reg_b(cpu, rd, (uint8_t)val); set_flags_logic_b(cpu, val);
    } else {
        int val = get_reg_b(cpu, rd);
        bus_write8_fast(cpu->bus, addr, (uint8_t)val); set_flags_logic_b(cpu, val);
    }
}

/* ---- decode6B: MOV.W absolute ---- */
static void H8S_HOT decode6B(h8s_cpu_t *cpu, int lo) {
    bool write = (lo & 0x80) != 0;
    bool addr24 = (lo & 0x20) != 0;
    int rd = lo & 0xF;
    uint32_t addr;
    if (addr24) addr = fetch32(cpu) & 0xFFFFFF;
    else { addr = (uint32_t)(int32_t)(int16_t)fetch16(cpu); addr &= 0xFFFFFF; }
    if (!write) {
        int val = bus_read16_fast(cpu->bus, addr);
        set_r(cpu, rd, (uint16_t)val); set_flags_logic_w(cpu, val);
    } else {
        int val = get_r(cpu, rd);
        bus_write16_fast(cpu->bus, addr, (uint16_t)val); set_flags_logic_w(cpu, val);
    }
}

/* ---- decode79: word immediate ---- */
static void H8S_HOT decode79(h8s_cpu_t *cpu, int lo) {
    int subop = (lo >> 4) & 0xF;
    int rd = lo & 0xF;
    int imm = fetch16(cpu);
    switch (subop) {
        case 0x0: set_r(cpu, rd, (uint16_t)imm); set_flags_logic_w(cpu, imm); break;
        case 0x1: {
            int d = get_r(cpu, rd); int result = d + imm;
            set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_nz_w(cpu, result);
            set_flag(cpu, BIT_C, result > 0xFFFF);
            set_flag(cpu, BIT_V, ((d ^ result) & (imm ^ result) & 0x8000) != 0);
            set_flag(cpu, BIT_H, ((d ^ imm ^ result) & 0x1000) != 0);
            break;
        }
        case 0x2: {
            int d = get_r(cpu, rd); int result = d - imm;
            set_nz_w(cpu, result);
            set_flag(cpu, BIT_C, (result & 0x10000) != 0);
            set_flag(cpu, BIT_V, ((d ^ imm) & (d ^ result) & 0x8000) != 0);
            set_flag(cpu, BIT_H, ((d ^ imm ^ result) & 0x1000) != 0);
            break;
        }
        case 0x3: {
            int d = get_r(cpu, rd); int result = d - imm;
            set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_nz_w(cpu, result);
            set_flag(cpu, BIT_C, (result & 0x10000) != 0);
            set_flag(cpu, BIT_V, ((d ^ imm) & (d ^ result) & 0x8000) != 0);
            set_flag(cpu, BIT_H, ((d ^ imm ^ result) & 0x1000) != 0);
            break;
        }
        case 0x4: { int result = get_r(cpu, rd) | imm; set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_flags_logic_w(cpu, result); break; }
        case 0x5: { int result = get_r(cpu, rd) ^ imm; set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_flags_logic_w(cpu, result); break; }
        case 0x6: { int result = get_r(cpu, rd) & imm; set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_flags_logic_w(cpu, result); break; }
        default: unimplemented(cpu, 0x7900 | lo, cpu->pc - 4); break;
    }
}

/* ---- decode7A: longword immediate ---- */
static void H8S_HOT decode7A(h8s_cpu_t *cpu, int lo) {
    int subop = (lo >> 4) & 0xF;
    int rd = lo & 0x7;
    uint32_t imm = fetch32(cpu);
    switch (subop) {
        case 0x0: cpu->er[rd] = imm; set_flags_logic_l(cpu, (int32_t)imm); break;
        case 0x1: {
            uint64_t d = cpu->er[rd]; uint64_t result = d + (uint64_t)imm;
            cpu->er[rd] = (uint32_t)result; set_nz_l(cpu, (int32_t)cpu->er[rd]);
            set_flag(cpu, BIT_C, (result & 0x100000000ULL) != 0);
            set_flag(cpu, BIT_V, (int32_t)((d ^ result) & (imm ^ result)) < 0);
            set_flag(cpu, BIT_H, ((int32_t)(d ^ imm ^ result) & 0x10000000) != 0);
            break;
        }
        case 0x2: {
            uint64_t d = cpu->er[rd]; uint64_t result = d - (uint64_t)imm;
            set_nz_l(cpu, (int32_t)result);
            set_flag(cpu, BIT_C, (result & 0x100000000ULL) != 0);
            set_flag(cpu, BIT_V, (int32_t)((d ^ imm) & (d ^ result)) < 0);
            set_flag(cpu, BIT_H, ((int32_t)(d ^ imm ^ result) & 0x10000000) != 0);
            break;
        }
        case 0x3: {
            uint64_t d = cpu->er[rd]; uint64_t result = d - (uint64_t)imm;
            cpu->er[rd] = (uint32_t)result; set_nz_l(cpu, (int32_t)cpu->er[rd]);
            set_flag(cpu, BIT_C, (result & 0x100000000ULL) != 0);
            set_flag(cpu, BIT_V, (int32_t)((d ^ imm) & (d ^ result)) < 0);
            set_flag(cpu, BIT_H, ((int32_t)(d ^ imm ^ result) & 0x10000000) != 0);
            break;
        }
        case 0x4: cpu->er[rd] |= imm; set_flags_logic_l(cpu, (int32_t)cpu->er[rd]); break;
        case 0x5: cpu->er[rd] ^= imm; set_flags_logic_l(cpu, (int32_t)cpu->er[rd]); break;
        case 0x6: cpu->er[rd] &= imm; set_flags_logic_l(cpu, (int32_t)cpu->er[rd]); break;
        default: unimplemented(cpu, 0x7A00 | lo, cpu->pc - 6); break;
    }
}

/* ---- decode7C_7F: bit ops on memory ---- */
static void H8S_HOT decode7C_7F(h8s_cpu_t *cpu, int hi, int lo) {
    int r = (lo >> 4) & 0x7;
    uint16_t op2 = fetch16(cpu);
    int bit = (op2 >> 4) & 0x7;
    int subop = (op2 >> 8) & 0xFF;

    switch (hi) {
        case 0x7C: {
            uint32_t addr = cpu->er[r];
            int val = bus_read8_fast(cpu->bus, addr);
            switch (subop) {
                case 0x63: case 0x73: set_flag(cpu, BIT_Z, (val & (1 << bit)) == 0); break;
                case 0x77:
                    if ((op2 & 0x80) == 0) set_flag(cpu, BIT_C, (val & (1 << bit)) != 0);
                    else set_flag(cpu, BIT_C, (val & (1 << bit)) == 0);
                    break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        }
        case 0x7D: {
            uint32_t addr = cpu->er[r];
            int val = bus_read8_fast(cpu->bus, addr);
            switch (subop) {
                case 0x60: bus_write8_fast(cpu->bus, addr, (uint8_t)(val | (1 << (get_rl(cpu, op2 & 0xF) & 0x7)))); break;
                case 0x62: bus_write8_fast(cpu->bus, addr, (uint8_t)(val & ~(1 << (get_rl(cpu, op2 & 0xF) & 0x7)))); break;
                case 0x67:
                    if ((op2 & 0x80) == 0) {
                        if (get_flag(cpu, BIT_C)) val |= (1 << bit); else val &= ~(1 << bit);
                    } else {
                        if (!get_flag(cpu, BIT_C)) val |= (1 << bit); else val &= ~(1 << bit);
                    }
                    bus_write8_fast(cpu->bus, addr, (uint8_t)val);
                    break;
                case 0x70: bus_write8_fast(cpu->bus, addr, (uint8_t)(val | (1 << bit))); break;
                case 0x72: bus_write8_fast(cpu->bus, addr, (uint8_t)(val & ~(1 << bit))); break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        }
        case 0x7E: {
            uint32_t addr = 0xFFFF00 | (lo & 0xFF);
            int val = bus_read8_fast(cpu->bus, addr);
            switch (subop) {
                case 0x63: case 0x73: set_flag(cpu, BIT_Z, (val & (1 << bit)) == 0); break;
                case 0x77:
                    if ((op2 & 0x80) == 0) set_flag(cpu, BIT_C, (val & (1 << bit)) != 0);
                    else set_flag(cpu, BIT_C, (val & (1 << bit)) == 0);
                    break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        }
        case 0x7F: {
            uint32_t addr = 0xFFFF00 | (lo & 0xFF);
            int val = bus_read8_fast(cpu->bus, addr);
            switch (subop) {
                case 0x70: bus_write8_fast(cpu->bus, addr, (uint8_t)(val | (1 << bit))); break;
                case 0x72: bus_write8_fast(cpu->bus, addr, (uint8_t)(val & ~(1 << bit))); break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        }
        default: break;
    }
}

/* ======== Group 0 ======== */
static void H8S_HOT decode0(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
    switch (hi) {
        case 0x00: break; /* NOP */
        case 0x01: decode01(cpu, lo); break;
        case 0x02: { int rd = lo & 0xF; set_reg_b(cpu, rd, cpu->ccr); break; }
        case 0x03: { int rs = lo & 0xF; cpu->ccr = get_reg_b(cpu, rs); cpu->irq_hold = true; break; }
        case 0x04: cpu->ccr |= (uint8_t)lo; cpu->irq_hold = true; break;
        case 0x05: cpu->ccr ^= (uint8_t)lo; cpu->irq_hold = true; break;
        case 0x06: cpu->ccr &= (uint8_t)lo; cpu->irq_hold = true; break;
        case 0x07: cpu->ccr = (uint8_t)lo; cpu->irq_hold = true; break;
        case 0x08: { /* ADD.B Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int s = get_reg_b(cpu, rs); int d = get_reg_b(cpu, rd);
            int result = d + s;
            set_reg_b(cpu, rd, (uint8_t)(result & 0xFF)); set_nz_b(cpu, result);
            set_flag(cpu, BIT_C, result > 0xFF);
            set_flag(cpu, BIT_V, ((d ^ result) & (s ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ s ^ result) & 0x10) != 0);
            break;
        }
        case 0x09: { /* ADD.W Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int s = get_r(cpu, rs); int d = get_r(cpu, rd);
            int result = d + s;
            set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_nz_w(cpu, result);
            set_flag(cpu, BIT_C, result > 0xFFFF);
            set_flag(cpu, BIT_V, ((d ^ result) & (s ^ result) & 0x8000) != 0);
            set_flag(cpu, BIT_H, ((d ^ s ^ result) & 0x1000) != 0);
            break;
        }
        case 0x0A: decode0A(cpu, lo); break;
        case 0x0B: decode0B(cpu, lo); break;
        case 0x0C: { /* MOV.B Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int val = get_reg_b(cpu, rs);
            set_reg_b(cpu, rd, (uint8_t)val); set_flags_logic_b(cpu, val);
            break;
        }
        case 0x0D: { /* MOV.W Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int val = get_r(cpu, rs);
            set_r(cpu, rd, (uint16_t)val); set_flags_logic_w(cpu, val);
            break;
        }
        case 0x0E: { /* ADDX.B Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int s = get_reg_b(cpu, rs); int d = get_reg_b(cpu, rd);
            int c = get_flag(cpu, BIT_C) ? 1 : 0;
            int result = d + s + c;
            set_reg_b(cpu, rd, (uint8_t)(result & 0xFF));
            set_flag(cpu, BIT_C, result > 0xFF);
            set_flag(cpu, BIT_V, ((d ^ result) & (s ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ s ^ result) & 0x10) != 0);
            if ((result & 0xFF) != 0) set_flag(cpu, BIT_Z, false);
            set_flag(cpu, BIT_N, (result & 0x80) != 0);
            break;
        }
        case 0x0F: decode0F(cpu, lo); break;
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/* ======== Group 1 ======== */
static void H8S_HOT decode1(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
    switch (hi) {
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
            decode10_17(cpu, hi, lo); break;
        case 0x18: { /* SUB.B Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int s = get_reg_b(cpu, rs); int d = get_reg_b(cpu, rd);
            int result = d - s;
            set_reg_b(cpu, rd, (uint8_t)(result & 0xFF)); set_nz_b(cpu, result);
            set_flag(cpu, BIT_C, (result & 0x100) != 0);
            set_flag(cpu, BIT_V, ((d ^ s) & (d ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ s ^ result) & 0x10) != 0);
            break;
        }
        case 0x19: { /* SUB.W Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int s = get_r(cpu, rs); int d = get_r(cpu, rd);
            int result = d - s;
            set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_nz_w(cpu, result);
            set_flag(cpu, BIT_C, (result & 0x10000) != 0);
            set_flag(cpu, BIT_V, ((d ^ s) & (d ^ result) & 0x8000) != 0);
            set_flag(cpu, BIT_H, ((d ^ s ^ result) & 0x1000) != 0);
            break;
        }
        case 0x1A: decode1A(cpu, lo); break;
        case 0x1B: decode1B(cpu, lo); break;
        case 0x1C: { /* CMP.B Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int s = get_reg_b(cpu, rs); int d = get_reg_b(cpu, rd);
            int result = d - s;
            set_nz_b(cpu, result);
            set_flag(cpu, BIT_C, (result & 0x100) != 0);
            set_flag(cpu, BIT_V, ((d ^ s) & (d ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ s ^ result) & 0x10) != 0);
            break;
        }
        case 0x1D: { /* CMP.W Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int s = get_r(cpu, rs); int d = get_r(cpu, rd);
            int result = d - s;
            set_nz_w(cpu, result);
            set_flag(cpu, BIT_C, (result & 0x10000) != 0);
            set_flag(cpu, BIT_V, ((d ^ s) & (d ^ result) & 0x8000) != 0);
            set_flag(cpu, BIT_H, ((d ^ s ^ result) & 0x1000) != 0);
            break;
        }
        case 0x1E: { /* SUBX.B Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int s = get_reg_b(cpu, rs); int d = get_reg_b(cpu, rd);
            int c = get_flag(cpu, BIT_C) ? 1 : 0;
            int result = d - s - c;
            set_reg_b(cpu, rd, (uint8_t)(result & 0xFF));
            set_flag(cpu, BIT_C, result < 0);
            set_flag(cpu, BIT_V, ((d ^ s) & (d ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ s ^ result) & 0x10) != 0);
            if ((result & 0xFF) != 0) set_flag(cpu, BIT_Z, false);
            set_flag(cpu, BIT_N, (result & 0x80) != 0);
            break;
        }
        case 0x1F: decode1F(cpu, lo); break;
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/* ======== Group 5 ======== */
static void H8S_HOT decode5(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
    switch (hi) {
        case 0x50: { /* MULXU.B */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int result = get_reg_b(cpu, rs) * get_reg_b(cpu, rd);
            set_r(cpu, rd, (uint16_t)(result & 0xFFFF));
            break;
        }
        case 0x51: { /* DIVXU.B */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int divisor = get_reg_b(cpu, rs);
            if (divisor == 0) { set_flag(cpu, BIT_Z, true); }
            else {
                int dividend = get_r(cpu, rd);
                int quotient = (dividend / divisor) & 0xFF;
                int remainder = (dividend % divisor) & 0xFF;
                set_r(cpu, rd, (uint16_t)((remainder << 8) | quotient));
                set_flag(cpu, BIT_N, (quotient & 0x80) != 0);
                set_flag(cpu, BIT_Z, quotient == 0);
            }
            break;
        }
        case 0x52: { /* MULXU.W */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0x7;
            uint32_t result = (uint32_t)get_r(cpu, rs) * (uint32_t)get_r(cpu, rd);
            cpu->er[rd] = result;
            break;
        }
        case 0x53: { /* DIVXU.W */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0x7;
            uint32_t divisor = get_r(cpu, rs) & 0xFFFF;
            if (divisor == 0) { set_flag(cpu, BIT_Z, true); }
            else {
                uint64_t dividend = cpu->er[rd] & 0xFFFFFFFFULL;
                uint32_t quotient = (uint32_t)((dividend / divisor) & 0xFFFF);
                uint32_t remainder = (uint32_t)((dividend % divisor) & 0xFFFF);
                cpu->er[rd] = (remainder << 16) | quotient;
                set_flag(cpu, BIT_N, (quotient & 0x8000) != 0);
                set_flag(cpu, BIT_Z, quotient == 0);
            }
            break;
        }
        case 0x54: { /* RTS */
            if (lo == 0x70) {
                cpu->pc = bus_read32_fast(cpu->bus, cpu->er[7]) & 0xFFFFFF;
                cpu->er[7] += 4;
            } else { unimplemented(cpu, op, cpu->pc - 2); }
            break;
        }
        case 0x55: { /* BSR d:8 */
            int8_t disp = (int8_t)lo;
            cpu->er[7] -= 4;
            bus_write32_fast(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = (cpu->pc + disp) & 0xFFFFFF;
            break;
        }
        case 0x56: { /* RTE */
            if (lo == 0x70) {
                cpu->ccr = (uint8_t)(bus_read16_fast(cpu->bus, cpu->er[7]) & 0xFF);
                cpu->er[7] += 2;
                cpu->pc = bus_read32_fast(cpu->bus, cpu->er[7]) & 0xFFFFFF;
                cpu->er[7] += 4;
            } else { unimplemented(cpu, op, cpu->pc - 2); }
            break;
        }
        case 0x57: { /* TRAPA */
            if ((lo & 0xC0) == 0x00) {
                int vec = (lo >> 4) & 0x3;
                cpu->er[7] -= 4;
                bus_write32_fast(cpu->bus, cpu->er[7], ((uint32_t)cpu->ccr << 24) | cpu->pc);
                cpu->pc = bus_read32_fast(cpu->bus, 0x20 + (uint32_t)vec * 4) & 0xFFFFFF;
                set_flag(cpu, BIT_I, true);
            } else { unimplemented(cpu, op, cpu->pc - 2); }
            break;
        }
        case 0x58: { /* Bcc d:16 */
            int cond = (lo >> 4) & 0xF;
            int16_t disp = (int16_t)fetch16(cpu);
            if (evaluate_condition(cpu, cond))
                cpu->pc = (cpu->pc + disp) & 0xFFFFFF;
            break;
        }
        case 0x59: { /* JMP @ERn */
            int rn = (lo >> 4) & 0x7;
            cpu->pc = cpu->er[rn] & 0xFFFFFF;
            break;
        }
        case 0x5A: { /* JMP @aa:24 */
            uint32_t addr = ((uint32_t)lo << 16) | fetch16(cpu);
            cpu->pc = addr & 0xFFFFFF;
            break;
        }
        case 0x5B: { /* JMP @@aa:8 */
            cpu->pc = bus_read32_fast(cpu->bus, lo & 0xFF) & 0xFFFFFF;
            break;
        }
        case 0x5C: { /* BSR d:16 */
            int16_t disp = (int16_t)fetch16(cpu);
            cpu->er[7] -= 4;
            bus_write32_fast(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = (cpu->pc + disp) & 0xFFFFFF;
            break;
        }
        case 0x5D: { /* JSR @ERn */
            int rn = (lo >> 4) & 0x7;
            cpu->er[7] -= 4;
            bus_write32_fast(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = cpu->er[rn] & 0xFFFFFF;
            break;
        }
        case 0x5E: { /* JSR @aa:24 */
            uint32_t addr = ((uint32_t)lo << 16) | fetch16(cpu);
            cpu->er[7] -= 4;
            bus_write32_fast(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = addr & 0xFFFFFF;
            break;
        }
        case 0x5F: { /* JSR @@aa:8 */
            uint32_t addr = bus_read32_fast(cpu->bus, lo & 0xFF) & 0xFFFFFF;
            cpu->er[7] -= 4;
            bus_write32_fast(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = addr;
            break;
        }
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/* ======== Group 6 ======== */
static void H8S_HOT decode6(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
    switch (hi) {
        case 0x60: { int rn = (lo >> 4) & 0xF; int rd = lo & 0xF; int bit = get_reg_b(cpu, rn) & 0x7; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) | (1 << bit))); break; }
        case 0x61: { int rn = (lo >> 4) & 0xF; int rd = lo & 0xF; int bit = get_reg_b(cpu, rn) & 0x7; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) ^ (1 << bit))); break; }
        case 0x62: { int rn = (lo >> 4) & 0xF; int rd = lo & 0xF; int bit = get_reg_b(cpu, rn) & 0x7; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) & ~(1 << bit))); break; }
        case 0x63: { int rn = (lo >> 4) & 0xF; int rd = lo & 0xF; int bit = get_reg_b(cpu, rn) & 0x7; set_flag(cpu, BIT_Z, (get_reg_b(cpu, rd) & (1 << bit)) == 0); break; }
        case 0x64: { /* OR.W */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int result = get_r(cpu, rd) | get_r(cpu, rs);
            set_r(cpu, rd, (uint16_t)result); set_flags_logic_w(cpu, result);
            break;
        }
        case 0x65: { /* XOR.W */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int result = get_r(cpu, rd) ^ get_r(cpu, rs);
            set_r(cpu, rd, (uint16_t)result); set_flags_logic_w(cpu, result);
            break;
        }
        case 0x66: { /* AND.W */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int result = get_r(cpu, rd) & get_r(cpu, rs);
            set_r(cpu, rd, (uint16_t)result); set_flags_logic_w(cpu, result);
            break;
        }
        case 0x67: { /* BST/BIST */
            int bit = (lo >> 4) & 0x7; int rd = lo & 0xF;
            if ((lo & 0x80) == 0) {
                if (get_flag(cpu, BIT_C)) set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) | (1 << bit)));
                else set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) & ~(1 << bit)));
            } else {
                if (!get_flag(cpu, BIT_C)) set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) | (1 << bit)));
                else set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) & ~(1 << bit)));
            }
            break;
        }
        case 0x68: { /* MOV.B @ER, reg */
            int erReg = (lo >> 4) & 0x7; int bReg = lo & 0xF;
            if ((lo & 0x80) == 0) {
                int val = bus_read8_fast(cpu->bus, cpu->er[erReg]);
                set_reg_b(cpu, bReg, (uint8_t)val); set_flags_logic_b(cpu, val);
            } else {
                int val = get_reg_b(cpu, bReg);
                bus_write8_fast(cpu->bus, cpu->er[erReg], (uint8_t)val); set_flags_logic_b(cpu, val);
            }
            break;
        }
        case 0x69: { /* MOV.W @ER, reg */
            int erReg = (lo >> 4) & 0x7; int wReg = lo & 0xF;
            if ((lo & 0x80) == 0) {
                int val = bus_read16_fast(cpu->bus, cpu->er[erReg]);
                set_r(cpu, wReg, (uint16_t)val); set_flags_logic_w(cpu, val);
            } else {
                int val = get_r(cpu, wReg);
                bus_write16_fast(cpu->bus, cpu->er[erReg], (uint16_t)val); set_flags_logic_w(cpu, val);
            }
            break;
        }
        case 0x6A: decode6A(cpu, lo); break;
        case 0x6B: decode6B(cpu, lo); break;
        case 0x6C: { /* MOV.B post-inc/pre-dec */
            int erReg = (lo >> 4) & 0x7; int bReg = lo & 0xF;
            if ((lo & 0x80) == 0) {
                int val = bus_read8_fast(cpu->bus, cpu->er[erReg]);
                cpu->er[erReg] += 1;
                set_reg_b(cpu, bReg, (uint8_t)val); set_flags_logic_b(cpu, val);
            } else {
                int val = get_reg_b(cpu, bReg);
                cpu->er[erReg] -= 1;
                bus_write8_fast(cpu->bus, cpu->er[erReg], (uint8_t)val); set_flags_logic_b(cpu, val);
            }
            break;
        }
        case 0x6D: { /* MOV.W post-inc/pre-dec */
            int erReg = (lo >> 4) & 0x7; int wReg = lo & 0xF;
            if ((lo & 0x80) == 0) {
                int val = bus_read16_fast(cpu->bus, cpu->er[erReg]);
                cpu->er[erReg] += 2;
                set_r(cpu, wReg, (uint16_t)val); set_flags_logic_w(cpu, val);
            } else {
                int val = get_r(cpu, wReg);
                cpu->er[erReg] -= 2;
                bus_write16_fast(cpu->bus, cpu->er[erReg], (uint16_t)val); set_flags_logic_w(cpu, val);
            }
            break;
        }
        case 0x6E: { /* MOV.B @(d:16, ER) */
            int erReg = (lo >> 4) & 0x7; int bReg = lo & 0xF;
            int16_t disp = (int16_t)fetch16(cpu);
            if ((lo & 0x80) == 0) {
                int val = bus_read8_fast(cpu->bus, cpu->er[erReg] + disp);
                set_reg_b(cpu, bReg, (uint8_t)val); set_flags_logic_b(cpu, val);
            } else {
                int val = get_reg_b(cpu, bReg);
                bus_write8_fast(cpu->bus, cpu->er[erReg] + disp, (uint8_t)val); set_flags_logic_b(cpu, val);
            }
            break;
        }
        case 0x6F: { /* MOV.W @(d:16, ER) */
            int erReg = (lo >> 4) & 0x7; int wReg = lo & 0xF;
            int16_t disp = (int16_t)fetch16(cpu);
            if ((lo & 0x80) == 0) {
                int val = bus_read16_fast(cpu->bus, cpu->er[erReg] + disp);
                set_r(cpu, wReg, (uint16_t)val); set_flags_logic_w(cpu, val);
            } else {
                int val = get_r(cpu, wReg);
                bus_write16_fast(cpu->bus, cpu->er[erReg] + disp, (uint16_t)val); set_flags_logic_w(cpu, val);
            }
            break;
        }
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/* ======== Group 7 ======== */
static void H8S_HOT decode7(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
    switch (hi) {
        case 0x70: { int bit = (lo >> 4) & 0x7; int rd = lo & 0xF; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) | (1 << bit))); break; }
        case 0x71: { int bit = (lo >> 4) & 0x7; int rd = lo & 0xF; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) ^ (1 << bit))); break; }
        case 0x72: { int bit = (lo >> 4) & 0x7; int rd = lo & 0xF; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) & ~(1 << bit))); break; }
        case 0x73: { int bit = (lo >> 4) & 0x7; int rd = lo & 0xF; set_flag(cpu, BIT_Z, (get_reg_b(cpu, rd) & (1 << bit)) == 0); break; }
        case 0x74: { /* BOR/BIOR */
            int bit = (lo >> 4) & 0x7; int rd = lo & 0xF;
            bool bitVal = (get_reg_b(cpu, rd) & (1 << bit)) != 0;
            if ((lo & 0x80) == 0) set_flag(cpu, BIT_C, get_flag(cpu, BIT_C) | bitVal);
            else set_flag(cpu, BIT_C, get_flag(cpu, BIT_C) | !bitVal);
            break;
        }
        case 0x75: { /* BXOR/BIXOR */
            int bit = (lo >> 4) & 0x7; int rd = lo & 0xF;
            bool bitVal = (get_reg_b(cpu, rd) & (1 << bit)) != 0;
            if ((lo & 0x80) == 0) set_flag(cpu, BIT_C, get_flag(cpu, BIT_C) ^ bitVal);
            else set_flag(cpu, BIT_C, get_flag(cpu, BIT_C) ^ !bitVal);
            break;
        }
        case 0x76: { /* BAND/BIAND */
            int bit = (lo >> 4) & 0x7; int rd = lo & 0xF;
            bool bitVal = (get_reg_b(cpu, rd) & (1 << bit)) != 0;
            if ((lo & 0x80) == 0) set_flag(cpu, BIT_C, get_flag(cpu, BIT_C) & bitVal);
            else set_flag(cpu, BIT_C, get_flag(cpu, BIT_C) & !bitVal);
            break;
        }
        case 0x77: { /* BLD/BILD */
            int bit = (lo >> 4) & 0x7; int rd = lo & 0xF;
            bool bitVal = (get_reg_b(cpu, rd) & (1 << bit)) != 0;
            if ((lo & 0x80) == 0) set_flag(cpu, BIT_C, bitVal);
            else set_flag(cpu, BIT_C, !bitVal);
            break;
        }
        case 0x78: { /* MOV.B/W @(d:32, ER) */
            int erReg = (lo >> 4) & 0x7;
            uint16_t op2 = fetch16(cpu);
            int hi2 = (op2 >> 8) & 0xFF;
            if (hi2 == 0x6A) {
                int bReg = op2 & 0xF;
                int32_t disp = (int32_t)fetch32(cpu);
                if ((op2 & 0x80) == 0) {
                    int val = bus_read8_fast(cpu->bus, cpu->er[erReg] + disp);
                    set_reg_b(cpu, bReg, (uint8_t)val); set_flags_logic_b(cpu, val);
                } else {
                    int val = get_reg_b(cpu, bReg);
                    bus_write8_fast(cpu->bus, cpu->er[erReg] + disp, (uint8_t)val); set_flags_logic_b(cpu, val);
                }
            } else if (hi2 == 0x6B) {
                int wReg = op2 & 0xF;
                int32_t disp = (int32_t)fetch32(cpu);
                if ((op2 & 0x80) == 0) {
                    int val = bus_read16_fast(cpu->bus, cpu->er[erReg] + disp);
                    set_r(cpu, wReg, (uint16_t)val); set_flags_logic_w(cpu, val);
                } else {
                    int val = get_r(cpu, wReg);
                    bus_write16_fast(cpu->bus, cpu->er[erReg] + disp, (uint16_t)val); set_flags_logic_w(cpu, val);
                }
            } else { unimplemented(cpu, op, cpu->pc - 4); }
            break;
        }
        case 0x79: decode79(cpu, lo); break;
        case 0x7A: decode7A(cpu, lo); break;
        case 0x7B: { /* EEPMOV */
            uint16_t op2 = fetch16(cpu);
            if (lo == 0x5C && op2 == 0x598F) {
                while (get_reg_b(cpu, 0xC) != 0) {
                    uint8_t b = bus_read8_fast(cpu->bus, cpu->er[5]);
                    bus_write8_fast(cpu->bus, cpu->er[6], b);
                    cpu->er[5]++; cpu->er[6]++;
                    set_reg_b(cpu, 0xC, (uint8_t)((get_reg_b(cpu, 0xC) - 1) & 0xFF));
                }
            } else if (lo == 0xD4 && op2 == 0x598F) {
                while (get_r(cpu, 4) != 0) {
                    uint8_t b = bus_read8_fast(cpu->bus, cpu->er[5]);
                    bus_write8_fast(cpu->bus, cpu->er[6], b);
                    cpu->er[5]++; cpu->er[6]++;
                    set_r(cpu, 4, (uint16_t)((get_r(cpu, 4) - 1) & 0xFFFF));
                }
            } else { unimplemented(cpu, op, cpu->pc - 4); }
            break;
        }
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            decode7C_7F(cpu, hi, lo); break;
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/* ======== Main decode ======== */
/* Inlined into h8s_cpu_run, its only caller: one call level fewer per
 * guest instruction. The second-level decoders stay separate functions;
 * inlining the whole tree made the run loop slower (register spills). */
static inline __attribute__((always_inline)) void decode(h8s_cpu_t *cpu, uint16_t op) {
    int hi = (op >> 8) & 0xFF;
    int lo = op & 0xFF;

    switch (hi >> 4) {
        case 0x0: decode0(cpu, op, hi, lo); break;
        case 0x1: decode1(cpu, op, hi, lo); break;
        case 0x2: { /* MOV.B @aa:8, Rd */
            int rd = (op >> 8) & 0xF;
            uint32_t addr = 0xFFFF00 | (op & 0xFF);
            int val = bus_read8_fast(cpu->bus, addr);
            set_reg_b(cpu, rd, (uint8_t)val); set_flags_logic_b(cpu, val);
            break;
        }
        case 0x3: { /* MOV.B Rs, @aa:8 */
            int rd = (op >> 8) & 0xF;
            uint32_t addr = 0xFFFF00 | (op & 0xFF);
            int val = get_reg_b(cpu, rd);
            bus_write8_fast(cpu->bus, addr, (uint8_t)val); set_flags_logic_b(cpu, val);
            break;
        }
        case 0x4: { /* Bcc d:8 */
            int cond = (op >> 8) & 0xF;
            int8_t disp = (int8_t)(op & 0xFF);
            uint32_t target = (cpu->pc + disp) & 0xFFFFFF;
            if (evaluate_condition(cpu, cond)) cpu->pc = target;
            break;
        }
        case 0x5: decode5(cpu, op, hi, lo); break;
        case 0x6: decode6(cpu, op, hi, lo); break;
        case 0x7: decode7(cpu, op, hi, lo); break;
        case 0x8: { /* ADD.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int d = get_reg_b(cpu, rd); int result = d + imm;
            set_reg_b(cpu, rd, (uint8_t)(result & 0xFF)); set_nz_b(cpu, result);
            set_flag(cpu, BIT_C, result > 0xFF);
            set_flag(cpu, BIT_V, ((d ^ result) & (imm ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ imm ^ result) & 0x10) != 0);
            break;
        }
        case 0x9: { /* ADDX.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int d = get_reg_b(cpu, rd); int c = get_flag(cpu, BIT_C) ? 1 : 0;
            int result = d + imm + c;
            set_reg_b(cpu, rd, (uint8_t)(result & 0xFF));
            set_flag(cpu, BIT_C, result > 0xFF);
            set_flag(cpu, BIT_V, ((d ^ result) & (imm ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ imm ^ result) & 0x10) != 0);
            if ((result & 0xFF) != 0) set_flag(cpu, BIT_Z, false);
            set_flag(cpu, BIT_N, (result & 0x80) != 0);
            break;
        }
        case 0xA: { /* CMP.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int d = get_reg_b(cpu, rd); int result = d - imm;
            set_nz_b(cpu, result);
            set_flag(cpu, BIT_C, (result & 0x100) != 0);
            set_flag(cpu, BIT_V, ((d ^ imm) & (d ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ imm ^ result) & 0x10) != 0);
            break;
        }
        case 0xB: { /* SUBX.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int d = get_reg_b(cpu, rd); int c = get_flag(cpu, BIT_C) ? 1 : 0;
            int result = d - imm - c;
            set_reg_b(cpu, rd, (uint8_t)(result & 0xFF));
            set_flag(cpu, BIT_C, result < 0);
            set_flag(cpu, BIT_V, ((d ^ imm) & (d ^ result) & 0x80) != 0);
            set_flag(cpu, BIT_H, ((d ^ imm ^ result) & 0x10) != 0);
            if ((result & 0xFF) != 0) set_flag(cpu, BIT_Z, false);
            set_flag(cpu, BIT_N, (result & 0x80) != 0);
            break;
        }
        case 0xC: { /* OR.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int result = get_reg_b(cpu, rd) | imm;
            set_reg_b(cpu, rd, (uint8_t)result); set_flags_logic_b(cpu, result);
            break;
        }
        case 0xD: { /* XOR.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int result = get_reg_b(cpu, rd) ^ imm;
            set_reg_b(cpu, rd, (uint8_t)result); set_flags_logic_b(cpu, result);
            break;
        }
        case 0xE: { /* AND.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int result = get_reg_b(cpu, rd) & imm;
            set_reg_b(cpu, rd, (uint8_t)result); set_flags_logic_b(cpu, result);
            break;
        }
        case 0xF: { /* MOV.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            set_reg_b(cpu, rd, (uint8_t)imm); set_flags_logic_b(cpu, imm);
            break;
        }
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/*
 * Native boot ROM loops shared by the Classic (cyrom112) and the Xtreme
 * (cyrom150), whose ROMs carry the same code at different addresses.
 * Each is guarded by its exact code bytes, runs every pass but the last,
 * and leaves the CPU at the loop with the registers, memory and flags its
 * own passes would have left (tests/hle_verify.sh checks every call).
 *
 * memset: ER3 = dst, R1L = value, ER2 = count.
 *   code: MOV.B R1L,@ER3 / ADDS #1,ER3 / hook: SUBS #1,ER2 / MOV.L ER2,ER2
 *         BGE code
 */
#if CYBIKO_HAS_SPI_FLASH
#define HLE_MEMSET_CODE 0x001744
#define HLE_FILL_LOOP   0x003A26
#define HLE_BLIT_LOOP   0x0049C0
#define HLE_SHCOPY_LOOP 0x004104
#define HLE_SHR_LOOP    0x0049AA
#define HLE_LCD_ROW_LOOP 0x206FA0   /* CyOS 1.2.46, in RAM */
#define HLE_LCD_PORT_REG 1
#else
#define HLE_MEMSET_CODE 0x001200
#define HLE_FILL_LOOP   0x0020C2
#define HLE_BLIT_LOOP   0x00305C
#define HLE_SHCOPY_LOOP 0x0027A0
#define HLE_SHR_LOOP    0x003046
#define HLE_LCD_ROW_LOOP 0x4AE0C8   /* CyOS 1.5.08, in RAM */
#define HLE_LCD_PORT_REG 0
#endif
#define HLE_MEMSET_LOOP (HLE_MEMSET_CODE + 4)
static const uint8_t hle_memset_code[] = {
    0x68,0xB9,0x0B,0x03,0x1B,0x02,0x0F,0xA2,0x4C,0xF6 };

static bool plain_ram(uint32_t a, uint32_t n) {
    return (EXT_RAM_HIT(a) && EXT_RAM_HIT(a + n - 1)) ||
           (ON_CHIP_HIT(a) && ON_CHIP_HIT(a + n - 1));
}

static uint8_t *plain_ptr(address_bus_t *bus, uint32_t a) {
    return EXT_RAM_HIT(a) ? bus->external_ram.data + EXT_RAM_OFF(a)
                          : bus->on_chip_ram.data + ON_CHIP_OFF(a);
}

static uint32_t hle_passes;      /* passes the last native call ran (for the verifier) */

static bool H8S_HOT hle_memset(h8s_cpu_t *cpu) {
    int32_t left = (int32_t)cpu->er[2];          /* bytes still to write */
    if (left < 2 || left > 0x80000)
        return false;
    if (memcmp(cpu->bus->boot_rom.data + HLE_MEMSET_CODE, hle_memset_code, sizeof hle_memset_code) != 0)
        return false;
    uint32_t dst = cpu->er[3] & 0xFFFFFF, k = (uint32_t)left - 1;
    if (!plain_ram(dst, k))
        return false;
    memset(plain_ptr(cpu->bus, dst), (uint8_t)cpu->er[1], k);
    cpu->er[3] += k;
    cpu->er[2] -= k;                             /* 1: the CPU does the last */
    /* a pass ends MOV.B R1L,@ER3 / ADDS: N and Z from R1L, V=0 */
    uint8_t v = (uint8_t)cpu->er[1];
    cpu->ccr = (uint8_t)((cpu->ccr & ~(CCR_N | CCR_Z | CCR_V)) |
                         ((v & 0x80) ? CCR_N : 0) | (v ? 0 : CCR_Z));
    hle_passes = k;
    return true;
}

#if CYBIKO_HAS_SPI_FLASH
/*
 * CyOS 1.2.46 checks the CRC-32 of every flash page at boot, a byte at a
 * time at ~15 instructions a byte: 23% of all boot instructions. When the
 * PC reaches that loop and its code is byte-for-byte the one below, the
 * CRC is computed here and the CPU is left exactly as the loop leaves it.
 * Any other CyOS runs the loop normally.
 *
 *   206BCC: MOV.B @ER1+,R3L / MOV.B #0,R3H / MOV.W @(2,ER0),R2 / XOR.W R3,R2
 *           SHLL.L #2,ER2 / AND.L #0x3FC,ER2 / MOV.L @ER0,ER3 / SHLR.L #2 x4
 *           MOV.L @(0xFFECBC,ER2),ER2 / XOR.L ER3,ER2 / MOV.L ER2,@ER0
 *           MOV.L ER4,ER2 / SUBS #1,ER4 / BNE 206BCC
 * ER0 -> CRC (big-endian longword), ER1 = data, ER4 = bytes - 1,
 * table in on-chip RAM at 0xFFECBC.
 */
#define V1_CRC_LOOP     0x206BCC
#define V1_CRC_LOOP_END 0x206C02
#define V1_CRC_TABLE    0xFFECBC
static const uint8_t v1_crc_code[V1_CRC_LOOP_END - V1_CRC_LOOP] = {
    0x6C,0x1B,0xF3,0x00,0x6F,0x02,0x00,0x02,0x65,0x32,0x10,0x72,0x7A,0x62,
    0x00,0x00,0x03,0xFC,0x01,0x00,0x69,0x03,0x11,0x73,0x11,0x73,0x11,0x73,
    0x11,0x73,0x01,0x00,0x78,0x20,0x6B,0x22,0x00,0xFF,0xEC,0xBC,0x01,0xF0,
    0x65,0x32,0x01,0x00,0x69,0x82,0x0F,0xC2,0x1B,0x04,0x46,0xCA };

static bool H8S_HOT hle_v1_crc_loop(h8s_cpu_t *cpu) {
    address_bus_t *bus = cpu->bus;
    const uint8_t *ram = bus->external_ram.data;
    if (memcmp(ram + EXT_RAM_OFF(V1_CRC_LOOP), v1_crc_code, sizeof v1_crc_code) != 0)
        return false;
    uint32_t crc_at = cpu->er[0] & 0xFFFFFF, p = cpu->er[1] & 0xFFFFFF;
    uint32_t n = cpu->er[4] + 1;
    if (n == 0 || n > 0x10000 || !EXT_RAM_HIT(crc_at) || !EXT_RAM_HIT(crc_at + 3) ||
        !EXT_RAM_HIT(p) || !EXT_RAM_HIT(p + n - 1))
        return false;
    const uint8_t *tab = bus->on_chip_ram.data + ON_CHIP_OFF(V1_CRC_TABLE);
    const uint8_t *c = ram + EXT_RAM_OFF(crc_at);
    uint32_t crc = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | c[3];
    const uint8_t *d = ram + EXT_RAM_OFF(p);
    uint32_t prev = crc;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *t = tab + (((crc ^ d[i]) & 0xFF) << 2);
        prev = crc;
        crc = (((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3]) ^ (crc >> 8);
    }
    uint8_t *w = bus->external_ram.data + EXT_RAM_OFF(crc_at);
    w[0] = (uint8_t)(crc >> 24); w[1] = (uint8_t)(crc >> 16);
    w[2] = (uint8_t)(crc >> 8);  w[3] = (uint8_t)crc;
    cpu->er[1] += n;                 /* MOV.B @ER1+ steps the whole ER1 */
    cpu->er[2] = 0;                  /* MOV.L ER4,ER2 on the last pass */
    cpu->er[3] = prev >> 8;          /* SHLR.L #2 x4 of the last CRC read */
    cpu->er[4] = 0xFFFFFFFFu;        /* SUBS #1 from 0 */
    /* MOV.L ER4(=0),ER2 set N=0 Z=1 V=0. C is the last bit the fourth
     * SHLR.L #2 shifted out: bit 7 of the CRC read on the last pass. */
    cpu->ccr = (cpu->ccr & ~(CCR_N | CCR_V | CCR_C)) | CCR_Z | ((prev & 0x80) ? CCR_C : 0);
    cpu->pc = V1_CRC_LOOP_END;
    return true;
}

/*
 * Boot ROM Reed-Solomon check (cyrom112 0x304E -> 0x3088), Rockliff's
 * decode_rs: CyOS runs it on every radio frame it receives, 5-6 codewords a
 * frame (n = 200: 120 data + 80 parity, or 50 = 30 + 20). The syndrome
 * stage is ~240k guest instructions a codeword, more than the whole T-Deck
 * budget at one frame a second. At 0x30AC (after the prologue) this
 * converts the codeword to log form and computes syndromes 1..nsyn-1
 * natively, writing the same workspace bytes, then hands the CPU the start
 * of the last syndrome pass (0x3138). Everything after that, including
 * error correction for a damaged frame, is the ROM's own code.
 *
 * Frame (ER7): @70 len, @72 data ptr, @86 nsyn, @66 syn_error.
 * Workspace (ER6): +0x286 recd[] in log form (parity first, then data),
 * +0x51+i log of syndrome i. Tables: alpha_to at 0x82C, index_of at 0x92C.
 */
#define V1_RS_START   0x0030AC
#define V1_RS_LASTSYN 0x003138
#define V1_RS_ALPHA   0x082C
#define V1_RS_INDEX   0x092C

static uint32_t rom_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
    return ~c;
}

static bool H8S_HOT hle_v1_rs(h8s_cpu_t *cpu) {
    static int8_t rom_ok = -1;   /* boot ROM checked once */
    address_bus_t *bus = cpu->bus;
    const uint8_t *rom = bus->boot_rom.data;
    if (rom_ok < 0)
        rom_ok = rom_crc32(rom + V1_RS_START, 0x31E8 - V1_RS_START) == 0x92D1EFE7u &&
                 rom_crc32(rom + 0x2EA2, 0x20) == 0xAFA4B8E4u;
    if (!rom_ok)
        return false;

    uint32_t sp = cpu->er[7] & 0xFFFFFF;
    if (!plain_ram(sp, 88))
        return false;
    const uint8_t *fr = plain_ptr(bus, sp);
    uint32_t len  = ((uint32_t)fr[70] << 8) | fr[71];
    uint32_t nsyn = ((uint32_t)fr[86] << 8) | fr[87];
    uint32_t data = (((uint32_t)fr[73] << 16) | ((uint32_t)fr[74] << 8) | fr[75]) & 0xFFFFFF;
    uint32_t n = (fr[71] + fr[87]) & 0xFF;          /* @62: byte add */
    uint32_t ws = cpu->er[6] & 0xFFFFFF;
    if (nsyn < 2 || nsyn > 0x80 || len + nsyn != n || n == 0 ||
        !plain_ram(data, n) || !plain_ram(ws, 0x286 + n) || !plain_ram(ws + 0x51, nsyn + 1))
        return false;

    const uint8_t *d = plain_ptr(bus, data);
    uint8_t *w = plain_ptr(bus, ws);
    const uint8_t *alpha = rom + V1_RS_ALPHA, *index = rom + V1_RS_INDEX;
    uint8_t *recd = w + 0x286;
    for (uint32_t i = 0; i < len; i++)  recd[nsyn + i] = index[d[i]];
    for (uint32_t i = 0; i < nsyn; i++) recd[i] = index[d[len + i]];

    uint8_t syn_error = 0;
    bool any = false;
    for (uint32_t i = 1; i < nsyn; i++) {
        uint8_t sum = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (recd[j] == 0xFF)
                continue;
            any = true;
            uint16_t x = (uint16_t)(recd[j] + (uint16_t)(j * i));   /* MULXU.W, ADD.W */
            while (x > 0xFE) {                                       /* 0x2EA2 */
                x = (uint16_t)(x - 0xFF);
                x = (uint16_t)((x >> 8) + (x & 0xFF));
            }
            sum ^= alpha[x & 0xFF];
        }
        syn_error |= sum;
        w[0x51 + i] = index[sum];
    }
    uint8_t *frw = plain_ptr(bus, sp);
    frw[66] = syn_error;
    if (any) {   /* MOV.L ER1,@(32,ER7) before each 0x2EA2 call */
        uint32_t v = (cpu->er[1] & 0xFFFF0000u) | (nsyn - 1);
        frw[32] = (uint8_t)(v >> 24); frw[33] = (uint8_t)(v >> 16);
        frw[34] = (uint8_t)(v >> 8);  frw[35] = (uint8_t)v;
    }
    cpu->er[0] = cpu->er[6] + nsyn - 1;                  /* E0 as loop 2 left it */
    cpu->er[1] = (cpu->er[1] & 0xFFFF0000u) | nsyn;      /* syndrome index */
    cpu->pc = V1_RS_LASTSYN;
    return true;
}

#endif   /* CYBIKO_HAS_SPI_FLASH */

/*
 * The two boot ROM drawing loops the idle Classic desktop spends most of
 * its time in (26% of its instructions, measured 27 Oct 2026). Both run
 * every pass but the last natively and leave the CPU at the loop top, so
 * the last pass is the CPU's own. Registers, memory and flags after the
 * native passes are what the CPU's passes leave (tests/hle_verify.sh
 * checks every call against the CPU).
 *
 * Span fill (cyrom112 0x3A26), inside the function at 0x39AC:
 *   3A26: ADDS #1,ER0 / SUBS #4,ER4 / MOV.W R4,R4 / BLE 3A32
 *         MOV.B R1L,@ER0 / BRA 3A26
 * A pass that continues writes R1L to the next byte.
 *
 * 1-bit to 2-bit blit (cyrom112 0x49C0), inside the function at 0x491A.
 * Per pixel: R1 holds source bits (bit 8 is the pixel), R3 the colour
 * pattern (R3H mask, R3L bits), R0L the destination byte being built.
 * Locals on the stack: @(12) destination pointer, @(18) pattern reload,
 * @(20) passes left - 1, @(34) bit position. ER6 walks the source.
 *   49C0: SHLL.W R1 / BTST #0,R1H / BEQ 49D2
 *         R0L = (R0L & ~R3H) | R3L             (via R2: R2 = ~R3H & 0xFF)
 *   49D2: @(34) += 1                           (MOV.W / ADDS #1,ER5 / MOV.W)
 *         SHLR.W #2,R3 / R2 = R3 >> 8 / BNE 49FE
 *         @(12) byte = R0L, @(12) += 1, R0L = byte at @(12), R3 = @(18)
 *   49FE: R2 = @(34) & 7 / BNE 4A10
 *         ER6 += 1, R1 = byte at ER6
 *   4A10: R2 = @(20) / ER4 = (E4:R2) - 1 / @(20) = R4 / BGT 49C0
 */
static const uint8_t hle_fill_code[] = {
    0x0B,0x00,0x1B,0x94,0x0D,0x44,0x4F,0x04,0x68,0x89,0x40,0xF4 };
static const uint8_t hle_blit_code[] = {
    0x10,0x11,0x73,0x01,0x47,0x0C,0x0D,0x32,0x0C,0x2A,0x18,0x22,
    0x17,0x0A,0x16,0xA8,0x14,0xB8,0x6F,0x75,0x00,0x22,0x0B,0x05,
    0x6F,0xF5,0x00,0x22,0x11,0x53,0x0D,0x32,0x0C,0x2A,0x18,0x22,
    0x0D,0x22,0x46,0x16,0x01,0x00,0x6F,0x75,0x00,0x0C,0x68,0xD8,
    0x0B,0x05,0x01,0x00,0x6F,0xF5,0x00,0x0C,0x68,0x58,0x6F,0x73,
    0x00,0x12,0x6F,0x72,0x00,0x22,0xEA,0x07,0xE2,0x00,0x0D,0x22,
    0x46,0x06,0x0B,0x06,0x68,0x69,0xF1,0x00,0x6F,0x72,0x00,0x14,
    0x0D,0x24,0x1B,0x04,0x6F,0xF4,0x00,0x14,0x0D,0x22,0x4E,0xA0 };
#define HLE_MAX_PASSES 4096   /* then the CPU carries on from the loop top */

static bool H8S_HOT hle_fill(h8s_cpu_t *cpu) {
    int16_t r4 = (int16_t)cpu->er[4];
    if (r4 <= 8)                                 /* fewer than two passes left */
        return false;
    if (memcmp(cpu->bus->boot_rom.data + HLE_FILL_LOOP, hle_fill_code, sizeof hle_fill_code) != 0)
        return false;
    uint32_t k = (uint32_t)(r4 - 1) / 4;         /* passes that write */
    if (k > HLE_MAX_PASSES) k = HLE_MAX_PASSES;
    uint32_t dst = (cpu->er[0] + 1) & 0xFFFFFF;
    if (!plain_ram(dst, k))
        return false;
    memset(plain_ptr(cpu->bus, dst), (uint8_t)cpu->er[1], k);
    cpu->er[0] += k;
    cpu->er[4] -= 4 * k;                         /* R4 stays above 0: no borrow */
    /* MOV.B R1L,@ER0 ends every pass that continues: N and Z from R1L, V=0 */
    uint8_t v = (uint8_t)cpu->er[1];
    cpu->ccr = (uint8_t)((cpu->ccr & ~(CCR_N | CCR_Z | CCR_V)) |
                         ((v & 0x80) ? CCR_N : 0) | (v ? 0 : CCR_Z));
    hle_passes = k;
    return true;
}

static bool H8S_HOT hle_blit(h8s_cpu_t *cpu) {
    address_bus_t *bus = cpu->bus;
    uint32_t sp = cpu->er[7] & 0xFFFFFF;
    if (!plain_ram(sp + 12, 24))
        return false;
    uint8_t *fr = plain_ptr(bus, sp);
    int16_t left = (int16_t)((fr[20] << 8) | fr[21]);
    if (left <= 0)                               /* this is the last pass */
        return false;
    if (memcmp(bus->boot_rom.data + HLE_BLIT_LOOP, hle_blit_code, sizeof hle_blit_code) != 0)
        return false;
    uint32_t n = (uint32_t)left;                 /* every pass but the last */
    if (n > HLE_MAX_PASSES) n = HLE_MAX_PASSES;
    uint32_t dst = (((uint32_t)fr[12] << 24) | ((uint32_t)fr[13] << 16) |
                    ((uint32_t)fr[14] << 8) | fr[15]);
    uint32_t src = cpu->er[6];
    /* n passes store at most n/4 + 1 bytes and read one past them, and
     * read at most n/8 + 1 source bytes */
    if (!plain_ram(dst & 0xFFFFFF, n / 4 + 2) || !plain_ram((src + 1) & 0xFFFFFF, n / 8 + 1))
        return false;

    uint32_t r0l = cpu->er[0] & 0xFF, r1 = cpu->er[1] & 0xFFFF, r2 = cpu->er[2] & 0xFFFF;
    uint32_t r3 = cpu->er[3] & 0xFFFF, er4 = cpu->er[4], er5 = cpu->er[5];
    uint16_t pos = (uint16_t)((fr[34] << 8) | fr[35]);
    uint16_t reload = (uint16_t)((fr[18] << 8) | fr[19]);
    for (uint32_t i = 0; i < n; i++) {
        r1 = (r1 << 1) & 0xFFFF;
        if (r1 & 0x100) {
            r2 = ~(r3 >> 8) & 0xFF;
            r0l = (r0l & r2) | (r3 & 0xFF);
        }
        er5 = ((er5 & 0xFFFF0000u) | pos) + 1;   /* ADDS #1,ER5 carries into E5 */
        pos = (uint16_t)er5;
        r3 >>= 2;
        r2 = r3 >> 8;
        if (r2 == 0) {
            uint8_t *d = plain_ptr(bus, dst & 0xFFFFFF);
            d[0] = (uint8_t)r0l;
            er5 = ++dst;
            r0l = d[1];
            r3 = reload;
        }
        r2 = pos & 7;
        if (r2 == 0) {
            src++;
            r1 = *plain_ptr(bus, src & 0xFFFFFF);
        }
        r2 = (uint16_t)left;
        er4 = ((er4 & 0xFFFF0000u) | r2) - 1;    /* SUBS #1,ER4 borrows from E4 */
        left = (int16_t)er4;
    }
    fr[12] = (uint8_t)(dst >> 24); fr[13] = (uint8_t)(dst >> 16);
    fr[14] = (uint8_t)(dst >> 8);  fr[15] = (uint8_t)dst;
    fr[20] = (uint8_t)((uint16_t)left >> 8); fr[21] = (uint8_t)left;
    fr[34] = (uint8_t)(pos >> 8); fr[35] = (uint8_t)pos;
    cpu->er[0] = (cpu->er[0] & ~0xFFu) | r0l;
    cpu->er[1] = (cpu->er[1] & 0xFFFF0000u) | r1;
    cpu->er[2] = (cpu->er[2] & 0xFFFF0000u) | r2;
    cpu->er[3] = (cpu->er[3] & 0xFFFF0000u) | r3;
    cpu->er[4] = er4;
    cpu->er[5] = er5;
    cpu->er[6] = src;
    /* SUB.B R2H,R2H at 49E2 cleared H and C; MOV.W R2,R2 at 4A1C set N
     * and Z from R2 (> 0 on every native pass) and cleared V */
    cpu->ccr &= ~(CCR_H | CCR_C | CCR_N | CCR_Z | CCR_V);
    hle_passes = n;
    return true;
}

/*
 * Shifted byte copy (Classic 0x4104, Xtreme 0x27A0): text not on a byte
 * boundary. Each pass shifts the next source byte left by @(27) bits
 * (one bit a pass of the loop at +0x32), ORs its high byte into the low
 * byte left by the previous one, and stores it.
 *   +00: R0L = @(15) / R3 = @(46) - @(24) -> @(46) / BLE out
 *   +18: ER5 = @(32) + 1 -> @(32) / R3 = byte at ER5 / R4L = @(27)
 *        R5L = R4L / BLE / SHLL.W R3, R5L times
 *   +38: @(14) = R3 / R2 = R3 >> 8 / R0L |= R2L
 *   +46: R4 = @(46) - @(26) -> @(46) / BLE out / byte at ER6 = R0L, ER6+1
 * A pass that would leave by either BLE is the CPU's.
 */
static const uint8_t hle_shcopy_code[] = {
    0x6E,0x78,0x00,0x0F,0x6F,0x73,0x00,0x2E,0x6F,0x74,0x00,0x18,0x19,0x43,
    0x6F,0xF3,0x00,0x2E,0x0D,0x33,0x58,0xF0,0x02,0xCE,0x01,0x00,0x6F,0x75,
    0x00,0x20,0x0B,0x05,0x01,0x00,0x6F,0xF5,0x00,0x20,0x68,0x5B,0xF3,0x00,
    0x6E,0x7C,0x00,0x1B,0x0C,0xCD,0x4F,0x06,0x10,0x13,0x8D,0xFF,0x46,0xFA,
    0x6F,0xF3,0x00,0x0E,0x6F,0x72,0x00,0x0E,0x0C,0x2A,0x18,0x22,0x14,0xA8,
    0x6F,0x74,0x00,0x2E,0x6F,0x75,0x00,0x1A,0x19,0x54,0x6F,0xF4,0x00,0x2E,
    0x0D,0x44,0x58,0xF0,0x02,0x8C,0x68,0xE8,0x0B,0x06,0x40,0xA0 };

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static bool H8S_HOT hle_shcopy(h8s_cpu_t *cpu) {
    address_bus_t *bus = cpu->bus;
    uint32_t sp = cpu->er[7] & 0xFFFFFF;
    if (!plain_ram(sp, 48))
        return false;
    if (memcmp(bus->boot_rom.data + HLE_SHCOPY_LOOP, hle_shcopy_code, sizeof hle_shcopy_code) != 0)
        return false;
    uint8_t *fr = plain_ptr(bus, sp);
    uint32_t er0 = cpu->er[0], er2 = cpu->er[2], er3 = cpu->er[3], er4 = cpu->er[4];
    uint32_t er5 = cpu->er[5], er6 = cpu->er[6];
    uint16_t sub = 0, after = 0;
    uint32_t n = 0;
    for (; n < HLE_MAX_PASSES; n++) {
        uint16_t w24 = be16(fr + 24), w26 = be16(fr + 26);
        uint16_t r3 = (uint16_t)(be16(fr + 46) - w24);
        if ((int16_t)r3 <= 0)
            break;
        uint16_t r4 = (uint16_t)(r3 - w26);
        if ((int16_t)r4 <= 0)
            break;
        uint32_t src = (((uint32_t)fr[32] << 24) | ((uint32_t)fr[33] << 16) |
                        ((uint32_t)fr[34] << 8) | fr[35]) + 1;
        if (!plain_ram(src & 0xFFFFFF, 1) || !plain_ram(er6 & 0xFFFFFF, 1))
            break;
        uint8_t r0l = fr[15];
        uint8_t cnt = fr[27];
        uint16_t v = *plain_ptr(bus, src & 0xFFFFFF);
        if ((int8_t)cnt > 0)
            v = (uint16_t)(v << cnt);
        fr[32] = (uint8_t)(src >> 24); fr[33] = (uint8_t)(src >> 16);
        fr[34] = (uint8_t)(src >> 8);  fr[35] = (uint8_t)src;
        put_be16(fr + 14, v);
        put_be16(fr + 46, r4);
        r0l |= (uint8_t)(v >> 8);
        *plain_ptr(bus, er6 & 0xFFFFFF) = r0l;
        er6++;
        er0 = (er0 & ~0xFFu) | r0l;
        er2 = (er2 & 0xFFFF0000u) | (uint16_t)(v >> 8);
        er3 = (er3 & 0xFFFF0000u) | v;
        er4 = (er4 & 0xFFFF0000u) | r4;
        er5 = (src & 0xFFFF0000u) | w26;         /* MOV.L @(32) then MOV.W @(26),R5 */
        sub = r3;
        after = w26;
    }
    if (n == 0)
        return false;
    cpu->er[0] = er0; cpu->er[2] = er2; cpu->er[3] = er3;
    cpu->er[4] = er4; cpu->er[5] = er5; cpu->er[6] = er6;
    /* SUB.W R5,R4 (r3 - @(26)) set H and C; MOV.B R0L,@ER6 then set N and
     * Z from R0L and cleared V */
    uint8_t r0l = (uint8_t)er0;
    cpu->ccr = (uint8_t)((cpu->ccr & ~(CCR_H | CCR_N | CCR_Z | CCR_V | CCR_C)) |
                         (((sub & 0xFFF) < (after & 0xFFF)) ? CCR_H : 0) |
                         (sub < after ? CCR_C : 0) |
                         ((r0l & 0x80) ? CCR_N : 0) | (r0l ? 0 : CCR_Z));
    hle_passes = n;
    return true;
}

/*
 * Pre-blit shift (Classic 0x49AA, Xtreme 0x3046):
 *   SHLR.W R3 / ADD.B #0xFF,R4L / BNE back      (R3 >>= R4L)
 */
static const uint8_t hle_shr_code[] = { 0x11,0x13,0x8C,0xFF,0x46,0xFA };

static bool H8S_HOT hle_shr(h8s_cpu_t *cpu) {
    uint8_t cnt = (uint8_t)cpu->er[4];
    if (cnt < 2)
        return false;
    if (memcmp(cpu->bus->boot_rom.data + HLE_SHR_LOOP, hle_shr_code, sizeof hle_shr_code) != 0)
        return false;
    uint32_t k = cnt - 1u;
    uint16_t r3 = (uint16_t)cpu->er[3];
    r3 = k >= 16 ? 0 : (uint16_t)(r3 >> k);
    cpu->er[3] = (cpu->er[3] & 0xFFFF0000u) | r3;
    cpu->er[4] = (cpu->er[4] & 0xFFFFFF00u) | 1u;
    /* ADD.B #0xFF to 2 left 1: carry and half carry out, N=Z=V=0 */
    cpu->ccr = (uint8_t)((cpu->ccr & ~(CCR_H | CCR_N | CCR_Z | CCR_V | CCR_C)) | CCR_H | CCR_C);
    hle_passes = k;
    return true;
}

/*
 * CyOS LCD row copy (Classic 0x206FA0, port ER1; Xtreme 0x4AE0C8, port
 * ER0), done natively for every pass but the last.
 *   MOV.B @ER5+,R2L / MOV.B R2L,@port / ADDS #1,ER3 / CMP.W #0x27,R3 / BLS
 * ER5 = src, R3 = column. The writes go through the bus to the LCD.
 */
static const uint8_t hle_lcd_row_code[] = {
    0x6C,0x5A,0x68,(uint8_t)(0x8A | (HLE_LCD_PORT_REG << 4)),0x0B,0x03,0x79,0x23,0x00,0x27,0x43,0xF4 };

static bool H8S_HOT hle_lcd_row(h8s_cpu_t *cpu) {
    uint16_t col = (uint16_t)cpu->er[3];
    if (col > 0x26)                              /* fewer than two passes left */
        return false;
    const uint8_t *ram = cpu->bus->external_ram.data;
    if (memcmp(ram + EXT_RAM_OFF(HLE_LCD_ROW_LOOP), hle_lcd_row_code, sizeof hle_lcd_row_code) != 0)
        return false;
    uint32_t src = cpu->er[5] & 0xFFFFFF, k = 0x27 - col;  /* passes but the last */
    if (!EXT_RAM_HIT(src) || !EXT_RAM_HIT(src + k - 1))
        return false;
    uint32_t port = cpu->er[HLE_LCD_PORT_REG] & 0xFFFFFF;
    const uint8_t *p = ram + EXT_RAM_OFF(src);
    for (uint32_t i = 0; i < k; i++)
        bus_write8(cpu->bus, port, p[i]);
    cpu->er[5] += k;
    cpu->er[3] = (cpu->er[3] & 0xFFFF0000u) | (uint16_t)(col + k);
    cpu->er[2] = (cpu->er[2] & 0xFFFFFF00u) | p[k - 1];
    return true;
}

/* Low byte of each hooked PC, so the run loop pays one table load */
static const uint8_t hle_lo[256] = {
    [HLE_MEMSET_LOOP & 0xFF] = 1, [HLE_FILL_LOOP & 0xFF] = 1, [HLE_BLIT_LOOP & 0xFF] = 1,
    [HLE_SHCOPY_LOOP & 0xFF] = 1, [HLE_SHR_LOOP & 0xFF] = 1, [HLE_LCD_ROW_LOOP & 0xFF] = 1,
#if CYBIKO_HAS_SPI_FLASH
    [V1_CRC_LOOP & 0xFF] = 1, [V1_RS_START & 0xFF] = 1,
#endif
};

static bool H8S_HOT hle_native(h8s_cpu_t *cpu) {
    switch (cpu->pc) {
    case HLE_MEMSET_LOOP: return hle_memset(cpu);
    case HLE_FILL_LOOP:   return hle_fill(cpu);
    case HLE_BLIT_LOOP:   return hle_blit(cpu);
    case HLE_SHCOPY_LOOP: return hle_shcopy(cpu);
    case HLE_SHR_LOOP:    return hle_shr(cpu);
    case HLE_LCD_ROW_LOOP: return hle_lcd_row(cpu);
#if CYBIKO_HAS_SPI_FLASH
    case V1_CRC_LOOP:     return hle_v1_crc_loop(cpu);
    case V1_RS_START:     return hle_v1_rs(cpu);
#endif
    }
    return false;
}

#ifdef CYBIKO_HLE_VERIFY
/*
 * Test builds (-DCYBIKO_HLE_VERIFY): each memset, fill, blit, shifted copy
 * and shift call is checked.
 * The native result is kept aside, the machine is rewound, and the CPU
 * runs the same number of passes (arrivals back at the loop top); every
 * register, the PC, the flags, and all of RAM must match. The CPU's result is the one kept.
 */
uint32_t cybiko_hle_checks, cybiko_hle_mismatches;

static bool hle(h8s_cpu_t *cpu) {
    uint32_t pc = cpu->pc;
    if (pc != HLE_FILL_LOOP && pc != HLE_BLIT_LOOP && pc != HLE_MEMSET_LOOP
        && pc != HLE_SHCOPY_LOOP && pc != HLE_SHR_LOOP && pc != HLE_LCD_ROW_LOOP)
        return hle_native(cpu);
    address_bus_t *bus = cpu->bus;
    static uint8_t ext0[CYBIKO_EXT_RAM_SIZE], chip0[0x4000];
    static uint8_t ext1[CYBIKO_EXT_RAM_SIZE], chip1[0x4000];
    size_t chip_n = bus->on_chip_ram.size < sizeof chip0 ? bus->on_chip_ram.size : sizeof chip0;
    h8s_cpu_t before = *cpu;
    static hd66421_t lcd0, lcd1;   /* the LCD row writes the controller */
    if (bus->lcd) lcd0 = *bus->lcd;
    memcpy(ext0, bus->external_ram.data, sizeof ext0);
    memcpy(chip0, bus->on_chip_ram.data, chip_n);
    if (!hle_native(cpu))
        return false;
    h8s_cpu_t native = *cpu;
    uint32_t passes = pc == HLE_LCD_ROW_LOOP ? 0x27 - (uint16_t)before.er[3] : hle_passes;
    if (bus->lcd) { lcd1 = *bus->lcd; *bus->lcd = lcd0; }
    memcpy(ext1, bus->external_ram.data, sizeof ext1);
    memcpy(chip1, bus->on_chip_ram.data, chip_n);
    *cpu = before;
    memcpy(bus->external_ram.data, ext0, sizeof ext0);
    memcpy(bus->on_chip_ram.data, chip0, chip_n);
    for (uint32_t arrivals = 0; arrivals < passes; ) {
        decode(cpu, fetch16(cpu));
        if (cpu->pc == pc) arrivals++;
    }
    cybiko_hle_checks++;
    bool same = memcmp(native.er, cpu->er, sizeof cpu->er) == 0 && native.pc == cpu->pc
             && native.ccr == cpu->ccr
             && memcmp(ext1, bus->external_ram.data, sizeof ext1) == 0
             && memcmp(chip1, bus->on_chip_ram.data, chip_n) == 0
             && (!bus->lcd || (memcmp(lcd1.vram, bus->lcd->vram, sizeof lcd1.vram) == 0
                               && memcmp(lcd1.regs, bus->lcd->regs, sizeof lcd1.regs) == 0
                               && lcd1.x_addr == bus->lcd->x_addr && lcd1.y_addr == bus->lcd->y_addr));
    if (!same) {
        if (cybiko_hle_mismatches++ < 5) {
            fprintf(stderr, "HLE mismatch at %06X after %u passes:", pc, (unsigned)passes);
            for (int i = 0; i < 8; i++)
                if (native.er[i] != cpu->er[i])
                    fprintf(stderr, " ER%d %08X/%08X", i, (unsigned)native.er[i], (unsigned)cpu->er[i]);
            if (bus->lcd && memcmp(lcd1.vram, bus->lcd->vram, sizeof lcd1.vram))
                fprintf(stderr, " LCD VRAM differs");
            if (bus->lcd && (lcd1.x_addr != bus->lcd->x_addr || lcd1.y_addr != bus->lcd->y_addr))
                fprintf(stderr, " LCD x,y %u,%u/%u,%u", lcd1.x_addr, lcd1.y_addr,
                        bus->lcd->x_addr, bus->lcd->y_addr);
            if (native.pc != cpu->pc)
                fprintf(stderr, " PC %06X/%06X", (unsigned)native.pc, (unsigned)cpu->pc);
            fprintf(stderr, " CCR %02X/%02X SP %08X", native.ccr, cpu->ccr, (unsigned)cpu->er[7]);
            for (size_t i = 0; i < chip_n; i++)
                if (chip1[i] != bus->on_chip_ram.data[i]) {
                    fprintf(stderr, " chip %06X %02X/%02X", (unsigned)(CYBIKO_ON_CHIP_BASE + i),
                            chip1[i], bus->on_chip_ram.data[i]);
                    break;
                }
            for (size_t i = 0; i < sizeof ext1; i++)
                if (ext1[i] != bus->external_ram.data[i]) {
                    fprintf(stderr, " RAM %06X %02X/%02X", (unsigned)(CYBIKO_EXT_RAM_BASE + i),
                            ext1[i], bus->external_ram.data[i]);
                    break;
                }
            fprintf(stderr, "\n");
        }
    }
    cpu->cycle_count += passes;   /* keep counting roughly as the native path */
    return true;
}
#else
#define hle hle_native
#endif

/* ======== Interrupt handling ======== */
static bool H8S_HOT process_interrupts(h8s_cpu_t *cpu) {
    if (cpu->pending_irq_count == 0) return false;
    if (get_flag(cpu, BIT_I)) return false;

    int vector = cpu->pending_irqs[0];
    cpu->pending_irq_count--;
    if (cpu->pending_irq_count > 0)
        memmove(&cpu->pending_irqs[0], &cpu->pending_irqs[1],
                (size_t)cpu->pending_irq_count * sizeof(int));

    cpu->er[7] -= 4;
    bus_write32_fast(cpu->bus, cpu->er[7], cpu->pc);
    cpu->er[7] -= 2;
    bus_write16_fast(cpu->bus, cpu->er[7], (uint16_t)cpu->ccr);

    set_flag(cpu, BIT_I, true);

    uint32_t handler = bus_read32_fast(cpu->bus, (uint32_t)vector * 4) & 0xFFFFFF;
    cpu->pc = handler;

    if (cpu->halted) cpu->halted = false;
    return true;
}

/* ======== Public API ======== */

void h8s_cpu_init(h8s_cpu_t *cpu, address_bus_t *bus) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->bus = bus;
    cpu->ccr = 0x80;
}

void h8s_cpu_reset(h8s_cpu_t *cpu) {
    for (int i = 0; i < 8; i++) cpu->er[i] = 0;
    cpu->ccr = 0x80;
    cpu->exr = 0;
    cpu->halted = false;
    cpu->irq_hold = false;
    cpu->cycle_count = 0;
    cpu->pending_irq_count = 0;
    cpu->pc = bus_read32_fast(cpu->bus, 0x000000) & 0xFFFFFF;
}

void H8S_HOT h8s_cpu_run(h8s_cpu_t *cpu, int n) {
    /* Executed instructions are counted in a register and added once:
     * the 64-bit memory increment per instruction was a measurable part
     * of the ~90 host cycles a guest NOP costs on the ESP32-S3. */
    int done = 0;
    for (int i = 0; i < n; i++) {
        /* After LDC/ANDC/ORC/XORC to CCR the H8S takes no interrupt before
         * the next instruction. CyOS's scheduler re-enables interrupts with
         * ANDC #0x7F one instruction before it loads the new task's SP; an
         * interrupt in between made the tick handler's stack check see the
         * old SP against the new task, take its overflow path, and leave
         * every CyOS timer stopped (the Classic froze at the desktop). */
        bool hold = cpu->irq_hold;
        cpu->irq_hold = false;
        if (cpu->pending_irq_count && !hold && process_interrupts(cpu)) {
            done++;
            continue;
        }
        if (cpu->halted) {
            if (cpu->pending_irq_count > 0)
                cpu->halted = false;
            else
                break;  /* cycle_count counts executed instructions only */
        }
        if (hle_lo[cpu->pc & 0xFF] && hle(cpu)) {
            done++;
            continue;
        }
        uint16_t op = fetch16(cpu);
        decode(cpu, op);
        done++;
    }
    cpu->cycle_count += (uint64_t)done;
}

void H8S_HOT h8s_cpu_step(h8s_cpu_t *cpu) {
    h8s_cpu_run(cpu, 1);
}

void h8s_cpu_request_interrupt(h8s_cpu_t *cpu, int vector) {
    for (int i = 0; i < cpu->pending_irq_count; i++) {
        if (cpu->pending_irqs[i] == vector) return;
        if (cpu->pending_irqs[i] > vector) {
            if (cpu->pending_irq_count < MAX_PENDING_IRQS) {
                memmove(&cpu->pending_irqs[i + 1], &cpu->pending_irqs[i],
                        (size_t)(cpu->pending_irq_count - i) * sizeof(int));
                cpu->pending_irqs[i] = vector;
                cpu->pending_irq_count++;
            }
            return;
        }
    }
    if (cpu->pending_irq_count < MAX_PENDING_IRQS) {
        cpu->pending_irqs[cpu->pending_irq_count++] = vector;
    }
}

uint32_t h8s_cpu_get_er(const h8s_cpu_t *cpu, int n) { return cpu->er[n]; }
void h8s_cpu_set_er(h8s_cpu_t *cpu, int n, uint32_t value) { cpu->er[n] = value; }

void h8s_cpu_dump_registers(const h8s_cpu_t *cpu) {
    printf("=== CPU Registers ===\n");
    for (int i = 0; i < 8; i++) {
        printf("  ER%d = 0x%08X  (E%d=0x%04X R%d=0x%04X R%dH=0x%02X R%dL=0x%02X)\n",
            i, cpu->er[i], i, get_e(cpu, i), i, get_r(cpu, i),
            i, get_rh(cpu, i), i, get_rl(cpu, i));
    }
    printf("  PC  = 0x%06X\n", cpu->pc);
    printf("  CCR = 0x%02X [%s%s%s%s%s%s%s%s]\n", cpu->ccr,
        get_flag(cpu, BIT_I) ? "I" : "-", get_flag(cpu, BIT_UI) ? "U" : "-",
        get_flag(cpu, BIT_H) ? "H" : "-", get_flag(cpu, BIT_U) ? "u" : "-",
        get_flag(cpu, BIT_N) ? "N" : "-", get_flag(cpu, BIT_Z) ? "Z" : "-",
        get_flag(cpu, BIT_V) ? "V" : "-", get_flag(cpu, BIT_C) ? "C" : "-");
    printf("  EXR = 0x%02X\n", cpu->exr);
    printf("  Cycles: %llu\n", (unsigned long long)cpu->cycle_count);
}
