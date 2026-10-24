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
example `kb_godMode 0`. The settings are saved in your config, except `kb_logFiles`.

**Dying and damage**

| Setting | Default | What it does |
|---|---|---|
| `kb_godMode` | 1 | You can't die: hits land normally, health stops at 1 (bots take normal damage) |
| `kb_noLastStand` | 1 | Nobody goes into last stand (Second Chance) |
| `kb_mohaaDamage` | 1 | Guns do Medal of Honor: Allied Assault damage at any distance: headshots 4x, the rest 1x |
| `kb_damagePercent` | 100 | Lowers every gun's damage except sniper rifles', in percent (1-100); the launcher's "Gun damage" |
| `kb_noMelee` | 1 | Nobody can knife; bots don't try |

**Weapons and classes**

| Setting | Default | What it does |
|---|---|---|
| `kb_noGrenades` | 1 | No grenades, equipment, grenade launchers or explosive crossbow, for anyone |
| `kb_noLaunchers` | 1 | No rocket launchers: RPG, M72 LAW or Strela-3 become an AK-47 |
| `kb_noKillstreaks` | 1 | Nobody earns killstreaks |
| `kb_noDualWield` | 1 | No dual wield: you get the single gun |
| `kb_blockGuns` | 1 | Blocks the FN FAL, Galil, M14, Olympia, HS-10, the light machine guns, the sniper rifles except the L96A1, and the Ballistic Knife; a class's blocked gun comes as an AK-47, a blocked sniper rifle as the L96A1 |
| `kb_allClassGuns` | 1 | You also carry the main guns of custom classes 1-5 |
| `kb_instantClass` | 1 | A class you pick while alive comes at once, where you stand, not at the next spawn |
| `kb_dlcWeapons` | 1 | The WWII guns from World at War in Create a Class and for bots (takes effect at the next start) |
| `kb_infiniteReserve` | 1 | Reloading never uses up your spare ammo |
| `kb_infiniteClip` | 1 | Your magazine never empties, so you never reload; the launcher's "Never reload" |

**Aiming and moving**

| Setting | Default | What it does |
|---|---|---|
| `kb_noAds` | 1 | No aiming down the sights; everything fires from the hip |
| `kb_sniperScope` | 1 | Sniper rifles still scope in, Medal of Honor style (instant, no sway) |
| `kb_staticCrosshair` | 1 | A small fixed Counter-Strike crosshair; your bullets land inside it |
| `kb_sprintFire` | 1 | Fire while sprinting without stopping the sprint |
| `kb_aaLean` | 1 | Medal of Honor lean: 40 degrees, the view tilts, full speed |
| `kb_noProne` | 1 | Nobody goes prone; the prone key crouches instead |
| `kb_wwiiGunMovement` | 1 | WWII guns move like Black Ops guns while walking |
| `kb_gunPlacement` | 1 | The WWII Thompson is held where the MP40 is |

**Crosshair look** (Counter-Strike's own crosshair settings)

| Setting | Default | What it does |
|---|---|---|
| `kb_crosshairSize` | 2.5 | Line length |
| `kb_crosshairGap` | -1 | Gap in the middle |
| `kb_crosshairThickness` | 1 | Line thickness |
| `kb_crosshairColor` | 1 1 1 1 | Colour and transparency (white) |
| `kb_crosshairUseAlpha` | 0 | Use the colour's transparency |
| `kb_crosshairDot` | 0 | A dot in the centre |
| `kb_crosshairOutline` | 0 | A black outline |
| `kb_crosshairOutlineThickness` | 1 | Outline thickness |

**Screen**

| Setting | Default | What it does |
|---|---|---|
| `kb_cleanHud` | 1 | Only the crosshair during play: no minimap, score, ammo, kill feed, names or markers |
| `kb_noHitMarker` | 1 | No hit marker when you hit someone (the hit sound stays) |
| `kb_noDeathCam` | 1 | No killcam or death view: you respawn right away |
| `kb_noVersion` | 1 | No version text at the bottom right |

**Match flow**

| Setting | Default | What it does |
|---|---|---|
| `kb_noCountdown` | 1 | No "match begins in" countdown |
| `kb_noRoundEndWait` | 1 | No waiting or scoreboard between rounds and maps |

**Sound, menus and logs**

| Setting | Default | What it does |
|---|---|---|
| `kb_noMusic` | 1 | No music in menus or matches |
| `kb_noAnnouncer` | 1 | No announcer voice (battle chatter stays) |
| `kb_cacButton` | 1 | The main menu's THEATER button opens CREATE A CLASS (takes effect at the next start) |
| `kb_logFiles` | 1 | Write the game's log files; the launcher's "Save logs" (off there unless you tick it) |
| `kb_consoleCommands` | 1 | The console runs what you type without the leading `/` (in a match the original says it in chat) |

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
