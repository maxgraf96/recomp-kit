/* x87_driver.c - runs x87 conformance vectors through translated code.
 *
 * tools/recomp/tests/x87_conformance.py translates one small function per
 * x87 operation - the same instruction bytes the hardware recording ran:
 * FNSTCW, FLDCW, FNCLEX, FLD m80 of each register input, the operation,
 * FNSTSW, FSTP m80 of each register left, FNSTSW, FNCLEX, FLDCW, RET - and
 * links them with harness.c, this file and the runtime's x87. This loop puts
 * each vector's inputs in the operand block those functions address through
 * ECX, calls the function, and copies the outputs back. One CPU context
 * serves every vector, as one thread ran them on the hardware, so state the
 * model leaks from one vector into the next shows up as a difference. */
#include <string.h>

#include "x86.h"

uint8_t *harness_mem(void);

/* Packed input record: function address, control word, three 80-bit
 * register inputs (ST(0), ST(1), ST(2)) and an 8-byte memory input. */
#define IN_SIZE 48u
#define IN_FN 0u
#define IN_CW 4u
#define IN_REGS 8u
#define IN_MEM 38u
/* Packed output record: three 80-bit outputs, the 8-byte memory output, the
 * status word after the operation and after the stores. */
#define OUT_SIZE 44u
#define OUT_REGS 0u
#define OUT_MEM 30u
#define OUT_SW 38u
#define OUT_SW_END 40u

/* The operand block, as the stub bytes address it ([ECX + disp8]). */
#define BLK_CW 0x00u
#define BLK_SW 0x04u
#define BLK_SW_END 0x06u
#define BLK_MEM_IN 0x08u
#define BLK_IN 0x10u
#define BLK_OUT 0x40u
#define BLK_MEM_OUT 0x70u
#define BLK_SIZE 0x80u

static X86 ctx;
static int ctx_ready;

/* A fresh thread's x87, as the runtime creates one. */
void x87_driver_reset(uint16_t cw) {
    memset(&ctx, 0, sizeof ctx);
    ctx.fpu_cw = cw;
    ctx.fpu_tag = 0xffffu;
    ctx.eflags_misc = 0x202u;
    ctx_ready = 1;
}

/* Run n vectors. block is a free guest address for the operand block, stack
 * the guest stack pointer to call with, ret the return address the harness's
 * table treats as the caller. */
void x87_driver_run(const uint8_t *in, uint8_t *out, uint32_t n, uint32_t block, uint32_t stack,
                    uint32_t ret) {
    uint8_t *mem = harness_mem();
    uint32_t i;
    if (!ctx_ready)
        x87_driver_reset(0x037fu);
    for (i = 0; i < n; i++) {
        const uint8_t *v = in + (size_t)i * IN_SIZE;
        uint8_t *r = out + (size_t)i * OUT_SIZE;
        uint8_t *blk = mem + block;
        uint32_t fn;
        memcpy(&fn, v + IN_FN, 4);
        memset(blk, 0, BLK_SIZE);
        memcpy(blk + BLK_CW, v + IN_CW, 2);
        memcpy(blk + BLK_MEM_IN, v + IN_MEM, 8);
        memcpy(blk + BLK_IN, v + IN_REGS, 10);
        memcpy(blk + BLK_IN + 0x10, v + IN_REGS + 10, 10);
        memcpy(blk + BLK_IN + 0x20, v + IN_REGS + 20, 10);
        ctx.r[R_ECX] = block;
        ctx.r[R_ESP] = stack;
        memcpy(mem + stack, &ret, 4);
        recomp_call(&ctx, fn);
        memcpy(r + OUT_REGS, blk + BLK_OUT, 10);
        memcpy(r + OUT_REGS + 10, blk + BLK_OUT + 0x10, 10);
        memcpy(r + OUT_REGS + 20, blk + BLK_OUT + 0x20, 10);
        memcpy(r + OUT_MEM, blk + BLK_MEM_OUT, 8);
        memcpy(r + OUT_SW, blk + BLK_SW, 2);
        memcpy(r + OUT_SW_END, blk + BLK_SW_END, 2);
    }
}

/* For the benchmark: the context, so a caller can time one operation. */
X86 *x87_driver_context(void) {
    return &ctx;
}
