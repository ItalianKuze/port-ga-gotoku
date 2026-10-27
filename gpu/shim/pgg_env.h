// SPDX-License-Identifier: GPL-2.0-or-later
// PortGaGotoku: the environment variables are named PGG_<name>. bbport's BB_<name> spelling is
// still read when PGG_<name> is not set, for existing scripts and shortcuts. Shared by the
// runtime (C) and the GPU library (C++).
#pragma once

#include <stdio.h>
#include <stdlib.h>

/// The value of PGG_<name>, else of BB_<name>, else null (pgg_getenv("FRAME_STATS")).
static inline const char* pgg_getenv(const char* name) {
    char key[128];
    snprintf(key, sizeof key, "PGG_%s", name);
    const char* value = getenv(key);
    if (value) {
        return value;
    }
    snprintf(key, sizeof key, "BB_%s", name);
    return getenv(key);
}
