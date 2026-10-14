/* Guest x86 SEH checkpoints. Included by generated C as well as the runtime.
 * Delphi frames land through recomp_seh_intercept; MSVC frames (msvc_eh.cpp)
 * through recomp_seh_resume. Both checkpoints are the same emitted pair:
 *   { jmp_buf *b_ = <enter>(c); if (setjmp(*b_)) { recomp_seh_land(c); return; } } */
#ifndef RECOMP_SEH_H
#define RECOMP_SEH_H
#include "x86.h"
#ifndef RECOMP_FREESTANDING /* x86.h brought freestanding.h's setjmp */
#include <setjmp.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* setjmp must be executed by the live generated function, never in enter(). */
jmp_buf *recomp_seh_frame_enter(X86 *c);
/* Helpers orphan their surviving records before returning. The caller adopts
 * the newest one and executes setjmp in its own live host frame. */
uint64_t recomp_seh_frame_mark(X86 *c);
void recomp_seh_frame_orphan(X86 *c, uint64_t mark);
jmp_buf *recomp_seh_frame_adopt(X86 *c);
/* Drop records below ESP at this callback level; retain active landings. */
void recomp_seh_frame_leave(X86 *c);
void recomp_seh_land(X86 *c);
uint32_t recomp_seh_pending_target(void);
void recomp_seh_intercept(X86 *c, uint32_t target);
/* MSVC frames with a try block or an __except scope: a checkpoint where the
 * frame links its registration (MOV FS:[0],ESP), retired when the frame
 * unlinks itself (MOV FS:[0],reg drops every MSVC record below the new head). */
jmp_buf *recomp_eh_frame_enter(X86 *c);
void recomp_eh_frame_unlink(X86 *c);
/* _setjmp3 (msvc_eh.cpp), emitted at the call site like _setjmp:
 *   { jmp_buf *b_ = recomp_setjmp3_prepare(c);
 *     if (setjmp(*b_) == 0) recomp_setjmp_return(c, 0); else recomp_setjmp3_resume(c); }
 * prepare fills the guest's _JUMP_BUFFER (registration, try level, unwind
 * function) for msvcrt's longjmp; resume completes the call with longjmp's value. */
jmp_buf *recomp_setjmp3_prepare(X86 *c);
void recomp_setjmp3_resume(X86 *c);
/* Verbose-only, bounded validation. Logs the first failure and last good point. */
void recomp_seh_validate_chain(X86 *c, const char *phase, const char *detail);

/* Context reuse/teardown on its owning host thread; NULL drops all its state. */
void recomp_seh_reset(X86 *c);
/* A callback return abandons frames/landings created at this call depth. */
void recomp_seh_callback_leave(X86 *c, uint32_t depth);
/* Returns zero for an exhausted chain; kernel32 retains its abort diagnostics.
 * A nonzero return is possible only through the test-only unhandled hook. */
int recomp_seh_raise(X86 *c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args);
void recomp_seh_unwind(X86 *c, uint32_t target, uint32_t target_ip, uint32_t record,
                       uint32_t retval);
/* A raise whose ExceptionAddress (and context EIP) is `address`: a fault. */
int recomp_seh_raise_at(X86 *c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args,
                        uint32_t address);
/* RtlUnwind's handler walk to `target` (0: the whole chain) without arming a
 * Delphi landing. `record` may be 0 (STATUS_UNWIND). */
void recomp_seh_global_unwind(X86 *c, uint32_t target, uint32_t record);
/* Resume at `target` in the live activation whose checkpoint is `reg`; the
 * caller has set EBP, ESP and FS:[0]. Abandons every newer checkpoint,
 * callback, profile entry and dispatch, then longjmps. Does not return. */
void recomp_seh_resume(X86 *c, uint32_t reg, uint32_t target)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noreturn))
#endif
    ;
/* A runtime-owned MSVC checkpoint (a catch block in progress) for `reg`, its
 * removal, and - after its setjmp returns nonzero - the continuation to run. */
jmp_buf *recomp_seh_frame_enter_at(X86 *c, uint32_t reg);
void recomp_seh_frame_drop(X86 *c, uint32_t reg);
uint32_t recomp_seh_take_resume(X86 *c);
int recomp_seh_has_checkpoint(X86 *c, uint32_t reg);
uint32_t recomp_seh_dispatch_depth(void);
void recomp_seh_dispatch_truncate(uint32_t depth);

#ifdef POPM_TESTING
typedef void (*RecompSehUnhandledHook)(X86 *, uint32_t record, uint32_t context);
void recomp_seh_test_unhandled_hook(RecompSehUnhandledHook hook);
uint32_t recomp_seh_test_frame_count(X86 *c);
#endif

#ifdef __cplusplus
}
#endif
#endif
