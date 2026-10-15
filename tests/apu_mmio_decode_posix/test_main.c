/* Differential test: each instruction runs on plain memory, then on a PROT_NONE
 * page whose faults go through apu_hook_handle_mmio. Results must match. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <stdbool.h>

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
bool apu_hook_handle_mmio(void *raw_ctx, uintptr_t fault_addr, uint32_t fault_xbox_va, int is_write);

static uint8_t regs[0x4000];
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t a, unsigned s) { uint64_t v = 0; (void)d; memcpy(&v, regs + a, s); return v; }
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t a, uint64_t v, unsigned s) { (void)d; memcpy(regs + a, &v, s); }

static uint8_t *mmio;
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    uintptr_t off = (uintptr_t)si->si_addr - (uintptr_t)mmio;
    (void)sig;
    if (off >= 0x4000 || !apu_hook_handle_mmio(uc, (uintptr_t)si->si_addr, 0xFE800000u + (uint32_t)off, 0)) {
        static const char m[] = "decoder refused instruction\n"; write(2, m, sizeof m - 1); _exit(3);
    }
}

#define ARITH 0x8D5u   /* CF PF AF ZF SF OF */
#define T(fn, insn) static void fn(uint8_t *b, uint64_t off, uint64_t *c, uint64_t *f) { \
    uint64_t cc = *c, ff; __asm__ volatile(insn "\n\tpushfq\n\tpopq %[f]" \
        : "+c"(cc), [f] "=&r"(ff) : "a"(b), "D"(off) : "memory", "cc"); *c = cc; *f = ff; }

T(t_test_imm32, "testl $0x18, (%%rax,%%rdi)")
T(t_cmp_imm8,   "cmpl $5, (%%rax,%%rdi)")
T(t_cmp_imm32,  "cmpl $0x12345, (%%rax,%%rdi)")
T(t_or_imm32,   "orl $0x100, (%%rax,%%rdi)")
T(t_and_imm8,   "andl $-16, (%%rax,%%rdi)")
T(t_add_toreg,  "addl (%%rax,%%rdi), %%ecx")
T(t_sub_tomem,  "subl %%ecx, (%%rax,%%rdi)")
T(t_cmpb_imm,   "cmpb $0x7f, (%%rax,%%rdi)")
T(t_testb_imm,  "testb $0x80, (%%rax,%%rdi)")
T(t_cmp_regmem, "cmpl (%%rax,%%rdi), %%ecx")
T(t_cmp_memreg, "cmpl %%ecx, (%%rax,%%rdi)")
T(t_xor_tomem,  "xorl %%ecx, (%%rax,%%rdi)")
T(t_cmpw_imm16, "cmpw $0x1234, (%%rax,%%rdi)")
T(t_add_disp8,  "addl $7, 4(%%rax,%%rdi)")
T(t_or_toreg_b, "orb (%%rax,%%rdi), %%cl")
T(t_mov_load,   "movl (%%rax,%%rdi), %%ecx")

typedef void (*tf)(uint8_t *, uint64_t, uint64_t *, uint64_t *);
static const struct { const char *name; tf fn; } tests[] = {
    {"test m32,imm32", t_test_imm32}, {"cmp m32,imm8", t_cmp_imm8}, {"cmp m32,imm32", t_cmp_imm32},
    {"or m32,imm32", t_or_imm32}, {"and m32,imm8", t_and_imm8}, {"add r32,m32", t_add_toreg},
    {"sub m32,r32", t_sub_tomem}, {"cmp m8,imm8", t_cmpb_imm}, {"test m8,imm8", t_testb_imm},
    {"cmp r32,m32", t_cmp_regmem}, {"cmp m32,r32", t_cmp_memreg}, {"xor m32,r32", t_xor_tomem},
    {"cmp m16,imm16", t_cmpw_imm16}, {"add m32+disp8,imm8", t_add_disp8}, {"or r8,m8", t_or_toreg_b},
    {"mov r32,m32", t_mov_load},
};
static const uint32_t vals[] = { 0, 5, 0x18, 0x80000000u, 0x7FFFFFFFu, 0xFFFFFFF0u, 0x12345, 0x1234, 0x7F, 0x80, 0xDEADBEEF };
static const uint64_t rcxs[] = { 0, 5, 0x80000000u, 0xFFFFFFFFu, 0x12345678 };

int main(void)
{
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL);
    static uint8_t dummy; g_apu_state = (MCPXAPUState *)&dummy;
    mmio = mmap(NULL, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *ram = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int fails = 0, runs = 0;
    const uint64_t off = 0x2000;
    for (unsigned t = 0; t < sizeof tests / sizeof tests[0]; t++)
      for (unsigned v = 0; v < sizeof vals / sizeof vals[0]; v++)
        for (unsigned r = 0; r < sizeof rcxs / sizeof rcxs[0]; r++) {
            uint64_t c1 = rcxs[r], c2 = rcxs[r], f1, f2;
            memset(ram, 0, 0x4000); memset(regs, 0, sizeof regs);
            memcpy(ram + off, &vals[v], 4); memcpy(regs + off, &vals[v], 4);
            memcpy(ram + off + 4, &vals[v], 4); memcpy(regs + off + 4, &vals[v], 4);
            tests[t].fn(ram, off, &c1, &f1);
            tests[t].fn(mmio, off, &c2, &f2);
            runs++;
            if (c1 != c2 || (f1 & ARITH) != (f2 & ARITH) || memcmp(ram + off, regs + off, 8)) {
                if (fails++ < 12)
                    printf("FAIL %-20s mem=%08X rcx=%llx: rcx %llx/%llx flags %03llx/%03llx mem %08X/%08X\n",
                           tests[t].name, vals[v], (unsigned long long)rcxs[r], (unsigned long long)c1, (unsigned long long)c2,
                           (unsigned long long)(f1 & ARITH), (unsigned long long)(f2 & ARITH),
                           *(uint32_t *)(ram + off), *(uint32_t *)(regs + off));
            }
        }
    printf("%d/%d instruction runs matched hardware\n", runs - fails, runs);
    return fails != 0;
}
