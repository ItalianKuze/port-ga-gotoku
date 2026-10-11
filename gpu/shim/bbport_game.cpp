// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace BbGame {

namespace {

// Fist of the North Star: Lost Paradise.
constexpr const char* HokutoIds[] = {"CUSA12781"};

std::atomic<unsigned> game{All};

} // namespace

void SetSerial(const char* serial) {
    unsigned mask = All;
    for (const char* id : HokutoIds) {
        if (serial && !std::strcmp(serial, id)) {
            mask = Hokuto;
        }
    }
    game = mask;
    std::printf("GPU: game profile: %s (%s)\n",
                mask == Hokuto ? "Hokuto ga Gotoku" : "unknown title",
                serial ? serial : "no title id");
}

unsigned Current() {
    return game;
}

} // namespace BbGame
