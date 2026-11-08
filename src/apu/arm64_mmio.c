/* ── MMIO faults on ARM64 hosts (Android, ARM Linux) ─────────────────────
 *
 * The APU's registers are left unmapped so that the recompiled code's loads
 * and stores to them fault and are routed to the emulated APU. On x86-64
 * apu_mmio_hook.c decodes the faulting x86 instruction; here the faulting
 * instruction is an AArch64 load or store, decoded below: the forms a C
 * compiler emits for plain and volatile accesses (LDR/STR with an unsigned
 * offset, unscaled, pre/post-indexed or register offsets, LDP/STP, and
 * LDAR/STLR). x86 read-modify-write instructions do not exist here: the
 * compiler emits a load and a store, which fault one at a time. */
#if defined(__aarch64__) && !defined(_WIN32)
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

#include "apu.h"

#define APU_MMIO_BASE 0xFE800000u
extern MCPXAPUState *g_apu_state;
#include <stddef.h>
ptrdiff_t xbox_GetMemoryOffset(void);

typedef struct {
    int load;           /* 1 load, 0 store */
    int size;           /* bytes per register: 1, 2, 4, 8 */
    int sext;           /* loads: sign-extend to 32 (1) or 64 (2) bits */
    int rt, rt2;        /* rt2 >= 0: a pair */
    uint64_t ea;        /* effective address of the (first) access */
    int wb;             /* write the new base back to rn */
    int rn;
    uint64_t wb_val;
} A64Access;

static uint64_t xreg(const mcontext_t *m, int r, int sp_ok)
{
    if (r == 31) return sp_ok ? m->sp : 0;
    return m->regs[r];
}

static int64_t sx(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

static uint64_t extend_reg(uint64_t v, int option, int shift)
{
    switch (option) {
    case 2: v = (uint32_t)v; break;            /* UXTW */
    case 3: break;                             /* LSL / UXTX */
    case 6: v = (uint64_t)(int64_t)(int32_t)v; break;   /* SXTW */
    case 7: break;                             /* SXTX */
    default: return (uint64_t)-1;              /* byte/half extends: not used for addressing */
    }
    return v << shift;
}

/* 0 when `insn` is not a general-register load or store this handles. */
static int a64_decode(uint32_t insn, const mcontext_t *m, A64Access *a)
{
    memset(a, 0, sizeof *a);
    a->rt2 = -1;
    a->rt = (int)(insn & 31);
    a->rn = (int)((insn >> 5) & 31);

    /* LDAR / STLR (and LDAPR): size 001000 1 L 0 11111 1 11111 Rn Rt */
    if ((insn & 0x3FBFFC00u) == 0x089FFC00u || (insn & 0x3FFFFC00u) == 0x38BFC000u) {
        a->size = 1 << (insn >> 30);
        a->load = (insn & 0x3FFFFC00u) == 0x38BFC000u ? 1 : (int)((insn >> 22) & 1);
        a->ea = xreg(m, a->rn, 1);
        return 1;
    }

    /* Load/store register: bits 29..27 = 111, V (bit 26) = 0 */
    if ((insn & 0x3C000000u) == 0x38000000u) {
        int size = (int)(insn >> 30), opc = (int)((insn >> 22) & 3);
        uint64_t base = xreg(m, a->rn, 1), off;
        a->size = 1 << size;
        if (opc == 0) a->load = 0;
        else if (opc == 1) a->load = 1;
        else {
            if (size == 3) return 0;               /* PRFM */
            if (size == 2 && opc == 3) return 0;   /* unallocated */
            a->load = 1;
            a->sext = opc == 2 ? 2 : 1;            /* LDRSx Xt / Wt */
        }
        if (insn & 0x01000000u) {                  /* unsigned immediate */
            off = (uint64_t)((insn >> 10) & 0xFFF) << size;
            a->ea = base + off;
            return 1;
        }
        if (insn & 0x00200000u) {                  /* register offset */
            int option = (int)((insn >> 13) & 7), s = (int)((insn >> 12) & 1);
            if (((insn >> 10) & 3) != 2) return 0;
            off = extend_reg(xreg(m, (int)((insn >> 16) & 31), 0), option, s ? size : 0);
            if (off == (uint64_t)-1) return 0;
            a->ea = base + off;
            return 1;
        }
        {
            int64_t imm9 = sx((insn >> 12) & 0x1FF, 9);
            switch ((insn >> 10) & 3) {
            case 0: case 2: a->ea = base + (uint64_t)imm9; return 1;              /* unscaled / unprivileged */
            case 1: a->ea = base; a->wb = 1; a->wb_val = base + (uint64_t)imm9; return 1;  /* post-index */
            case 3: a->ea = base + (uint64_t)imm9; a->wb = 1; a->wb_val = a->ea; return 1; /* pre-index */
            }
        }
        return 0;
    }

    /* Load/store pair: bits 29..27 = 101, V = 0 */
    if ((insn & 0x3C000000u) == 0x28000000u) {
        int opc = (int)(insn >> 30), mode = (int)((insn >> 23) & 3), scale;
        uint64_t base = xreg(m, a->rn, 1);
        int64_t imm7;
        if (opc == 3) return 0;
        scale = opc == 2 ? 3 : 2;
        a->size = 1 << scale;
        a->load = (int)((insn >> 22) & 1);
        if (opc == 1) { if (!a->load) return 0; a->sext = 2; }   /* LDPSW */
        a->rt2 = (int)((insn >> 10) & 31);
        imm7 = sx((insn >> 15) & 0x7F, 7) * (1 << scale);
        switch (mode) {
        case 0: case 2: a->ea = base + (uint64_t)imm7; break;               /* offset (and non-temporal) */
        case 1: a->ea = base; a->wb = 1; a->wb_val = base + (uint64_t)imm7; break;
        case 3: a->ea = base + (uint64_t)imm7; a->wb = 1; a->wb_val = a->ea; break;
        }
        return 1;
    }
    return 0;
}

static void set_reg(mcontext_t *m, int r, uint64_t v, int size, int sext)
{
    if (r == 31) return;                        /* xzr */
    if (sext == 1) v = (uint32_t)sx(v, size * 8);
    else if (sext == 2) v = (uint64_t)sx(v, size * 8);
    else if (size < 8) v &= size == 4 ? 0xFFFFFFFFull : size == 2 ? 0xFFFFull : 0xFFull;
    m->regs[r] = v;
}

static unsigned long s_fail;

/* A faulting store, for a handler that performs it itself (the AC'97
 * bus-master trap, kernel/xbox_memory_layout.c): where it writes, the value(s)
 * and their size. The instruction is retired (pc += 4, base written back).
 * 0, untouched, when it is not a store this decodes. */
int xbox_arm64_take_store(void *raw_ctx, uint64_t *ea, uint64_t vals[2], int *size, int *count)
{
    ucontext_t *uc = (ucontext_t *)raw_ctx;
    mcontext_t *m = &uc->uc_mcontext;
    A64Access a;
    if (!a64_decode(*(const uint32_t *)(uintptr_t)m->pc, m, &a) || a.load) return 0;
    *ea = a.ea;
    *size = a.size;
    *count = a.rt2 >= 0 ? 2 : 1;
    vals[0] = a.rt == 31 ? 0 : m->regs[a.rt];
    vals[1] = a.rt2 < 0 || a.rt2 == 31 ? 0 : m->regs[a.rt2];
    if (a.wb) {
        if (a.rn == 31) m->sp = a.wb_val;
        else m->regs[a.rn] = a.wb_val;
    }
    m->pc += 4;
    return 1;
}

/* The SIGSEGV handler's ARM64 path for the APU aperture (src/main.c). */
int xbox_arm64_mmio_fault(void *raw_ctx, uintptr_t host, uint32_t va)
{
    ucontext_t *uc = (ucontext_t *)raw_ctx;
    mcontext_t *m = &uc->uc_mcontext;
    uint32_t insn = *(const uint32_t *)(uintptr_t)m->pc;
    uintptr_t mem = (uintptr_t)xbox_GetMemoryOffset();
    A64Access a;
    int n, i;
    (void)host; (void)va;
    if (!g_apu_state || !a64_decode(insn, m, &a)) {
        if (s_fail++ < 20)
            fprintf(stderr, "[APU] ARM64 MMIO decode fail at pc=%p insn %08X va %08X\n", (void *)(uintptr_t)m->pc, insn, va);
        return 0;
    }
    n = a.rt2 >= 0 ? 2 : 1;
    for (i = 0; i < n; i++) {
        uint64_t ea = a.ea + (uint64_t)(i * a.size);
        uint32_t guest = (uint32_t)(ea - mem), off;
        int rt = i ? a.rt2 : a.rt;
        if (guest < APU_MMIO_BASE || guest >= APU_MMIO_BASE + 0x80000u) return 0;
        off = guest - APU_MMIO_BASE;
        if (a.load)
            set_reg(m, rt, mcpx_apu_mmio_read(g_apu_state, off, a.size), a.size, a.sext);
        else
            mcpx_apu_mmio_write(g_apu_state, off, rt == 31 ? 0 : m->regs[rt], a.size);
    }
    if (a.wb) {
        if (a.rn == 31) m->sp = a.wb_val;
        else m->regs[a.rn] = a.wb_val;
    }
    m->pc += 4;
    return 1;
}
#endif
