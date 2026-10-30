Streets of SimCity (Maxis, 1997) - fix for modern Windows
=========================================================

Makes the 1997 game run properly on Windows 10/11: OpenGL at your monitor's resolution and
shape (widescreen), steady frame rate, no CD needed, longer draw distance, HD textures,
smoke and fire effects, and a few crash fixes. The game's own files are not included; you
need your own copy.

INSTALL
The easy way is StreetsOfSimCity-Setup-<version>.exe from the releases page
(https://github.com/jasonwarta/sosc/releases): it downloads the game too. This zip is for
installing by hand:
1. Get the game: Streets_of_SimCity_Win_ISO_EN.zip (345 MB, the full CD as an ISO) from
   https://oldgamesdownload.com/game/streets-of-simcity-6jg/#game-downloads
   It's the copy the fix was made and tested with. Skip the site's setup guide (compatibility
   mode, 256 colours): the fix replaces all of that.
2. Unzip this release into a new folder, for example C:\Games\Streets.
3. Drag the game download (the zip, or the .iso inside it) onto "Copy game from CD.cmd" in
   that folder. It copies the game in, laid out the way the fix expects. (Copying the CD by
   hand doesn't work: the game itself sits in the CD's exe folder.)
4. Double-click "Play (OpenGL).cmd".

The first launch installs the moose (see below) and makes logs\. Windows SmartScreen or your
antivirus may ask about dsound.dll / streets_fix.dll the first time; they're this fix.

KEYS (in game)
  W A S D            drive (same as the arrow keys)
  Ctrl+Shift+F9      draw distance: cycles 77 -> 91 -> 61 tiles (91 can be slow toward the city)
  Ctrl+Shift+F10     screenshot (saved in screenshots\)
  Ctrl+Shift+F12     emergency quit, if the game ever hangs

SETTINGS
Everything is in streets_fix.ini (open it in Notepad; each setting has a comment). The ones
you're most likely to want:
  fps=120            frame rate cap
  widescreen=auto    auto = your monitor's shape, off = the original 4:3, or 16:9, 21:9, ...
  worldtiles=91      the largest draw distance; worldstart=0 starts in the middle (77)
                     (worldstart=61 starts at the original distance, for a slower PC)
  hdtextures=1       HD textures (0 = the original look)
  moose=1            the cow becomes a bull moose (0 puts the cow back)

IF SOMETHING GOES WRONG
Send the newest file in the logs\ folder (Start menu > "Streets of SimCity logs" if you
used the installer) (session_<date>_<time>.log) along with a
screenshot if you can. The log records what the fix did and where the game got stuck or
crashed.

CREDITS
- cnc-ddraw (ddraw.dll, Shaders\) by FunkyFr3sh, MIT license: licenses\cnc-ddraw.txt
- Moose sounds made from public domain recordings by the US Fish and Wildlife Service
  (via SoundBible).
- The fix: MIT license, licenses\streets-fix.txt. Source and notes:
  https://github.com/jasonwarta/sosc
