/*
 * MCPX APU MMIO Hook - Cross-platform x86-64 instruction decoder
 * Supports Windows (VEH PCONTEXT) and Linux (POSIX sigaction ucontext_t).
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

MCPXAPUState *g_apu_state = NULL;

#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

#if defined(_WIN32)
#include <windows.h>
#include "apu_env.h"
typedef PCONTEXT ApuCpuContext;

static uint64_t *ctx_reg64(ApuCpuContext ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&ctx->Rax;
    case 1:  return (uint64_t *)&ctx->Rcx;
    case 2:  return (uint64_t *)&ctx->Rdx;
    case 3:  return (uint64_t *)&ctx->Rbx;
    case 4:  return (uint64_t *)&ctx->Rsp;
    case 5:  return (uint64_t *)&ctx->Rbp;
    case 6:  return (uint64_t *)&ctx->Rsi;
    case 7:  return (uint64_t *)&ctx->Rdi;
    case 8:  return (uint64_t *)&ctx->R8;
    case 9:  return (uint64_t *)&ctx->R9;
    case 10: return (uint64_t *)&ctx->R10;
    case 11: return (uint64_t *)&ctx->R11;
    case 12: return (uint64_t *)&ctx->R12;
    case 13: return (uint64_t *)&ctx->R13;
    case 14: return (uint64_t *)&ctx->R14;
    case 15: return (uint64_t *)&ctx->R15;
    default: return NULL;
    }
}
static uint64_t *ctx_rip(ApuCpuContext ctx)    { return (uint64_t *)&ctx->Rip; }
static uint32_t *ctx_eflags(ApuCpuContext ctx) { return (uint32_t *)&ctx->EFlags; }

#elif defined(__linux__) && defined(__x86_64__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ucontext.h>
typedef ucontext_t *ApuCpuContext;

static uint64_t *ctx_reg64(ApuCpuContext ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RAX];
    case 1:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RCX];
    case 2:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RDX];
    case 3:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RBX];
    case 4:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RSP];
    case 5:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RBP];
    case 6:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RSI];
    case 7:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RDI];
    case 8:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R8];
    case 9:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R9];
    case 10: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R10];
    case 11: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R11];
    case 12: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R12];
    case 13: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R13];
    case 14: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R14];
    case 15: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R15];
    default: return NULL;
    }
}
static uint64_t *ctx_rip(ApuCpuContext ctx)    { return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RIP]; }
static uint32_t *ctx_eflags(ApuCpuContext ctx) { return (uint32_t *)&ctx->uc_mcontext.gregs[REG_EFL]; }
#endif

#if defined(_WIN32) || (defined(__linux__) && defined(__x86_64__))

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = modrm & 7;            /* REX.B never changes the encoding length */
    int len = 1;
    (void)has_rex_b;

    if (mod == 3) return 1;
    if (rm == 4) {
        /* SIB. Base 5 with mod 0 means disp32 and no base register. */
        uint8_t sib = ip[1];
        len += 1;
        if (mod == 0 && (sib & 7) == 5) len += 4;
    }
    if (mod == 0 && rm == 5) len += 4;          /* RIP-relative disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* ---- ALU forms GCC emits at -O2 (memory operand = the MMIO register) ----
 * -O0 code only ever loads/stores MMIO with mov, so the decoder above was
 * enough; optimised code folds the access into test/cmp/or/and/add/sub/xor
 * with an immediate or register operand. Flags follow the x86 definitions
 * (CF PF AF ZF SF OF); the guest code branches on them. */
#define EF_CF 0x0001u
#define EF_PF 0x0004u
#define EF_AF 0x0010u
#define EF_ZF 0x0040u
#define EF_SF 0x0080u
#define EF_OF 0x0800u
#define EF_ARITH (EF_CF | EF_PF | EF_AF | EF_ZF | EF_SF | EF_OF)

enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP, ALU_TEST };

static uint64_t size_mask(int size) { return size == 8 ? ~0ULL : ((1ULL << (size * 8)) - 1); }

static uint32_t flags_result(uint64_t r, int size)
{
    uint32_t f = 0;
    r &= size_mask(size);
    if (r == 0) f |= EF_ZF;
    if (r >> (size * 8 - 1) & 1) f |= EF_SF;
    if (!(__builtin_popcount((unsigned)(r & 0xFF)) & 1)) f |= EF_PF;
    return f;
}

/* Returns 1 and the result when the op writes its destination (not CMP/TEST). */
static int alu_apply(int op, uint64_t a, uint64_t b, int size, uint32_t *eflags, uint64_t *out)
{
    uint64_t m = size_mask(size), r;
    uint32_t f;
    int sign = size * 8 - 1;
    a &= m; b &= m;
    switch (op) {
    case ALU_ADD:
        r = (a + b) & m;
        f = flags_result(r, size);
        if (r < a) f |= EF_CF;
        if ((((a ^ r) & (b ^ r)) >> sign) & 1) f |= EF_OF;
        if ((a ^ b ^ r) & 0x10) f |= EF_AF;
        break;
    case ALU_SUB: case ALU_CMP:
        r = (a - b) & m;
        f = flags_result(r, size);
        if (a < b) f |= EF_CF;
        if ((((a ^ b) & (a ^ r)) >> sign) & 1) f |= EF_OF;
        if ((a ^ b ^ r) & 0x10) f |= EF_AF;
        break;
    case ALU_OR:  r = a | b; f = flags_result(r, size); break;
    case ALU_XOR: r = a ^ b; f = flags_result(r, size); break;
    case ALU_AND: case ALU_TEST: r = a & b; f = flags_result(r, size); break;
    default: return -1;                                  /* ADC/SBB: not emulated */
    }
    *eflags = (*eflags & ~EF_ARITH) | f;
    *out = r;
    return op != ALU_CMP && op != ALU_TEST;
}

/* Register operand of a ModRM `reg` field, honouring 8-bit AH/CH/DH/BH
 * (regs 4-7 without a REX prefix) and 32-bit writes zero-extending. */
static uint64_t reg_read(ApuCpuContext ctx, int reg, int size, int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8)
        return (*ctx_reg64(ctx, reg - 4) >> 8) & 0xFF;
    return *ctx_reg64(ctx, reg) & size_mask(size);
}

static void reg_write(ApuCpuContext ctx, int reg, int size, int has_rex, uint64_t v)
{
    uint64_t *d;
    if (size == 1 && !has_rex && reg >= 4 && reg < 8) {
        d = ctx_reg64(ctx, reg - 4);
        *d = (*d & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    d = ctx_reg64(ctx, reg);
    if (size == 1)      *d = (*d & ~0xFFULL) | (v & 0xFF);
    else if (size == 2) *d = (*d & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *d = v & 0xFFFFFFFFULL;
    else                *d = v;
}

static bool apu_decode_and_handle(ApuCpuContext ctx, uint32_t mmio_offset, int is_write)
{
    uint64_t *rip_ptr = ctx_rip(ctx);
    uint32_t *eflags_ptr = ctx_eflags(ctx);
    const uint8_t *ip = (const uint8_t *)(*rip_ptr);
    if (!g_apu_state) return false;
    (void)is_write;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0, has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, val, access_size);
        *rip_ptr += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint32_t imm = *(uint32_t *)(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, access_size);
        *rip_ptr += prefix_len + 1 + modrm_len + 4;
        g_apu_mmio_write_count++;
        return true;
    }

    if (opcode[0] == 0xC6) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, 1);
        *rip_ptr += prefix_len + 1 + modrm_len + 1;
        g_apu_mmio_write_count++;
        return true;
    }

    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (access_size == 1) *dst = (*dst & ~0xFFULL) | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFF;
        else *dst = val;
        *rip_ptr += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 1);
        *ctx_reg64(ctx, reg) = val & 0xFF;
        *rip_ptr += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 2);
        *ctx_reg64(ctx, reg) = val & 0xFFFF;
        *rip_ptr += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;
        *eflags_ptr &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
        if (result == 0) *eflags_ptr |= 0x0040u;
        if (result & (1ULL << (access_size * 8 - 1))) *eflags_ptr |= 0x0080u;
        *rip_ptr += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* CMP r/m, reg (38/39): handled by the register ALU forms below, which set OF too. */

    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val | reg_val, access_size);
        *rip_ptr += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val & reg_val, access_size);
        *rip_ptr += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* Group 1 with an immediate: 80 /n ib, 81 /n iw|id, 83 /n ib (sign-extended). */
    if (opcode[0] == 0x80 || opcode[0] == 0x81 || opcode[0] == 0x83) {
        int size = opcode[0] == 0x80 ? 1 : access_size;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int op = (opcode[1] >> 3) & 7;
        const uint8_t *immp = opcode + 1 + modrm_len;
        int imm_len = opcode[0] == 0x81 ? (size == 2 ? 2 : 4) : 1;
        int64_t imm = imm_len == 1 ? (int8_t)immp[0]
                    : imm_len == 2 ? (int16_t)(immp[0] | immp[1] << 8)
                    : (int32_t)(immp[0] | immp[1] << 8 | immp[2] << 16 | (uint32_t)immp[3] << 24);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, size), r;
        int wb = alu_apply(op, mem_val, (uint64_t)imm, size, eflags_ptr, &r);
        if (wb >= 0) {
            if (wb) { mcpx_apu_mmio_write(g_apu_state, mmio_offset, r, size); g_apu_mmio_write_count++; }
            else g_apu_mmio_read_count++;
            *rip_ptr += prefix_len + 1 + modrm_len + imm_len;
            return true;
        }
    }

    /* TEST r/m, imm: F6 /0 ib, F7 /0 iw|id. */
    if ((opcode[0] == 0xF6 || opcode[0] == 0xF7) && ((opcode[1] >> 3) & 7) == 0) {
        int size = opcode[0] == 0xF6 ? 1 : access_size;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        const uint8_t *immp = opcode + 1 + modrm_len;
        int imm_len = size == 1 ? 1 : size == 2 ? 2 : 4;
        int64_t imm = imm_len == 1 ? immp[0]
                    : imm_len == 2 ? (immp[0] | immp[1] << 8)
                    : (int32_t)(immp[0] | immp[1] << 8 | immp[2] << 16 | (uint32_t)immp[3] << 24);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, size), r;
        alu_apply(ALU_TEST, mem_val, (uint64_t)imm, size, eflags_ptr, &r);
        *rip_ptr += prefix_len + 1 + modrm_len + imm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* Register ALU forms: op r/m, reg (00/01 08/09 20/21 28/29 30/31 38/39)
     * and op reg, r/m (02/03 0A/0B 22/23 2A/2B 32/33 3A/3B). The r/m, reg
     * OR/AND/CMP and TEST forms handled above keep their old paths. */
    {
        uint8_t o = opcode[0];
        int fam = o >> 3, low = o & 7;
        static const int fam_op[8] = { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };
        if (o < 0x40 && low <= 3) {
            int size = (low & 1) ? access_size : 1;
            int to_reg = (low & 2) != 0;
            int modrm_len = decode_modrm_len(opcode + 1, rex_b);
            int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
            uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, size), r;
            uint64_t reg_val = reg_read(ctx, reg, size, has_rex);
            int wb = to_reg ? alu_apply(fam_op[fam], reg_val, mem_val, size, eflags_ptr, &r)
                            : alu_apply(fam_op[fam], mem_val, reg_val, size, eflags_ptr, &r);
            if (wb >= 0) {
                if (wb && to_reg) reg_write(ctx, reg, size, has_rex, r);
                else if (wb) { mcpx_apu_mmio_write(g_apu_state, mmio_offset, r, size); g_apu_mmio_write_count++; }
                if (!wb || to_reg) g_apu_mmio_read_count++;
                *rip_ptr += prefix_len + 1 + modrm_len;
                return true;
            }
        }
    }

    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                (void *)(*rip_ptr), mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

bool apu_hook_handle_mmio(void *raw_ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    (void)fault_addr;
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    bool ok = apu_decode_and_handle((ApuCpuContext)raw_ctx, mmio_offset, is_write);

    if (getenv("RECOMP_APU_TRACE")) {
        static unsigned n;
        if (n++ < 400) {
            uint64_t v = g_apu_state
                       ? mcpx_apu_mmio_read(g_apu_state, mmio_offset, 4) : 0;
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X%s\n",
                    is_write ? "write" : "read ", mmio_offset,
                    (uint32_t)v, ok ? "" : "  (decode failed)");
        }
    }
    return ok;
}

#endif