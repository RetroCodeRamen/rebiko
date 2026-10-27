/*
 * Hitachi H8S/2323 CPU emulator core.
 * Ported from H8SCpu.java.
 */
#include "core/h8s_cpu.h"
#include <stdio.h>
#include <string.h>

/* Forward declarations for bus functions */
extern uint8_t  bus_read8(address_bus_t *bus, uint32_t address);
extern uint16_t bus_read16(address_bus_t *bus, uint32_t address);
extern uint32_t bus_read32(address_bus_t *bus, uint32_t address);
extern void     bus_write8(address_bus_t *bus, uint32_t address, uint8_t value);
extern void     bus_write16(address_bus_t *bus, uint32_t address, uint16_t value);
extern void     bus_write32(address_bus_t *bus, uint32_t address, uint32_t value);

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
    set_flag(cpu, BIT_N, (result & 0x80) != 0);
    set_flag(cpu, BIT_Z, (result & 0xFF) == 0);
}
static inline void set_nz_w(h8s_cpu_t *cpu, int result) {
    set_flag(cpu, BIT_N, (result & 0x8000) != 0);
    set_flag(cpu, BIT_Z, (result & 0xFFFF) == 0);
}
static inline void set_nz_l(h8s_cpu_t *cpu, int32_t result) {
    set_flag(cpu, BIT_N, (result & (int32_t)0x80000000) != 0);
    set_flag(cpu, BIT_Z, result == 0);
}

/* ---- Fetch helpers ---- */
static inline uint16_t fetch16(h8s_cpu_t *cpu) {
    uint16_t val = bus_read16(cpu->bus, cpu->pc);
    cpu->pc = (cpu->pc + 2) & 0xFFFFFF;
    return val;
}
static inline uint32_t fetch32(h8s_cpu_t *cpu) {
    uint16_t hi = fetch16(cpu);
    uint16_t lo = fetch16(cpu);
    return ((uint32_t)hi << 16) | lo;
}

/* ---- Branch condition evaluator ---- */
static bool evaluate_condition(const h8s_cpu_t *cpu, int cond) {
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
static void unimplemented(h8s_cpu_t *cpu, int op, uint32_t addr) {
    fprintf(stderr, "Unimplemented opcode 0x%04X at PC=0x%06X\n", op, addr);
    (void)cpu;
}

/* ---- Shift helpers ---- */
static void shift_b(h8s_cpu_t *cpu, int rd, bool left, bool logical, int count) {
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

static void shift_w(h8s_cpu_t *cpu, int rd, bool left, bool logical, int count) {
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

static void shift_l(h8s_cpu_t *cpu, int erd, bool left, bool logical, int count) {
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
static void rotate_b(h8s_cpu_t *cpu, int rd, bool left, bool through_carry, int count) {
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

static void rotate_w(h8s_cpu_t *cpu, int rd, bool left, bool through_carry, int count) {
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

static void rotate_l(h8s_cpu_t *cpu, int erd, bool left, bool through_carry, int count) {
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
static void execute_bit_op_at_address(h8s_cpu_t *cpu, uint32_t addr, uint16_t bit_op) {
    int op_type = (bit_op >> 8) & 0xFF;
    int bit_num = (bit_op >> 4) & 0x07;
    int val = bus_read8(cpu->bus, addr);

    switch (op_type) {
        case 0x70: bus_write8(cpu->bus, addr, (uint8_t)(val | (1 << bit_num))); break;
        case 0x71: bus_write8(cpu->bus, addr, (uint8_t)(val ^ (1 << bit_num))); break;
        case 0x72: bus_write8(cpu->bus, addr, (uint8_t)(val & ~(1 << bit_num))); break;
        case 0x73: set_flag(cpu, BIT_Z, (val & (1 << bit_num)) == 0); break;
        case 0x74: if ((val & (1 << bit_num)) != 0) set_flag(cpu, BIT_C, true); break;
        case 0x75: if ((val & (1 << bit_num)) != 0) set_flag(cpu, BIT_C, !get_flag(cpu, BIT_C)); break;
        case 0x76: if ((val & (1 << bit_num)) == 0) set_flag(cpu, BIT_C, false); break;
        case 0x77: set_flag(cpu, BIT_C, (val & (1 << bit_num)) != 0); break;
        case 0x67: {
            if (get_flag(cpu, BIT_C)) val |= (1 << bit_num);
            else val &= ~(1 << bit_num);
            bus_write8(cpu->bus, addr, (uint8_t)val);
            break;
        }
        case 0x60: {
            int rn = (bit_op >> 4) & 0xF;
            int bit = get_reg_b(cpu, rn) & 0x7;
            bus_write8(cpu->bus, addr, (uint8_t)(val | (1 << bit)));
            break;
        }
        case 0x61: {
            int rn = (bit_op >> 4) & 0xF;
            int bit = get_reg_b(cpu, rn) & 0x7;
            bus_write8(cpu->bus, addr, (uint8_t)(val ^ (1 << bit)));
            break;
        }
        case 0x62: {
            int rn = (bit_op >> 4) & 0xF;
            int bit = get_reg_b(cpu, rn) & 0x7;
            bus_write8(cpu->bus, addr, (uint8_t)(val & ~(1 << bit)));
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
static void decode(h8s_cpu_t *cpu, uint16_t op);

/* ---- MOV.L with absolute address (6B_32) ---- */
static void decode6B_32(h8s_cpu_t *cpu, uint16_t op2) {
    int lo2 = op2 & 0xFF;
    bool write = (lo2 & 0x80) != 0;
    bool addr24 = (lo2 & 0x20) != 0;
    int rd = lo2 & 0x7;
    uint32_t addr;
    if (addr24) addr = fetch32(cpu) & 0xFFFFFF;
    else { addr = (uint32_t)(int32_t)(int16_t)fetch16(cpu); addr &= 0xFFFFFF; }

    if (!write) {
        cpu->er[rd] = bus_read32(cpu->bus, addr);
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_V, false);
    } else {
        bus_write32(cpu->bus, addr, cpu->er[rd]);
        set_nz_l(cpu, (int32_t)cpu->er[rd]);
        set_flag(cpu, BIT_V, false);
    }
}

/* ---- MULXS ---- */
static void decode_mulxs(h8s_cpu_t *cpu, int hi2, uint16_t op2) {
    int rs = (op2 >> 4) & 0xF;
    int rd = op2 & 0xF;
    if (hi2 == 0x50) {
        int result = (int8_t)get_reg_b(cpu, rs) * (int8_t)(get_r(cpu, rd) & 0xFF);   /* as MULXU.B */
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
static void decode_divxs(h8s_cpu_t *cpu, int hi2, uint16_t op2) {
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
static void decode0141(h8s_cpu_t *cpu, uint16_t op2) {
    cpu->irq_hold = true;   /* ORC/XORC/ANDC/LDC #imm, EXR */
    int hi2 = (op2 >> 8) & 0xFF;
    switch (hi2) {
        case 0x04: cpu->exr |= (op2 & 0xFF); break;
        case 0x05: cpu->exr ^= (op2 & 0xFF); break;
        case 0x06: cpu->exr &= (op2 & 0xFF); break;
        case 0x07: cpu->exr = (uint8_t)(op2 & 0xFF); break;
        default: unimplemented(cpu, 0x0141, cpu->pc - 2); break;
    }
}

/* ---- decode0140: LDC/STC CCR word-width ---- */
static void decode0140(h8s_cpu_t *cpu, uint16_t op2) {
    if ((op2 & 0x80) == 0)
        cpu->irq_hold = true;   /* LDC @mem, CCR (bit 7 set = STC) */
    int hi2 = (op2 >> 8) & 0xFF;
    switch (hi2) {
        case 0x69: {
            int r = (op2 >> 4) & 0x7;
            if ((op2 & 0x80) == 0) cpu->ccr = (uint8_t)(bus_read16(cpu->bus, cpu->er[r]) & 0xFF);
            else bus_write16(cpu->bus, cpu->er[r], cpu->ccr);
            break;
        }
        case 0x6B: {
            bool write = (op2 & 0x80) != 0;
            bool addr24 = (op2 & 0x20) != 0;
            uint32_t addr;
            if (addr24) addr = fetch32(cpu) & 0xFFFFFF;
            else { addr = (uint32_t)(int32_t)(int16_t)fetch16(cpu); addr &= 0xFFFFFF; }
            if (!write) cpu->ccr = (uint8_t)(bus_read16(cpu->bus, addr) & 0xFF);
            else bus_write16(cpu->bus, addr, cpu->ccr);
            break;
        }
        case 0x6D: {
            int r = (op2 >> 4) & 0x7;
            if ((op2 & 0x80) == 0) {
                cpu->ccr = (uint8_t)(bus_read16(cpu->bus, cpu->er[r]) & 0xFF);
                cpu->er[r] += 2;
            } else {
                cpu->er[r] -= 2;
                bus_write16(cpu->bus, cpu->er[r], cpu->ccr);
            }
            break;
        }
        case 0x6F: {
            int r = (op2 >> 4) & 0x7;
            int16_t disp = (int16_t)fetch16(cpu);
            if ((op2 & 0x80) == 0) cpu->ccr = (uint8_t)(bus_read16(cpu->bus, cpu->er[r] + disp) & 0xFF);
            else bus_write16(cpu->bus, cpu->er[r] + disp, cpu->ccr);
            break;
        }
        default: unimplemented(cpu, 0x0140, cpu->pc - 2); break;
    }
}

/* ---- decode0100: MOV.L variants ---- */
static void decode0100(h8s_cpu_t *cpu, uint16_t op2) {
    int hi2 = (op2 >> 8) & 0xFF;
    int lo2 = op2 & 0xFF;
    switch (hi2) {
        case 0x69: {
            int rh = (lo2 >> 4) & 0x7;
            int rl = lo2 & 0x7;
            if ((lo2 & 0x80) == 0) {
                cpu->er[rl] = bus_read32(cpu->bus, cpu->er[rh]);
                set_nz_l(cpu, (int32_t)cpu->er[rl]); set_flag(cpu, BIT_V, false);
            } else {
                bus_write32(cpu->bus, cpu->er[rh], cpu->er[rl]);
                set_nz_l(cpu, (int32_t)cpu->er[rl]); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        case 0x6B: decode6B_32(cpu, op2); break;
        case 0x6D: {
            int rh = (lo2 >> 4) & 0x7;
            int rl = lo2 & 0x7;
            if ((lo2 & 0x80) == 0) {
                uint32_t val = bus_read32(cpu->bus, cpu->er[rh]);
                cpu->er[rh] += 4;
                cpu->er[rl] = val;
                set_nz_l(cpu, (int32_t)cpu->er[rl]); set_flag(cpu, BIT_V, false);
            } else {
                uint32_t val = cpu->er[rl];
                cpu->er[rh] -= 4;
                bus_write32(cpu->bus, cpu->er[rh], val);
                set_nz_l(cpu, (int32_t)val); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        case 0x6F: {
            int rh = (lo2 >> 4) & 0x7;
            int rl = lo2 & 0x7;
            int16_t disp = (int16_t)fetch16(cpu);
            if ((lo2 & 0x80) == 0) {
                cpu->er[rl] = bus_read32(cpu->bus, cpu->er[rh] + disp);
                set_nz_l(cpu, (int32_t)cpu->er[rl]); set_flag(cpu, BIT_V, false);
            } else {
                bus_write32(cpu->bus, cpu->er[rh] + disp, cpu->er[rl]);
                set_nz_l(cpu, (int32_t)cpu->er[rl]); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        case 0x78: {
            int r1 = (lo2 >> 4) & 0x7;
            uint16_t op3 = fetch16(cpu);
            int32_t disp = (int32_t)fetch32(cpu);
            int r2 = op3 & 0x7;
            if ((op3 & 0x0080) == 0) {
                cpu->er[r2] = bus_read32(cpu->bus, cpu->er[r1] + disp);
                set_nz_l(cpu, (int32_t)cpu->er[r2]); set_flag(cpu, BIT_V, false);
            } else {
                bus_write32(cpu->bus, cpu->er[r2] + disp, cpu->er[r1]);
                set_nz_l(cpu, (int32_t)cpu->er[r1]); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        default: unimplemented(cpu, 0x0100, cpu->pc - 4); break;
    }
}

/* ---- decode01: prefix 0x01xx ---- */
static void decode01(h8s_cpu_t *cpu, int lo) {
    switch (lo) {
        case 0x00: { uint16_t op2 = fetch16(cpu); decode0100(cpu, op2); break; }
        case 0x10: case 0x20: case 0x30: {
            int count = (lo >> 4);
            uint16_t op2 = fetch16(cpu);
            int rn = op2 & 0x7;
            if ((op2 & 0xFFF8) == 0x6D70) {
                for (int i = 0; i <= count; i++) {
                    cpu->er[rn - i] = bus_read32(cpu->bus, cpu->er[7]);
                    cpu->er[7] += 4;
                }
            } else if ((op2 & 0xFFF8) == 0x6DF0) {
                for (int i = 0; i <= count; i++) {
                    cpu->er[7] -= 4;
                    bus_write32(cpu->bus, cpu->er[7], cpu->er[rn + i]);
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
                set_nz_l(cpu, (int32_t)cpu->er[rd]); set_flag(cpu, BIT_V, false);
            } else if (lo == 0xF0 && hi2 == 0x65) {
                int rs = (op2 >> 4) & 0x7; int rd = op2 & 0x7;
                cpu->er[rd] = cpu->er[rd] ^ cpu->er[rs];
                set_nz_l(cpu, (int32_t)cpu->er[rd]); set_flag(cpu, BIT_V, false);
            } else if (lo == 0xF0 && hi2 == 0x66) {
                int rs = (op2 >> 4) & 0x7; int rd = op2 & 0x7;
                cpu->er[rd] = cpu->er[rd] & cpu->er[rs];
                set_nz_l(cpu, (int32_t)cpu->er[rd]); set_flag(cpu, BIT_V, false);
            } else { unimplemented(cpu, 0x0100 | lo, cpu->pc - 4); }
            break;
        }
        case 0x80: cpu->halted = true; break;
        default: { uint16_t op2 = fetch16(cpu); (void)op2; unimplemented(cpu, 0x0100 | lo, cpu->pc - 4); break; }
    }
}

/* ---- decode0A: INC.B, ADD.L reg ---- */
static void decode0A(h8s_cpu_t *cpu, int lo) {
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
static void decode0B(h8s_cpu_t *cpu, int lo) {
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
static void decode0F(h8s_cpu_t *cpu, int lo) {
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
static void decode17(h8s_cpu_t *cpu, int lo) {
    int subop = (lo >> 4) & 0xF;
    int rd = lo & 0xF;
    int erd = lo & 0x7;
    switch (subop) {
        case 0x0: { int v = (~get_reg_b(cpu, rd)) & 0xFF; set_reg_b(cpu, rd, (uint8_t)v); set_nz_b(cpu, v); set_flag(cpu, BIT_V, false); break; }
        case 0x1: { int v = (~get_r(cpu, rd)) & 0xFFFF; set_r(cpu, rd, (uint16_t)v); set_nz_w(cpu, v); set_flag(cpu, BIT_V, false); break; }
        case 0x3: { cpu->er[erd] = ~cpu->er[erd]; set_nz_l(cpu, (int32_t)cpu->er[erd]); set_flag(cpu, BIT_V, false); break; }
        case 0x5: { set_r(cpu, rd, get_rl(cpu, rd)); set_nz_w(cpu, get_r(cpu, rd)); set_flag(cpu, BIT_V, false); break; }
        case 0x7: { cpu->er[erd] = get_r(cpu, erd); set_nz_l(cpu, (int32_t)cpu->er[erd]); set_flag(cpu, BIT_V, false); break; }
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
        case 0xD: { int v = (int8_t)get_rl(cpu, rd); set_r(cpu, rd, (uint16_t)(v & 0xFFFF)); set_nz_w(cpu, v); set_flag(cpu, BIT_V, false); break; }
        case 0xF: { int32_t v = (int16_t)get_r(cpu, erd); cpu->er[erd] = (uint32_t)v; set_nz_l(cpu, v); set_flag(cpu, BIT_V, false); break; }
        default: unimplemented(cpu, 0x1700 | lo, cpu->pc - 2); break;
    }
}

/* ---- Shift/rotate/extend group (0x10-0x17) ---- */
static void decode10_17(h8s_cpu_t *cpu, int hi, int lo) {
    if (hi == 0x14) { /* OR.B reg */
        int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
        int result = get_reg_b(cpu, rd) | get_reg_b(cpu, rs);
        set_reg_b(cpu, rd, (uint8_t)result); set_nz_b(cpu, result); set_flag(cpu, BIT_V, false);
        return;
    }
    if (hi == 0x15) { /* XOR.B reg */
        int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
        int result = get_reg_b(cpu, rd) ^ get_reg_b(cpu, rs);
        set_reg_b(cpu, rd, (uint8_t)result); set_nz_b(cpu, result); set_flag(cpu, BIT_V, false);
        return;
    }
    if (hi == 0x16) { /* AND.B reg */
        int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
        int result = get_reg_b(cpu, rd) & get_reg_b(cpu, rs);
        set_reg_b(cpu, rd, (uint8_t)result); set_nz_b(cpu, result); set_flag(cpu, BIT_V, false);
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
static void decode1A(h8s_cpu_t *cpu, int lo) {
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
static void decode1B(h8s_cpu_t *cpu, int lo) {
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
static void decode1F(h8s_cpu_t *cpu, int lo) {
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
static void decode6A(h8s_cpu_t *cpu, int lo) {
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
        int val = bus_read8(cpu->bus, addr);
        set_reg_b(cpu, rd, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
    } else {
        int val = get_reg_b(cpu, rd);
        bus_write8(cpu->bus, addr, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
    }
}

/* ---- decode6B: MOV.W absolute ---- */
static void decode6B(h8s_cpu_t *cpu, int lo) {
    bool write = (lo & 0x80) != 0;
    bool addr24 = (lo & 0x20) != 0;
    int rd = lo & 0xF;
    uint32_t addr;
    if (addr24) addr = fetch32(cpu) & 0xFFFFFF;
    else { addr = (uint32_t)(int32_t)(int16_t)fetch16(cpu); addr &= 0xFFFFFF; }
    if (!write) {
        int val = bus_read16(cpu->bus, addr);
        set_r(cpu, rd, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
    } else {
        int val = get_r(cpu, rd);
        bus_write16(cpu->bus, addr, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
    }
}

/* ---- decode79: word immediate ---- */
static void decode79(h8s_cpu_t *cpu, int lo) {
    int subop = (lo >> 4) & 0xF;
    int rd = lo & 0xF;
    int imm = fetch16(cpu);
    switch (subop) {
        case 0x0: set_r(cpu, rd, (uint16_t)imm); set_nz_w(cpu, imm); set_flag(cpu, BIT_V, false); break;
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
        case 0x4: { int result = get_r(cpu, rd) | imm; set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_nz_w(cpu, result); set_flag(cpu, BIT_V, false); break; }
        case 0x5: { int result = get_r(cpu, rd) ^ imm; set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_nz_w(cpu, result); set_flag(cpu, BIT_V, false); break; }
        case 0x6: { int result = get_r(cpu, rd) & imm; set_r(cpu, rd, (uint16_t)(result & 0xFFFF)); set_nz_w(cpu, result); set_flag(cpu, BIT_V, false); break; }
        default: unimplemented(cpu, 0x7900 | lo, cpu->pc - 4); break;
    }
}

/* ---- decode7A: longword immediate ---- */
static void decode7A(h8s_cpu_t *cpu, int lo) {
    int subop = (lo >> 4) & 0xF;
    int rd = lo & 0x7;
    uint32_t imm = fetch32(cpu);
    switch (subop) {
        case 0x0: cpu->er[rd] = imm; set_nz_l(cpu, (int32_t)imm); set_flag(cpu, BIT_V, false); break;
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
        case 0x4: cpu->er[rd] |= imm; set_nz_l(cpu, (int32_t)cpu->er[rd]); set_flag(cpu, BIT_V, false); break;
        case 0x5: cpu->er[rd] ^= imm; set_nz_l(cpu, (int32_t)cpu->er[rd]); set_flag(cpu, BIT_V, false); break;
        case 0x6: cpu->er[rd] &= imm; set_nz_l(cpu, (int32_t)cpu->er[rd]); set_flag(cpu, BIT_V, false); break;
        default: unimplemented(cpu, 0x7A00 | lo, cpu->pc - 6); break;
    }
}

/* ---- decode7C_7F: bit ops on memory ---- */
static void decode7C_7F(h8s_cpu_t *cpu, int hi, int lo) {
    int r = (lo >> 4) & 0x7;
    uint16_t op2 = fetch16(cpu);
    int bit = (op2 >> 4) & 0x7;
    int subop = (op2 >> 8) & 0xFF;

    switch (hi) {
        case 0x7C: {
            uint32_t addr = cpu->er[r];
            int val = bus_read8(cpu->bus, addr);
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
            int val = bus_read8(cpu->bus, addr);
            switch (subop) {
                case 0x60: bus_write8(cpu->bus, addr, (uint8_t)(val | (1 << (get_rl(cpu, op2 & 0xF) & 0x7)))); break;
                case 0x62: bus_write8(cpu->bus, addr, (uint8_t)(val & ~(1 << (get_rl(cpu, op2 & 0xF) & 0x7)))); break;
                case 0x67:
                    if ((op2 & 0x80) == 0) {
                        if (get_flag(cpu, BIT_C)) val |= (1 << bit); else val &= ~(1 << bit);
                    } else {
                        if (!get_flag(cpu, BIT_C)) val |= (1 << bit); else val &= ~(1 << bit);
                    }
                    bus_write8(cpu->bus, addr, (uint8_t)val);
                    break;
                case 0x70: bus_write8(cpu->bus, addr, (uint8_t)(val | (1 << bit))); break;
                case 0x72: bus_write8(cpu->bus, addr, (uint8_t)(val & ~(1 << bit))); break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        }
        case 0x7E: {
            uint32_t addr = 0xFFFF00 | (lo & 0xFF);
            int val = bus_read8(cpu->bus, addr);
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
            int val = bus_read8(cpu->bus, addr);
            switch (subop) {
                case 0x70: bus_write8(cpu->bus, addr, (uint8_t)(val | (1 << bit))); break;
                case 0x72: bus_write8(cpu->bus, addr, (uint8_t)(val & ~(1 << bit))); break;
                default: unimplemented(cpu, (hi << 8) | lo, cpu->pc - 2); break;
            }
            break;
        }
        default: break;
    }
}

/* ======== Group 0 ======== */
static void decode0(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
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
            set_reg_b(cpu, rd, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
            break;
        }
        case 0x0D: { /* MOV.W Rs, Rd */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int val = get_r(cpu, rs);
            set_r(cpu, rd, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
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
static void decode1(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
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
static void decode5(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
    switch (hi) {
        case 0x50: { /* MULXU.B Rs, Rd: Rd = Rs x the low byte of Rd. Rd is a
                      * word register (R0-R7, E0-E7), not a byte register
                      * code: reading it as one took R0H for R0, which lost
                      * every BCD tens digit CyOS converts (13:45 read 3:05) */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int result = get_reg_b(cpu, rs) * (get_r(cpu, rd) & 0xFF);
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
                cpu->pc = bus_read32(cpu->bus, cpu->er[7]) & 0xFFFFFF;
                cpu->er[7] += 4;
            } else { unimplemented(cpu, op, cpu->pc - 2); }
            break;
        }
        case 0x55: { /* BSR d:8 */
            int8_t disp = (int8_t)lo;
            cpu->er[7] -= 4;
            bus_write32(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = (cpu->pc + disp) & 0xFFFFFF;
            break;
        }
        case 0x56: { /* RTE */
            if (lo == 0x70) {
                cpu->ccr = (uint8_t)(bus_read16(cpu->bus, cpu->er[7]) & 0xFF);
                cpu->er[7] += 2;
                cpu->pc = bus_read32(cpu->bus, cpu->er[7]) & 0xFFFFFF;
                cpu->er[7] += 4;
            } else { unimplemented(cpu, op, cpu->pc - 2); }
            break;
        }
        case 0x57: { /* TRAPA */
            if ((lo & 0xC0) == 0x00) {
                int vec = (lo >> 4) & 0x3;
                cpu->er[7] -= 4;
                bus_write32(cpu->bus, cpu->er[7], ((uint32_t)cpu->ccr << 24) | cpu->pc);
                cpu->pc = bus_read32(cpu->bus, 0x20 + (uint32_t)vec * 4) & 0xFFFFFF;
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
            cpu->pc = bus_read32(cpu->bus, lo & 0xFF) & 0xFFFFFF;
            break;
        }
        case 0x5C: { /* BSR d:16 */
            int16_t disp = (int16_t)fetch16(cpu);
            cpu->er[7] -= 4;
            bus_write32(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = (cpu->pc + disp) & 0xFFFFFF;
            break;
        }
        case 0x5D: { /* JSR @ERn */
            int rn = (lo >> 4) & 0x7;
            cpu->er[7] -= 4;
            bus_write32(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = cpu->er[rn] & 0xFFFFFF;
            break;
        }
        case 0x5E: { /* JSR @aa:24 */
            uint32_t addr = ((uint32_t)lo << 16) | fetch16(cpu);
            cpu->er[7] -= 4;
            bus_write32(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = addr & 0xFFFFFF;
            break;
        }
        case 0x5F: { /* JSR @@aa:8 */
            uint32_t addr = bus_read32(cpu->bus, lo & 0xFF) & 0xFFFFFF;
            cpu->er[7] -= 4;
            bus_write32(cpu->bus, cpu->er[7], cpu->pc);
            cpu->pc = addr;
            break;
        }
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/* ======== Group 6 ======== */
static void decode6(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
    switch (hi) {
        case 0x60: { int rn = (lo >> 4) & 0xF; int rd = lo & 0xF; int bit = get_reg_b(cpu, rn) & 0x7; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) | (1 << bit))); break; }
        case 0x61: { int rn = (lo >> 4) & 0xF; int rd = lo & 0xF; int bit = get_reg_b(cpu, rn) & 0x7; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) ^ (1 << bit))); break; }
        case 0x62: { int rn = (lo >> 4) & 0xF; int rd = lo & 0xF; int bit = get_reg_b(cpu, rn) & 0x7; set_reg_b(cpu, rd, (uint8_t)(get_reg_b(cpu, rd) & ~(1 << bit))); break; }
        case 0x63: { int rn = (lo >> 4) & 0xF; int rd = lo & 0xF; int bit = get_reg_b(cpu, rn) & 0x7; set_flag(cpu, BIT_Z, (get_reg_b(cpu, rd) & (1 << bit)) == 0); break; }
        case 0x64: { /* OR.W */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int result = get_r(cpu, rd) | get_r(cpu, rs);
            set_r(cpu, rd, (uint16_t)result); set_nz_w(cpu, result); set_flag(cpu, BIT_V, false);
            break;
        }
        case 0x65: { /* XOR.W */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int result = get_r(cpu, rd) ^ get_r(cpu, rs);
            set_r(cpu, rd, (uint16_t)result); set_nz_w(cpu, result); set_flag(cpu, BIT_V, false);
            break;
        }
        case 0x66: { /* AND.W */
            int rs = (lo >> 4) & 0xF; int rd = lo & 0xF;
            int result = get_r(cpu, rd) & get_r(cpu, rs);
            set_r(cpu, rd, (uint16_t)result); set_nz_w(cpu, result); set_flag(cpu, BIT_V, false);
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
                int val = bus_read8(cpu->bus, cpu->er[erReg]);
                set_reg_b(cpu, bReg, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
            } else {
                int val = get_reg_b(cpu, bReg);
                bus_write8(cpu->bus, cpu->er[erReg], (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        case 0x69: { /* MOV.W @ER, reg */
            int erReg = (lo >> 4) & 0x7; int wReg = lo & 0xF;
            if ((lo & 0x80) == 0) {
                int val = bus_read16(cpu->bus, cpu->er[erReg]);
                set_r(cpu, wReg, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
            } else {
                int val = get_r(cpu, wReg);
                bus_write16(cpu->bus, cpu->er[erReg], (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        case 0x6A: decode6A(cpu, lo); break;
        case 0x6B: decode6B(cpu, lo); break;
        case 0x6C: { /* MOV.B post-inc/pre-dec */
            int erReg = (lo >> 4) & 0x7; int bReg = lo & 0xF;
            if ((lo & 0x80) == 0) {
                int val = bus_read8(cpu->bus, cpu->er[erReg]);
                cpu->er[erReg] += 1;
                set_reg_b(cpu, bReg, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
            } else {
                int val = get_reg_b(cpu, bReg);
                cpu->er[erReg] -= 1;
                bus_write8(cpu->bus, cpu->er[erReg], (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        case 0x6D: { /* MOV.W post-inc/pre-dec */
            int erReg = (lo >> 4) & 0x7; int wReg = lo & 0xF;
            if ((lo & 0x80) == 0) {
                int val = bus_read16(cpu->bus, cpu->er[erReg]);
                cpu->er[erReg] += 2;
                set_r(cpu, wReg, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
            } else {
                int val = get_r(cpu, wReg);
                cpu->er[erReg] -= 2;
                bus_write16(cpu->bus, cpu->er[erReg], (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        case 0x6E: { /* MOV.B @(d:16, ER) */
            int erReg = (lo >> 4) & 0x7; int bReg = lo & 0xF;
            int16_t disp = (int16_t)fetch16(cpu);
            if ((lo & 0x80) == 0) {
                int val = bus_read8(cpu->bus, cpu->er[erReg] + disp);
                set_reg_b(cpu, bReg, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
            } else {
                int val = get_reg_b(cpu, bReg);
                bus_write8(cpu->bus, cpu->er[erReg] + disp, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        case 0x6F: { /* MOV.W @(d:16, ER) */
            int erReg = (lo >> 4) & 0x7; int wReg = lo & 0xF;
            int16_t disp = (int16_t)fetch16(cpu);
            if ((lo & 0x80) == 0) {
                int val = bus_read16(cpu->bus, cpu->er[erReg] + disp);
                set_r(cpu, wReg, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
            } else {
                int val = get_r(cpu, wReg);
                bus_write16(cpu->bus, cpu->er[erReg] + disp, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
            }
            break;
        }
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/* ======== Group 7 ======== */
static void decode7(h8s_cpu_t *cpu, uint16_t op, int hi, int lo) {
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
                    int val = bus_read8(cpu->bus, cpu->er[erReg] + disp);
                    set_reg_b(cpu, bReg, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
                } else {
                    int val = get_reg_b(cpu, bReg);
                    bus_write8(cpu->bus, cpu->er[erReg] + disp, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
                }
            } else if (hi2 == 0x6B) {
                int wReg = op2 & 0xF;
                int32_t disp = (int32_t)fetch32(cpu);
                if ((op2 & 0x80) == 0) {
                    int val = bus_read16(cpu->bus, cpu->er[erReg] + disp);
                    set_r(cpu, wReg, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
                } else {
                    int val = get_r(cpu, wReg);
                    bus_write16(cpu->bus, cpu->er[erReg] + disp, (uint16_t)val); set_nz_w(cpu, val); set_flag(cpu, BIT_V, false);
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
                    uint8_t b = bus_read8(cpu->bus, cpu->er[5]);
                    bus_write8(cpu->bus, cpu->er[6], b);
                    cpu->er[5]++; cpu->er[6]++;
                    set_reg_b(cpu, 0xC, (uint8_t)((get_reg_b(cpu, 0xC) - 1) & 0xFF));
                }
            } else if (lo == 0xD4 && op2 == 0x598F) {
                while (get_r(cpu, 4) != 0) {
                    uint8_t b = bus_read8(cpu->bus, cpu->er[5]);
                    bus_write8(cpu->bus, cpu->er[6], b);
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
static void decode(h8s_cpu_t *cpu, uint16_t op) {
    int hi = (op >> 8) & 0xFF;
    int lo = op & 0xFF;

    switch (hi >> 4) {
        case 0x0: decode0(cpu, op, hi, lo); break;
        case 0x1: decode1(cpu, op, hi, lo); break;
        case 0x2: { /* MOV.B @aa:8, Rd */
            int rd = (op >> 8) & 0xF;
            uint32_t addr = 0xFFFF00 | (op & 0xFF);
            int val = bus_read8(cpu->bus, addr);
            set_reg_b(cpu, rd, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
            break;
        }
        case 0x3: { /* MOV.B Rs, @aa:8 */
            int rd = (op >> 8) & 0xF;
            uint32_t addr = 0xFFFF00 | (op & 0xFF);
            int val = get_reg_b(cpu, rd);
            bus_write8(cpu->bus, addr, (uint8_t)val); set_nz_b(cpu, val); set_flag(cpu, BIT_V, false);
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
            set_reg_b(cpu, rd, (uint8_t)result); set_nz_b(cpu, result); set_flag(cpu, BIT_V, false);
            break;
        }
        case 0xD: { /* XOR.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int result = get_reg_b(cpu, rd) ^ imm;
            set_reg_b(cpu, rd, (uint8_t)result); set_nz_b(cpu, result); set_flag(cpu, BIT_V, false);
            break;
        }
        case 0xE: { /* AND.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            int result = get_reg_b(cpu, rd) & imm;
            set_reg_b(cpu, rd, (uint8_t)result); set_nz_b(cpu, result); set_flag(cpu, BIT_V, false);
            break;
        }
        case 0xF: { /* MOV.B #imm, Rd */
            int rd = (op >> 8) & 0xF; int imm = op & 0xFF;
            set_reg_b(cpu, rd, (uint8_t)imm); set_nz_b(cpu, imm); set_flag(cpu, BIT_V, false);
            break;
        }
        default: unimplemented(cpu, op, cpu->pc - 2); break;
    }
}

/* ======== Interrupt handling ======== */
static bool process_interrupts(h8s_cpu_t *cpu) {
    if (cpu->pending_irq_count == 0) return false;
    if (get_flag(cpu, BIT_I)) return false;

    int vector = cpu->pending_irqs[0];
    cpu->pending_irq_count--;
    if (cpu->pending_irq_count > 0)
        memmove(&cpu->pending_irqs[0], &cpu->pending_irqs[1],
                (size_t)cpu->pending_irq_count * sizeof(int));

    cpu->er[7] -= 4;
    bus_write32(cpu->bus, cpu->er[7], cpu->pc);
    cpu->er[7] -= 2;
    bus_write16(cpu->bus, cpu->er[7], (uint16_t)cpu->ccr);

    set_flag(cpu, BIT_I, true);

    uint32_t handler = bus_read32(cpu->bus, (uint32_t)vector * 4) & 0xFFFFFF;
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
    cpu->pc = bus_read32(cpu->bus, 0x000000) & 0xFFFFFF;
}

void h8s_cpu_step(h8s_cpu_t *cpu) {
    /* No interrupt right after LDC/ANDC/ORC/XORC to CCR: CyOS's scheduler
     * re-enables interrupts one instruction before loading the new task's
     * SP, and an interrupt in between stopped every CyOS timer. */
    bool hold = cpu->irq_hold;
    cpu->irq_hold = false;
    if (!hold && process_interrupts(cpu)) { cpu->cycle_count++; return; }

    if (cpu->halted) {
        if (cpu->pending_irq_count > 0) {
            cpu->halted = false;
        } else {
            cpu->cycle_count++;
            return;
        }
    }

    cpu->last_start_pc = cpu->pc;
    uint16_t op = fetch16(cpu);
    decode(cpu, op);
    cpu->cycle_count++;
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
