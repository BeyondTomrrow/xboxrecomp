/*
 * mmio_decode_arm64.h -- trapped-MMIO emulation for arm64 hosts (macOS).
 *
 * The x86-64 decoders (mmio_decode.h, apu_mmio_hook.c) step over the host
 * instruction that touched a PAGE_NOACCESS device page. On Apple Silicon the
 * faulting instruction is an AArch64 load or store, and the recompiled code's
 * MEM8/16/32 accesses compile to the ordinary forms below. Fixed 4-byte
 * encodings make this much smaller than the x86 side: decode, compute the
 * effective address from the saved registers, run the access through the
 * device callbacks, write the destination and any base writeback, PC += 4.
 *
 * Covered (integer and FP/SIMD register variants):
 *   LDR/STR (unsigned immediate), LDUR/STUR, pre/post-indexed, register offset
 *   (LSL/UXTW/SXTW/SXTX), LDRB/LDRH/LDRSB/LDRSH/LDRSW, PRFM (skipped),
 *   LDP/STP/LDPSW (offset, pre, post, non-temporal).
 * Anything else -- exclusives, LSE atomics, LD1/ST1 -- returns false so the
 * caller falls through to a crash that names the fault instead of silently
 * stepping over something it did not understand.
 *
 * Header-only, static inline: same reasoning as mmio_decode.h.
 */
#ifndef MMIO_DECODE_ARM64_H
#define MMIO_DECODE_ARM64_H

#if defined(__APPLE__) && defined(__aarch64__)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>

/* Access callbacks. host is the effective host address of the access; size is
 * 1, 2, 4 or 8 (16-byte SIMD and pairs are split). Return false to refuse. */
typedef bool (*a64_mmio_read_fn)(void *dev, uintptr_t host, int size, uint64_t *out);
typedef bool (*a64_mmio_write_fn)(void *dev, uintptr_t host, uint64_t val, int size);

typedef struct {
    _STRUCT_MCONTEXT64 *m;
} a64_ctx;

/* General register n; 31 is XZR or SP depending on the operand. */
static inline uint64_t a64_xreg(a64_ctx c, int n, bool sp)
{
    if (n < 29) return c.m->__ss.__x[n];
    if (n == 29) return c.m->__ss.__fp;
    if (n == 30) return c.m->__ss.__lr;
    return sp ? c.m->__ss.__sp : 0;
}

static inline void a64_set_xreg(a64_ctx c, int n, bool sp, uint64_t v)
{
    if (n < 29) c.m->__ss.__x[n] = v;
    else if (n == 29) c.m->__ss.__fp = v;
    else if (n == 30) c.m->__ss.__lr = v;
    else if (sp) c.m->__ss.__sp = v;
    /* XZR: discarded */
}

static inline int64_t a64_sext(uint64_t v, int bits)
{
    uint64_t m = 1ull << (bits - 1);
    v &= (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    return (int64_t)((v ^ m) - m);
}

/* One element of `bytes` (1..16) between a register and memory. */
static inline bool a64_load_elem(void *dev, a64_mmio_read_fn rd, uintptr_t addr,
                                 int bytes, uint8_t *out16)
{
    memset(out16, 0, 16);
    if (bytes == 16) {
        uint64_t lo, hi;
        if (!rd(dev, addr, 8, &lo) || !rd(dev, addr + 8, 8, &hi)) return false;
        memcpy(out16, &lo, 8);
        memcpy(out16 + 8, &hi, 8);
        return true;
    }
    uint64_t v;
    if (!rd(dev, addr, bytes, &v)) return false;
    memcpy(out16, &v, 8);
    return true;
}

static inline bool a64_store_elem(void *dev, a64_mmio_write_fn wr, uintptr_t addr,
                                  int bytes, const uint8_t *in16)
{
    if (bytes == 16) {
        uint64_t lo, hi;
        memcpy(&lo, in16, 8);
        memcpy(&hi, in16 + 8, 8);
        return wr(dev, addr, lo, 8) && wr(dev, addr + 8, hi, 8);
    }
    uint64_t v = 0;
    memcpy(&v, in16, 8);
    if (bytes < 8) v &= (1ull << (bytes * 8)) - 1;
    return wr(dev, addr, v, bytes);
}

/* Register operand of an element access: integer or SIMD, read as 16 bytes. */
static inline void a64_get_rt(a64_ctx c, int rt, bool simd, uint8_t *out16)
{
    memset(out16, 0, 16);
    if (simd) {
        memcpy(out16, &c.m->__ns.__v[rt], 16);
    } else {
        uint64_t v = a64_xreg(c, rt, false);
        memcpy(out16, &v, 8);
    }
}

/* Destination write of a load. Integer: `bytes` loaded, sign-extended to
 * sext_bits if nonzero, then zero-extended into X (a W write clears the top
 * half, which is the same thing). SIMD: the whole V register is replaced. */
static inline void a64_put_rt(a64_ctx c, int rt, bool simd, const uint8_t *in16,
                              int bytes, int sext_to)
{
    if (simd) {
        memcpy(&c.m->__ns.__v[rt], in16, 16);
        return;
    }
    uint64_t v = 0;
    memcpy(&v, in16, 8);
    if (bytes < 8) v &= (1ull << (bytes * 8)) - 1;
    if (sext_to) {
        v = (uint64_t)a64_sext(v, bytes * 8);
        if (sext_to == 32) v &= 0xFFFFFFFFull;
    }
    a64_set_xreg(c, rt, false, v);
}

/* Emulate the load/store at the saved PC and advance past it. */
static inline bool a64_mmio_emulate(void *ucontext, void *dev,
                                    a64_mmio_read_fn rd, a64_mmio_write_fn wr)
{
    ucontext_t *uc = (ucontext_t *)ucontext;
    if (!uc || !uc->uc_mcontext) return false;
    a64_ctx c = { uc->uc_mcontext };
    uint32_t insn = *(const uint32_t *)(uintptr_t)c.m->__ss.__pc;
    uint8_t buf[16], buf2[16];

    int rt = insn & 31, rn = (insn >> 5) & 31;
    bool simd = (insn >> 26) & 1;

    /* ---- load/store pair: x0 1 0 1 V 0 xx L ------------------------------ */
    if ((insn & 0x3A000000u) == 0x28000000u) {
        int opc = insn >> 30, mode = (insn >> 23) & 3, rt2 = (insn >> 10) & 31;
        bool load = (insn >> 22) & 1;
        int bytes, sext_to = 0;
        if (simd) {
            if (opc == 3) return false;
            bytes = 4 << opc;
        } else {
            if (opc == 0) bytes = 4;
            else if (opc == 2) bytes = 8;
            else if (opc == 1 && load) { bytes = 4; sext_to = 64; }   /* LDPSW */
            else return false;
        }
        int64_t off = a64_sext((insn >> 15) & 0x7F, 7) * bytes;
        uint64_t base = a64_xreg(c, rn, true);
        uint64_t addr = (mode == 1) ? base : base + (uint64_t)off;   /* 1: post */
        if (load) {
            if (!a64_load_elem(dev, rd, (uintptr_t)addr, bytes, buf) ||
                !a64_load_elem(dev, rd, (uintptr_t)(addr + bytes), bytes, buf2))
                return false;
            a64_put_rt(c, rt, simd, buf, bytes, sext_to);
            a64_put_rt(c, rt2, simd, buf2, bytes, sext_to);
        } else {
            a64_get_rt(c, rt, simd, buf);
            a64_get_rt(c, rt2, simd, buf2);
            if (!a64_store_elem(dev, wr, (uintptr_t)addr, bytes, buf) ||
                !a64_store_elem(dev, wr, (uintptr_t)(addr + bytes), bytes, buf2))
                return false;
        }
        if (mode == 1 || mode == 3)
            a64_set_xreg(c, rn, true, base + (uint64_t)off);
        c.m->__ss.__pc += 4;
        return true;
    }

    /* ---- load/store register: xx 1 1 1 V 0 x ----------------------------- */
    if ((insn & 0x3A000000u) != 0x38000000u)
        return false;

    int size = insn >> 30, opc = (insn >> 22) & 3;
    uint64_t base = a64_xreg(c, rn, true);
    uint64_t addr, wb = 0;
    bool writeback = false;

    if (insn & (1u << 24)) {                        /* unsigned immediate */
        int scale = (simd && (opc & 2)) ? 4 : size;
        addr = base + ((uint64_t)((insn >> 10) & 0xFFF) << scale);
    } else if ((insn >> 21) & 1) {                  /* register offset */
        if (((insn >> 10) & 3) != 2) return false;  /* atomics live here too */
        int rm = (insn >> 16) & 31, option = (insn >> 13) & 7;
        bool s = (insn >> 12) & 1;
        int scale = (simd && (opc & 2)) ? 4 : size;
        uint64_t m = a64_xreg(c, rm, false);
        switch (option) {
        case 2: m = (uint32_t)m; break;                       /* UXTW */
        case 3: case 7: break;                                /* LSL / SXTX */
        case 6: m = (uint64_t)(int64_t)(int32_t)(uint32_t)m; break;  /* SXTW */
        default: return false;
        }
        addr = base + (s ? (m << scale) : m);
    } else {                                        /* imm9: unscaled/pre/post */
        int kind = (insn >> 10) & 3;                /* 0 unscaled, 1 post, 2 unpriv, 3 pre */
        int64_t imm = a64_sext((insn >> 12) & 0x1FF, 9);
        if (kind == 1) { addr = base; wb = base + (uint64_t)imm; writeback = true; }
        else if (kind == 3) { addr = base + (uint64_t)imm; wb = addr; writeback = true; }
        else addr = base + (uint64_t)imm;
    }

    int bytes, sext_to = 0;
    bool load;
    if (simd) {
        if (size == 0 && (opc & 2)) bytes = 16;
        else if (opc & 2) return false;
        else bytes = 1 << size;
        load = opc & 1;
    } else {
        bytes = 1 << size;
        if (opc == 0) load = false;
        else if (opc == 1) load = true;
        else if (opc == 2) {
            if (size == 3) {                        /* PRFM: no access at all */
                if (writeback) a64_set_xreg(c, rn, true, wb);
                c.m->__ss.__pc += 4;
                return true;
            }
            load = true; sext_to = 64;
        } else {
            if (size >= 2) return false;
            load = true; sext_to = 32;
        }
    }

    if (load) {
        if (!a64_load_elem(dev, rd, (uintptr_t)addr, bytes, buf)) return false;
        a64_put_rt(c, rt, simd, buf, bytes, sext_to);
    } else {
        a64_get_rt(c, rt, simd, buf);
        if (!a64_store_elem(dev, wr, (uintptr_t)addr, bytes, buf)) return false;
    }
    if (writeback) a64_set_xreg(c, rn, true, wb);
    c.m->__ss.__pc += 4;
    return true;
}

#endif /* __APPLE__ && __aarch64__ */
#endif /* MMIO_DECODE_ARM64_H */
