// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in pgg.ini (PGG_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>

namespace BbSettings {

/// Italian is offered where the menu has Italian texts (PortGaGotoku, Hokuto's entries).
enum class MenuLanguage { English, Italian };

struct Values {
    std::atomic<MenuLanguage> menu_language{MenuLanguage::English};
    std::atomic<bool> show_fps{false};
    std::atomic<bool> show_hud{false}; ///< PortGaGotoku: the statistics HUD (F11)
    /// The settings menu's position (fraction of the screen), -1 until it is moved.
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
};

Values& Get();
/// Localized overlay text; the Language selector itself stays in English. Italian comes from a
/// table keyed by the English text, used only on Hokuto (English where it has no entry).
const char* MenuText(const char* english);

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Writes the file (menu changes).
void Save();


} // namespace BbSettings
