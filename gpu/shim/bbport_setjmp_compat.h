// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: POSIX exposes sigsetjmp/siglongjmp so a recovery point survives a signal handler.
// MinGW-w64 has no signal handling and no sigjmp_buf, and there is nothing to save or restore,
// so degrade to plain setjmp/longjmp, which on Windows also does not spill the frame pointer.
#pragma once

#include <csetjmp>

#ifdef _WIN32

using sigjmp_buf = std::jmp_buf;
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, value) longjmp(env, value)

#endif