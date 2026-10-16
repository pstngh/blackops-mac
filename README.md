# Black Ops for Mac

Call of Duty: Black Ops multiplayer, running natively on Apple Silicon Macs: no Rosetta, no Wine.

This is a port of [KisakBlack](https://github.com/SwagSoftware/KisakBlack), the open-source rebuild of the game's
multiplayer executable. You need your own copy of the game (the Steam version) for its data.

## What works

- Matches against bots on every multiplayer map, started from a simple launcher app
- Dedicated servers, and joining a server over the network
- Graphics through Metal, sound, mouse and keyboard

## Build

Install Xcode, then:

```sh
brew install cmake ninja pkgconf sdl2-compat glew openal-soft speex libvpx sse2neon
cmake -S . -B build_macos -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build_macos
```

## Install

Point this at your game folder (the one with `main/` and `zone/` in it):

```sh
tools/make_portable.sh "/path/to/your/game folder"
```

It copies the game and the libraries it needs into that folder and adds `Black Ops Launcher.app`. Run it again after
every build.

## Play

Open `Black Ops Launcher.app` in the game folder, pick a map, a mode and the bots, and press Play.

## Gameplay changes

This version is set up for playing alone against bots, and it changes the game by default. For example: you can't die,
there are no grenades, launchers, killstreaks or knifing, no aiming down the sights (sniper rifles still scope in), a
fixed Counter-Strike-style crosshair, Medal of Honor-style damage and leaning, a clean HUD, and no death cam.

Each change is a setting whose name starts with `kb_`. Set it to 0 in the game's console to get the original back, for
example `kb_godMode 0`. They are all listed in [CLAUDE.md](CLAUDE.md).

## Other platforms

The code still has KisakBlack's Windows and Linux (32-bit) builds; see [KisakBlack](https://github.com/SwagSoftware/KisakBlack)
for how to build them. They are not tested here.

## More

- [HANDOFF.md](HANDOFF.md): the port's status and how each part was done
- [docs/64bit.md](docs/64bit.md): how the 32-bit game was made to run as 64-bit

This is a game from 2010 with known exploits. Be careful playing online.

## Credits

- The original Black Ops developers
- [KisakBlack](https://github.com/SwagSoftware/KisakBlack) and its [development blog](https://lwss.github.io/Kisak-Black/),
  which this is built on, and the projects it credits: [KisakCOD](https://github.com/SwagSoftware/KisakCOD),
  [jk3src](https://github.com/PJayB/jk3src), [CoD2rev_Server](https://github.com/voron00/CoD2rev_Server),
  [BO3Enhanced](https://github.com/shiversoftdev/BO3Enhanced) and [RAD Game Tools](https://www.radgametools.com/) (Bink)
- [OpenMoHAA](https://github.com/openmoh/openmohaa) for the lean, and FFmpeg for the WMA sound decoder
