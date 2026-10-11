// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game being run, from its title id (param.sfo TITLE_ID, Common::ElfInfo), so the
// parts written for one game (menu entries, patches, hooks) can say which games they are for and
// stay in the code for the others: PortGaGotoku runs Fist of the North Star: Lost Paradise on the
// same port, and keeps Bloodborne's code for the updates from upstream.

#pragma once

namespace BbGame {

enum Mask : unsigned {
    Bloodborne = 1u << 0,
    Hokuto = 1u << 1, ///< Hokuto ga Gotoku / Fist of the North Star: Lost Paradise
    All = ~0u,
};

/// Called once by bbgpu_init with the game's title id.
void SetSerial(const char* serial);
/// The running game's bit. A title id that is no known game gets every bit, so nothing is hidden
/// (the upstream behaviour); so does a call before SetSerial.
unsigned Current();
inline bool For(unsigned games) {
    return (Current() & games) != 0;
}
inline bool IsHokuto() {
    return Current() == Hokuto;
}

} // namespace BbGame
