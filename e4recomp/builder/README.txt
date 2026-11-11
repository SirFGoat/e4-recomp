E4: Every Extend Extra Extreme - PC/Mac builder
================================================

This kit turns YOUR copy of E4 (the Xbox Live Arcade version) into a native PC or Mac game.
It contains no game data: the game is recompiled from your own files, on your computer.

1. Put your E4 XBLA zip (or the package file inside it) in this folder, next to
   "Build E4".
2. Run the builder:
     Windows: double-click "Build E4.bat"
     Mac:     right-click "Build E4.command" > Open (the first time; afterwards a
              double-click works)
3. Wait. The first run installs a compiler if your computer has none:
     Windows: Visual Studio Build Tools + LLVM (about 5 GB; Windows asks for permission)
     Mac:     Xcode Command Line Tools (macOS asks you to click Install)
   Then it recompiles and builds the game (10-20 minutes). You need an internet
   connection for the first run.
4. Play:
     Windows: the E4 folder appears here; run E4\e4_launcher.exe
     Mac:     E4.dmg opens; drag E4 onto Applications
   Enter a name for the online leaderboards and press PLAY.

The result contains your game files - keep it to yourself, don't share it.

Requirements: Windows 10/11 64-bit, or a Mac with Apple Silicon (M1 or newer, macOS 13+).
A gamepad is recommended (Xbox, PlayStation, ...).

Source code, help and issues: https://github.com/SirFGoat/e4-recomp
