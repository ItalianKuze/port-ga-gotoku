// SPDX-License-Identifier: GPL-2.0-or-later
// PortGaGotoku: the game being run, from its title id (param.sfo TITLE_ID, Common::ElfInfo), so the
// parts written for one game can say which games they are for. Only Fist of the North Star: Lost
// Paradise has a profile (bbport's Bloodborne-specific parts are not in this fork).

#pragma once

namespace BbGame {

enum Mask : unsigned {
    Hokuto = 1u << 1, ///< Hokuto ga Gotoku / Fist of the North Star: Lost Paradise
    All = ~0u,
};

/// Called once by bbgpu_init with the game's title id.
void SetSerial(const char* serial);
/// The running game's bit; a title id that is no known game (or a call before SetSerial) gets
/// every bit.
unsigned Current();
inline bool For(unsigned games) {
    return (Current() & games) != 0;
}
inline bool IsHokuto() {
    return Current() == Hokuto;
}

} // namespace BbGame
