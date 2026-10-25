# KisakBlack, mac-port branch

Decompiled Call of Duty: Black Ops multiplayer, being ported to native macOS on
Apple Silicon (arm64). Read HANDOFF.md for status and next steps, and
docs/64bit.md before touching anything that handles pointers.

## Build (macOS)

```sh
cmake -S . -B build_macos -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build_macos            # -> build_macos/blackops
```

The macOS build links the system zlib (twice as fast inflating the game's archives); src/zlib
is the Linux and Windows builds' (HANDOFF, Decisions session 49).

The Windows (MSVC x86) and Linux (GCC i386) builds must keep working. They can't
be built here, so check every changed file with
`python3 tools/check_file.py build_macos --i386 <files>` as well as without
`--i386`. Known artefacts of the i386 check (not code errors): phys_gjk.cpp
"expects x87 floats", cm_mesh.cpp long double, TargetConditionals.h, SDL headers,
and the gfx_gl/ files (they include the macOS GLEW header).

## Run

Game data: /Users/pstn/Documents/Games/codbo. From that directory:

```sh
/Users/pstn/Documents/kisakblack-mac/build_macos/blackops +set dedicated 1 +set developer 1
```

The game folder (base path `fs_b`, zones, and everything the game writes: config,
stats, logs, screenshots) is the executable's directory when `main/` and `zone/`
are next to it, else the current directory; `+set fs_basepath` does nothing.
`tools/make_portable.sh <game dir>` copies the build and its Homebrew libraries
(into `<game dir>/lib`) into a game folder, which then runs from anywhere and can
be moved; rerun it after rebuilding. The user's game folder has such a copy
(`codbo/blackops`): don't run that one in tests (it writes the real profile).

macOS has no `timeout`; use `perl -e 'alarm shift; exec @ARGV' 60 <cmd>`, or
`perl tools/run_sampled.pl <seconds> <log> <cmd...>` to run, capture every
thread's stack (macOS `sample`) into `<log>.sample`, and kill it. The crash
handler prints the signal, its fault address and the interrupted pc, lr, fp and sp (pc and lr
symbolized), an `atos -o <exe> -l <load address> <pc> <lr>` line to run for files and lines,
then the backtrace, which on arm64 starts at the link register, not the pc: a crash inside a
frameless leaf is only in the pc line. Other printed addresses go to that atos command in
place of pc and lr. `lldb --batch -o run -k "bt" -k "frame variable" -- <cmd>`
works; if a launch under lldb hangs in dyld `open`, kill leftover blackops
processes first. lldb turns ASLR off (image at 0x100000000), which hides bugs
that depend on the image address: reproduce outside it too.

A map with bots: add `+set developer_script 1 +set scr_testclients 4 +map mp_nuked`
(the bots come from the `/# #/` blocks of maps/mp/gametypes/_dev.gsc). Drop
`+set dedicated 1` for the client (a listen server; the player may stay on the team
menu, there is no input). `KB_ASSERT_BT=1` prints a backtrace the first time each
assert fires (`iassert` compiles to nothing in RelWithDebInfo; only the decompiled explicit
`Assert_MyHandler`/`_tlAssert` checks run). `KB_PHYS_STRESS=2500` grows every ragdoll's and
vehicle's physics query to cover the map and wakes every dynent it reaches, so all the physics
threads build the same environment objects at once (HANDOFF, Decisions session 21): a race there
crashed the old code within a minute on mp_nuked with bots. Use it on mp_nuked with 7 bots: on the
16-bot free-for-all it uses up the physics memory within ~30 s and crashes (HANDOFF, Open items, session 43).

The game folder's `Black Ops Launcher.app` (tools/launcher, built by
make_portable.sh or `tools/launcher/build.sh <game dir> [icon picture]`; its icon is made by
tools/launcher/make_icon.swift from `build_macos/launcher_icon.png`, game art kept out of git; the app keeps
the picture in Contents/Resources and a rebuild without one reuses it: HANDOFF, Decisions session 32; the map is chosen from a
mosaic of the game's map-menu pictures, which build.sh copies from the game folder's main/*.iwd into Contents/Resources/maps,
game art too, reused the same way: HANDOFF, Decisions session 49) starts matches with the
PC managed bots instead: `+set sv_botsAllowMovement 1 +set sv_botsPressAttackBtn 1
+set sv_botsPressMeleeBtn 1 +set scr_bots_managed_spawn 1 +set
scr_bots_managed_allies 3 +set scr_bots_managed_axis 4 +set scr_bot_difficulty
normal` (easy/normal/hard/fu; the bots arrive ~10 s in), `+set kb_damagePercent <n>` ("Gun damage"), `+set
kb_infiniteClip 0/1` ("Never reload") and `+set kb_logFiles 0/1` ("Save logs", off by default: off, the game's output
goes nowhere and it writes no main/console_mp.log or main/games_mp.log; on, the output goes to `blackops.log` next to the
app; kb_logFiles is 1 in a game started by hand), all at every start. `KB_LAUNCHER_TEST=<dir>`
makes the app save its window as `<dir>/launcher.png` and press Play itself (with `KB_LAUNCHER_TEST_PICK=<x>,<y>`
it first opens the map mosaic, saves it as `<dir>/maps.png` and clicks it at that pixel of maps.png); run it
in a scratch copy of the game folder (`main/` a real folder of links to the archives, as the game writes logs and
screenshots into `<folder>/main`; links to `zone`, `lib` and a copy of
`blackops`), as the app starts the `blackops` next to it with that folder's profile.

A client and a dedicated server as separate processes (UDP over loopback): start
the server with `+set net_port 28960` (plus the bots and `+map` above), then, once
its map is up (~20 s), the client with `+set net_port 28961 +connect 127.0.0.1:28960`.
Without Steam any client is accepted (stubs_online.cpp). The engine keeps at most
32 `+` commands from the command line and silently drops the rest (`com_consoleLines`). Two processes need
`perl -e 'alarm ...'` each, or kill them yourself; leftover servers keep the port.

Offline stats (rank, unlocks, custom classes) live in `<home>/players/mpstats.dat`;
the home path dvar is `fs_h` (defaults to the base path, i.e. the user's real
profile). Test runs that touch stats should pass `+set fs_h <scratch dir>`. That
only moves writes: the config read at startup is always the game folder's `players/config_mp.cfg`
(the user's: fullscreen 1920x1080), even when the profile holds one (only the base path's `players/` is
searched; session 35), so test clients pass `+set r_fullscreen 0 +set r_mode 1024x768`, and a test's
other settings go on the command line or into a scratch game folder's `players/config_mp.cfg`. A fresh
profile: `+set fs_b <dir>` where `<dir>/main` links to the game's `main`.
Everything is unlocked and owned, rank 50/prestige 15, by default (HANDOFF,
session 8); to test the progression underneath, pass `+set allItemsUnlocked 0
+set allItemsPurchased 0 +set allEmblemsUnlocked 0 +set allEmblemsPurchased 0`.
Item indices are the rows of mp/statsTable.csv (e.g. 43 L96A1, 150/151
Lightweight/Pro, 209 Attack Dogs); `statReadDDL cacLoadouts customclass1 primary`
prints a loadout slot.

The user's gameplay changes are archived dvars that default to 1 (HANDOFF, Decisions
sessions 16-18, 20, 23, 26, 27, 28, 29, 31, 32, 34, 40 and 46): `kb_godMode` (human players can't die: hits land as in normal play, kick, flinch, sounds, but health stops at 1), `kb_noGrenades`,
`kb_noLaunchers` (a class's RPG, M72 LAW or Strela-3 comes as an AK-47), `kb_noKillstreaks`, `kb_noLastStand`,
`kb_noDualWield` (a dual-wield pick comes as the single gun, for bots too; Create a Class offers none),
`kb_dlcWeapons` (the 16 WWII guns of zone/Common/kb_dlc_weapons.ff in Create a Class and for the bots;
read at startup: a change takes effect at the next start),
`kb_noDeathCam` (respawn 0.1 s after dying), `kb_noMusic`, `kb_noAnnouncer`,
`kb_infiniteReserve`, `kb_infiniteClip` (the human players' magazines never empty: no reloads; the launcher's "Never reload"),
`kb_staticCrosshair` (the user's Counter-Strike crosshair, CS:GO's classic
static one, in screen pixels; its look is `kb_crosshairSize`/`Gap`/`Thickness`/`Color`/`UseAlpha`/
`Dot`/`Outline`/`OutlineThickness`, CS's `cl_crosshair*`; the human players' shots land inside its
gap, 0.4 degrees), `kb_noVersion` (no version text at the bottom right), `kb_noAds`,
`kb_sprintFire`, `kb_aaLean` (Medal of Honor: Allied
Assault's lean from OpenMoHAA: 40 degrees, the view rolls, full speed and sprint while leaning;
`ps->leanf` is the angle / 80), `kb_noProne` (nobody goes prone or dives to prone; the prone keys
crouch, `toggleprone` toggles crouch), `kb_noHitMarker` (no hit marker around the crosshair; the hit
sound stays), `kb_cacButton` (the main menu's THEATER button is CREATE A CLASS; read at startup), `kb_cleanHud` (only the
crosshair during play: no minimap, score, ammo, kill feed, popups, names or hints; the FPS counter, menus,
scoreboard and round outcome stay), `kb_noCountdown` (no "Match begins in" countdown at a match's or round's start:
players move as soon as they spawn), `kb_allClassGuns` (human players also carry the primaries of custom classes 1-5 on
top of the chosen class, whose pistol is the only one, up to the 15 held weapons; not in gun game and the other wager modes), `kb_mohaaDamage`
(rifles, SMGs, machine guns and pistols do the closest MoHAA gun's damage at any distance, SMGs 25, assault rifles
27, ...; headshots 4x, body, arms and legs 1x; bots' guns too; shotguns keep theirs), `kb_gunPlacement` (the
first-person WWII Thompson is held closer, where the MP40 is, a little lower; a table in cg_weapons.cpp),
`kb_wwiiGunMovement` (the first-person WWII guns move like Black Ops' while walking: no drop, a 2 degree tilt),
`kb_sniperScope` (with kb_noAds, the scoped sniper rifles L96A1, WA2000, Dragunov, PSG1 and Scoped Kar98k still scope in;
a human player's scope as MoHAA's: in and out in 0.05 s, no drift or walking sway, full walking speed scoped),
`kb_noRoundEndWait` (no outcome screen delay between rounds or after a match and no 30 s scoreboard: the next round
or map at once), `kb_damagePercent` (an int 1-100, default 100: lowers every gun's damage but the sniper rifles', everyone's, in
percent; 100 or more changes nothing; the launcher's "Gun damage" passes it), `kb_blockGuns` (nobody gets the FN FAL, Galil,
M14, Olympia, HS-10, HK21, RPK, M60, Stoner63, Dragunov, WA2000, PSG1 or Ballistic Knife, any variant: a class's comes as an
AK-47, a sniper rifle as the L96A1 (the default Sniper class's Dragunov), the knife as nothing; bots pick others and, with kb_noLaunchers and kb_noGrenades, always a pistol second; the WWII guns stay; Create a Class lists none of them, nor
launchers, the crossbow or the lethal/tactical/equipment rows), `kb_noMelee` (the melee button does nothing, bots don't
melee), `kb_instantClass` (a class a human player picks while alive comes at once, where they stand, as in the grace
period: a script patch of menuClass), `kb_consoleCommands` (the console runs a line without the leading / as a command, quit and
kill too; in a match the original says it in chat, at the menu it says quit and kill). With kb_cleanHud the world markers (the teammates' death skull, objective markers) are hidden too. Tests
where the player should die pass
`+set kb_godMode 0`, tests of ADS `+set kb_noAds 0`, of explosives `+set kb_noGrenades 0` (and
`kb_noLaunchers`, `kb_noKillstreaks`), of prone `+set kb_noProne 0`, tests that read the HUD (popups, kill feed, hints) in screenshots `+set kb_cleanHud 0`, of Black Ops' own
damage `+set kb_mohaaDamage 0`, of the blocked guns or melee `+set kb_blockGuns 0` / `+set kb_noMelee 0`, of the round/match outcome screens and scoreboard `+set kb_noRoundEndWait 0`; a scratch profile keeps whatever a run set
(they are written to its config_mp.cfg). `+leanleft`/`+leanright` lean in `KB_CMDS`; `viewpos` prints the
eye position (60 above the feet standing, 40 crouched, 11 prone). Hits on demand: with `+set
developer_script 1 +set scr_testclients 4 +set sv_botsAllowMovement 0`, `set scr_playerwarp enemies_host`
moves the enemy bots to the player; `setviewpos` back ~130 units facing them and fire.

Playing without a keyboard: `KB_CMDS` runs console commands at times (seconds) after
the client becomes active in a map (again after each map change), `|`-separated;
entries after `loop@<start>/<period>` repeat. It (and KB_MENU_CMDS below) keeps 64 entries and 4 KB, dropping the rest silently: a
longer sequence steps through dvars from a cfg, `set g1 "give thompson_mp;set wn vstr g2"`, ..., with `vstr wn` in
the loop (session 36). `+set scr_tdm_timelimit 1` ends a
match after a minute, to test the end of a match and map rotation. This joins, picks a class and fights:

```sh
KB_CMDS='8:openscriptmenu team_marinesopfor autoassign|11:openscriptmenu changeclass assault_mp,0|loop@14/10|0:+forward|1:+attack|2.5:-attack|3:+right|3.6:-right|4:+gostand|4.2:-gostand|5:+usereload|5.2:-usereload|6:weapnext|7:+frag|7.3:-frag|9.5:-forward'
```

`changeclass custom1,0` (to `custom5`, then `prestige1`-`prestige5`) picks a
custom class; the default classes are `assault_mp`, `smg_mp`, `cqb_mp` (shotgun), `lmg_mp`,
`sniper_mp`. Weapons: `take all|give rpg_mp|weapnext`. On mp_nuked, `setviewpos -100 360 200 90`
drops the player into the street with ~700 units of clear run north (sprint tests). `KB_MENU_CMDS` takes the same list but times it from startup and runs it once,
in a map or not: the main menu (`12:openmenu cac_main|30:quit`). Clicking through menus from either list:
`kb_mouse <x> <y>` puts the menu cursor at that back-buffer pixel (no arguments: the mouse again) and
`kb_key <key>` presses and releases a key by bind name (`MOUSE1` clicks there, `ESCAPE`, `ENTER`); read
positions off a `KB_SCREENSHOT` frame. In a 1024x768 window the main menu's CREATE A CLASS is at 170 131,
then Custom 1 160 190, Primary 154 191 (twice: the gun screen, then its groups), the SMG group 190 223
(HANDOFF, Decisions session 28); frames lag the command clock by ~2 s at `com_maxfps 60`.

With vsync on (the default), a GL client started while the display sleeps hangs in
its first buffer swap (SDL3 waits for a display refresh that never comes): wake
the display first (`caffeinate -u -t 2`) or pass `+set r_vsync 0`. Metal's Present
waits for a drawable there instead (untested with the display asleep).

Seeing the client: `screencapture` lacks Screen Recording permission and the
console is unreachable, so `KB_SCREENSHOT=<dir> KB_SCREENSHOT_EVERY=<n>` writes the
back buffer as `<dir>/present_N.tga` every n presents (with draw counters on stderr);
the console command `kb_shots <count> [<every>]` (from KB_CMDS) takes the next <count> every
<every> presents from then on, to film one moment (a kill at 30 per second: `3.3:kb_shots 90 2`;
`KB_SCREENSHOT_EVERY=0` takes no periodic ones). On the GL layer (`KB_RENDERER=gl`) only, `KB_SCREENSHOT_WINDOW=1` adds `window_N.tga`,
what the window shows after scaling, and `KB_TRACEFRAME=n1,n2` logs those frames'
render-target/viewport/clear/blit/draw calls and, with KB_SCREENSHOT, dumps the back
buffer at the first resolves.

Hearing the client: OpenAL Soft's wave writer records the game's final mix in
place of the device, in real time: `ALSOFT_CONF=<file>` holding `[general]`
`drivers=wave` and `[wave]` `file=<out.wav>`. `tools/wavstat.py <out.wav> [s]`
prints its peak/RMS per s-second window (no sound: -120 dBFS). `KB_SND_STATS=<s>`
prints the voices (new ones by codec), buffers, output peak and mixing time every s
seconds; `KB_SND_QUEUE=1` forces the mixer-thread output that OpenAL Soft < 1.22
gets. xWMA sounds (weapons, footsteps, impacts, UI navigation) go through
src/audio_openal/wma (FFmpeg's WMA decoder, ported). `set snd_solo_alias_substring <s>`
(a cheat dvar, on in map games) mutes every alias whose name lacks `<s>`, so a capture
holds one group (`wpn_`, `fly_step`, `prj_`, `vox_`...). The sound console channel is
hidden: `+con_showchannel sound`.

Measuring the client: `tools/bench_views.py` runs it uncapped at fixed views
(`tools/views.txt`: mp_nuked's benchmark views a-f and sky, an `overview` on every map;
`setviewpos` with `god` and `noclip`, scratch profile) and prints fps, frame and swap
time and draws per view. `--bots` adds the launcher's 7 managed bots, `--map all` runs
every map, `--shots DIR` saves a screenshot per view, `--sample S` / `--gpu-trace S`
record a CPU sample / Metal System Trace at each view (sampling lowers that view's fps).
`tools/cmp_screens.py A B` compares two screenshot sets (two runs of one build differ in
0.05-5% of pixels by map; its header lists them), `tools/sample_report.py` summarizes `sample` files (threads,
self, libs, incl, callers, tree; `render`/`main` select those threads) and
`tools/gpu_trace.py report T --fps F` gives the GPU's busy share and command buffers
per frame. `KB_FPS=<s>` alone prints fps, draws, blits and buffer uploads per frame.
bench_views.py times its commands by the game's clock (`KB_CMDS_GAMETIME=1`), but the
match clock, script movers and `autoassign`'s team still vary between runs: compare a
candidate with the closer of two reference runs.

Writing zones (HANDOFF, Decisions sessions 25-27): a dedicated server's `+writezone <out.ff>
<zone>[,<zone>,<zone>] [all] weapon <name> [<type> <name> ...]` loads the source zones without
linking their assets and writes the named assets with everything they reach (stubs for what
multiplayer has, a cut sound bank, localized names); write to a scratch dir. `weapon <old>=<new>[:<ref>]`
renames (the statsTable reference, parentWeaponName, defaults to `<new>` without `_mp`; `:` alone
leaves none); `damage <weapon>=<mp weapon>` copies a multiplayer gun's damage, `rows`/`addrows
<table>=<file>` put rows into a copy of a string table (into its empty rows by index / at the end),
`strings <file>` adds localized strings (`NAME text` lines), `alias <name>` keeps a sound alias no asset names (with its
secondaries; a merge's bank is cut again from what the zone names, so each merge passes it on); attachments (HANDOFF,
Decisions session 38): `part <model>=<donor model>:<bone>:<x>,<y>,<z>` copies a model's attachment part (its bone and
surfaces; the donor from the source zones or multiplayer) into a model, moved by the offset, `variant
<new>=<base>:<mp variant>:<display name>` makes an attachment variant of a gun as the multiplayer variant differs from
its base (flags, sounds, flash), `hide <weapon>=<tag>[,<tag>]` sets the parts a weapon hides (after the variants), `sound
<weapon>=<alias>:<new alias>` swaps a weapon's sound. Source zones load by name from `zone/`
of the install path, so a written zone is a source only from a scratch game folder; tool runs pass
`+set kb_dlcWeapons 0` (else the DLC zone loads with the common zones and counts as multiplayer's).
`listzone <zone>[,<zone>] [<type> [<name>]]` lists a zone's assets (weapons with the names they hold,
their damage, hip spread and view kick, localized strings with their text, `menu <name>` a menu's items with their rect,
visibility and text expressions and the scripts with their conditions, in postfix order), `sound` every alias with
its group, category (sfx/music/voice/ui) and file (loaded in the zone or streamed from main/*.iwd; `- sound` multiplayer's),
`xmodel <name>` a model's bones and surfaces (vertex boxes), `listzone - weapon [prefix]` multiplayer's weapons (their
hidden parts, flags, flash and models), `- xmodel <name>` its models, `listzone - localize [prefix]` its strings, `listzone -
rawfile <script>` a loaded script, `listzone - stringtable mp/statsTable.csv` a table. Commands too long
for the command line (1 KB) go in a cfg under `<fs_h>/main` (`+exec`).
`tools/make_dlc_weapons.sh <game dir> <out.ff>` writes the DLC weapons' zone, kb_dlc_weapons.ff (16
WWII guns with multiplayer damage and their rows of mp/statsTable.csv, mp/attributesTable.csv and
mp/weaponOptions.csv from tools/dlc_weapons_*, the Thompson's suppressor (`thompson_silencer_mp`: the MP40's World at War
silencer on both Thompson models, the Black Ops-era suppressed Thompson shots), and 7 suppressed shot sounds; ~10 s, in
build_macos/dlc_weapons) from the World at War zombie maps; `--sources <game dir>` checks a folder has them. The user's
game folder has no zombie zones since session 36 (HANDOFF, Decisions session 36): they come from the backup folder
/Users/pstn/Documents/Games/codbo_wwii_guns_backup (README.txt there; read only) with `--extra-sources <dir>`, which
make_portable.sh passes (`--dlc-sources <dir>`, by default the `codbo_wwii_guns_backup` next to the game folder;
without one it keeps the installed zone; HANDOFF, Decisions session 38). The game loads it at startup after common_mp; to try a
new one, link it as `zone/Common/kb_dlc_weapons.ff` of a scratch game folder (links to `main`,
`zone/English` and every `zone/Common` file). Create a Class from KB_MENU_CMDS: HANDOFF, Decisions
session 27 (the console sequences). A zone with weapons outside the weapon table (`loadzone`, then
`give`) allows one such weapon per map load (a second: "Weapon index mismatch"). `src/database/db_save.cpp`
is generated: rerun `tools/gen_db_save.py` after changing db_load.cpp. `tools/ff_info.py <zone.ff>`
prints a zone's header. `tools/strip_voices.py <game dir> <out dir>` rebuilds main/*.iwd without the voices (silent
same-format placeholders for the ones multiplayer names; the user's game folder has them since session 33: HANDOFF,
Decisions session 33). `tools/strip_unused.py <game dir> <out dir> [--dry-run]` rebuilds main/*.iwd without the
images and sounds no zone, executable or archive text names (the list with every file's reason in
`<out dir>/strip_unused.txt`; `<game dir> - --check` unpacks every archive file as the engine reads it; HANDOFF,
Decisions session 35). `tools/strip_dupes.py <game dir> <out dir> [--dry-run]` rebuilds them without the second copies
of images and sounds stored in more than one archive (only copies byte-identical to the one the game opens, which
stays: every `iw_` archive is searched before any `localized_` one, the highest-numbered first; the list in
`<out dir>/strip_dupes.txt`; HANDOFF, Decisions session 37). Moving the game folder's files to the Trash: `/usr/bin/trash` refuses most of them (the folder is
in iCloud Drive); Finder works: `osascript -e 'tell application "Finder" to delete (POSIX file "<path>" as alias)'`. `tools/iwi_to_png.py <game dir> <out dir> <image>...` converts the game's images
(`images/<name>.iwi` in main/*.iwd: menu art, emblems, rank and prestige icons) to PNGs.

Metal: the macOS client renders through src/gfx_mtl (a D3D9 device on Metal);
`KB_RENDERER=gl` picks the GL layer instead, the macOS fallback and Linux's renderer
(HANDOFF, "Metal backend").
Metal draws what D3D9 specifies where the GL layer doesn't (mipmaps, shadow-map compares,
separate alpha blending, particle clouds); `KB_MTL_GLCOMPAT=1` draws like the GL layer,
to compare screenshots with it. Anything the backend doesn't implement prints `[mtl] BUG`
once (`KB_MTL_STRICT=1` aborts there): grep logs for it. Develop with `MTL_DEBUG_LAYER=1
MTL_SHADER_VALIDATION=1`. Metal's `KB_FPS` line adds frames presented and shown per
second (shown: what the display reported; 0 while the screen is locked, when nothing is
composited) and the pipelines built at first draw with their time (stalls), split into
waiting for shader batches still compiling, function lookups and the longest build. The
system's shader cache keeps about one map's batches; `KB_MTL_BATCH=<n>` (shaders per
batch, default 64; a size not used before) gives a cold one to measure with.
`KB_MTL_SHADERLOG=<file>` logs each shader's creation and batch, batch submits and
completions, a draw's waits, first uses and frames over 15 ms with the pipelines built in them
and their ms (timestamps);
`KB_MTL_COMPILES=<n>` sets the batches Metal compiles at once (default cores - 2). The
pipeline cache (HANDOFF, Decisions sessions 20 and 49): `players/metal_pipelines/<map>.bin` under fs_h,
the pipelines each map drew the last time, written at the map's end only when something in it compiled;
menus.bin opens at startup, a map's file as its load starts (Com_LoadLevelFastFiles), behind the loading
screen (opening all of them at startup took the user 29 s); a scratch profile starts
without it, `bench_views.py --home <dir>` keeps one between runs; off under
`MTL_SHADER_VALIDATION` (Metal's GPU validation crashes reading binary archives).
`KB_DUMP_MSL=<dir>` writes each translated shader. The shader library (HANDOFF, Decisions
session 20): `<game>/kb_shaders.metallib`, made by `tools/make_shaderlib.sh <game dir>` (run by
make_portable.sh; it collects the shaders once into `<game>/kb_shaders/bytecode`, ~10 minutes
in a window, and needs Xcode's Metal Toolchain to compile); shaders are named `kb_<hash of their
MSL>`, and what isn't in the library compiles at run time with a log line. On the GL
backend (`KB_RENDERER=gl`), `KB_SPEC=<file>` (`%p` for the pid) records every D3D9 method, state, format,
lock flag and shader pairing the game uses (the list Metal implements), and
`KB_DUMP_SHADERS=<dir>` writes each shader's bytecode; `tools/shader_test.sh <dump> <out>`
translates them all to GLSL and MSL and compiles the MSL.

AddressSanitizer: configure `build_asan` with `-DCMAKE_CXX_FLAGS="-fsanitize=address
-fno-omit-frame-pointer"` (same for C, Objective-C++ and the linker). ASan's dlopen interceptor
makes sdl2-compat look for libSDL3 next to the ASan runtime, so copy
`libclang_rt.asan_osx_dynamic.dylib` into `build_asan/asanrt/`, symlink
`/opt/homebrew/lib/libSDL3.dylib` there, and run with
`DYLD_LIBRARY_PATH=build_asan/asanrt`. SIP strips `DYLD_*` when it runs
/usr/bin/perl or /bin/bash, so don't launch through those wrappers (Homebrew's
`/opt/homebrew/bin/python3` keeps them: `subprocess` with a timeout works).
`ASAN_OPTIONS=detect_stack_use_after_return=1` gives every frame its own redzones and finds
reads and writes past stack objects that the normal layout hides (two at map load in session 21).
ASan's slowdown hides races between threads: try those on the release build.
perl's `exec @ARGV` with a single argument splits it at spaces (`Black Ops
Launcher.app`): use `exec { $ARGV[0] } @ARGV`. The launcher writes its window frame
into its defaults domain whenever it runs; test it from a copy with another
`CFBundleIdentifier` (PlistBuddy, then `codesign --force -s -`).

The agent shell's `grep` is a function that skips Latin-1 files (19 sources,
g_main_mp.cpp among them); search with `command grep -a`.

## Conventions

- Match the decompiled style around the edit; no reformatting.
- Pointer <-> 32-bit int: `Ptr32_Encode` / `Ptr32_Decode`. Never `.v`/`.get()`
  on a Ptr32 (it is `T*` on 32-bit builds): use `Ptr32_Raw`/`Ptr32_SetRaw` or a cast.
- Asset and networked structs keep the i386 layout: `tools/layout_diff.py`.
- Scalar locals IDA split out of one array (`Vec3Normalize(&v10)` with v10-v12 floats,
  `char s;` used as a buffer): `tools/audit_splitlocals.py` lists them; keep it at 0 open
  sites (rebuild the array from IDA's stack offsets, or add a reviewed entry).
- Commit messages end with the Co-Authored-By line the session provides.
