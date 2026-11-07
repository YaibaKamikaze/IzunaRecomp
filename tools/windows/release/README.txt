Ninja Gaiden II - IzunaRecomp @VERSION@ for Windows
====================================================

A native recompilation of Ninja Gaiden II (Xbox 360), drawn by the Kasumi
Renderer (Vulkan). No emulator and no game files come with it: you need your
own copy of the game.

Needs: Windows 10 or 11 (64-bit), a GPU with Vulkan (NVIDIA GTX 900 /
AMD RX 400 / Intel Arc or newer) and an up-to-date graphics driver. Nothing
else to install (no Visual C++ Redistributable).

Two programs: ng2.exe is built for processors with AVX2 (Intel since 2013,
every AMD Ryzen) and is faster; ng2-compat.exe is the same game for older
processors. Always start ng2.exe - on an older processor it starts
ng2-compat.exe by itself. Keep both in the folder.


Starting
--------
1. Run ng2.exe.
2. The hub asks for your game folder: choose the folder of your Ninja Gaiden II
   copy (the one with default.xex). It is remembered.
3. START GAME.

The first start takes a little longer: ng2.exe reads the game's shaders from
your copy and compiles them for your GPU (on every core, with dxcompiler.dll -
keep it beside ng2.exe; the window title shows how far along it is). They are
kept in %LOCALAPPDATA%\IzunaRecomp\shaders, so later starts skip this.

The folder can also be given on the command line:
  ng2.exe --game_data_root="D:\Games\Ninja Gaiden II"


Controls
--------
Hub and in-game menu (the overlay):
  Controller   D-pad / left stick move, Cross/A choose, Circle/B back
  Keyboard     arrows or WASD move, Enter or Space choose, Esc or Backspace back,
               Q / E change tabs
  Mouse        click to choose, right-click back, wheel scrolls

In game:
  Esc or F10, or hold the View/Share button   open or close the in-game menu
  Alt+Enter                                    fullscreen / window

Playing with the keyboard (on by default; it works alongside a controller,
both drive player 1). No mouse buttons are used. Change any key in SETTINGS >
Controls: choose the row, press Enter, then the key.
  W A S D   move                 T F G H   camera (up, left, down, right)
  Space     jump (A)             J         attack (X)
  K         strong attack (Y)    L         projectile (B)
  Shift     block (LT)           Q         centre camera (RT)
  U         LB                   O         RB
  C / V     left / right stick press
  Arrows    d-pad                Enter     Start       Tab   Back


Settings worth knowing (SETTINGS in the hub, or the in-game menu)
-------------------------------------------------------------
Display > V-Sync
  Auto     follows your monitor (60, 120, 144 Hz... and G-Sync / FreeSync).
  60 Hz / 59.94 Hz
           the game's clock locked to that rate. Try one of these if motion is
           not smooth on your screen.
Display > Resolution     Auto (your screen's) or 720p to 2160p, each drawn at that
                         true resolution. Above your screen is sharper, heavier.
Display > Present mode   FIFO (V-Sync), FIFO relaxed, Mailbox or Immediate (tearing):
                         how finished frames reach the screen. None changes the
                         game's speed. From the next start.
Display > Fullscreen refresh
                         On a 144-170 Hz screen without G-Sync / FreeSync, fullscreen
                         switches it to 120, 180 or 240 Hz so every frame shows
                         equally long; your desktop's rate comes back on exit.
Game > Skip cutscenes with Cross / A
                         Cross (A on Xbox, or Start / Options) skips a cutscene
                         also the first time you see it - the game normally
                         lets you skip only the ones already watched. On by
                         default.
Sound > Audio delay (A/V sync)
                         Holds the sound back to meet the picture (40 ms default).
Sound > Volume / Stereo level / Bass on stereo
                         Full stereo level plays the game's mix at its own
                         level with a limiter keeping loud moments clean; Bass
                         on stereo brings the 5.1 mix's bass channel into
                         headphones and stereo speakers.
Performance > Input latency  Low shows each frame sooner; turn off if the picture
                         tears or hitches on your PC.
Sound > Cutscene audio fix
                         keeps sound in step with the picture.
Game > Button prompts    Auto shows PlayStation buttons when a PlayStation
                         controller was in use as the game started, Xbox
                         buttons otherwise.


Folders
-------
  DLC\              your own Xbox 360 DLC packages (DLC\README.txt)
  Saves Import\     Xbox 360 saves to bring in (Saves Import\README.txt)
  mods\             mods: one folder each (models, textures, scripts...); turn
                    them on and off in the hub's MODS page (mods\README.txt)
  hub\              the hub's pictures and music (hub\music\README.txt)
  user\             settings and saves (made on the first run; back this up)
  logs\             logs, and crash-<time>.dmp after a crash
  %LOCALAPPDATA%\IzunaRecomp\cache
                    the built GPU pipelines, shared by every copy of the game on
                    this PC so a new version starts smooth. Safe to delete.
  %LOCALAPPDATA%\IzunaRecomp\shaders
                    the shaders compiled from your game at the first start.
                    Safe to delete (they are compiled again).


When something goes wrong
-------------------------
Send logs\ (the newest ng2_*.log, and crash-*.dmp if there is one). The log
says which GPU draws the game and which one drives your screen.

Logging is on by default. Settings > Developer > Logging turns it off from
the next start: no log is written at all. Turn it back on before reproducing
a problem to report.

On a laptop the game picks the discrete GPU. If Windows still runs it on the
integrated one: Settings > System > Display > Graphics > ng2.exe > High
performance.


IzunaRecomp is not affiliated with Koei Tecmo or Microsoft. Ninja Gaiden is a
trademark of Koei Tecmo. The licenses of the libraries inside ng2.exe are in
licenses\.
