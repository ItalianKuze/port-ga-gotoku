// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: POSIX sigaction/siginfo_t/SA_* for the free-check single-step tracer. MinGW has none of
// them, and Windows has no signal handlers at all, so sigaction() is a no-op here: the tracer
// never arms and OnStepTrap is never reached. Include this only where those names are used;
// shadowing <signal.h> globally would also catch the copy libstdc++ and pthread pull in.
#pragma once

#include <csignal>

#ifdef _WIN32

// MinGW's signal.h only defines the C signal set; the tracer needs the POSIX pieces.
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif

struct siginfo_t {
    int si_signo;
    int si_code;
    int si_errno;
};

#define SA_SIGINFO 0x00000004
#define SA_NODEFER 0x40000000

struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t*, void*);
    void* sa_mask;
    int sa_flags;
};

inline int sigemptyset(void* set) {
    if (set) {
        *static_cast<void**>(set) = nullptr;
    }
    return 0;
}

inline int sigaction(int, const struct sigaction*, struct sigaction*) {
    // No-op: Windows has no signal handlers to install.
    return 0;
}

#endif