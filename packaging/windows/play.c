/* SPDX-License-Identifier: GPL-2.0-or-later
 * Gioca Hokuto.exe: starts Fist of the North Star: Lost Paradise with the settings saved by the
 * launcher, without opening it: runs `PortGaGotoku.exe --play` from its own folder and waits for
 * it, so shortcuts and front ends see the game running; returns the game's exit code. Extra
 * arguments are passed on. From Supermedo's packaging/windows/play.c (bloodborne_pc v1.6). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show)
{
    (void)instance, (void)previous, (void)show;
    static wchar_t dir[32768], exe[32768], line[32768];
    DWORD length = GetModuleFileNameW(NULL, dir, 32768);
    if (length == 0 || length >= 32768)
        return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;
    swprintf(exe, 32768, L"%ls\\PortGaGotoku.exe", dir);
    swprintf(line, 32768, L"\"%ls\" --play%ls%ls", exe, arguments && *arguments ? L" " : L"",
             arguments ? arguments : L"");

    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, dir, &startup, &process)) {
        MessageBoxW(NULL, L"PortGaGotoku.exe non trovato accanto a questo file.\n"
                          L"PortGaGotoku.exe was not found next to this file.",
                    L"PortGaGotoku", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
