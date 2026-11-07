/* Host coroutines (coro.h). x86-64 Windows only.
 * A switched-out context, from the saved stack pointer upwards:
 *   [0x00] StackBase  [0x08] StackLimit  [0x10] DeallocationStack   (the TEB fields: the OS checks them when it dispatches an exception, e.g. the
 *                                                                    VRAM page faults of the GPU renderer, so each stack must show its own bounds)
 *   [0x18] 0xb0 bytes: xmm6..xmm15 (0x00..0x9f), MXCSR (0xa0), x87 control word (0xa4)
 *   [0xc8] r15 r14 r13 r12 rdi rsi rbx rbp, then the return address.
 * A new coroutine gets a frame like that whose return address is coro_tramp, with r12 = function and r13 = argument. */
#include "coro.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
/* Linux (System V AMD64): the OS does not keep stack bounds in a TEB, XMM registers are all
 * volatile, and signals run on the alternate stack installed per thread (shim_kern), so a
 * switch is just the six callee-saved GPRs. A new coroutine gets a frame whose return address
 * is tf_coro_tramp, with r12 = function and r13 = argument (rdi = argument at the call). */
/* A chamada do trampolin TEM de sair com rsp alinhado a 16 bytes (SysV): a funcao da corotina -
 * thread_entry -> psp_call -> codigo JIT - roda inteira numa pilha que nasce aqui, e o GCC usa
 * movaps/movdqa na pilha (o psp_call empacota quatro uint32_t de g_hle_now_a num so slot de 16).
 * Sem o alinhamento o primeiro acesso desses da #GP, o Linux entrega SIGSEGV com si_addr == 0,
 * e o relatorio dizia "(nil)" - um ponteiro nulo que nao existia. O trampolin do Windows acerta
 * o alinhamento de nascenca (o coro_create dele poe o quadro a top-0x40 e o sub 0x20 mantem);
 * a porta Linux esqueceu disso. andq $-16 vale para qualquer aritmetica de coro_create. */
asm(
    ".text\n"
    ".globl tf_coro_switch\n"
    "tf_coro_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    retq\n"
    ".globl tf_coro_tramp\n"
    "tf_coro_tramp:\n"
    "    movq %r13, %rdi\n"
    "    andq $-16, %rsp\n"    /* antes do call: rsp % 16 == 0, como a ABI exige (a entrada chega com rsp % 16 == 8) */
    "    callq *%r12\n"
    "    int3\n"
);
extern "C" void tf_coro_tramp(void);

static const size_t GUARD = 0x1000;
static size_t stack_size(void) {
    static size_t sz = [] { const char *e = getenv("TF_STACK_KB"); size_t kb = e ? (size_t)atoi(e) : 4096; if (kb < 256) kb = 256; return kb * 1024; }();
    return sz;
}
bool coro_recommit(Coro &c) {
    if (!c.base) return false;
    if (c.committed) return true;
    if (!VirtualAlloc(c.base, c.size, MEM_COMMIT, PAGE_READWRITE)) return false;
    DWORD old; VirtualProtect(c.base, GUARD, PAGE_NOACCESS, &old);
    c.committed = true; return true;
}
void coro_release(Coro &c) {
    if (!c.base || !c.committed) return;
    VirtualFree(c.base, c.size, MEM_DECOMMIT);
    c.committed = false;
}
bool coro_create(Coro &c, CoroFn fn, void *arg) {
    if (!c.base) {
        c.size = stack_size();
        c.base = (uint8_t *)VirtualAlloc(nullptr, c.size, MEM_RESERVE, PAGE_READWRITE);
        if (!c.base) { fprintf(stderr, "[coro] could not reserve a %zu KB stack\n", c.size / 1024); return false; }
        c.committed = false;
    }
    if (!coro_recommit(c)) { fprintf(stderr, "[coro] could not commit a %zu KB stack\n", c.size / 1024); return false; }
    uint64_t *f = (uint64_t *)(c.top() - 8 - 7 * 8);   /* f[6] return address .. f[0] r15; top stays 16-byte aligned */
    memset(f, 0, 7 * 8);
    f[6] = (uint64_t)&tf_coro_tramp;
    f[3] = (uint64_t)fn;                               /* r12 */
    f[2] = (uint64_t)arg;                              /* r13 */
    c.sp = f;
    return true;
}
#else /* _WIN32: the original x86-64 Windows switch, with the TEB stack bounds */

asm(
    ".text\n"
    ".globl tf_coro_switch\n"
    "tf_coro_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %rsi\n"
    "    pushq %rdi\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $0xb0, %rsp\n"
    "    stmxcsr 0xa0(%rsp)\n"
    "    fnstcw 0xa4(%rsp)\n"
    "    movups %xmm6, 0x00(%rsp)\n"
    "    movups %xmm7, 0x10(%rsp)\n"
    "    movups %xmm8, 0x20(%rsp)\n"
    "    movups %xmm9, 0x30(%rsp)\n"
    "    movups %xmm10, 0x40(%rsp)\n"
    "    movups %xmm11, 0x50(%rsp)\n"
    "    movups %xmm12, 0x60(%rsp)\n"
    "    movups %xmm13, 0x70(%rsp)\n"
    "    movups %xmm14, 0x80(%rsp)\n"
    "    movups %xmm15, 0x90(%rsp)\n"
    "    movq %gs:0x30, %r10\n"
    "    pushq 0x1478(%r10)\n"
    "    pushq 0x10(%r10)\n"
    "    pushq 0x08(%r10)\n"
    "    movq %rsp, (%rcx)\n"
    "    movq %rdx, %rsp\n"
    "    movq %gs:0x30, %r10\n"
    "    popq 0x08(%r10)\n"
    "    popq 0x10(%r10)\n"
    "    popq 0x1478(%r10)\n"
    "    movups 0x00(%rsp), %xmm6\n"
    "    movups 0x10(%rsp), %xmm7\n"
    "    movups 0x20(%rsp), %xmm8\n"
    "    movups 0x30(%rsp), %xmm9\n"
    "    movups 0x40(%rsp), %xmm10\n"
    "    movups 0x50(%rsp), %xmm11\n"
    "    movups 0x60(%rsp), %xmm12\n"
    "    movups 0x70(%rsp), %xmm13\n"
    "    movups 0x80(%rsp), %xmm14\n"
    "    movups 0x90(%rsp), %xmm15\n"
    "    ldmxcsr 0xa0(%rsp)\n"
    "    fldcw 0xa4(%rsp)\n"
    "    addq $0xb0, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rdi\n"
    "    popq %rsi\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    retq\n"
    ".globl tf_coro_tramp\n"
    "tf_coro_tramp:\n"
    "    movq %r13, %rcx\n"
    "    subq $0x20, %rsp\n"
    "    callq *%r12\n"
    "    int3\n"
);
extern "C" void tf_coro_tramp(void);

static const size_t GUARD = 0x1000;
static size_t stack_size(void) {
    static size_t sz = [] { const char *e = getenv("TF_STACK_KB"); size_t kb = e ? (size_t)atoi(e) : 4096; if (kb < 256) kb = 256; return kb * 1024; }();
    return sz;
}
bool coro_recommit(Coro &c) {
    if (!c.base) return false;
    if (c.committed) return true;
    if (!VirtualAlloc(c.base, c.size, MEM_COMMIT, PAGE_READWRITE)) return false;
    DWORD old; VirtualProtect(c.base, GUARD, PAGE_NOACCESS, &old);
    c.committed = true; return true;
}
void coro_release(Coro &c) {
    if (!c.base || !c.committed) return;
    VirtualFree(c.base, c.size, MEM_DECOMMIT);
    c.committed = false;
}
bool coro_create(Coro &c, CoroFn fn, void *arg) {
    if (!c.base) {
        c.size = stack_size();
        c.base = (uint8_t *)VirtualAlloc(nullptr, c.size, MEM_RESERVE, PAGE_READWRITE);
        if (!c.base) { fprintf(stderr, "[coro] could not reserve a %zu KB stack\n", c.size / 1024); return false; }
        c.committed = false;
    }
    if (!coro_recommit(c)) { fprintf(stderr, "[coro] could not commit a %zu KB stack\n", c.size / 1024); return false; }
    uint8_t *y = c.top() - 0x40;                  /* rsp right after the first "ret": 16-byte aligned, coro_tramp makes the 0x20 of shadow space itself */
    uint64_t *f = (uint64_t *)(y - 8 - 64 - 0xb0 - 24);   /* frame: 3 TEB words, saved xmm/mxcsr area, 8 registers, return address */
    memset(f, 0, 24 + 0xb0 + 64 + 8);
    f[0] = (uint64_t)c.top(); f[1] = (uint64_t)(c.base + GUARD); f[2] = (uint64_t)c.base;
    uint32_t *fp = (uint32_t *)((uint8_t *)f + 24 + 0xa0); fp[0] = 0x1f80; fp[1] = 0x037f;   /* MXCSR and x87 control word: the usual defaults */
    uint64_t *regs = (uint64_t *)((uint8_t *)f + 24 + 0xb0);   /* r15 r14 r13 r12 rdi rsi rbx rbp, return address */
    regs[2] = (uint64_t)arg; regs[3] = (uint64_t)fn; regs[8] = (uint64_t)&tf_coro_tramp;
    c.sp = f;
    return true;
}
#endif /* __linux__ / _WIN32 */
