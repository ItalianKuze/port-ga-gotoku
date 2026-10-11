/* SPDX-License-Identifier: GPL-2.0-or-later
 * bbport: the recovery point for speculative reads of guest memory, one declaration for the
 * runtime (C) and libbbgpu (C++).
 *
 * A thread that reads guest memory ahead of the game (the GPU draw-preparation workers, the SRT
 * walker check) sets a recovery point, arms it, reads, and disarms it. A fault while it is armed
 * resumes at the recovery point as if BB_RECOVER_SET returned 1, instead of stopping the game:
 *
 *     BbRecoverBuf recover;
 *     if (BB_RECOVER_SET(recover)) { BB_RECOVER_ARM(nullptr); ... give up ... }
 *     BB_RECOVER_ARM(&recover);
 *     ... read guest memory ...
 *     BB_RECOVER_ARM(nullptr);
 *
 * Linux: a sigjmp_buf and the thread-local runtime_fault_recover, resumed with siglongjmp from
 * the SIGSEGV handler (probe.c).
 *
 * Windows: the buffer layout is the one of the Mrsuss60 Windows port (RuntimeRecoverBuf): rbx,
 * rbp, rdi, rsi, r12-r15, rsp after the return, the return address, xmm6-xmm15, MXCSR and the x87
 * control word, saved by runtime_setjmp (runtime_win.c). The thread-local pointer lives only in
 * bbport_runtime.dll and is set through runtime_recover_arm(): a __thread variable is not shared
 * between two DLLs, which is why libbbgpu's recovery points never reached the VEH and a faulting
 * speculative read crashed the game. The VEH (probe.c) resumes by loading these registers into
 * the exception CONTEXT and returning EXCEPTION_CONTINUE_EXECUTION. */
#ifndef BBPORT_RECOVER_H
#define BBPORT_RECOVER_H

#ifdef _WIN32

typedef struct BbRecoverBuf {
    unsigned char registers[256];
} __attribute__((aligned(16))) BbRecoverBuf;

#ifdef __cplusplus
static_assert(sizeof(BbRecoverBuf) == 256 && alignof(BbRecoverBuf) == 16, "BbRecoverBuf layout");
extern "C" {
#else
_Static_assert(sizeof(BbRecoverBuf) == 256 && _Alignof(BbRecoverBuf) == 16, "BbRecoverBuf layout");
#endif
/* Saves the callee-saved state; returns 0 now and 1 when a fault resumes here. */
int runtime_setjmp(BbRecoverBuf *buf) __attribute__((returns_twice));
/* This thread's recovery point; NULL disarms it. */
void runtime_recover_arm(BbRecoverBuf *buf);
#ifdef __cplusplus
}
#endif
#define BB_RECOVER_SET(buf) runtime_setjmp(&(buf))
#define BB_RECOVER_ARM(buf) runtime_recover_arm(buf)

#else /* POSIX */

#include <setjmp.h>
typedef sigjmp_buf BbRecoverBuf;
#ifdef __cplusplus
extern "C" __thread sigjmp_buf *runtime_fault_recover;
#else
extern __thread sigjmp_buf *runtime_fault_recover;
#endif
#define BB_RECOVER_SET(buf) sigsetjmp(buf, 0)
#define BB_RECOVER_ARM(buf) (runtime_fault_recover = (buf))

#endif
#endif
