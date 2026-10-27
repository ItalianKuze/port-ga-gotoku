PortGaGotoku for Linux / Steam Deck (test package)
==================================================

Runs Fist of the North Star: Lost Paradise (CUSA12781) from your own dump of the game. The game
files are only read.

Start
-----
Extract the archive anywhere (on the Steam Deck: in Desktop Mode, for example in your home folder)
and run PortGaGotoku.sh. The first time it asks for the game folder (the one with eboot.bin and
sce_sys), then prepares the game (a few seconds) and starts it. To choose another folder:
  ./PortGaGotoku.sh --choose-game

Adding it to Steam (Steam Deck Game Mode)
-----------------------------------------
1. In Desktop Mode, run PortGaGotoku.sh once and choose the game folder.
2. Steam > Games > Add a Non-Steam Game to My Library > Browse, show "All files", choose
   PortGaGotoku.sh, Add Selected Programs.
3. In Game Mode, start it from the library. The controller works with Steam's default layout
   (Gamepad). In Game Mode the game runs fullscreen.
Do not set a Proton compatibility tool for it: this is a native Linux program.

Files and logs
--------------
Everything the game writes goes to PortGaGotoku-data, next to the extracted folder:
  PortGaGotoku-data/logs/     one log per start (hokuto-<date>-<time>.log; -prepare.log for the
                              preparation): attach it when you report a problem
  PortGaGotoku-data/user/     saves
  PortGaGotoku-data/cache/    shader cache
  PortGaGotoku-data/pgg.ini   in-game menu settings (Insert or L3+R3 opens the menu)
  PortGaGotoku-data/launcher.json  the game folder

Needs: a Vulkan driver (Mesa RADV on the Steam Deck), python3 (part of SteamOS), zenity for the
folder dialog (part of SteamOS; without it the folder is asked in the terminal).
Problems: https://github.com/ItalianKuze/port-ga-gotoku/issues
