// SPDX-License-Identifier: GPL-2.0-or-later
// bbport_settings.cpp: the menu saves its keys into bbport.ini and keeps the launcher's (controls).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "gpu/shim/bbport_settings.h"

static std::string Read(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    char path[] = "/tmp/bbport-settings-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    const char ini[] = "# launcher\nupscaler=fsr3\nkey.cross=X, Space\npad.circle=a\nshow_fps=0\n"
                       "fullscreen_hint=1\n";
    assert(write(fd, ini, sizeof(ini) - 1) == ssize_t(sizeof(ini) - 1));
    close(fd);
    setenv("BB_CONFIG", path, 1);

    BbSettings::Load();
    auto& s = BbSettings::Get();
    assert(!s.show_fps && !s.show_hud && s.menu_x < 0.0f);
    s.show_fps = true;
    s.show_hud = true;
    s.menu_x = 0.625f;
    s.menu_y = 0.125f;
    BbSettings::Save();

    const std::string saved = Read(path);
    // The launcher's controls, its other keys, keys the menu no longer has (upscaler) and
    // comments stay; the menu's keys are replaced in place, new ones appended.
    assert(saved.find("# launcher\nupscaler=fsr3\nkey.cross=X, Space\npad.circle=a\nshow_fps=1\n"
                      "fullscreen_hint=1\n") == 0);
    assert(saved.find("show_hud=1\n") != std::string::npos);
    assert(saved.find("menu_pos=0.6250,0.1250\n") != std::string::npos);

    s.menu_x = -1.0f;
    s.menu_y = -1.0f;
    s.show_hud = false;
    BbSettings::Load();
    assert(s.menu_x == 0.625f && s.menu_y == 0.125f && s.show_hud);
    unlink(path);
    std::puts("PASS: settings save keeps other keys, menu position");
}
