# Hexen II (uHexen2) - Miyoo Mini Plus / OnionOS Port

This is a port of [**Hammer of Thyrion (uHexen2)**](https://github.com/sezero/uhexen2) - the cross-platform
source port of Raven Software's **Hexen II** (1997), maintained by O. Sezer - to the **Miyoo Mini Plus** handheld
running **OnionOS**.

The Miyoo Mini Plus has no 3D GPU, so this port runs on the game's original software renderer, which was spread
over both CPU cores for it: the picture is exactly the one the PC version draws, at 640x480, with the status bar,
menus and text kept at their original size.

**[Download the latest release](https://github.com/cacuracaptors/uhexen2-miyoomini/releases)**

## OnionOS Exclusive Features

- 3D view at a native 640x480, with the status bar, menus and text at 320x240 (big and readable on the small
  screen), no overclock needed
- The original software renderer, spread over both CPU cores, with the very same image:
  - walls, lit surfaces, water and the monsters drawn by both cores at once
  - monsters fully hidden behind walls skipped
  - the picture sent to the screen by the second core, with vsync: no screen tearing
- 50 to 60 FPS
- The CD soundtrack, from the music files of the GOG version (Ogg Vorbis, decoded on the second core)
- Easy installation: just copy the `data1` folder of the GOG version; upper or lower case file names don't matter
- A control scheme for the Miyoo Mini Plus buttons, including looking up and down and sidestepping
- A Cheats page in the Options menu, since there is no keyboard for the console
- Yes/no questions (Quit, New Game...) answered with the Miyoo buttons

## Installation

1. Download the release zip and extract it to the root of your OnionOS SD card. This places the `Hexen2`
   folder in `Roms/PORTS/Games/`, and the shortcut and its image in `Roms/PORTS/Shortcuts/` and
   `Roms/PORTS/Imgs/`.
2. You need your own legitimate copy of Hexen II, in the **GOG version**:
   [Hexen II](https://www.gog.com/en/game/hexen_ii). From its installation folder:
   - copy everything inside the `data1` folder (`pak0.pak`, `pak1.pak`, `progs.dat`, `progs2.dat`,
     `strings.txt`, `hexen.rc` and the rest) into `Roms/PORTS/Games/Hexen2/data1/` (required - the game data)
   - copy the `music` folder (`Track02.ogg` to `Track17.ogg`) into that same `data1` folder, so that it becomes
     `Roms/PORTS/Games/Hexen2/data1/music/` (optional - the music)

   To find the installation folder in GOG Galaxy: select the game, click the settings button next to "Play",
   then "Manage installation" > "Show folder".
3. On OnionOS, open **Hexen II** from the Ports list (use "Refresh roms" at the bottom of the list if it
   doesn't show up).

The game's settings and saved games go to `Roms/PORTS/Games/Hexen2/userdir/data1/`.

## Controls

| Button | Action |
|---|---|
| D-pad | Walk forward / back, turn left / right |
| A | Attack |
| B | Jump |
| Y | Use (doors, levers, lift objects) |
| R1 (hold) | Run |
| L2 / R2 | Previous / next weapon |
| Select | Next inventory item |
| Start | Use the selected inventory item |
| X + Up / Down | Look up / down |
| X + Start | Look straight ahead |
| L1 + Left / Right | Sidestep left / right |
| Menu | Open the menu |

In the menus, use the D-pad to choose, Start to confirm and Menu to go back. Yes/no questions take Start or A
for yes, and B or Menu for no. To save or load, open Single Player in the menu; to exit, choose Quit.

The buttons are set in `Roms/PORTS/Games/Hexen2/userdir/data1/autoexec.cfg`, which is read every time the game
starts (so it also wins over changes made in the game's own controls menu). It lists the key each Miyoo button
sends; edit it to change the controls.

## Cheats

The console needs a keyboard, so the usual cheats are offered in **Options > Cheats**:

| Option | Effect |
|---|---|
| God mode | No damage |
| No clip | Fly through walls |
| No target | Monsters don't notice you |
| Health to 200 | Sets the health to 200 |
| All weapons + mana | All weapons, full mana |
| Weapons, mana, items | All weapons, full mana and items |
| 20 of each artifact | 20 of every inventory artifact |

Like the console cheats, they work only in a single player game on Hard or easier. Weapons and items arrive when
you leave the menu.

## Known issues

- Dynamic lights (the torch, fireballs, some spells) make the game redraw the lighting of the nearby walls on
  every frame: in the busiest areas, with the torch lit, the game runs at about 52-56 FPS.
- The music is quieter than the sound effects; lowering the Sound Volume in the Options menu balances them.
  "Music Type" in the Options menu must stay on CD (the default) for the music to play.
- Only the original game is supported: the Portal of Praevus mission pack and multiplayer are not.
- The game's log is in `log.txt`, in the `Hexen2` folder - please attach it to your report.

## Changelog

- **v1.0.0** - Initial release.

## Building from source

This port cross-compiles for ARMv7 hard-float with a Docker-based toolchain. Tested on Windows + WSL2 + Docker
Desktop.

### Prerequisites

- WSL2 with Ubuntu, and Docker Desktop with WSL integration enabled
- The [union-miyoomini-toolchain](https://github.com/shauninman/union-miyoomini-toolchain) container
- A Miyoo Mini buildroot sysroot providing SDL 1.2, expected at
  `/root/workspace/mini/arm-buildroot-linux-gnueabihf/sysroot` inside the container
- [steward-fu/sdl2](https://github.com/steward-fu/sdl2) at `/root/workspace/sdl2-miyoo`, for the Miyoo Mini
  audio (MI_AO) headers and libraries in `mini/inc` and `mini/lib`

### Steps

Inside the toolchain container, with this repository at `/root/workspace/uhexen2`:

```bash
cd /root/workspace/uhexen2/engine/hexen2
sh build_miyoomini.sh
```

This produces two binaries in `/root/workspace/uhexen2/build-miyoo/`: `hexen2-miyoo-debug` (with symbols, for
`addr2line` on crash reports) and `hexen2-miyoo` (stripped: the one shipped as `hexen2` in the release).
Then:

```bash
sh /root/workspace/uhexen2/miyoomini/package_miyoomini.sh v1.0.0
```

puts the release together in `build-miyoo/release/` (the `Roms` folder, ready to zip), with the shortcut, the
image, the controls (`autoexec.cfg`) and `libSDL-1.2.so.0` from the sysroot.

### What this fork changes (compared to upstream sezero/uhexen2)

- **`engine/h2shared/vid_miyoo.c`** - the Miyoo Mini video driver: the 8-bit picture turned into the panel's
  32-bit pixels (rotated 180 degrees) by a thread on the second core and shown with page flipping at vsync, the
  3D view at 640x480 combined with the 320x240 status bar and menus (`-ui320`), and timing lines in the log
  (`-perf`)
- **`engine/h2shared/snd_miyoo.c`** - sound through the Miyoo Mini audio output (MI_AO)
- **`engine/h2shared/d_edge.c`**, **`d_scan.c`**, **`d_surf.c`**, **`r_surf.c`**, **`r_edge.c`**,
  **`d_sprite.c`**, **`d_vars.c`**, **`d_iface.h`**, **`d_local.h`**, **`r_local.h`**, **`r_shared.h`** - the
  renderer on two cores: a job queue for the second core (bands of the screen, whole lit textures, the screen
  conversion in its idle time), with the first core taking queued jobs back instead of waiting; translucent water
  in a single pass; drawing loops working on local copies
- **`engine/h2shared/d_polyse.c`**, **`engine/hexen2/r_alias.c`**, **`engine/hexen2/r_main.c`** - the monsters
  and the weapon drawn by both cores (the rows of the screen shared by the time each core takes), and monsters
  fully hidden behind the world skipped (a conservative depth test)
- **`engine/h2shared/screen.c`**, **`engine/h2shared/draw.c`**, **`engine/hexen2/r_misc.c`** - the separate
  3D and 2D pictures of `-ui320`; yes/no questions answered with Start / A and B / Menu
- **`engine/h2shared/in_sdl.c`** - the Miyoo buttons: X and L1 as modifiers (X + Up / Down / Start and
  L1 + Left / Right become keys of their own) and Select apart from B
- **`engine/h2shared/snd_vorbis.c`**, **`engine/h2shared/stb_vorbis.c`** - Ogg Vorbis music with
  [stb_vorbis](https://github.com/nothings/stb) (no external library), decoded ahead by a low priority thread on
  the second core
- **`engine/hexen2/cl_parse.c`**, **`engine/hexen2/menu.c`** - the CD tracks played from
  `data1/music/trackNN.ogg`; the Cheats page in the Options menu
- **`engine/hexen2/sbar.c`** - the selected item's icon in the status bar updated when the inventory closes
- **`engine/hexen2/host.c`** - timing of the frame stages for `-perf`
- **`engine/hexen2/build_miyoomini.sh`**, **`miyoomini/`** - the build script, the OnionOS shortcut, the
  controls and the release packaging

The upstream `README.txt` is in [docs/README.uhexen2.txt](docs/README.uhexen2.txt).

## Credits

- Hammer of Thyrion (uHexen2) by O. Sezer and contributors - https://github.com/sezero/uhexen2 -
  http://uhexen2.sourceforge.net
- stb_vorbis by Sean Barrett - https://github.com/nothings/stb
- Miyoo Mini audio output (MI_AO) based on Steward Fu's SDL2 port - https://github.com/steward-fu/sdl2
- Miyoo Mini toolchain by Shaun Inman - https://github.com/shauninman/union-miyoomini-toolchain
- Original Hexen II (1997) by Raven Software, published by Activision
- Miyoo Mini Plus port by [cacuracaptors](https://github.com/cacuracaptors)

## License

Hammer of Thyrion and this port are distributed under the GNU General Public License, version 2 (see
[docs/COPYING](docs/COPYING)); stb_vorbis is public domain. Hexen II is a trademark of its respective owners; no
game data is included - you need your own copy of the game.
