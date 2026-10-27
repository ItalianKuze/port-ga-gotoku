PortGaGotoku for Linux / Steam Deck (AppImage, test build)
==========================================================

Runs Fist of the North Star: Lost Paradise (CUSA12781) from your own dump of the game. The game
files are only read.

Start
-----
Put the .AppImage in a folder of its own (on the Steam Deck: in Desktop Mode, for example
~/Games/PortGaGotoku), make it executable and start it:
  chmod +x PortGaGotoku-*.AppImage        (or: Properties > Permissions > Is executable)
  ./PortGaGotoku-*.AppImage
The first time it asks for the game folder (the one with eboot.bin and sce_sys), prepares the game
(a few seconds) and starts it. To choose another folder: ./PortGaGotoku-*.AppImage --choose-game

Adding it to Steam (Steam Deck Game Mode)
-----------------------------------------
1. In Desktop Mode, start the AppImage once and choose the game folder.
2. Steam > Games > Add a Non-Steam Game to My Library > Browse, show "All files", choose the
   .AppImage, Add Selected Programs.
3. In Game Mode, start it from the library. The controller works with Steam's default layout
   (Gamepad); the game runs fullscreen. Do not set a Proton compatibility tool: it is a native
   Linux program.

Files and logs
--------------
Everything the game writes goes to PortGaGotoku-data, next to the .AppImage file:
  PortGaGotoku-data/logs/     one log per start (hokuto-<date>-<time>.log; -prepare.log for the
                              preparation): attach it when you report a problem
  PortGaGotoku-data/user/     saves
  PortGaGotoku-data/out/      the prepared boot image
  PortGaGotoku-data/cache/    shader cache
  PortGaGotoku-data/pgg.ini   in-game menu settings (Insert or L3+R3 opens the menu)
  PortGaGotoku-data/launcher.json  the game folder

Needs: a Vulkan driver (Mesa RADV on the Steam Deck), FUSE (to mount the AppImage), python3 and
zenity (all part of SteamOS; without zenity the folder is asked in the terminal).
Problems: https://github.com/ItalianKuze/port-ga-gotoku/issues
