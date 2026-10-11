// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace BbGame {

namespace {

// Bloodborne's editions (the save data list in src/runtime_savedata.c) and Hokuto's.
constexpr const char* BloodborneIds[] = {"CUSA00207", "CUSA00208", "CUSA00299", "CUSA00900",
                                         "CUSA01363", "CUSA03014", "CUSA03023", "CUSA03173"};
constexpr const char* HokutoIds[] = {"CUSA12781"};

std::atomic<unsigned> game{All};

} // namespace

void SetSerial(const char* serial) {
    unsigned mask = All;
    for (const char* id : BloodborneIds) {
        if (serial && !std::strcmp(serial, id)) {
            mask = Bloodborne;
        }
    }
    for (const char* id : HokutoIds) {
        if (serial && !std::strcmp(serial, id)) {
            mask = Hokuto;
        }
    }
    game = mask;
    std::printf("GPU: game profile: %s (%s)\n",
                mask == Hokuto       ? "Hokuto ga Gotoku"
                : mask == Bloodborne ? "Bloodborne"
                                     : "unknown title, every menu entry shown",
                serial ? serial : "no title id");
}

unsigned Current() {
    return game;
}

} // namespace BbGame
