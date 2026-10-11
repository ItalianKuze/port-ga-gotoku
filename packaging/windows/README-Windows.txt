PortGaGotoku - Fist of the North Star: Lost Paradise (PS4, CUSA12781) on Windows
==================================================================================

You need your own legal dump of the game (the CUSA12781 folder with eboot.bin and sce_sys). No game
files are included. Unofficial project, not affiliated with SEGA, Ryu Ga Gotoku Studio or the
rights holders of Fist of the North Star.

  PortGaGotoku.exe   the launcher: game folder, options, log. It asks for the game folder at the
                     first start.
  Start Hokuto.exe   starts the game with the saved settings, without opening the launcher.

Everything the program writes (settings, boot image, saves, save backups, logs, shader cache) goes
to PortGaGotoku-data, next to this folder. In the game: Insert or L3+R3 opens the port's menu, F11
the statistics HUD. To quit: the window's X (Alt+F4 in fullscreen); a save in progress is finished
first.

Requirements: 64-bit Windows 10/11, a CPU with AVX, BMI1, MOVBE and LZCNT, a Vulkan 1.3 graphics
card, Microsoft Edge WebView2 Runtime (part of Windows 11) for the launcher window. Tested on one
machine (NVIDIA GeForce RTX 5070).

Based on bbport (github.com/deadinside28/bloodborne_pc), shadPS4, and Supermedo's Windows launcher
and package (github.com/Supermedo/bloodborne_pc). License: GPL-2.0-or-later (LICENSE). Fonts:
Figtree (SIL Open Font License 1.1), DejaVu Sans (Bitstream Vera license).
