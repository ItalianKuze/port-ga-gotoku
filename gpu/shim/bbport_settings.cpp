// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include "bbport_game.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "pgg_env.h"

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = pgg_getenv("CONFIG");
    return env && env[0] ? env : "pgg.ini";
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const int i = std::atoi(value.c_str());
    if (key == "menu_language") {
        if (value == "en") v.menu_language = MenuLanguage::English;
        else if (value == "it") v.menu_language = MenuLanguage::Italian;
        else std::printf("Settings: unknown menu_language '%s' (expected en or it)\n", value.c_str());
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "show_hud") {
        v.show_hud = i != 0;
    } else if (key == "menu_pos") {
        float x = -1.0f, y = -1.0f;
        if (std::sscanf(value.c_str(), "%f,%f", &x, &y) == 2 && x >= 0.0f && x <= 1.0f && y >= 0.0f &&
            y <= 1.0f) {
            v.menu_x = x;
            v.menu_y = y;
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

// PortGaGotoku: the menu's Italian texts, for the entries shown on Hokuto.
constexpr std::pair<const char*, const char*> ItalianTexts[] = {
    {"Statistics (HUD)", "Statistiche (HUD)"},
    {"Show statistics (F11)", "Mostra le statistiche (F11)"},
    {"Start measurement (90 s, F9)", "Avvia la misura (90 s, F9)"},
    {"Records 90 s of frame times, CPU and GPU time and thread load into the logs folder (perf-*.csv).",
     "Registra 90 s di tempi dei frame, tempo CPU e GPU e carico dei thread nella cartella dei log (perf-*.csv)."},
    {"Measuring: %.0f / %d s", "Misura in corso: %.0f / %d s"},
    {"CPU translation %s  GPU %s", "CPU traduzione %s  GPU %s"},
    {"Pipeline compiles: %u in 5 s, %llu total", "Compilazioni pipeline: %u in 5 s, %llu in tutto"},
    {"Busiest threads:", "Thread più carichi:"},
    {"REC %.0f / %d s", "REC %.0f / %d s"},
    {"Game settings", "Impostazioni del gioco"},
    {"Hokuto's own settings will be here", "Qui arriveranno le impostazioni di Hokuto"},
    {"Other", "Altro"},
    {"FPS counter in corner", "Contatore FPS nell'angolo"},
    {"Close", "Chiudi"},
    {"Settings are saved to pgg.ini", "Le impostazioni sono salvate in pgg.ini"},
};

const char* MenuText(const char* english) {
    if (Get().menu_language.load() == MenuLanguage::Italian && BbGame::IsHokuto()) {
        for (const auto& [en, it] : ItalianTexts) {
            if (!std::strcmp(en, english)) {
                return it;
            }
        }
    }
    return english;
}

void Load() {
    auto& v = Get();
    // bbport: the menu's language until one is chosen in it (menu_language in pgg.ini): the
    // launcher's (PGG_MENU_LANGUAGE), else the system's, as the launcher picks its own.
    {
        const char* lang = pgg_getenv("MENU_LANGUAGE");
        if (!lang || !*lang) {
            for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
                if (const char* value = std::getenv(key); value && *value) {
                    lang = value;
                    break;
                }
            }
        }
        const auto is = [lang](char a, char b) {
            return lang && (lang[0] | 0x20) == a && (lang[1] | 0x20) == b;
        };
        v.menu_language = is('i', 't') ? MenuLanguage::Italian : MenuLanguage::English;

    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
}

void Save() {
    const auto& v = Get();
    // The keys the menu writes, in this order; the rest of the file stays as it is: the
    // launcher's controls (key.* / pad.*) and its other keys, comments.
    std::vector<std::pair<std::string, std::string>> keys;
    const auto put = [&](const char* key, std::string value) { keys.emplace_back(key, std::move(value)); };
    const auto fixed = [](float value, int digits) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", digits, value);
        return std::string(text);
    };
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    put("menu_language", v.menu_language == MenuLanguage::Italian ? "it" : "en");
    put("show_fps", flag(v.show_fps));
    put("show_hud", flag(v.show_hud));
    if (v.menu_x >= 0.0f && v.menu_y >= 0.0f) {
        put("menu_pos", fixed(v.menu_x, 4) + "," + fixed(v.menu_y, 4));
    }

    std::string out;
    bool had_lines = false;
    std::vector<bool> written(keys.size());
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), file)) {
            had_lines = true;
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            const auto eq = text.find('=');
            if (!text.empty() && text[0] != '#' && eq != std::string::npos) {
                std::string key = text.substr(0, eq);
                key.erase(key.find_last_not_of(" \t") + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                const auto it = std::find_if(keys.begin(), keys.end(),
                                             [&](const auto& k) { return k.first == key; });
                if (it != keys.end()) {
                    out += it->first + "=" + it->second + "\n";
                    written[size_t(it - keys.begin())] = true;
                    continue;
                }
            }
            out += text + "\n";
        }
        std::fclose(file);
    }
    if (!had_lines) {
        out = "# PortGaGotoku settings (in-game menu: Insert / L3+R3)\n";
    }
    for (size_t k = 0; k < keys.size(); ++k) {
        if (!written[k]) {
            out += keys[k].first + "=" + keys[k].second + "\n";
        }
    }
    // Replaced whole: a crash while writing leaves the old file.
    const std::string temporary = std::string(Path()) + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    bool ok = file && std::fwrite(out.data(), 1, out.size(), file) == out.size();
    ok = file && std::fclose(file) == 0 && ok;
    if (!ok || std::rename(temporary.c_str(), Path()) != 0) {
        std::printf("Settings: cannot write %s\n", Path());
    }
}

} // namespace BbSettings
