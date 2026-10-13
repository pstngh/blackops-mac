# HANDOFF — native macOS (Apple Silicon) port

Branch `mac-port` of https://github.com/pstngh/kisakblack-mac. Local checkout:
`/Users/pstn/Documents/kisakblack-mac`. Game data: `/Users/pstn/Documents/Games/codbo`
(Steam Black Ops: `main/*.iwd`, `zone/Common`, `zone/English`). Build, run and
debugging commands are in CLAUDE.md; the 64-bit design and rules in docs/64bit.md.

## Goal

A native arm64 macOS build of the multiplayer executable: no Rosetta, no Wine.
Apple Silicon has no 32-bit mode, so this is also the engine's first 64-bit port.

## Status (2026-10-12, session 32)

- **Builds and links**: `build_macos/blackops`, Mach-O arm64, and the ASan build
  `build_asan`. Every changed file passes the i386 syntax check (gfx_gl/ and
  platform/sdl/ files: only the known GLEW/SDL header artefacts).
- **Dedicated server** with 4 bots: deaths, killstreaks, ragdolls, no asserts; also
  clean under ASan with heap redzones (120 s).
- **Client**: main menu, team menu over the blurred world, local matches that render
  on every MP map; on Metal (the default since session 15) mp_nuked's fixed views run
  490-800 fps uncapped on an M4 at 1280x720, 390-700 with 7 bots (GL: 116-304;
  Performance and Metal backend below). Keyboard, mouse and console work.
  `KB_CMDS` (CLAUDE.md) plays without a keyboard: join, class, move, fire, grenade,
  reload, weapon switch, death and respawn. A soak of all 14 MP maps x 180 s with
  the scripted player ran without a crash; 1-minute matches end (scoreboard) and
  rotate maps; the ASan client ran 8 match ends/rotations with no report.
- **Networking** (session 6): UDP on Linux/macOS (`src/platform/linux/sys_net.cpp`).
  A client process connects to a separate dedicated server (CLAUDE.md has the
  commands), plays, follows match ends and map rotations (3 maps), and
  `disconnect` drops it on the server at once; a second process also joins a listen
  server. Two 64-bit processes read each other's packets, so the Huffman fix holds.
  ASan client vs. server and ASan server vs. client: 3 maps each, no report. A
  21-minute soak (ASan client, scripted player, dedicated server rotating dm, dom,
  koth, ctf, sd, dem, sab, hq, tdm with 1-minute limits) played 12 maps with no
  report after the two fixes below.
- **Offline progression** (session 7): the local player signs in at startup and
  their stats live in `players/mpstats.dat` (under `fs_h`, the home path dvar;
  `.bak` is the previous save). A `map`/`devmap` listen server plays as a ranked
  online game for the host: XP, rank-ups, CP, unlocks by rank, purchases,
  custom classes (spawned with an equipped bought weapon), prestige, and they
  persist across restarts, map rotations, `quit` and hard kills (since the last
  save). Tested with `KB_CMDS` (`set scr_givexp N`, `purchaseItem <idx>`,
  `equipClassItem customclass1 <idx>`, `uploadstats`, `prestigerequest`,
  `getxp` prints NORMAL/global XP, rank, CP) and the ASan client (3 maps).
  Screens checked: in-game "choose class" lists Custom 1-5 with "new" badges.
  Not exercised: the main-menu Create-a-Class/barracks/AAR menus by hand.
- **Everything maxed out** (session 8, the default): every weapon, attachment,
  perk and pro perk, camo/reticle/lens/tag, killstreak, emblem part, clan tag
  feature, face paint and feature (Custom 6-10) is unlocked and owned, and the
  player is rank 50, prestige 15. The stats file only matters for what the player
  sets up. `sv_cheats` stays on for `map` games. Tested on a listen server
  (mp_nuked, 4 bots, scratch `fs_h`): `getxp` shows rank 50/prestige 15 on a
  fresh and on an old profile; `equipClassItem` of L96A1 (43), G11 (32, a
  "classified" one), Tomahawk (65), Warlord (168), Ninja (172), Hacker Pro (177) and
  `statWriteDDL cacLoadouts killstreak3 209` (Attack Dogs) take, `purchaseItem`
  answers "already purchased", the player spawns with the L96A1 or G11, and the
  setup (read back with `statReadDDL`) survives restarts and a match end with map
  rotation, perks upgraded to their pro versions. In-game "choose class" lists
  Custom 1-10 without "new" badges; the main-menu Create-a-Class (`cac_main`,
  `cac_weapon`) and Killstreaks screens show no locks or prices (with the dvars
  off, a fresh profile's Killstreaks screen says "Unlocked at level 10"). `god`,
  `noclip`, `give m60_mp` work on a `map` game; after `disableCheats` they answer
  "Cheats are not enabled on this server" and `sv_cheats 1` brings them back. A
  dedicated `map` server keeps `sv_cheats 1`. ASan client: a fresh profile through
  two match ends (mp_nuked, mp_array, mp_cracked) with the scripted player, and the
  main-menu screens above, no report.
- **Portable game folder** (session 8): `tools/make_portable.sh <game dir>` puts
  `blackops` and its six Homebrew libraries (`lib/`) in the game folder; the
  user's `/Users/pstn/Documents/Games/codbo` has it. Started from another
  directory with no arguments, it loads only its own libraries (`DYLD_PRINT_LIBRARIES`:
  none from /opt/homebrew), finds its data, and writes config, stats, logs and
  screenshots only inside the folder; a scan of the home and temp folders after a
  match and a main-menu session found nothing else from the game except the
  system's Metal shader cache.
- **Graphics options** (session 8): Apply (`vid_restart`) froze the game; fixed.
  Fullscreen and vsync work now (they did nothing before): fullscreen on/off
  from the main menu and six times in a match (also under ASan, no report),
  starting fullscreen, and the user's own Apply (AA 1, fullscreen, 16:9, vsync
  off).
- **Antialiasing, resolutions, Retina** (session 9): `r_aaSamples` works (MSAA
  back buffer; Apple GPUs allow 2x and 4x, 8x/16x give 4x), and so does
  transparency AA (`r_aaAlpha`, on by default with MSAA: alpha-tested foliage and
  fences). 1080p, fixed view on mp_nuked, vsync off: 97 fps without AA, 90 with
  4x, transparency AA free there. The resolution list is the display's (the user's
  main display: 12 sizes from 800x600 to 1920x1080; refresh rate: the desktop's,
  240 Hz), and fullscreen at a smaller size fills the display with the engine's
  geometry (1280x720, 1024x768 checked against native 1920x1080). Each display is
  an adapter: `r_monitor 2` goes fullscreen on the user's 4K display at native
  3840x2160 (27 fps with 4x AA), a window on it (`vid_xpos` there) gets a pixel per
  back-buffer pixel (2560x1440 in a 1280x720-point window). Runtime changes as the
  menu applies them (`vid_restart`: AA 4x/2x/off, fullscreen 1280x720, windowed
  1600x900) and the `screenshot` command work with MSAA. A 4.5-minute soak (4
  bots, scripted player, 3 maps) and the ASan client (4 maps, each with AA,
  fullscreen and resolution changes) ran without an assert or report. The menu
  cursor's mapping through a scaled or Retina window is computed, not tried by
  hand (no input here).
- **Launcher** (session 9): `Black Ops Launcher.app` in the game folder
  (tools/launcher, a SwiftUI app; make_portable.sh builds it in) picks map, mode,
  time and score limit, and bots (enemy and friendly counts, or a free-for-all
  count; Recruit/Regular/Hardened/Veteran) and whether to show the first-person
  arms (`cg_drawArms`), then starts the game on that match,
  or plainly at the main menu, without a Terminal window. The menu's PLAY can't
  start anything: it needs the removed online service (`IsSignedInToLive` is 0,
  so it shows "No network connection detected"). Tested from the app into the
  real game (scratch folder): TDM on mp_nuked, the player on allies, 3 friendly
  and 4 enemy bots, class menu up; bots fight (300-500 after a minute).
- **Sound, weapons and footsteps** (session 11): every weapon, footstep, foley,
  explosion, bullet impact and whizby alias is xWMA (format 7), which the backend
  played as silence: that was "only voices" (voices and ambience are MS-ADPCM). A
  port of FFmpeg's WMA v2 decoder decodes them now (src/audio_openal/wma/,
  Decisions session 11). Checked per alias group in the final mix, soloing one group
  at a time with `snd_solo_alias_substring` (Decisions) on mp_mountain, 7 managed
  bots, scripted player; decoder off -> on: `wpn_` -98 -> -3.4 dBFS peak (-25 RMS,
  85% of 100 ms windows active), the player's `fire_plr` silent -> -3.4, `fly_step`
  silent -> -22.5 (99% active), `fly_gear` silent -> -22.6, `prj_` silent -> -3.6,
  `wpn_grenade` silent -> -5.6, `vox_` (ADPCM) unchanged. Through the portable copy's
  launcher (re-identified copy, `KB_LAUNCHER_TEST`, TDM 3+4 bots): weapons -1..-8 dBFS,
  footsteps -16..-40, impacts to -4. 5-minute matches: ~10000 xWMA voices, mixing
  0.47 ms per 10 ms pass on average (2.7 ms worst; 0.1-0.3 before), no underruns;
  3 samples over full scale in 5 minutes (the engine's own limiter).
- **Vehicles** (session 11): script vehicles (most likely the bots' RC-XD) crashed the
  game ~100 s into a match, then gave NaN bones; the engine bugs behind it are fixed
  (Decisions session 11).
  Two 4.5-minute fu-bot matches on mp_nuked (release and ASan client): no crash, no
  ASan report, no assert (402k NaN-pose asserts before the last fix).
- **Texture streamer overflow** (session 11): the ASan client's
  global-buffer-overflow in `R_StreamUpdate_EndQuerySort` after mp_mountain ->
  mp_array is fixed. The streamer's per-image arrays held 4224 images, the image
  pool holds 4608 (retail sizes), and on mp_array the streamed glass decals sit at
  pool indices 4224-4231: their bits landed one word past each array, in the
  neighbouring fields (Decisions session 11). ASan client, 1-minute DM, 7 managed fu
  bots, scripted player: 3 runs, 25 maps, 22 rotations over 13 maps (mountain,
  array, cracked, crisis, firingrange, duga, hanoi, cairo, havoc, cosmodrome,
  radiation, villa, russianbase); no report from the streamer, no "image ... is
  missing", no assert. The one other report, at mp_hanoi's load (`SP_light`), is
  fixed too. Textures still stream at full resolution: release client, spectator
  view on mp_nuked, streaming on vs `r_stream 0`: 373 high-mip loads vs 0
  (`r_streamLog 1`), sharp vs blurry surfaces (mean |Laplacian| of the back buffer
  30.0 vs 23.9).
- **Sound** (session 10): menus and matches have sound. The main menu's
  streamed music plays, through the engine's panning, the maps' reverb, the
  mastering EQ/compressor/limiter and the options' volume. src/audio_openal is a
  software XAudio2 now (Decisions). Session 10's claim of weapons and footsteps came
  from voice counts and output levels, not from checking which aliases are heard.
  Checked with OpenAL Soft's wave writer
  (CLAUDE.md, "Hearing the client"), against -120 dBFS (silence) before: main-menu
  music at -24..-30 dBFS RMS; a match: up to 68 voices, peaks -2..-7 dBFS; a
  6.5-minute soak over 4 maps and 3 match ends; `snd_restart`, `vid_restart`,
  `map_restart`, `quit` from the menu and from a match; the mixer-thread output
  path; the real device (External Headphones: 100 passes/s, no underruns); the
  ASan client (5 minutes, 3 maps, 2 match ends; 2 minutes at the main menu): no
  report; the launcher's portable copy into a match against its managed bots. The
  MS-ADPCM decoder matches CoreAudio's bit for bit over a 108-second track.
- **Texture streaming** (session 10): the stream thread never ran on macOS/Linux
  (`Stream_Init` was a stub), so no high texture mip ever loaded (world and models
  drew their small in-fastfile mips) and no streamed sound played. It runs now;
  same view on mp_nuked against session 9's build: full-resolution textures.
- **Map packs (DLC)**: First Strike, Escalation and Annihilation's 12 MP maps
  (`zone/Common/mp_*.ff` plus `main/iw_26.iwd`-`iw_41.iwd`; the `en_*` zones are in
  the base install) load with 4 bots on a dedicated server and play in the client.
  Nothing gates them offline (`Content_GetAvailableContentPacks` only filters online
  playlists). The launcher lists the ones whose zone file the folder has. The
  Rezurrection and other zombie maps need the SP executable, which isn't ported.
- **Web-build code removed** (session 11): the D3D9-to-GL layer (src/gfx_gl) and
  src/platform carried ~3,200 lines for a WebGL/Emscripten target this branch never
  builds: the `__EMSCRIPTEN__`/GLES branches, the batching and op-trace hooks (no-ops
  on desktop), lightmap and material texture arrays, world draw merging, instanced
  XSurface draws, the vertex buffer arena and the `KB_DEVHOT` devirtualisation. Gone;
  same rendering on mp_nuked (HUD, lighting, killcam DOF, no builtin fallbacks).
- **Performance** (session 12): measured (Performance below) and three engine bugs
  fixed that cost every build (Decisions session 12): +17-29% uncapped on the fixed
  views with bots. The render thread is the limit, and in it Apple's GL driver.
- **Metal backend** (session 13): `KB_RENDERER=metal` renders through a D3D9 device
  on Metal (src/gfx_mtl): menus, every MP map, MSAA and transparency AA, `vid_restart`,
  screenshots. Its screenshots match GL's within GL's own run-to-run differences (with
  the GL layer's deviations reproduced, `KB_MTL_GLCOMPAT=1`), the Metal validation layer
  and ASan are quiet, and it runs 4-8x GL's fps: mp_nuked's views 600-730 (GL 147-578,
  the engine's frame pacing caps ~730), the overviews 294-806 (GL 67-161). By default it
  draws what D3D9 specifies and the GL layer never did (mipmaps, sun shadows, particle
  clouds, stencil). The default since session 15 (the user's decision; `KB_RENDERER=gl`
  picks the GL layer), and the game folder's portable copy has it. On the user's 240 Hz
  display, unlocked: 240 frames shown a second with vsync, 330-395 without. A cold
  system shader cache no longer stalls the spawn (Decisions session 15).
- **The user's gameplay changes, stage A** (session 16; the queue and stages are in Next
  steps): permanent god mode for the human players, no grenades of any kind (lethal,
  tactical, equipment, grenade launchers, the explosive crossbow) for anyone, no last stand
  for anyone, no killcam, final killcam or death view (respawn 0.1 s after dying), no music
  (menus and matches), no announcer, and an infinite reserve for the human players (the clip
  still empties and reloads). Each is a dvar that defaults to 1 and is archived (`kb_godMode`,
  `kb_noGrenades`, `kb_noLastStand`, `kb_noDeathCam`, `kb_noMusic`, `kb_noAnnouncer`,
  `kb_infiniteReserve`); 0 gives back the original game. Verified in play (Decisions session
  16); the game folder's copy has it.
- **The user's gameplay changes, stage B** (session 17): a small fixed crosshair for every
  weapon, the smallest one the game draws (1 degree), with the human players' shots locked
  inside it (shotguns keep their pellet spread); no aiming down the sights for human players
  (every weapon fires from the hip; the Strela locks on from the hip); human players keep the
  weapon up while sprinting and fire without ending the sprint. Bots keep their spread, ADS
  and sprint. Dvars `kb_staticCrosshair`, `kb_noAds`, `kb_sprintFire` (default 1, archived);
  verified in play (Decisions session 17); the game folder's copy has it.
- **The user's gameplay changes, stage C** (session 18): the lean of Medal of Honor: Allied
  Assault, from OpenMoHAA's source: it eases to 40 degrees and back at AA's rates, the camera
  rolls 0.4 of the angle, the eye turns about a point below it (AA's lengths scaled to BO's
  soldier), the camera and the shots stop at walls where AA's camera does, the body bends like
  AA's, and leaning no longer slows the player or ends a sprint. Dvar `kb_aaLean` (default 1,
  archived); verified in play against AA's formulas (Decisions session 18); the game folder's
  copy has it. With it the user's list of 10 changes is done.
- **The user's crosshair and no version text** (session 20, asked for on top of the plan): the
  crosshair is now the user's Counter-Strike one (CS:GO's classic static style with the user's
  own settings: white, size 2.5, gap -1, thickness 1, added to the picture), drawn as CS draws it,
  pixel for pixel at the screen's centre; it stays put and visible while reloading, switching
  weapons, meleeing, sprinting and leaning, and doesn't turn red over enemies. The human players'
  shots land inside its gap (0.4 degrees). The version text at the bottom right is gone
  (`kb_noVersion`). Verified in play (Decisions session 20); the game folder's copy has it.
- **Metal leftovers, cold shader cache** (session 19, verified on every map in session 20:
  Decisions sessions 19 and 20): with the system shader cache cold, mp_nuked's
  first frame (7 bots) waited 2.6-3.2 s, now 1.0-1.2 s, and the startup 0.33-0.41 s, now
  0.18-0.24 s. The draws' function specializations had queued behind Metal's own compile
  queue; the backend now hands Metal at most cores - 2 shader batches. Shadow maps compare
  in hardware (`sample_compare` on a `depth2d`, the samplers found by name in the shaders'
  constant tables): 21% less compile work, same images on every map. The game folder's copy has
  it (session 20).
- **Metal leftovers, the pipeline cache** (session 20): the game keeps the compiled pipelines each
  map drew in `players/metal_pipelines/<map>.bin` (under fs_h) and takes them from there in later
  runs: after another map has evicted a map's pipelines from the system's shader cache, the 4-5
  frames per 2-minute match that stalled 20-390 ms building 10-100 pipelines at once are gone (one
  22 ms frame left in one run: a pipeline not seen before). It fills and refreshes itself (Decisions
  session 20), ~15 MB a map. The game folder's copy has it.
- **Metal leftovers, the shader file** (session 20, work in progress): the shaders' names, the
  loader, the collection and `tools/make_shaderlib.sh` (run by make_portable.sh) are done and
  checked; compiling the file waits for Xcode's Metal Toolchain download (the user's yes). Until
  then the game compiles its shaders at run time, as before (Next steps 0).
- **Metal leftovers, CPU at view A** (session 20): the model lighting volume (1 MB, patched by the
  engine every frame) is locked in place and uploaded through a reused per-frame staging buffer:
  10% -> 2.4% of the render thread, +6% fps at mp_nuked's view A and overview with bots
  (Decisions session 20). The game folder's copy has it.
- **Physics crash on arm64** (session 21): the physics workers used environment objects that
  another worker had just published without memory ordering (x86 keeps stores in order, arm64
  does not), which crashed session 20's client when an object's box spanned the map. Fixed with
  release/acquire fences; `KB_PHYS_STRESS=2500` (CLAUDE.md) crashes the old code within about a
  minute and ran 5 x 4 minutes clean on the new (Decisions session 21). The game folder's copy
  has it.
- **Memory-ordering audit of the worker threads** (session 22): every job-queue module and what it
  shares, for session 21's pattern. Real on arm64 and fixed with fences: the scene entities'
  bounds hand-off (skinning could read a stale skeleton), `DObjUnlock` and the glass renderer's
  lock (plain-store unlocks), and the effects system's ring, element, trail and spot-light
  hand-offs (a drawer could see a freed effect); the job queue, physics, streaming, sound, demo
  and cinematic workers were clean. None was reproduced; regression and fps checks passed
  (Decisions session 22). The game folder's copy has it.
- **Effect bolts** (session 23): `FxBoltAndSortOrder` had the bone index 8 bits wide, so "no bone"
  (511) became bone 255. Runner sub-effects of unbolted effects and impacts were bolted to entity
  255 or the world and killed on their first update (grenade explosions' flash and smoke, impacts'
  dust, sparks, falling snow, waves, stars), and effects on an entity without a tag (silenced
  muzzle flashes, glass hits, grenade and tomahawk trails) stopped at once. Fixed with the IDA
  widths, plus effects on dynents (stored as DObj bolts); checked in game, before and after
  (Decisions session 23). The game folder's copy has it (make_portable.sh, which also collected
  the shaders into `codbo/kb_shaders/bytecode`: 16140; the library still waits for the toolchain).
- **No launchers, no killstreaks** (session 23, the user's request on top of `kb_noGrenades`):
  `kb_noLaunchers` (nobody holds an RPG, M72 LAW or Strela-3; a class's comes as an AK-47) and
  `kb_noKillstreaks` (nobody earns any), both for everyone, bots too (Decisions session 23).
  The game folder's copy has them.
- **Glossy look, reflections** (session 24, the user: "all textures look glassy or glossy, like
  covered by ice"; "even the m16 is extremely shiny", seen on GL too): the Metal device reported
  no D3D9 texture caps, so the engine kept only level 0 of every cube map and the shaders'
  roughness-to-LOD reads of the reflection probes reflected mirror-sharp on every surface; it never
  asked for the anisotropic filtering materials specify either. Both caps are reported now
  (Decisions session 24): the M16's carry handle is satin metal instead of chrome, ground at shallow
  angles slightly sharper, fps unchanged. The weapon's overall brightness is the data's (the map's
  vision file scales the viewmodel's sun light x3, specular x1.5). The GL layer keeps its
  deviations (no mipmaps: still mirror-sharp). The game folder's copy has it (700fffb7).
- **DLC weapons** (session 24 designed it; the user's request, Next steps first item): the Thompson
  and the other WWII guns are Rezurrection's zombie-map weapons, absent from every MP zone; the MP loader
  reads those zones in step but they can't simply be loaded (memory, pools, their world data), so a
  fastfile writer extracts just the weapons (Decisions session 24).
- **DLC weapons, stage 1: the zone writer** (session 25): `src/database/db_save.cpp`, generated by
  `tools/gen_db_save.py` from db_load.cpp (400 Save_* mirrors of the 404 Load_*; 4 hand-written), its
  runtime and tool mode (db_save_stream.cpp, db_save_tool.cpp), and the dedicated-server command
  `writezone <out.ff> <zone>[,<zone>,<zone>] weapon <name> ...` (CLAUDE.md). From
  zombie_cod5_prototype + common_zombie it writes the Thompson into a 536 KB zone (stream 850 KB: temp
  1200, virtual 478128, physical 401408 bytes): the weapon, 18 animations, 2 models, 4 materials, 7
  images (their pixels stream from Rezurrection's main/iw_38.iwd), 2 effects, 2 localized strings and a
  bank of its 26 sound aliases with their audio; what multiplayer has (4 techsets, 2 images, the hands
  model, 4 effect materials, the shell-eject effect) is referenced by name (stubs). In an MP match (`loadzone`, then `give
  zombie_thompson`) the viewmodel draws with its textures, hands and watch, raises, fires (muzzle flash,
  tracers, impacts, 20-round clip), reloads with the magazine animation, is named "Thompson", and its
  fire, reload and magazine sounds play (-1.4 dBFS). Re-writing a written zone gives the same bytes (the
  Thompson's and all 1760 of zombie_cod5_prototype's assets, 16 MB), the client loads the latter
  without an error, and ASan (tool and client) is quiet (Decisions session 25). Not done yet: every
  weapon, MP names, Create a Class (stages 2 and 3).
- **DLC weapons, stage 2: every weapon in one zone** (session 26; the user narrowed the list this
  session: no wonder weapons, no Pack-a-Punch versions): `tools/make_dlc_weapons.sh <game dir> <out.ff>`
  writes kb_dlc_weapons.ff in ~10 s: per map (factory, asylum, prototype, sumpf, each with its patch
  zone and common_zombie) the guns under MP names, then merges. 16 weapons: thompson_mp, type100_mp,
  stg44_mp, fg42_mp, bar_mp, bipodbar_mp, m1garand_mp, gewehr43_mp, m1carbine_mp, kar98k_mp,
  scopedkar98k_mp, type99_mp, trenchgun_mp, doublebarrel_mp, sawedoff_mp, mp40_mp. 4,220,062 bytes
  (stream 7.79 MB: virtual 6.01 MB, physical 1.96 MB), 447 root assets; written: 205 xanims, 33 models,
  63 materials, 12 techsets, 101 images, 26 effects, 20 localized strings, 1 physics preset, 16 weapons
  and one bank of 211 sound aliases. Each gun raises, fires (its own fire sound, -1 to -7 dBFS, alone
  under `snd_solo_alias_substring`) and reloads (animation, magazine sounds) in an mp_nuked listen
  match; HUD icons and display names are in the zone. Two runs give the same bytes, re-writing the
  zone gives the same bytes, ASan (tool and client) is quiet (Decisions session 26). Create a Class,
  damage, bots and loading at startup are stage 3.
- **No dual wield** (session 26, the user's request: "remove ANY and ALL dual wields, from stock game
  too"): `kb_noDualWield`, archived, default 1. A class's Dual Wield pick comes as the single gun with
  its other attachments, bots never roll one, any other dual-wield give comes as the single gun, and
  Create a Class reports no Dual Wield option (Decisions session 26). Session 27 saw the menu: the
  attachment lists still offered Dual Wield; fixed (Decisions session 27). The game folder's copy has
  it (make_portable.sh at 3fcbb7f4, the user's request).
- **DLC weapons, stage 3: Create a Class** (session 27): the 16 guns are in Create a Class, in custom
  classes and with the bots, behind `kb_dlcWeapons` (archived, default 1, read at startup: a change takes
  effect at the next start). kb_dlc_weapons.ff now also carries copies of mp/statsTable.csv (the guns' rows in
  16 empty rows below 99), mp/attributesTable.csv (Create a Class's stat bars) and mp/weaponOptions.csv,
  their descriptions, and multiplayer damage (each gun the closest multiplayer gun's: Thompson and MP40
  the MP5K's, STG-44 the AK47's, the bolt-actions the L96A1's, ...); the game loads it at startup right
  after common_mp (flag 1024) and reads the items again, so the weapon table registers the guns with
  multiplayer's (1654 -> 1670 weapons). Create a Class lists them after multiplayer's guns in each group
  (SMG: Thompson, Type 100, MP40; assault: STG-44, M1 Garand, Gewehr 43, M1 Carbine; LMG: FG42, BAR,
  Deployable BAR; sniper: Kar98k, Scoped Kar98k, Arisaka; shotguns: M1897 Trench Gun, Double-Barreled
  Shotgun, Sawed-Off Shotgun) with their HUD icons, names and bars; a custom class with one spawns with
  it; bots take them (16 of ~44 primaries in a 2-minute match) and kill with them at multiplayer
  damage. The Dual Wield option is now gone from the attachment lists (session 26's change missed
  them). make_portable.sh writes the zone into the game folder: the game folder has it (make_portable.sh
  at 3fcbb7f4, the user's request; its kb_dlc_weapons.ff is the tested one, same bytes; the shader
  library step still stops at the missing Metal Toolchain). Checks in Decisions session 27.
- **No prone, no hit marker** (session 28, the user's requests: "disable/prevent prone, only crouch";
  "when im shooting someone, i get a second crosshair around my new static one, i dont want that, only
  the static one"): `kb_noProne` (nobody goes prone or dives to prone, bots and scripts' commands
  included; every prone key or command crouches instead, and the user's Ctrl, `toggleprone`, toggles
  crouch) and `kb_noHitMarker` (the X the scripts flashed around the crosshair for a second after each
  hit is gone; the hit sound stays), both archived, default 1. Verified in play (Decisions session 28).
  The game folder has it (make_portable.sh at fc432bdf, the user's request; the DLC zone came out the
  same bytes, the shader library step still stops at the missing Metal Toolchain).
- **Create a Class from the main menu, by mouse** (session 28, the user: "add a create a class button to the
  main menu, ideally replace a button i can no longer use"): the main menu's THEATER (films, which need the
  removed online service) is CREATE A CLASS (`kb_cacButton`, archived, default 1, read at startup), and the
  Create-a-Class lists work with the mouse now. They had never been used by hand: a click in a gun list
  equipped the item whose number was the row's (the Uzi's row 4: the Makarov), the perk lists likewise, and
  an attachment click equipped attachment number <count> (nothing); upstream's sharedUiInfo rework left
  those fields doubled up (Decisions session 28). Clicked through in the game: the button, Custom 1, a gun
  (Thompson, MP5K), Perk 1 (Ghost Pro), an attachment (Suppressor), a camo (Tiger), Esc saves; the class
  survives a restart and spawns in a match with the MP5K, suppressor and Tiger camo. New test commands
  `kb_mouse` and `kb_key` click through menus (CLAUDE.md). The game folder has it (make_portable.sh at
  342765a3, the user's request).
- **Only the crosshair on screen** (session 29, the user: "clean up the ingame hud? only keep crosshair, and
  disable the achivement +XP pop ups, or the tags top left"): `kb_cleanHud`, archived, default 1. During play
  the screen shows the crosshair and nothing else of the HUD: no minimap, score and timer, ammo and
  killstreak wheel, kill feed, +XP numbers, medal and challenge popups ("Headshot", "First Blood", "The
  Shortcut +250 XP"), player cards ("Killed ..."), objective text, weapon pickup hints, names over heads,
  spawn perk list, damage and grenade indicators, chat, or objective markers in the world. Kept: the user's
  own FPS counter (`cg_drawFPS`, their setting), menus (team, class, Esc), the scoreboard, the
  round's/match's outcome ("VICTORY, Time limit reached", the scores, the match bonus) and the After
  Action Report. Checked in play (Decisions session 29). The game folder has it (make_portable.sh at
  1c9ef755 plus the launcher fix below, the user's request; the DLC zone came out the same bytes, the shader
  library step still stops at the missing Metal Toolchain).
- **Bot bodies stay where the bot died** (session 30, the user's report: "when i kill bots, they disapear
  INSTANTLY, AND/OR they teleport like a meter away"): a body that took over one of the 4 body slots far from
  the slot's last body, and a bot that respawned far from where it died, were decoded by the client tens to
  hundreds of units from their real place when the old coordinate was negative (the network code truncated
  where the original rounds), and stayed off: a body shifted ~1 m, or out of sight ("disappears"), and a
  respawned bot drawn away from where the server had it (shots at it missed). Every build of this decompile
  had it, with or without the user's `kb_*` changes. Fixed in the coordinate encoder and decoder (Decisions
  session 30). Checked in 7-bot matches (the launcher's managed bots): before, 1 to 10 bodies a run (of 23-29;
  four runs) appeared 18-876 units off; after, 32 of 35 within 5 units and the rest at the server's exact position (a
  sprinting bot is drawn 50-75 ms behind the server, as in the original). Bodies still go when 4 newer ones
  exist (the game's limit, MAX_CLIENT_CORPSES 4). The game folder has it (make_portable.sh at 2012ed90, the
  user's request; the DLC zone rewritten, the shader library step still stops at the missing Metal Toolchain).
- **No start countdown; every custom class's guns at once** (session 31, the user: "remove the game start
  coutndown" and "let me choose the class, then after i choose start the game automatically. and also equip me
  with ALL custom classes", then, asked, "carry all their guns at once"): `kb_noCountdown` (no "MATCH BEGINS IN"
  and no frozen players at a match's or round's start: a player picks a class and plays at once) and
  `kb_allClassGuns` (a human player spawns with the chosen class as before plus the primaries, then the
  secondaries, of all 10 custom classes, each with its attachments and camo, a gun held already left out; weapon
  switching cycles through them all; bots don't get them; not in gun game and the other wager modes), both
  archived, default 1. Checked in play (Decisions session 31): 10 classes with different guns gave 14 guns (10
  primaries, 4 distinct secondaries), cycled through with weapnext, again after a respawn. Fixed on the way: the
  15 clip slots overflowed and crashed the server in `giveWeapon` (two slots were held by the frag's and the
  tactical's ammo, which kb_noGrenades never gives), and an old 64-bit bug in the console's game message windows
  (bold game text and subtitles used a window straddling two: a garbage pointer, then a crash or a stray write;
  every build of this port had it). The game folder has it (make_portable.sh at f5eacfea, the user's request;
  the DLC zone rewritten, the shader library step still stops at the missing Metal Toolchain); pushed.
- **Medal of Honor damage** (session 32, the user, after asking how the guns compare with MoHAA's SMGs and StG:
  "make the damage like mohaa", "Arms and legs you can keep same as body"): `kb_mohaaDamage`, archived, default 1.
  Every rifle, SMG, machine gun and pistol does the damage of its closest MoHAA: Allied Assault gun (1.11's
  multiplayer values) at any distance: SMGs 25 (Thompson, MP40), assault rifles 27 (StG 44), machine guns 30 (BAR),
  semi-automatic rifles and the WA2000, Dragunov and PSG1 48 (M1 Garand), Kar98k and Arisaka 90 (Kar98k), L96A1 and
  Scoped Kar98k 106 (Springfield), pistols 30 (Colt); headshots (helmet, head, neck) 4x, so one kills with any of
  them; body, arms and legs 1x. Everyone's guns, the bots' too; shotguns, the knife, explosives and killstreak or
  vehicle guns keep Black Ops' damage; walls still take their share. Checked in play (Decisions session 32). The
  game folder has it (make_portable.sh at 3622e5b3, the user's request; the DLC zone rewritten); pushed.
- **The Thompson held closer** (session 32, the user: "does the thompson (first person view model) have the same
  size as other weapons? it looks smaller"): `kb_gunPlacement`, archived, default 1. Its model is not small (the
  same height as its third-person model); its own animations hold it ~5 cm farther from the eye than the MP40,
  STG-44 and Type 100 (and much farther than the AK-47/AK74u/M16/Galil), so it looked ~15% smaller. Now it sits
  where the MP40 does, then a little closer and lower so its back end is off screen (the user picked that from
  three pictures). Only the player's own first-person gun moves. Checked with screenshots and a measurement
  (Decisions session 32). The game folder has it (make_portable.sh at cef623ac, the user's request); pushed.
  Undoing it: `kb_gunPlacement 0`, or revert cef623ac (one self-contained commit) and reinstall.
- **WWII guns move like Black Ops' while walking** (session 32, the user: "why does the thompson move down so much
  when i start moving forward? other weapons dont seem to do that"): `kb_wwiiGunMovement`, archived, default 1. The
  WWII guns kept World at War's walking motion (the first-person gun drops 1-3.25 units and doesn't tilt); every
  Black Ops gun stays up and tilts 2 degrees. Now all 15 WWII guns (the MP40 already did) move like Black Ops'
  standing and crouched; prone keeps each gun's own crawl pose. The user chose all of them over only the Thompson.
  Checked (Decisions session 32). The game folder has it (make_portable.sh at 806f09a7, the user's request); pushed.
- **Classes 1-5 only; sniper scopes** (session 32, the user: "only equip me with class 1-5 + my chosen class if its
  not in 1-5, not all 10", then "only equip me with the secondary (pistol) of my chosen class, not 1-5", and "i want
  a scope on teh snipers"): `kb_allClassGuns` now adds only the primaries of custom classes 1-5 on top of the chosen
  class's loadout, whose pistol is the only one (a class 6-10 or a default class picked still comes with its own guns). New `kb_sniperScope`, archived, default 1: with kb_noAds on, the scoped sniper rifles
  (L96A1, WA2000, Dragunov, PSG1, WWII Scoped Kar98k) look through their scope with the aim button (the user's MOUSE2
  is `+toggleads_throw`); everything else still fires from the hip, the iron-sighted Kar98k and Arisaka too. Checked
  (Decisions session 32). The game folder has both (make_portable.sh at f91de0c8, the user's request); pushed.
- **No waiting at a round's or match's end** (session 32, the user: "can you remove the "wait" at the end of a round? i
  dont care about the score and stuff"): `kb_noRoundEndWait`, archived, default 1. The outcome screen's delays
  (~5 s between rounds, the side switch, ~7 s after a match) and the 30 s scoreboard before the next map are gone: the
  next round starts, or the next map loads, at once (measured: 0.07 s between S&D rounds, 0.1 s from a TDM time limit
  to the map rotation; the load itself ~3-4.5 s). The match bonus and stats are still handled. Checked (Decisions
  session 32). The game folder has it (make_portable.sh at 34dbf1e8, the user's request); pushed.
- **Gun damage setting** (session 32, the user: "can we have a setting for weapon damage? maybe percentage, 100 being
  current level (and it affects all mg/smg (non-snipers))"; asked, they chose every gun but the sniper rifles, in the
  launcher and the console, bots' guns too; then "i dont need more than 100, the main goal of it is to lower it"):
  `kb_damagePercent`, archived, an int from 1 to 100, default 100. Every gun's bullet damage but the sniper rifles'
  (L96A1, WA2000, Dragunov, PSG1, Kar98k, Scoped Kar98k, Arisaka) is multiplied by it, on top of kb_mohaaDamage, before
  the hit location's multiplier; everyone's guns; it only lowers. The launcher has a "Gun damage" row (5-100% in steps
  of 5) and passes it at every start. Checked (Decisions session 32). The game folder has it (make_portable.sh at
  c8a862fc, the user's request; the launcher with the new row); pushed.
- **The launcher's icon** (session 32, the user: "id like to add an icon to the launcher"): the Black Ops cover art
  the user picked on SteamGridDB, in Apple's rounded-square icon shape. The picture stays out of git
  (`build_macos/launcher_icon.png`); tools/launcher/build.sh makes the app's AppIcon.icns from it with
  tools/launcher/make_icon.swift, and make_portable.sh passes it. Checked (Decisions session 32). The game folder's
  launcher has it (make_portable.sh at 5e3a856d, the user's request; macOS shows it); the code is pushed. Asked why the
  picture can't go in the repo: the repository is public (a fork of riicchhaarrd/KisakBlack) and the picture is
  Activision's cover art; the user agreed to keep it out. Then "my gamefolder has to be fully portable": the launcher
  now also keeps the picture (Contents/Resources/launcher_icon.png), and build.sh reuses the installed app's copy when
  it gets no picture, so the game folder carries everything, even for a rebuild elsewhere. The game folder has it
  (make_portable.sh at 6fb3d477, the user's request); pushed.
- Diagnostics: `KB_FPS=<seconds>` (1 if empty) prints fps, frame time average and
  maximum, the buffer swap's average time, and per frame the draw calls, StretchRect
  blits, buffer uploads (count, KB) and buffer storage orphaned by DISCARD locks, to
  stderr every interval;
  `KB_SCREENSHOT=<dir>` (+`KB_SCREENSHOT_EVERY=n`) writes the back
  buffer as a top-down TGA every n presents with per-interval draw counters
  (`KB_SCREENSHOT_WINDOW=1` adds `window_N.tga`, what the window shows after
  scaling, at its pixel size);
  `KB_TRACEFRAME=n1,n2,..` logs the frames' SetRenderTarget/SetViewport/Clear/
  StretchRect calls and every draw (GL state, GL error after the draw, bound
  textures) and, with KB_SCREENSHOT, dumps the back buffer at the first 16 resolves
  (`resolve_N.tga`). `KB_PTR32STATS=1` prints the busiest slow-path
  `Ptr32_Encode` call sites (return addresses; `atos` after the slide).

## Performance (session 12)

The user wants the client well past their 240 Hz display: benchmark uncapped
(`+set r_vsync 0 +set com_maxfps 0`). Their config (`codbo/players/config_mp.cfg`):
1280x720 fullscreen, no AA, r_picmip 2, and since 2026-10-09 22:18 already r_vsync 0,
com_maxfps 0.

**Method** (now `tools/bench_views.py`, `cmp_screens.py`, `sample_report.py`,
`gpu_trace.py`; CLAUDE.md "Measuring the client"):
- Fixed views: `+map mp_nuked` (no bots, or the launcher's 3+4 managed bots in TDM),
  `KB_FPS=1`, and `KB_CMDS` joining (`openscriptmenu team_marinesopfor autoassign`,
  `changeclass assault_mp,0`, `god`) then `setviewpos <x y z yaw pitch>` every 8-10 s;
  average the `[fps]` lines of each position after the first two. Views: A `-1868 153
  -4 25 0` (spawn, heaviest), B `-667 -263 1 301 0`, C `-1419 -12 -4 217 0`, D `-1704 569
  -4 132 0` (lightest), E `-1668 555 -4 49 0`, F `-1380 104 12 132 0`, sky `-1868 153 -4
  25 -85`. A/B/D/E repeat within ~3%; C and F vary more between runs. Free play with the
  scripted player is too noisy to compare builds (where it looks dominates). The numbers
  below were taken without `noclip`, which bench_views.py adds (F floats instead of
  landing); rerun the tool for a baseline before comparing.
- CPU: `sample <pid> 10` while sitting at one view; sum each thread's self time by
  function and library, and inclusive time of the GL calls (`RB_RenderThread` is the
  thread holding `com.Metal.CommandQueueDispatch`).
- GPU: `xcrun xctrace record --template 'Metal System Trace' --attach <pid>
  --time-limit 4s`, then `xctrace export --xpath` of the `metal-gpu-intervals` table
  (busy = union of the intervals) and `metal-application-command-buffer-submissions`.
  A GL_TIME_ELAPSED query around the frame is useless here: it reports about the CPU's
  submission time, and the queries cost ~1.5 ms a frame.
- Rendering checks: the console `screenshot` command at each view (`<fs_h>/main/
  screenshots`), compared pixel by pixel with an unmodified build (a `git worktree` of
  HEAD built in its own directory).

**Numbers** (uncapped, 1280x720 fullscreen, the user's settings; fps):

| view | draws/frame before -> after | before | after | after, prepass off |
|---|---|---|---|---|
| A | 6950 -> 4400 | 104-106 | 133 | 150 |
| B | 4600 -> 3220 | 140-147 | 178 | 194 |
| C | 2200-2950 -> 1710 | 178-223 | 241 | 279 |
| D | 1160 -> 990 | 246-251 | 324-359 | 387 |
| E | 4150 -> 3100 | 143-145 | 180 | 183 |
| F | 2440-4190 -> 2150 | 146-170 | 210-223 | 240 |
| sky | ~200 | 400-440 | 530 | |

With 7 bots playing TDM, same views: 94/136/204/236/139/158 before, 116/162/237/304/
163/199 after (harmonic mean 148 -> 179). "Prepass off" is `+set r_depthPrepass 0`
with `KB_NOPREPASS=1` (the lit pass's EQUAL depth test becomes LEQUAL; the original game
has no such switch, session 2's port wired the dvar up).

**Where the time goes** (after the fixes):
- The render thread is busy 100% of the time at every view; the main thread waits
  ~85% (at view A ~1.3 ms of work per frame without bots; with the 7 bots at view D
  ~0.7 ms at ~310 fps, 21% busy: the listen server's frames are not the limit). 640x480 instead of 1280x720: +5-8%. The GPU is busy ~40-43%: 4.1 ms a frame at
  view A (before the fixes), 1.7 ms at D, 1.1 ms at the sky.
- View A, render thread: Apple's GL driver ~65-70% (`glDrawElementsBaseVertex` 64%
  inclusive, ~1.2 us a draw: pipeline-state lookups, texture/sampler and uniform
  binding, Metal encoding), IOKit command-buffer submissions ~15%, the D3D9->GL layer
  ~6%, the engine's backend ~8%.
- Apple's GL submits a Metal command buffer for nearly every render pass: ~15 command
  buffers (~28 encoders) a frame, ~80 us each; every `glBlitFramebuffer` (StretchRect,
  6 a frame), framebuffer switch and renderbuffer reallocation does one.
- The sky view (530 fps, 1.9 ms): ~65% of the render thread is in the kernel: the swap
  (`NSOpenGLContext flushBuffer`: synchronous WindowServer calls `_CGSWindowIsOrderedIn`
  and `SLSGetSurfaceOpacity`, ~0.7 ms) and the submissions.
- Tried and dropped: Apple's multithreaded GL engine (`CGLEnable(kCGLCEMPEngine)`): +9%
  at A, 3-8% slower elsewhere. Every `glMapBufferRange` (dynamic index uploads) and
  occlusion-query read waits for the GL worker thread to drain (67% of the render
  thread waiting), and the late query results made the engine draw more (990 -> 1500
  draws at D).

**Options** (the user's choice; numbers above):
- Keep GL: what's left is small (the layer's own ~6%, uniform upload size, fewer
  passes); the driver's per-draw cost and per-pass submissions stay. Ceiling roughly
  150-170 fps at view A, ~400 at D, with prepass off.
- ANGLE (GLES on Metal): would batch passes into one command buffer and present
  through CAMetalLayer, but adds its own validation and state tracking per draw; a
  large download and build (depot_tools/gn). Ceiling between GL and Metal, unmeasured.
- Native Metal backend (D3D9 -> Metal, replacing src/gfx_gl on macOS): one or two
  command buffers a frame, no per-pass submissions, CAMetalLayer presentation, cached
  pipeline states, constants by `setVertexBytes`, ~0.2-0.3 us of encoding a draw.
  Estimated render thread at view A ~2.5-3 ms (engine backend ~0.7 ms + translation +
  encoding), so the GPU (~3.5-4 ms there today, less with fewer passes and proper
  load/store actions) becomes the limit: ~250-350 fps at A, 500-800 at D. Several
  sessions: resources, state -> pipeline cache, D3D9 bytecode -> MSL (from the GLSL
  translator), render passes, presentation, then correctness passes.

## Metal backend (session 13)

`Direct3DCreate9` (src/gfx_mtl/mtl_window.mm) returns a D3D9 device on Metal, or the
GL layer's with `KB_RENDERER=gl`; the engine's renderer (src/gfx_d3d) is unchanged.
Same binary (cmake/macos.cmake: Objective-C++ with ARC, Metal, QuartzCore, Foundation;
no new dependencies). Metal is the default since session 15, and the game folder's
portable copy and its launcher have it (make_portable.sh rerun). Design and the reasons
behind it: Decisions (session 13); the display rate and the shader cache: Decisions
(session 15).

Files (~3,700 lines, plus the 365-line shared parser):
- src/gfx_shader/sm3.{h,cpp}: SM3 bytecode -> IR, shared with gl_shader.cpp's GLSL.
- src/gfx_mtl/mtl_msl.{h,cpp}: IR -> MSL.
- mtl_d3d9.h, mtl_resources.mm: formats, buffers, textures, surfaces, vertex
  declarations, shaders and their batch compiler, queries.
- mtl_device.{h,mm}: the device: D3D state, the per-draw translation
  (`prepareDraw`), the pipeline/depth-stencil/sampler caches, render passes and
  clears, StretchRect, readbacks and screenshots, presentation, Reset, `KB_FPS`.
- mtl_window.mm: the SDL window and its CAMetalLayer, the presenter thread, the
  backend choice.
- src/gfx_gl/gl_spec.{h,cpp} and hooks in the GL layer (`KB_SPEC`);
  src/gfx_mtl/tests/shader_test.mm and tools/shader_test.sh.

**Checks** (tools/bench_views.py on the game's clock, scratch profile, the user's
settings: 1280x720 fullscreen, no AA; every run's log grepped for `[mtl] BUG`, never
found):
- Screenshots against GL with `KB_MTL_GLCOMPAT=1` (the GL layer's deviations
  reproduced): mp_nuked's 7 views and all 26 overviews are within the GL-vs-GL noise
  of the closer of two GL runs, or differ only by something that also varies between
  GL runs, each identified: the team `autoassign` picks (emblem), the match clock (up
  to 2 s apart between GL runs: the objective text, script movers such as mp_mountain's
  gondola), particle effects (they step per frame, so they differ at 370 vs 79 fps:
  with `fx_draw 0` mp_array's lower half goes from |d| 5 to 0.1), texture streaming
  (one GL run had mp_radiation's roof at a lower mip), Nuketown's mannequin (placed at
  random), the weapon's sway after `setviewpos`, the fps counter. 4x MSAA with
  supersampled transparency AA (`r_aaAlpha 2`): the same, after the per-sample fix
  (Decisions).
- Default mode (D3D9's behaviour): 3-48% of pixels differ from GL, by design: mipmaps,
  sun shadows (mp_nuked d: the hedge under the house dark), particle clouds (smoke and
  mist sheets; `fx_drawClouds 0` changes GL's image not at all, Metal's by |d| 2 on
  mp_hanoi), stencil. Looked at mp_havoc, mp_duga and mp_nuked a, b, d: nothing broken;
  there is no Windows screenshot to compare with.
- `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` (API and GPU validation): mp_nuked's
  views, the mp_crisis, mp_berlinwall2, mp_hanoi and mp_havoc overviews, 4x MSAA with
  transparency AA, compat mode: no message. ASan build with `MTL_DEBUG_LAYER=1`, 7
  bots, the scripted player, 1-minute TDM (mp_nuked -> mp_array -> mp_cracked, 240 s):
  no report from the backend; on the final code one engine bug (`CanDamage`, below),
  then a clean rerun.
- `vid_restart` (4x MSAA, windowed 1600x900 and back), the `screenshot` command,
  memory flat at ~930 MB over a 3-minute match.

**Numbers** (uncapped fps, same tool and settings, GL measured this session):

| view | GL | Metal | GL, 7 bots | Metal, 7 bots |
|---|---|---|---|---|
| A | 147 | 602 | 131 | 538 |
| B | 183 | 711 | 161 | 693 |
| C | 244 | 730 | 240 | 619 |
| D | 330 | 700 | 332 | 638 |
| E | 168 | 674 | 163 | 592 |
| F | 239 | 733 | 196 | 683 |
| sky | 578 | 716 | 565 | 610 |
| overview | 120 | 524 | 102 | 465 |

Session 15, Metal as the default, screen unlocked (the table's Metal runs had it
locked), no bots: A 558, B 681, C 747, D 765, E 672, F 730, sky 806, overview 490.
Runs of one build vary by up to ~10%. Overviews, no bots, GL -> Metal: mp_crisis 67 ->
294, mp_berlinwall2 70 -> 332, mp_outskirts 73 -> 350, mp_cracked 76 -> 635, mp_array 79
-> 377, mp_zoo 85 -> 394, mp_kowloon 86 -> 418, mp_villa 87 -> 421, mp_stadium 93 -> 408,
mp_radiation 95 -> 422, mp_mountain 100 -> 453, mp_silo 106 -> 459, mp_golfcourse 107 ->
486, mp_russianbase 108 -> 516, mp_drivein 109 -> 536, mp_duga 110 -> 500, mp_cosmodrome
110 -> 738, mp_hotel 111 -> 532, mp_area51 112 -> 508, mp_nuked 120 -> 513,
mp_firingrange 120 -> 629, mp_cairo 123 -> 574, mp_discovery 124 -> 582, mp_gridlock 133
-> 556, mp_havoc 139 -> 621, mp_hanoi 161 -> 806 (4.2-8.3x; the build before the
shader-side shadow compare, which moved mp_nuked's views by <3%). 4x MSAA, mp_nuked
A-F/sky: GL 131-231/388; Metal 415-694/721 with transparency AA off, 224-392/727 with
it on (the default with MSAA; both modes supersample, Decisions).

**Where the time goes** (view A, no bots, ~1.8 ms a frame): the render thread is busy
99%: the translation (`prepareDraw`) 27%, Metal's encoding of the draws 23%, polling
occlusion queries (`GetData`) 7%, re-uploading the model lighting volume each frame
~5% (a staging buffer and the relock copy), the engine's backend most of the rest. The
main thread is busy 45%. The GPU is busy 76% (1.7 ms a frame: vertex 1.1 ms, fragment
0.85 ms) in 1.2 command buffers and 24 encoders a frame. The lighter views run into
the engine's frame pacing (~730 fps: the main loop's `NET_Sleep(1)` until the next
millisecond), so CPU and GPU are close to balanced at A and the engine caps the rest.

## Next steps (in order)

**Next (the user's decision, 2026-10-12, session 32): strip the campaign's levels and all voices from the game folder,
in a fresh session, safely.** The user: "id like to strip the single player stuff SAFELY, like maps, videos,
voices/sounds"; shown the inventory below, they chose: the campaign levels, the campaign menu and briefings, and
"i dont want the voices", which they confirmed means ALL voices (the campaign's dialogue, multiplayer's battle chatter
and announcer recordings, the zombies' voices). Not chosen, so they stay: music and sound effects, textures, the zombie
maps (common_zombie and the cod5 maps also feed the WWII guns file), the small shared campaign zones (code_pre_gfx,
code_post_gfx, common, patch and their en_ files, 24 MB). Videos: the folder has none (no .bik anywhere).
Measured (read-only, session 32; the game folder is 11 GB): zone/ campaign levels 1.6 GB, 36 files (creek_1, cuba,
flashpoint, fullahead, hue_city, int_escape, khe_sanh, kowloon, outro, pentagon, pow, rebirth, river, terminal,
underwaterbase, vorkuta, wmd, wmd_sr71 and their en_ files); campaign menu and briefings 58 MB, 14 files (frontend,
so_narrative1-6_frontend and en_). Voices live inside the archives, mixed with other files: localized_English_iw01-14
hold english/sound/vox/scripted 1.41 GB (18788 files), vox/mpl 200 MB (the multiplayer announcer), dds/us, rn, br 66 MB
(battle chatter), vox/dds 10 MB, vox/mp 4 MB, vox/contextual_melee 4 MB, voiceovers/zmb 2 MB, mid/int_hudson_explains_*
(campaign), but also images 40 MB (localized_English_iw00, iw12: keep), mus/licensed 55 MB and evt/front_end 3 MB
(not voices: keep); the plain archives hold sound/vox/dds 55 MB and vox/scripted 46 MB (iw_23, iw_24), vox/zmb (iw_32)
and more to find. The plan agreed with the user: (1) the whole zone files; (2) rebuild the archives without the voice
files (keep every other file; an archive left empty can go), deciding what is a voice by folder (vox, dds, voiceovers,
mid dialogue) and checking the list before removing; (3) test everything on a scratch game folder (links, plus the
rebuilt archives) before touching the real one: every multiplayer map and several modes, the main menu, Create a Class,
the launcher, the WWII guns rebuild (tools/make_dlc_weapons.sh), the sound console channel (`+con_showchannel sound`)
and main/missingasset.csv for errors from the missing voices, and that a match doesn't stall on failed sound opens (a
kb_* mute for voice aliases, like kb_noAnnouncer, if it does); (4) then move the removed zone files and the original
archives to the Trash (never delete; the user empties it) and put the rebuilt archives in. Don't run this at the same
time as the Rezurrection removal below (both change the game folder and its archives). If the folder came from Steam,
Steam's file check would bring the files back.

**Next (the user's decision, 2026-10-12, in session 31's conversation): remove Rezurrection from the game folder,
keep the 16 WWII guns.** Facts found (read-only): each DLC added its own map files and numbered archives (dates and
contents): First Strike `iw_26`-`28` + mp_berlinwall2, mp_discovery, mp_kowloon, mp_stadium, zombie_cosmodrome;
Escalation `iw_29`-`32` + mp_gridlock, mp_hotel, mp_outskirts, mp_zoo, zombie_coast; Annihilation `iw_33`-`35` +
mp_area51, mp_drivein, mp_golfcourse, mp_silo, zombie_temple; Rezurrection (zombies only, ~800 MB) `iw_36`-`41` +
zombie_cod5_prototype/asylum/sumpf/factory and zombie_moon, each with a `_patch` zone and an `en_` zone in
zone/English (common_zombie is the base game's: keep it). At play time kb_dlc_weapons.ff needs from Rezurrection
only 53 images in `iw_38` (8.7 MB, the guns' high mips) and one sound in `iw_40`
(sound/wawzmb/sfx/levels/nazi_zombie_sumpf/radio/message_2/m2.wav, 0.8 MB); found by matching the zone's strings
(decompressed) against the archives' file names. The MP40's suppressor, aperture sight and drum are parts of its
view model, which is in kb_dlc_weapons.ff; their textures are in base-game archives (`iw_09`, `iw_11`). Suppressed
sounds no gun uses yet, to keep: World at War's suppressed Thompson (`weap_thompson_fire_plr_sil`, its own recording
wpn_thompson_st_sil.wav) and Type 100 (`weap_type100_fire_plr_act_sil`, `weap_type100_act_sil`:
wpn_type100_st_sup.wav, wpn_type100_m_sup.wav), the Black Ops-era `wpn_thompson_silencer_fire_plr/_npc` and
`wpn_mp40_silencer_fire_plr/_npc` (both play the AUG's suppressed recordings, `sound\wpn\assault\aug\plr\shot\silenced\
silenced_00-03.wav`; secondaries `*_sweet_*`, `*_bass_swt`). World at War's other `_sil` aliases (Carbine, STG-44
as mp44, MP40, Garand) are the normal shots quieter or mechanism noise: not wanted. In main/ of the base path the
game loads only archives named `iw_*` (com_files.cpp, "Invalid IWD"). The plan the user approved ("ok go, in a fresh
session"): add the suppressed aliases to kb_dlc_weapons.ff (the writer needs an option to keep aliases no asset
names); copy the 54 files into a new `main/iw_kb_dlc_weapons.iwd`; make make_portable.sh keep both files (not rebuild)
when Rezurrection's zones are missing; test without Rezurrection (scratch game folder); back both files up in
build_macos (ignored by git; game content never goes to GitHub); then move Rezurrection's files to the Trash. Things
the user was told would be lost: the 5 maps (so no Nacht-as-multiplayer-map later), Pack-a-Punch versions and wonder
weapons (dropped earlier anyway), zombies content, rebuilding the guns file from scratch.

**Session 32: Medal of Honor damage (`kb_mohaaDamage`): done** (the user's request; Status, Decisions session 32).
Installed in the game folder and pushed at the user's request. Then the Thompson held closer (`kb_gunPlacement`, the
user's request; Status): installed and pushed at the user's request. Then the WWII guns' walking motion
(`kb_wwiiGunMovement`; Status): installed and pushed at the user's request. Then classes 1-5's primaries only and the sniper
scopes (`kb_allClassGuns`, `kb_sniperScope`; Status): installed and pushed at the user's request. Then no waiting at a
round's or match's end (`kb_noRoundEndWait`; Status): installed and pushed at the user's request. Then the gun damage
setting (`kb_damagePercent`, the launcher's "Gun damage"; Status): installed and pushed at the user's request. Then the
launcher's icon (Status): installed and pushed at the user's request (the picture itself stays local). Then the
picture kept inside the launcher (Status): installed and pushed at the user's request. The user also asked how recoil and spread compare
with MoHAA's (answered, facts in Decisions session 32); offered to make the guns spray and kick like MoHAA's, they
chose only the damage. Next: ask how the damage plays; the Rezurrection item above was approved by the user in
another conversation for a fresh session (check git log and ask whether it is done or under way before starting it);
then whether they have more fixes; then offer the list below again.

**Session 31: no start countdown and every custom class's guns at once: done** (the user's requests; Status,
Decisions session 31). Installed in the game folder and pushed at the user's request. Earlier in session 31 the user tried item 4 below
(attachments for the WWII guns: a suppressor on the Thompson), then asked to undo it, which was done (nothing of it
remains); asked what the game itself has, the user said to "leave as is for now". Their answers for when it comes
back: only attachments that come with the game; the Thompson's suppressed sound would be the Black Ops one
(`wpn_thompson_silencer_fire_*`, which plays the AUG's suppressed recordings). Next: ask the user whether they
have more fixes, then offer the list below again. Session 30's bot-body fix is installed and pushed.

**The user's list for later** (session 28: offered as the menu of what to do next, the user answered
"remember these we'll come back to them. i wanna fix a few more things first"; ask before each, in plain
words): 1 the shader library's Metal Toolchain download (~700 MB; Xcode shows the size only once the
download starts; the user's yes first) and the rest of item 0's stage 2; 2 colour pictures for the WWII
guns in Create a Class (white HUD silhouettes now; new pictures would have to be rendered); 3 camo on the
WWII guns; 4 attachments for them; 5 the knife-only custom class of a client on a separate dedicated
server (Open items, session 27). Session 28 did the user's first fixes (no prone, no hit marker: Status);
the user may have more before the list.

**DLC weapons in Create a Class: done (sessions 25-27)** (the user's request, session 24; facts and the
reasons for the design in Decisions sessions 24-27; what is left is optional, below). The user's answers: the 12 WWII guns (Thompson, Type 100, STG-44,
FG42, BAR, M1 Garand, Gewehr 43, M1 Carbine, Kar98k, Type 99, M1897 Trench Gun, double-barrel) plus
the scoped Kar98k, sawed-off and bipod BAR, and the MP40: 16 guns. Session 26 dropped, at the user's
word, the wonder weapons ("i dont want any ray guns (or wonder weapons?) nor any crossbows either":
all of them) and then the Pack-a-Punch versions ("drop the pack a punch versions too"); the DLC
throwables stay blocked by `kb_noGrenades`, the MP crossbow too. Damage matched to the closest MP
gun. Bots get them too. A `kb_*` dvar, archived, default 1 (`kb_dlcWeapons`), like the other changes.
Stages, each handed off per the protocol in 0 (update HANDOFF, commit, task card):
- **Stage 1, the fastfile writer and the extraction tool: done (session 25)** (Status, Decisions session
  25): `tools/gen_db_save.py` -> `src/database/db_save.cpp`, db_save_stream.cpp, db_save_tool.cpp, the
  `writezone` command; the Thompson works in an MP match from a written zone (`loadzone`, `give`).
- **Stage 2, every weapon: done (session 26)** (Status, Decisions session 26): `tools/make_dlc_weapons.sh`
  writes kb_dlc_weapons.ff, the 16 guns under MP names; each works in a match (`loadzone`, `give`).
  What follows is the plan stage 2 started from, kept for its facts: one zone with all
  of them. `writezone` loads at most 3 source zones at once (flag pairs; physical memory: common_mp ~107
  MB + common_zombie ~80 + factory ~102 of 328 MiB), so extract per map (`<map>,common_zombie`, plus the
  map's `_patch` zone if it changes the weapons: zombie_cod5_prototype_patch has 1 weapon, 5 models, 3
  anims) into intermediate zones and merge those (small, self-contained but for multiplayer's stubs) in a
  last run (more flag pairs, or more than 3 small zones), deduplicating shared assets by name. The
  variants renamed `<ref>_mp` with references that are no prefix of another
  (BG_UnlockablesGetItemIndexforWeapon matches with I_strnicmp, first item wins: not `kar98k` +
  `kar98kscoped`; Pack-a-Punch e.g. `up<ref>`): the variant's name, its alt weapon's and the other weapon
  names it holds (szAltWeaponName, parentWeaponName, szDualWieldWeaponName...). Check per weapon what
  stage 1 covers by itself (sounds named by strings, localized names, effects named by effects) and what
  not (alias prefixes such as PhysPreset::sndAliasPrefix; projectile and impact effects of the wonder
  weapons; the HUD and menu icons Create a Class needs). The zone must hold no asset of a type without a
  default (DestructibleDef, ...) that a map also has (Com_Error "Attempting to override asset").
- **Stage 3, Create a Class: done (session 27)** (Status, Decisions session 27): the zone carries
  copies of mp/statsTable.csv, mp/attributesTable.csv and mp/weaponOptions.csv with the guns' rows
  (tools/dlc_weapons_*), their descriptions and multiplayer damage; the game loads it at startup after
  common_mp (flag 1024, `kb_dlcWeapons`, read at startup); Create a Class, custom classes, the weapon table
  and the bots have the guns; the Dual Wield option is gone from the attachment lists;
  make_portable.sh writes the zone into the game folder.
- **Left, optional** (ask the user before any of it; the game folder has stage 3, make_portable.sh at
  3fcbb7f4): Create a Class shows the WWII guns with their white HUD
  silhouettes where multiplayer's guns have colour pictures (`menu_mp_weapons_*`, none exist for the
  WWII guns: rendering new ones would be a project of its own); camo leaves the WWII guns as they are
  (their models have none of multiplayer's camo textures); they have no attachments.

The user's own gameplay changes (queued 2026-10-09 for after the performance work) are done:
stages A, B and C (sessions 16-18) cover all 10. **Next: the Metal leftovers in 0** (the user's
choice at the end of session 18, from the optional work: those, and the items 1-9 below: graphics
leftovers, the FX freeze seen once, sound leftovers, the match-end crash seen once, the main
menu's PLAY, menus by hand, soaks, rendering fidelity against Windows, later wishes). Session 19
did the first part of leftover 1 and left its verification open; the user's answers for the rest
are in 0's list.

- The list: 1 lean exactly as OpenMoHAA's (speed, angle, everything); 2 no grenades of any
  kind; 3 no death cam; 4 permanent god mode for the player; 5 no music or announcer; 6 no
  last stand ("coward's way out"); 7 a static, small crosshair (the smallest the game already
  uses), the same for every weapon; 8 no aiming down the sights; 9 no weapon holstering while
  sprinting (the player can shoot while running); 10 unlimited ammo, but an empty magazine
  still needs a reload (infinite reserve, not infinite clip).
- The user's answers (session 16): grenades means lethal, tactical, equipment and grenade
  launchers (underbarrel, China Lake, the explosive crossbow); bots lose grenades and last
  stand too, but god mode, the infinite reserve, no ADS and sprint fire are the human player's
  only; each change is a dvar that defaults on (`kb_*`, archived); "death cam" means the
  killcam, the final killcam and the ~2 s death view.
- **Stage A (done, session 16)**: 2, 3, 4, 5, 6, 10 (Decisions session 16).
- **Stage B (done, session 17)**: 7, 8, 9 (Decisions session 17). The user's answers: the
  hip-fire spread is locked to the crosshair too; shotguns keep their pellet spread.
- **Stage C (done, session 18)**: 1, the lean (Decisions session 18). The user's answers: AA's
  lean (MoHAA 1.11), not Spearhead's or Breakthrough's; AA's lengths scaled to BO's soldier; the
  same 40 degrees prone (AA has no prone).

0. **Metal backend leftovers** (Metal backend above; built in session 13, the default
   since session 15). The rules agreed with the user (2026-10-09) still hold for any work
   on it, and session 13's deviations are in its Decisions (DISCARD renames instead of
   a per-frame ring, by the spec; the runtime compiler in place of `xcrun metal`):
   - Spec first: instrument the GL layer to list every D3D9 method, render state and
     value, format, lock flag, primitive type and shader opcode the game uses (menus
     and several maps); implement that list, and make anything outside it fail loudly.
   - No silent fallbacks or self-healing (the GL layer's ignored states, trusted link
     failures, builtin-shader fallback and skipped draws hid bugs): log once, treat as
     a bug. Develop with `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1`; any
     validation message is a bug.
   - Use what Metal shares with D3D: 0..1 depth, top-left origins for viewport,
     scissor and textures (no Y flips; the D3D9 half-pixel offset stays); unified
     memory (Lock returns a pointer into the MTLBuffer); command buffers keep the
     resources they use alive (no name-reuse invalidation).
   - One sync model: Metal only on the render thread (window and layer once on the
     main thread), 2-3 frames in flight behind one semaphore, per-frame transient
     data (DISCARD locks, constants) from one ring per frame, CPU waits only there and
     for readbacks. Automatic hazard tracking until a profile says otherwise.
   - One shader translator: a shared D3D9 bytecode parser with GLSL and MSL emitters;
     dump every shader the game loads and translate + compile them all in a test tool.
   - Plain D3D state structs with dirty bits, translated to Metal in one place per
     draw; one cache each for pipeline, depth-stencil and sampler states, keyed by
     hashes. No optimization without a profile.
   - Same binary, backend chosen at startup by an environment variable; GL stays the
     macOS fallback until Metal matches it, and Linux's renderer.
   - Stages that each run the game (clear screen, 2D menus, world, lighting and
     shadows, post effects and MSAA), each done when tools/bench_views.py screenshots
     match GL on every map's overview and mp_nuked's views (tools/cmp_screens.py, with
     each map's GL-vs-GL noise as the tolerance; compare with the closer of two GL
     runs), the validation layer and ASan are quiet and fps is measured. Objective-C++
     (.mm) with ARC, no new dependencies; aim for ~3,000-4,000 lines.
   - Hand-offs (the user's standing instruction): at each stage's end, or sooner after
     more than one compaction, update HANDOFF.md, commit (pre-authorized at stage ends
     and hand-offs once the checks pass, or as work in progress on an early hand-off,
     saying so; CLAUDE.md's conventions; never push; mid-stage commits need the user's
     OK), create a task card with a self-contained brief for the next stage that
     repeats these points, the rules, the harness (bench_views.py, cmp_screens.py,
     sample_report.py, gpu_trace.py) and the user's constraints, then stop.
   Leftovers (session 15 did the default, the display rate and the spawn stall: Decisions
   session 15; session 19 the compile queue and hardware shadow compares: Decisions session 19).
   The user's answers at the end of session 19 (to plain questions; their numbers there):
   - **Done (session 20): session 19's commit verified** on every map (Decisions session 20),
     make_portable.sh rerun.
   - Load time with a cold system shader cache (common: it keeps about one map's batches):
     the map's first frame waits 1.0-1.2 s on mp_nuked with bots (was 2.6-3.2 s), the startup
     ~0.2 s, for batches still compiling (~165 M instructions, 10-17 ms of a P core, a shader;
     ~3,200 a map, ~12% of them drawn). **The user chose Apple's way** (end of session 19,
     after both were explained): every shader the game has (8,864, plus the per-sample
     variants transparency AA uses, so no setting compiles at run time) compiled ahead of time
     into one .metallib in the game folder (`xcrun metal`/`metallib`; the Metal Toolchain
     download: confirm its size and ask once before downloading). make_portable.sh builds it
     from the game's own shaders and rebuilds it by itself whenever the shader translation
     changes (a stamp of it), and stops with the download command when the toolchain is
     missing (it is per Xcode version). The game loads it at startup; a function is keyed so
     a stale file can't be used for a changed translation, and a shader missing from it
     compiles at run time with a log line. AIR is chip-independent; the user needs only M4
     (no pre-M4 support). **Stage 2, in progress (session 20, work in progress)**: everything
     but the compile is built and checked (Decisions session 20): the shaders' names from a
     hash of their translation, the game's loader (`kb_shaders.metallib` in the game folder;
     what isn't in it compiles at run time with a log line), the collection (16,140 shaders,
     31,915 functions with the per-sample variants) and `tools/make_shaderlib.sh`, which
     make_portable.sh now runs. **Left**: the Metal Toolchain download (`xcodebuild
     -downloadComponent MetalToolchain`; `-showComponent` says "uninstalled", Xcode 27.0
     27A266a; its size shows only once the download starts, ~700 MB for Xcode 26's): the
     user's one final yes first (session 20 ran unattended, the user asleep, and didn't
     download). Then rerun make_portable.sh on the game folder (the collection runs once,
     ~10 minutes in a window, then the compile), measure the cold map loads and the startup
     (cold runs as in Decisions session 19; the stage-3 pipeline cache files refill once, the
     functions' identity changes), screenshots, validation and ASan with the library, the
     library's size, and commit; the game folder's copy is still the leftover-4 build
     (61cfe958) until then. Not the shader list (a session-19 prototype of it gave 0.36 s). MTLBinaryArchive keeps only backend binaries, not the MSL
     front end. `KB_MTL_BATCH=<n>` gives a cold cache to measure with, `KB_MTL_SHADERLOG` the
     timeline. Also every load waits 0.2-0.4 s for a missing asset, `python_speed_snub_mp`
     (`Error: Waited ... msec for missing asset`), in every build: not looked at.
   - **Done (session 20): the warm-cache hitches**, the self-filling pipeline cache (Decisions
     session 20). The plan, as the user agreed it in session 19: a Metal binary
     archive (MTLBinaryArchive) the game fills by itself as it plays, in the game folder
     (`players/`, under fs_h, so test runs with a scratch fs_h never touch the user's), saved
     after a map and at quit, so later runs reuse specialized functions
     (`MTLFunctionDescriptor.binaryArchives`) and pipelines. Not pre-built: the user chose
     the self-filling file (no recording runs, no per-chip builds); it holds M4 code (the
     user needs only M4), works on another M4 Mac with the same macOS, and refills itself,
     with a log line, after a macOS update or on another chip.
     Today a few frames a match take 25-65 ms when 20-54 pipelines are first used at once:
     ~1.2 ms of specialization each (a request to the compiler service) plus 3-20 ms of backend
     compile when the shared system cache has dropped it, which varies run to run. Measure that
     the archive removes it before keeping it (does a lookup still go to the compiler service?).
     GL's worst frame: 39 ms.
   - **Without vsync ~35% of the frames rendered are not shown: the user wants it looked at**
     (session 19; it is input latency): three drawables, the presenter thread waits in
     `nextDrawable`; presented/s stays above the display's 240. **Not measured yet (session 20)**:
     measuring it needs the display showing frames, and from ~03:30 the screen was locked (the
     user asleep; `CGSessionCopyCurrentDictionary` says CGSSessionScreenIsLocked; KB_FPS
     "120 presented/s, 0 shown/s", presentedTime 0). Earlier that night, unlocked, the
     tearing-flip state showed fewer drawables than frames: mp_nuked 490 fps -> 426 presented,
     735 -> 577, 800 -> 594 (all presented frames shown). The plan for it (Decisions session 20).
   - **Done (session 20): CPU at view A** (Decisions session 20): profiled at the user's
     settings, the model lighting volume's relock and upload were 10% of the render thread (now
     2.4%), +6% fps at view A and the overview; the occlusion-query polling is <0.1% there now.
   - The GL layer's deviations from D3D9 (Decisions session 13) stay for Linux: no
     mipmaps, raw-depth shadow maps, no stencil or separate alpha, dropped worker-thread
     vertex uploads (particle clouds), relative reads that miss `def`'d constants.
   - `xcrun metal` (offline MSL compile) needs Apple's Metal Toolchain download; not
     installed (it was not asked for). tools/shader_test.sh compiles through the
     runtime compiler instead.

Done in session 11 (see Status and Decisions): weapons, footsteps and impacts are
heard (an xWMA decoder), and script vehicles (the bots' RC-XD) no longer crash or
go NaN. Session 10: sound output and texture streaming. The user's `codbo/` has the
build (`tools/make_portable.sh`: rerun after each build).

1. **Graphics leftovers**: try the menu cursor by hand in fullscreen at a smaller
   resolution and in a window on the Retina display (the mapping is computed:
   points -> pixels -> back buffer); the options menu's Apply with AA, resolution
   and fullscreen together (the console's `vid_restart` path is tested). Changing
   `r_monitor` takes effect at the next start (a reset keeps the window's
   display). A fresh profile starts at 1024x768 with 4x AA: `configure_mp.csv`'s
   row for unknown GPUs, as on Windows (1024x768 used to be missing from the list).
2. **Found while testing session 11, not fixed** (the user was offered it as a
   separate task; the other one, the texture streamer's overflow, is fixed: Status).
   Once, under ASan ~1 minute into mp_mountain (before the vehicle NaN fix), the
   client froze: the main thread spun in `FX_SortEffects`' inlined
   `FX_WaitBeginIteratingOverEffects_Exclusive` (`FxSystemShared::iteratorCount`,
   offset 0x830, stayed non-zero with no other thread iterating). Not seen in ~25
   minutes of matches since; maybe effects bolted to NaN vehicle bones. Every
   begin/decrement pair in src/EffectsCore is balanced; suspects: a counter leak,
   the plain `iteratorCount = 0` stores without an arm64 release fence, and
   `FX_RunGarbageCollectionAndPrioritySort`'s `effect[1].def & 0x3FFF` (an alias of
   the effect's reference count, rule 9; sizeof(FxEffect) differs on 64-bit).
   Session 22: every Begin has an End on every path; the plain stores could not leak a
   count (only release ordering was missing: fenced, Decisions session 22); the alias is
   fine (FxEffect is 192 bytes on both ABIs, `effect[1]` is `atomics.status`). The audit's
   remaining suspect, not checked: `FX_ResetSystem` and the `FX_InitSystem`/`FX_ShutdownSystem`
   memsets (`CG_MapRestart`) zero the count while an iteration is still running (the ppu job
   is not in `R_WaitFrontendWorkerCmds`); its decrement then leaves -1, which blocks every
   Begin. A stuck value of 0xFFFFFFFF would confirm it, a positive one a leaked Begin.
3. **Sound leftovers**: (a) streamed ambient loops churn: in a 110 s match on
   mp_mountain the 10 stream voices started 7675 times (`amb_wind_edge_r` 1704
   times) and ~33000 loaded-sound requests found no free voice (reason: the engine's
   priority replacement in `SND_FindFreeVoice`, all ambient); each stream start
   opens the file again. Compare with Windows before changing anything. (b) The
   weapons' `*_bass_swt` and `wpn_grenade_explode_sub` aliases are LFE sweeteners:
   the engine's stereo speaker map sends them nowhere (as it would on Windows with
   stereo speakers). (c) `SND_PatchValue` ignores patches of types NORM_BYTE
   (`occlusion_level`, ~350 a map, coded as value/65535 of the byte), CENTS (pitch)
   and ENUM_BITS: the decompiled switch has no case, so the original may not have
   applied them either. (d) One output device, the system default (OpenAL Soft
   follows it); `sd_xa2_device_name` could list OpenAL's devices. (e) Stereo only:
   the engine handles 6 and 8 speakers, the mastering voice is 2 channels. (f) Bink
   video sound (stubbed). (g) WMA Pro (0x162) has no decoder; no game asset seen uses it.
4. **Match-end crash** seen once in session 8 (Open items: `Demo_WritePlayerStates`
   on the server thread after the demo client was freed twice). Soak match ends
   with `KB_CMDS` and `scr_tdm_timelimit 1` to reproduce it.
5. The main menu's PLAY (Find Match, Private Match, Combat Training) depends on
   the removed online service; the launcher starts matches against bots instead.
   Combat Training proper (`xblive_basictraining`, its own rank) is not wired.
6. (Session 28: `kb_mouse`/`kb_key` click through menus now, CLAUDE.md; Create a Class's lists were
   fixed that way, Decisions session 28.) Drive the main-menu menus by hand (keyboard/mouse) beyond the screens
   `KB_MENU_CMDS` reached (`cac_main`, `cac_weapon`, killstreaks): attachment/
   camo/reticle pickers, emblem editor, clan tag, barracks, combat record,
   after-action report; combat training (`xblive_basictraining`, its own buffer
   and rank, not maxed) is untested. To show a prestige other than 15,
   `LiveStorage_SetTopRank` is the place (a dvar would do).
7. Keep soaking with `KB_CMDS` (longer matches, other gametypes) and the ASan
   client, at the main menu too; each fix of this kind so far came from a run.
8. Rendering fidelity: compare against a Windows screenshot (shadows, reflections,
   gamma). `vFace`, `vPos` and the half-pixel offset follow D3D9 now; textures
   stream their high mips since session 10. Session 24: mipmapped reflection probes and
   anisotropic filtering (Decisions session 24), checked only against a 2010 console-video
   frame the user found (Nuketown backyard, M16). Left: the brightness slider (`r_gamma`) does
   nothing on either backend (no `D3DCAPS2_FULLSCREENGAMMA`, `SetGammaRamp` empty; on Windows it
   is the fullscreen gamma ramp); the GL layer's caps are still all zero but the D3D9 basics.
9. Later: wire compatibility with a Windows/Linux server, an .app bundle (a
   double-clickable app; today `codbo/blackops` opens in Terminal), controller
   support.

Reading game data: the scripts, menus and string tables (e.g. mp/statsTable.csv)
are inside the fastfiles; session 7-8 extracted them with throwaway scripts that
are not in the repo (decompress the .ff's zlib stream, then scan for RawFile and
StringTable assets; table strings shared with earlier assets are references
that weren't resolved). Item indices are listed in CLAUDE.md.

## Open items found but not fixed

Seen in session 32 (kb_mohaaDamage): Create a Class's damage and range bars still show Black Ops' numbers
(mp/attributesTable.csv); the bots still choose their fighting distances by Black Ops' damage ranges
(sv_bot_mp.cpp reads fMinDamageRange/fMaxDamageRange); the scripts' `getWeaponDamage`-style builtins return Black
Ops' values. None of these was asked for.

Seen in session 31: in one test with `kb_noCountdown 0` the player, standing still, was 1200 units away 16 s later
(a respawn? the team balance of `teamchangeGracePeriod` moving the only human?); not looked at. The
"CG_SetWeaponHidePartBits: No such bone tag" errors (tag_brass2, tag_stock, tag_extended_clip, multiplayer guns'
hideTags their models lack) print at each switch to those guns; only `developer` shows them on screen.

Seen in session 30 (the body fix, Decisions session 30): the decompile's `(int)(x + 9.313225746154785e-10)`
is the original's rounding conversion, and only the network coordinates round now; the ~200 other sites
(dvar and HUD colours, `cl_input_mp.cpp`'s usercmd moves and selected location, the `-86` net field's writer
and its equality test, the timescale's msec, demo UI) truncate, at most 1 off (255 * 0.5 gives 127, not 128).
Not seen to matter. Also: only 4 bodies at a time (the 5th removes the oldest), as in the original.

Seen in session 28 (the sharedUiInfo rework, Decisions session 28): other fields still serve two meanings.
UI_FeederSelection's mods list (feeder 9) stores the picked mod in `modCount`; the camo/option index
(feeder 60, the UI viewer's camo, equipClassCurrentOption) and the combat record's pivot share
`sortedItemPivot`; the clan tag feature to buy and the combat record's item count share `clanTagFeature`.
Different screens, so no clash was seen. Also: every Create-a-Class screen shows a loading spinner in its
middle (the UI viewer's character?); not looked at.

Seen in session 27: a client that joins a separate dedicated server (two processes, no online service)
and picks a custom class spawns with only the knife, whatever the class holds (Custom 1 with a DLC gun,
Custom 2 with multiplayer's); a default class works. Most likely the server has no stats for a remote
player (`getdstat` reads zeroes: weapon 0 is weapon_null); a listen server's own player is fine. Not
looked into; the launcher's matches are listen servers.

Seen in session 25: the `screenshot` command writes the 1024x768 back buffer into the top-left corner of
a 1920x1080 TGA (the rest black), at the scratch profile's default settings; not looked at.

Seen in session 22 (the worker audit, Decisions session 22; races on x86 too, so not ordering bugs):
- Model lighting: `R_AllocModelLighting` runs from fx_draw (glass, `ShardGroup::GenerateVerts`,
  the port's "LWSS HACK" in place of `R_AllocModelLighting_PrimaryLight`) while r_add_sceneent's
  batches call it, with plain read-modify-writes of `pixelFreeRover`/`currPixelFreeBits`
  (two models can get one entry); glass's r_model_lighting batches are never waited for (its wait
  is commented out; `R_WaitFrontendWorkerCmds` has no r_model_lighting), so one can still write
  `frontEndDataOut` patches across `R_ToggleSmpFrame`. Reported by the audit, not reproduced.
- Effects: `FX_DelRefToEffect` tests the count, then decrements (two drops can both miss the
  last); trail elements are linked before they are filled in (`FX_SpawnTrailElem_NoCull`); the
  drawers walk element lists while the main thread kills or relinks them (the "Loop in element
  list" check); `g_fxMarkPoints`' four rotating buffers are shared by the update workers and the
  main thread.
- Physics: a wheel trace on a worker can create a dynent's physics object
  (`GetTraceResultsRigidBody`) without a lock; the GJK cache inserts after a read-unlocked miss
  without re-checking (each body is handled by one thread, so the same pair is never looked up
  twice at once).
- `imageTouchBits` are OR'ed plainly from the main, render and fx_draw threads (a lost bit costs a
  frame of streaming priority); `SND_FreePlayback` runs on the sound worker without
  `CRITSECT_SOUND_PLAYBACK_ALLOC`; `GlassRenderer::Update` returns with the lock held if
  `CG_GetLocalClientGlobals` gives null (it never does).
- Not ordering, found in passing: `FxBoltAndSortOrder`'s bitfield widths. Fixed in session 23
  (Decisions session 23).

Seen in session 21 (the physics crash, Decisions session 21):
- Only the broad phase's environment queries were checked for objects published under a lock
  and read without it on the job-queue workers; the GJK cache, the auto rigid bodies and the
  bpei database's own map take read locks (full barriers). The other workers were audited in
  session 22.
- A body that wakes a dynent (auto-activation) creates its physics object on the worker
  (`DynEntCl_CreatePhysObj`, which calls `CScr_NotifyNum`): safe only because the main thread
  waits in the physics flush meanwhile, as in the original.
- `phys_memory_heap::fast_allocate` (the contact manifold's 16 KB stack buffer) is bounds-checked
  only by `iassert`, which RelWithDebInfo compiles out, like every `iassert`.
- The natural trigger, an object whose box spans the map (session 20's bench run: a 12-body
  ragdoll flung across 15,000 units, "about to freakout"), still happens; it no longer crashes.

Seen in session 14 (the split-locals scan, Decisions session 14):
- `CG_SP_trigger_multiple`/`_radius` (cg_spawn.cpp) pass a `char` parameter as `int *`
  to `CG_InitTriggerWait`; unreachable (the flag is always 64, which takes the other branch).
- `UI_FeederDoubleClickDlcMaps` reads a dvar name at `mapInfo::splitscreen` (an `int`;
  IDA's struct is off there); PC has no DLC double-click.
- `G_RunMissileInternal`'s `missileDebugAttractors` path reassigns the missile `ent` to the
  attractor's entity before `G_RunThink(ent)` (decompile or original; debug dvar only).
- `CL_CompositeDrawEmblemPhysical`'s matrix setup (rotation, scale through `pow`, flip) is a
  reconstruction nobody checked against IDA; only its UV array was fixed.

Seen in session 13:
- The GL layer's deviations from D3D9 (Decisions session 13), kept for Linux.

Seen in session 11 (details in Next steps 2 and 3):
- One client freeze in `FX_SortEffects` waiting for the effects iterator count.
- "weapon rc_car_weapon_mp not found in weapon table" and "Couldn't find weapon
  parent 'killstreak_rcbomb' for weapon 'rcbomb_mp' in weaponOptions.csv" at map
  load; script vehicles still spawn (most likely the bots' RC-XD; not identified).

Seen in session 10:
- SIGTERM doesn't stop the game (session 9's build neither): SDL turns it into a
  quit event that nothing handles. `quit` exits; test scripts kill with SIGALRM
  (`perl -e 'alarm ...'`) or SIGKILL.
- "R_Cinematic_Init: Unable to initialize sound" at startup (Bink is stubbed).
- Sound patches of three field types are dropped (Next steps 2b).

Seen in session 2, unverified against a Windows run:
- "Could not load xanim pb_huey_*", "xmodel t5_veh_civ_tiara", "weapon X not found
  in weapon table" (killstreak/utility weapons), bg_shock dvars set to 0.

Seen in session 3:
- `ui_mp/after_action_report.menu` lines 1989/2087: "Expected 4 params to function
  StringTableLookup, found 3" every frame, and "Could not load stringtable
  PlayerStatsList"; only these two expressions, so probably the newer menu data.
- "Unknown command" xsigninlive, updatevehiclebindings, resetCustomGametype;
  "missing TECHNIQUE_BUILD_FLOAT_Z" spam for some vehicle decal materials; several
  missing code materials/images ($floatz_donotremove, bloom_mip3_blur, redact*).
- Glass shard memory (`shardMemorySize` from the asset) also holds the 40-byte
  64-bit `Allocator::Memory` headers (20 on i386): fewer shards fit before it runs out.
- `Key_KeynumToString` (French/German digit keys) reads a table that followed
  `keynames_localized` in the original binary and is missing from the decompile.

Seen in session 4:
- `live_pcache` asserts `xuid != PCACHE_INVALID_XUID` at map load: gone since
  session 7 signs the local player in with an XUID.
- 50 decompiled structs whose i386 layout no longer matches IDA's `sizeof` comment
  (e.g. cgs_t 12712 vs 12708, trace_t 64 vs 56, pmove_t, actor_s, client_t,
  sharedUiInfo_t): any raw offset into them is wrong on every build. The
  cg_ents_mp.cpp corpse lookup was one (fixed).

Seen in session 9:
- Started while the display sleeps (e.g. from a remote shell), with vsync on, the
  game hangs for good in its first buffer swap: SDL3 waits there for a
  CVDisplayLink tick that never comes, even after the display wakes (no display
  link thread). A display that sleeps during play is fine. Test runs wake the
  display first (`caffeinate -u`, then check `CGDisplayIsAsleep`) or use
  `r_vsync 0`.
- SDL (sdl2-compat over SDL3) reports a high-density window's drawable at 2x on
  a 1x display when another display is Retina (`SDL_GL_GetDrawableSize`
  3840x2160, GL surface 1920x1080). glcontext_sdl.cpp computes points x the
  display's density instead (SDL's DPI / 96 on macOS).
- `R_SetAlphaAntiAliasingState` decides from merged state bits whose blend op is
  the last blended draw's (Decisions); if that was the original's own behaviour,
  transparency AA barely ever ran on Windows either.
- The `screenshot` command in a window smaller than the monitor writes a
  monitor-sized TGA with the picture in a corner (R_GetFrontBufferData sizes its
  surface by the monitor in windowed mode; seen at 1280x720 on a 1920x1080 display).
- Apple's GL has no fractional sample shading: "dither (fast)" and "supersample
  (nice)" look and cost the same (every sample shaded).

Seen in session 8:
- One crash in ~10 match ends (listen server, scripted player, 4 bots): at the end
  of the match `G_FreeEntity: Player 0 is being freed.` without the usual
  `SV_RemoveDemoClient: democlient removed 0.` before it, then the asserts
  `g_utils_mp.cpp (2258) ed->r.inuse` and `g_spawn_mp.cpp (1067) ent->r.inuse`
  twice, then SIGSEGV on the server thread in `Demo_WritePlayerStates` <-
  `Demo_BuildDemoSnapshot` <- `SV_SendClientMessages`. Client 0 is the server's
  demo-recording client; it looks like it was freed twice and then recorded. The
  graphics options menu had just opened (`ui/options_graphics.cfg`), probably
  from a click in the window; opening it from the console at the match end didn't
  reproduce it. Not reproduced since.
- The user's `codbo/main` held junk from an Oct 8 run (sessions 3-4): a directory
  named by the bytes 0x01-0x2E (one 1 KB file in it) and empty files named 0x02 and
  0x03 (moved to the Trash in session 8). If such names reappear, a file path was
  built from garbage.
- The system's Metal shader cache (`$(getconf DARWIN_USER_CACHE_DIR)com.apple.metal`)
  is written by Apple's GL driver for any GL program, outside the game folder.

Seen in session 7:
- Contracts have no data offline (`LiveStorage_DoWeHaveContracts` is 0) though
  `level.contractsEnabled` follows `IsGlobalStatsServer`; nothing broke in tests.
- A fresh profile's first `ResetStats` runs `stats_init.cfg` with basic training
  on; `LiveStorage_ReadStats` sets `onlinegame` around it (else ValidateGameModes
  asserts hundreds of times).
- `SV_UpdatePersonalBestsForClient` stays stubbed (server-side "new item" flags
  and personal bests for remote clients); the client computes its own on disconnect.

Seen in session 6:
- `CL_Connect_f` connects only to LAN addresses (`Sys_IsLANAddress`: 10/8, 127/8,
  169.254/16, 172.16/12, 192.168/16, or the first three octets of a local interface);
  any other address silently stays `CA_DISCONNECTED` (upstream; every build).
- A remote client is "Unknown Soldier N" and every non-Steam client has the same
  SteamID (stubs_online.cpp); the server keys nothing on it today.
- "CG_SetWeaponHidePartBits: No such bone tag (tag_iron_sightlow) for weapon (m16_mp)":
  probably the data (the viewmodel lacks a tag the weapon file hides).

Pre-existing (wrong on every build):
- `actor_fields.cpp` `aifields` offsets don't match `actor_s` (e.g. "fovcosine"
  3860); the hard-coded `ofs != 604/5800/3472/548` checks too.
- `KeyValueToField` case 9 (FX) falls through into the Material cases.
- Undefined-behaviour stack tricks: `ParseConfigStringToStructMerged` `(&dest)[i]`;
  `BG_LoadWeaponVariantDefFile` `&pszBuffer`/`&sourceName`, `szBuffer[0x4000*i]`;
  `xanim_load_obj.cpp` 622/664 `*(&i + type)`; `xmodel_load_phys_collmap` 981-988.
- `cscr_parsetree.cpp` `debugger_buffer` returns the type slot, not a node.
- `phys_colgeom.cpp:1156` allocates `gjk_double_sphere_t` with alignment 0 (NULL).
- `rb_pixelcost` key leaves its upper 24 bits uninitialised.
- `Material_CompileShader` passes NULL constants to D3DXCompileShader.
- `ui_server.cpp:603` writes 1 over `serverStatusInfo.lines[30][7]` (kept as on
  32-bit; probably meant `serverStatus.refreshActive`, offsets don't confirm it).
- `g_pop_iter` (physics) is never initialised.

64-bit, deliberately left:
- Physics buffer (0x380000) and 16 KB transient blocks are unchanged while 64-bit
  objects are larger: e.g. a SAP axis list block holds ~1022 nodes instead of 2046.
- Code that relies on linear encoding (fine while the memory is hunk/zone/global):
  script `OP_EvalLocal*Cached` (`Encode(top) + 12`), vector reads
  `Decode(intValue + 4)`, destructibledef/fx_profile/fx_load_obj, r_xsurface_load_obj:1446.
- `rb_resource` CREATEVERTEXDECL/LOADINDEXBUFFER treat `resource` as native `T **`
  (no producer today). r_cinematic's skip/wait logic is decompiler garbage (dead
  while Bink is stubbed; radbase.h defines `__RAD32__`, a real Bink would need 64).
- `tools/audit_rawofs.py`'s remaining sites are reviewed: centity_s and the GfxModel
  surfaces keep the i386 layout; WeaponComponent, the rigid_body fields reached (m_last_position,
  m_mat, m_a_vel) and the other listed structs lay out the same on both ABIs; the rest are byte-wise
  string copies through a struct pointer.
- `mem_fixed.cpp` (HU_SCHEME_FIXED) keeps its header in pointer slots with i386
  sizes; nothing creates a hunk user with that scheme.

## Decisions (session 32)

**How the guns compare with MoHAA's** (the user's question: recoil and spread against MoHAA's SMGs and StG, and the
damage). Sources: the user's MoHAA install, `/Users/pstn/Documents/Games/moh` (OpenMoHAA; read only), whose
`main/pak7.pk3` (the 1.11 patch) holds the final `models/weapons/*.tik` (`unzip -p`; Pak0's are older), and
OpenMoHAA's code on GitHub (weapon.cpp, weaputils.cpp, sentient.cpp, cg_commands.cpp, cg_view.c; read with WebFetch,
not cloned). Multiplayer uses the `dm*` lines.
- **MoHAA's numbers** (dmbulletdamage, dmfiredelay, dmbulletspread min / max, dmfirespreadmult add falloff cap
  maxtime, viewkick): Thompson 25, 0.086 s, 40 / 50, 0.3 0.6 150 0.25, kick -1.58 a shot ("V", yaw x -0.16 of the
  pitch, recentre 0.9, caps 8.5 / 8.6); MP40 25, 0.086, 38 / 50, same mult, -1.65 ("V", yaw x +-0.12, recentre 1);
  StG 44 27, 0.086, 10 / 53, 0.475 0.5 200 0.8, -1.8 ("T", yaw -0.2..0.05, recentre 3.2, caps 12 / 0.2, scatter
  2.2); BAR 30, 0.12, 12 / 52; M1 Garand 48, 0.15; Kar98k 90, 1.1; Springfield and KAR98 sniper 106, 2.0; shotgun
  20 pellets of 17, 0.75; Colt 30; P38 35; silenced pistol 15. Player health 100.
- **How MoHAA uses them**: a shot goes to `dir * 4000 + right * grandom() * x + up * grandom() * y` (grandom is a
  Gaussian, sigma 1, so the spread is one sigma at 4000 units: 40 = 0.57 degrees), x/y lerped from min to max by
  velocity / sv_runspeed, times (1 + mult); mult gains `add` a shot, loses `falloff` a second, resets after
  `maxtime` without firing. The view kick adds to the camera angles, which the client sends as its eye angles
  (CL_EyeInfo -> the server's m_vViewAng), so it moves the aim; it recentres at kick x recentre degrees a second,
  clamped to 12-25. Damage has no distance falloff (only penetration); hit locations scale it (Sentient's
  m_fDamageMultipliers, every multiplayer game): head, helmet, neck 4, torso 1, pelvis 0.9, upper arms and legs
  0.8, lower 0.6, hands and feet 0.5 (realism mode, Spearhead's: head 5).
- **Black Ops here**: `kb_staticCrosshair` keeps a human player's spread at 0.4 degrees whatever they do (bots keep
  theirs); the view kick (BG_WeaponFireRecoil, cg_view_mp.cpp's kick integration) sets a pitch/yaw velocity a shot
  that the centre speed brakes, returns at 0.06 of the rate, clamps at 10 degrees, and goes into the usercmd angles
  (it moves the aim too). Simulated 30-round bursts (a throwaway script of both games' rules): climb after 10 / 30
  shots, MoHAA Thompson 4.9 / 7.4 (and 7 degrees left by the end), MP40 5.5 / 7.4, StG 5.0 / 5.6; BO MP5K 1.2 / 2.5
  (+-2 sideways), AK-47 0.8 / 1.5, Uzi 2.0 / 5.7, WWII Thompson 2.4 / 7.2, WWII MP40 0.5 / 0.5, WWII STG-44 7.5 /
  9.7. MoHAA's spread (90% circle) grows from 1.2 degrees (Thompson, first shot, standing) to 4 at the 10th and 10
  at the 30th; StG 0.3 -> 1.5 -> 4.2 standing, 1.6 -> 7.7 -> 21 running. Offered (the user's choice): MoHAA's spray
  and kick, MoHAA's damage, or both; they took the damage only.
- `listzone` now prints each weapon's fire type, bolt action, hip spread (stand/ducked/prone min-max, fire and move
  add, decay), ADS spread, move speed, and hip and ADS view kick and gun kick (db_save_tool.cpp).

**The Thompson held closer (`kb_gunPlacement`)**, the user's question whether the first-person Thompson is smaller than
the other guns (Status).
- **Measured** (temporary prints, removed): each gun's first-person and third-person model bounds (XModel mins/maxs,
  via listzone) and, in the game, where the view model's `tag_weapon` (the grip) and `tag_flash` (the muzzle) sit
  from the eye at idle (CG_DObjGetWorldTagPos in CG_AddViewWeapon, in refdef view axes). A first answer compared the
  bounds with the real guns' lengths ("built ~80% of real size"); the user asked whether that was seen or guessed,
  and it doesn't hold: view models often lack or fold the stock, Black Ops' own include hidden attachment parts, and
  the Thompson's view model is as high as its world model (length 0.75 of it, like BO's Kiparis 0.71, MAC-11 0.77,
  Spectre and PM63 0.79). What differs is the hold: grip forward/left/up from the eye, Thompson 14.2 / -4.6 / -5.8,
  MP40 12.2 / -3.7 / -4.5, STG-44 12.1 / -3.7 / -6.2, Type 100 12.3 / -3.1 / -5.0, MP5K 15.4, Uzi 15.7, Spectre
  11.1, Kiparis 12.9, AK74u 7.8, AK-47 8.3, M16 8.0, Galil 7.1 (forward). It comes from the gun's own animations
  (Treyarch's, unchanged by the port). The user's game hides the arms (`cg_drawArms 0`).
- **How**: `CG_KbGunPlacement` (cg_weapons.cpp), called in CG_CalculateWeaponMovement_ClientSpecific before
  LocalToWorld, adds a per-gun offset in the view model's local axes (forward, left, up), faded by
  (1 - fWeaponPosFrac) like cg_gun_fovcomp so sights would still line up with ADS on; matched by the variant name's
  first token. The Thompson's: (-2.0, 0.9, 1.3) puts its grip on the MP40's (measured 12.19 / -3.56 / -4.68 against
  12.19 / -3.74 / -4.43); the user saw it and asked to move it "back just a little bit so i dont see its back side";
  three variants tried with `cg_gun_x`/`cg_gun_z` (same local axes): 1 closer; 1 closer and 0.5 lower; 0.5 closer and
  1 lower; the user picked the last: (-2.5, 0.9, 0.3). Black Ops' own SMGs were shown to the user (most are short
  guns held far out whose back ends show too); none changed. Only the drawn first-person gun moves: aim, shots and
  third person are unchanged.
- **Checked**: screenshots (the chosen place matches the cg_gun preview), the ASan client holding, firing,
  reloading and sprinting with the Thompson and toggling the dvar: no report. check_file.py natively and `--i386`,
  audit_splitlocals.py 0. A test window ran once while the user was playing (their game had started; pgrep was
  printed but not acted on): check before every run, and wait (a background loop on pgrep) while they play.

**WWII guns' walking motion (`kb_wwiiGunMovement`)**, the user's question why the Thompson drops when they walk (Status).
- **The data** (a temporary listzone print, removed): BG_CalculateWeaponMovement_Base_Internal_Movement moves the
  first-person gun by `vStandMove` / `vDuckedMove` and turns it by `vStandRot` / `vDuckedRot` (x speed / max speed).
  All 1600+ Black Ops bullet-weapon variants: move 0 0 0, rot 2 0 -2, standing and crouched (prone differs per gun,
  large crawl offsets like -160 3 -120 / 0 300 -300). The WWII guns: Thompson move 0 0 -2.5 (crouched -2), STG-44
  -3.25 with rot -0.3 0 0, BAR, Garand, Carbine, Gewehr 43, Kar98k, Arisaka -2, Scoped Kar98k -1.2, FG42 -1.6, Type
  100 -1.5, the shotguns 0 0.7 -1, no rot; their prone values are crawl poses of the same kind as Black Ops'. The MP40
  (Black Ops' model) is already 0 0 0 / 2 0 -2.
- **How**: in that function, for the 15 WWII guns by name (BG_KbWwiiGun, bg_weapons_view.cpp; not the MP40), standing
  and crouched use 0 0 0 / 2 0 -2; prone untouched. The server runs the same function (G_ClientThink, for ADS gun
  angles); the dvar is a BG one (bg_misc.cpp), like kb_staticCrosshair.
- **Checked** (temporary grip-height log, removed): Thompson walking at full speed, grip height -7.9 with the dvar off
  (-5.8 standing: a 2-unit drop), -5.7 with it on (-5.7 standing); screenshots sent to the user. The ASan client
  walking and crouch-walking with the Thompson, STG-44, Kar98k and MP5K, the dvar toggled: no report. check_file.py
  natively and `--i386`, audit_splitlocals.py 0.

**Classes 1-5 only (`kb_allClassGuns`) and sniper scopes (`kb_sniperScope`)**, the user's requests (Status).
- **Classes**: kbGiveAllClassGuns (the _class.gsc patch in cscr_parser.cpp) loops over custom classes 0-4 instead of
  0-9; giveLoadout gives the chosen class first, so a chosen class 6-10 (`prestige1`-`prestige5` in changeclass) or a
  default class comes on top. Checked (temporary print in PlayerCmd_giveWeapon, removed): classes 1-5 given AK74u,
  MP5K, AK-47, L96A1, FAMAS with statWriteDDL, class 7 picked (`changeclass prestige2,0`; `custom7` isn't a class and
  leaves the player unspawned): its own loadout, then exactly those five primaries and their secondaries. Then the
  user dropped the secondaries of 1-5: the loop gives primaries only. Checked the same way: class 7 gave its knife,
  M1911 and Olympia, then the five primaries, no other pistol; class 2 (after `kill`) its own loadout and the other
  four primaries (its MP5K not twice).
  (statWriteDDL to `customclass6`-`10` did nothing: the DDL names classes 6-10 otherwise; not looked up.)
- **Scopes**: BG_KbNoAds (bg_weapons.cpp) no longer blocks ADS while the held weapon (ps->weapon) is one of the
  scoped sniper rifles, by the name's first token (l96a1, wa2000, dragunov, psg1, scopedkar98k; any attachment).
  The static crosshair already draws only at fWeaponPosFrac 0, so the scope overlay shows alone. Checked (temporary
  print of fWeaponPosFrac, removed): L96A1, WA2000 (hold and toggle) and Scoped Kar98k reach 1.00; AK-47 and the iron
  Kar98k stay 0.00; screenshots of the three scopes and the AK-47 sent to the user. A toggle press made right after a
  `take all`/`give` swap in a test can leave `+toggleads_throw` out of step (the Scoped Kar98k didn't scope in one run,
  did with `+speed_throw` and in a fresh run). The ASan client (`kb_godMode 0`, class 7, the L96A1, PSG1 with hold
  breath and Dragunov scoped and fired, a death and respawn): no report. check_file.py natively and `--i386`,
  audit_splitlocals.py 0, gen_db_save.py `--check`.

**The launcher's signing, again** (tools/launcher/build.sh): installing f91de0c8 stopped at `codesign` ("resource fork,
Finder information, or similar detritus not allowed") although build.sh deleted the Finder info just before signing
(session 29's fix): the File Provider over ~/Documents put `com.apple.FinderInfo` back on the new bundle in between
(three installs earlier that day had passed). The bundle is now put together and signed in the script's temporary
folder (outside the File Provider) and copied into the game folder with `ditto`; the tag that comes back afterwards
doesn't break `codesign --verify` or launching (checked: the installed app verifies; a renamed copy opened and ran).

**No waiting at a round's or match's end (`kb_noRoundEndWait`)**, the user's request (Status).
- **The waits** (_globallogic.gsc, dumped with `listzone - rawfile`): every round-end display goes through
  `roundEndWait( delay, matchBonus )`: displayRoundEnd (level.roundEndDelay, 5 s), displayRoundSwitch
  (level.halftimeRoundEndDelay) and displayGameEnd (level.postRoundTime, 7 s, the match bonus notified halfway),
  each also waiting for players' notifies (`doingNotify`) in 0.5 s steps; then, after a match on PC, `wait
  scr_show_unlock_wait` (0.1 s) and `wait scr_intermission_time` (30 s, the scoreboard) before `exitLevel( false )`.
- **How** (two scrKbPatches in cscr_parser.cpp): `roundEndWait` is wrapped (the original renamed kbRoundEndWait) so
  that with the dvar on it notifies `give_match_bonus` (when it would) and `round_end_done` at once and returns; the
  scoreboard's wait is skipped. The final killcam was already gone (kb_noDeathCam). The outcome texts start but the
  map change or map_restart follows within the frame.
- **Checked** (a timestamping runner on the release client's output; scratch profiles): TDM, 1-minute limit, 2 test
  bots: the match ended at 67.8 s (the after-action-report menu's load, whose expression errors are the menu file's
  own), `map_rotate` at 67.9 s, the next map's game at 72.4 s. S&D, 15 s rounds, round limit 2, a side switch each
  round: round 2 began 0.07 s after round 1 ended (screenshots: the player in round 1, then at round 2's spawn, no
  outcome screen), `map_rotate` right at round 2's end, three maps in a row. The ASan client (S&D, 30 s rounds, 3 test
  bots, `kb_godMode 0`): 3 round restarts and 2 map rotations, no report. check_file.py natively and `--i386`.

**Gun damage setting (`kb_damagePercent`)**, the user's request (Status).
- **How**: g_combat_mp.cpp's gun table (`g_kbGuns`, by the name's first token as before) now lists every infantry gun
  with its kb_mohaaDamage value (0 for the shotguns, which keep theirs) and whether it is a sniper rifle (Create a
  Class's sniper group). `G_KbScaleGunDamage`, called in Bullet_Process after the MoHAA substitution and the walls'
  share, returns `(damage * pct + 50) / 100` (whole numbers: 25 at 110% is 28, a float 0.01 gave 27) and at least 1,
  for every gun in the table but the snipers; turrets, the Death Machine and vehicle guns aren't in it. First built
  for 1-1000% (launcher 10-500); the user wants it for lowering only: the dvar's range is 1-100, the function returns
  the damage unchanged at 100 or more (a `+set` on the command line or a config line is set before the game registers
  the dvar's range: a dedicated server with `+set kb_damagePercent 150` showed 150 until a map loaded, then 100), and
  the launcher's Stepper is 5...100 step 5 and clamps what it passes. Read per hit,
  so the console value works mid-match. The launcher (tools/launcher/Launcher.swift): `@AppStorage("damagePercent")`,
  a "Guns" section with a Stepper and a one-line footer (two lines crowd objective modes), passed as `+set kb_damagePercent <n>` with
  cg_drawArms at every start (Play and Main Menu); the window is 720 high (690 clipped the last row in objective modes,
  whose bots section has a footer).
- **Checked**: a scratch game folder (links to main, zone, lib; build_macos's blackops; a windowed config) with the
  launcher built there, re-identified (`local.kisakblack.launcher.test`) and its defaults set to 120% and TDM, run with
  KB_LAUNCHER_TEST: the game's command line had `+set kb_damagePercent 120`; a temporary per-hit log (removed) showed
  SMGs 30 (head 120), assault rifles 32 (head 128), HK21/RPK 36 (head 144), M14 and Carbine 58 (head 232), the
  Trench Gun and double-barrel 48 a pellet (their 40), the Scoped Kar98k unchanged 106 (head 424). The launcher's
  window in TDM and in Domination (screenshots). The ASan client (7 managed bots, `kb_godMode 0`, 150%, then 30% and
  500% mid-match): no report. check_file.py natively and `--i386`, audit_splitlocals.py 0, gen_db_save.py `--check`.
  The test launcher's defaults domain was deleted afterwards. After the 100% cap: the test launcher at 70% passed
  `+set kb_damagePercent 70`, its window in Domination fits (screenshot).

**The launcher's icon**, the user's request (Status).
- **Official images offered first**: the game folder has no Windows program (so no Black Ops app icon). Menu art is in
  main/*.iwd as `images/<name>.iwi` (the UI zones only name them; their GfxImage has no pixels in tool mode): new
  tools/iwi_to_png.py (the IWI header, mips smallest first so the full image is the file's last bytes; formats 1-5 and
  DXT1/3/5, as r_image_load_obj.cpp's Image_LoadFromData) made a numbered sheet for the user: menu_mp_background_logo
  (the white "BLACK OPS" text), logo_small_cod5 (despite the name, "CALL OF DUTY BLACK OPS"), the faction logos
  menu_mp_lobby_ops/spets/sog/nva/op40/tropas, playlist_prestige, rank_comm_128, rank_gen1_128, rank_prestige_1..15_128.
  The `em_*_full` emblem layers came out blank (not looked into).
- **The user's pick**: SteamGridDB's Call of Duty: Black Ops icon 81254 by BELWOUAKES (the cover art, square), 256x256
  PNG, https://cdn2.steamgriddb.com/icon/285d6d711add325920ca8caff4d8020f/32/256x256.png (downloaded at the user's
  word; the largest size the user found). The built-in browser was refused steamgriddb.com's pages; the user copied the
  image address. It is in build_macos/launcher_icon.png only (game art: not in git); to rebuild elsewhere, download it
  there again.
- **The shape** (asked "what shape should it be?"): Apple's app icon grid, a 1024 canvas with an 824-pixel rounded
  square of continuous corners (SwiftUI's `Path(roundedRect:cornerRadius:style: .continuous)`, radius 185.4) 100 pixels
  in, a soft shadow (macOS 26 puts icons of other shapes on a gray square). make_icon.swift draws every .icns size from
  the picture (cropped to its centre square) and runs `iconutil`; build.sh compiles it (without `-parse-as-library`: it
  has top-level code) when given a picture, puts AppIcon.icns in Contents/Resources and adds CFBundleIconFile.
- **Checked**: a launcher built into a scratch folder with the picture: Resources/AppIcon.icns with all ten sizes,
  CFBundleIconFile AppIcon, the signature verifies, and `NSWorkspace.iconForFile` (what Finder and the Dock show)
  returns the cover in the rounded square, not the gray one. The 256 picture is upscaled for the 512 and 1024 sizes
  (slightly soft only in Finder's largest views). The picture kept in the app: a scratch build with the picture, then a rebuild without
  one: AppIcon.icns, launcher_icon.png (the same bytes) and CFBundleIconFile kept, the signature verifies.

**MoHAA damage (`kb_mohaaDamage`)**, the user's request (Status). The user's answers (asked in plain words): each Black
Ops gun gets its closest MoHAA gun's damage (SMGs the Thompson's, assault rifles the StG's, machine guns the BAR's,
semi-automatic rifles the Garand's, iron-sighted bolt-actions the Kar98k's, pistols the Colt's); of the snipers only
the bolt-actions kill with one body hit (L96A1, Scoped Kar98k: 106), the semi-automatic WA2000, Dragunov and PSG1
hit like the Garand; shotguns stay Black Ops' (MoHAA's fires 20 pellets, BO's 8); bots' guns too; arms and legs as
the body (MoHAA's limbs take less).
- **How**: `G_KbMohaaDamage` (g_combat_mp.cpp) maps a weapon variant to its MoHAA damage by the name's first token
  (`mp5k_silencer_mp` -> `mp5k`; a token that starts with a table name counts, so the dual-wield halves `aspdw`,
  `pythonlh` too; the attachments' own weapons `gl_`, `mk_`, `ft_` don't), only for bullet weapons; 0 for the
  rest. `Bullet_Process` (bullet.cpp) replaces Bullet_GetDamage's ranged damage with it times `damageMultiplier`
  (penetration: a bullet through a wall or a player still loses its share, as in both games), so no falloff;
  `G_GetWeaponHitLocationMultiplier` returns 4 for helmet, head and neck and 1 for the rest (a hit on the gun
  keeps the weapon's own 0). The kill feed's headshots and the script's damage path are unchanged (no Black Ops
  perk changes bullet damage: `cac_modified_damage`'s bullet perks don't exist in MP). Read per hit: the dvar
  works mid-match. It changes what bullets do to vehicles, glass and destructibles too.
- **Checked** (temporary per-hit log in G_DamageClient, removed; release client, listen server on mp_nuked, scratch
  profiles): test bots and the player with ten guns, then the launcher's 7 managed bots, 100 s each: body hits mp5k,
  thompson, pm63 25, famas, galil, aug, m16, ak47, stg44 27, rpk, fg42 30, m14, fnfal, m1garand, gewehr43, m1carbine
  48, l96a1, scopedkar98k 106; headshots 100 (SMGs), 108 (AK-47, AUG), 120 (FG42), 192 (Dragunov, FAL, Garand), 424
  (L96A1); neck 100; the PM63 at 1708-1760 units 25 and 100 (Black Ops: 20 and 28); a few lower values through
  walls; shotguns (Olympia, Stakeout, Trench Gun, double-barrel) their own 38-40 on head and body; the knife 135-139;
  `set kb_mohaaDamage 0` mid-match gave Black Ops' (RPK 40, head 55; FAMAS 33-35 with falloff). The ASan client
  (`kb_godMode 0`, 7 managed bots, the dvar toggled off and on): no report. check_file.py natively and `--i386`,
  audit_splitlocals.py 0, gen_db_save.py `--check`.

## Decisions (session 31)

**No start countdown (`kb_noCountdown`) and every custom class's guns (`kb_allClassGuns`)**, the user's requests
(Status). Both are script patches (cscr_parser.cpp's scrKbPatches) read when the scripts run, dvars registered in
g_main_mp.cpp (archived, default 1).
- **The countdown** is _globallogic.gsc's prematch period: `level.prematchPeriod` (the "prematchperiod" tweakable,
  15 s, then +-2 s at random above 2 s) runs `matchStartTimer` ("MATCH BEGINS IN", the grey mpIntro vision) while
  spawned players are frozen; it starts at the map's start, so it runs while a player sits in the team and class
  menus. The patch zeroes it where Callback_StartGameType sets `level.inPrematchPeriod` (every round), so
  `prematchPeriod()` takes its no-countdown branch: vision restored, players unfrozen at once. The grace period
  (late spawns in round modes) is untouched.
- **The guns**: giveLoadout (_class.gsc), after any class's loadout (custom or default) and before
  `cac_selector`, calls a new `kbGiveAllClassGuns` (patched in before giveLoadout) for a player that is no bot
  and no test client, unless the game mode gives its own loadout (`level.giveCustomLoadout`: gun game, one in the
  chamber, sharpshooter, sticks and stones). It loads the custom classes (`cac_getdata`: 0-4, and 5-9 in an online
  game, which `map` games are) and gives every class's primary, then every class's secondary, with their weapon
  options (camo), skipping guns held already; kb_noLaunchers and kb_noDualWield apply as in giveWeapon. The chosen
  class's primary stays the spawn weapon; weapnext cycles through all.
- **Limits**: 15 held weapons, 15 clips and 15 ammo pools a player. The first test crashed the server in
  BG_AddAmmoToClip (null clip: "Tried to add ammo ... no free available clips", which the original asserts and then
  writes through): the class script sets the frag's and the tactical's clips (SetWeaponAmmoClip) after giving them,
  and kb_noGrenades never gives them, so two clip slots held ammo for weapons nobody had ("found ammo clip 2 that
  we do not have a weapon for"). Now SetWeaponAmmoClip/Stock skip a weapon a kb_* dvar blocks that the player
  doesn't hold, and giveWeapon leaves out a weapon whose clip or ammo pool has no slot (a developer print), which
  makes any number of gives safe.
- **The console's game message windows** (cl_console.cpp Con_GetDestWindow): the decompile's
  `&gamemsgText[2][52 * dest + 1892]` is `gamemsgWindows[dest - CON_DEST_GAME_FIRST]` with i386's 52-byte
  MessageWindow; on 64-bit the window is larger, so GAME2/GAME3 (bold game text, subtitles) got a window starting
  inside the previous one: its `messages` pointer was half of one field and half of the next window's `lines`
  (fault address 0x640199b00000000c), and Con_UpdateMessage crashed or wrote through it. Seen once in this
  session's tests (a print at the spawn); every 64-bit build had it.
- **Checked** (release client, listen server on mp_nuked, scratch profiles, KB_SCREENSHOT): with
  `kb_noCountdown 0` and the class picked 1 s after joining, "MATCH BEGINS IN 12" and the player frozen (same
  `viewpos` after a second of +forward); with 1 the clock running at once and the player walking. Ten classes set
  with `statWriteDDL` to ten different primaries (Thompson, MP5K, L96A1, AK-47, SPAS-12, MP40, Uzi, FN FAL,
  WA2000, STG-44) and three secondaries: weapnext went through those and the CZ75, M1911, Makarov and ASP (the
  M72 LAW of one class came as the AK-47, held already), the same after `kill` and the respawn; with
  `kb_allClassGuns 0` only the class's Thompson and ASP. Two more release runs and the ASan client (the whole
  sequence, a death and respawn): no crash or report. check_file.py natively and `--i386`, audit_splitlocals.py 0,
  gen_db_save.py `--check`. Not tried: a round-based mode's later rounds, managed bots (the script skips them).

## Decisions (session 30)

**Bot bodies away from where the bot died** (the user's report, Status).
- **What a body is**: the death callback (_globallogic_player.gsc Callback_PlayerKilled) calls `clonePlayer`
  (PlayerCmd_ClonePlayer): a new ET_PLAYER_CORPSE entity at the player's `ps.origin`, in one of 4 slots
  (entities 32-35, round robin, G_SpawnPlayerClone; the 5th body frees the oldest), moved by the death
  animation's root motion (G_RunCorpseMove, a few units a frame) and then a ragdoll. The client hides a dead
  player (EF_DEAD) and draws the body entity in its place, with the bot's animation tree copied over.
- **Measured** (temporary logging, removed: the client's drawn position of each bot and body per frame, the
  server's body position, the coordinates each side encoded and decoded): with test bots killed where they
  stood, every body appeared 1-8 units from the bot; in 7-bot matches (the launcher's managed bots, TDM on
  mp_nuked) some appeared 18-876 units away. The server had them right (`ps.origin` at the clone, the body's
  trBase); the client decoded one coordinate wrong in the snapshot and kept the error.
- **The cause**: an entity's or player's x/y/z that moved 64 units or more since the client's old state is
  sent as 16 (z: 17) bits, (old - centre + 0x8000) XOR (new - centre + 0x8000), with both rounded to whole
  units by `(int)(x + 9.313225746154785e-10)` (MSG_WriteOriginFloat/ZFloat, MSG_ReadOriginFloat/ZFloat). The
  client holds whole numbers, so for its negative old -325.0 the cast gave -324 (the 2^-30 bias, then toward
  zero) where the server got -325 from its -325.17; XORed with the off-by-one old value the new one came out
  wrong in its low bits: 1 unit off half the time, 31 or more about one time in 16 (e.g. -82 read as -55).
  Later small moves are sent as differences, so the error stayed until the next large move. Large moves are a
  new body in a slot whose last body lay elsewhere and a respawn; so bodies landed off (a metre, or out of
  sight) and respawned bots were drawn off the server's position. The original rounds (IDA shows a rounding
  conversion, cvtsd2si/fistp, as a C cast; the 2^-30 only makes sense as a tie bias for rounding), and the
  protocol only works if both sides round. Not caused by `kb_noDeathCam`, which only makes deaths, and so
  reused body slots, more frequent.
- **The fix** (msg_mp.h `MSG_RoundCoord`, `lrint` of the biased value, i.e. round to nearest as cvtsd2si):
  the writers (origins, the theater demo's rounded floats), MSG_ValuesAreEqual's test for those fields (it
  must round as the writer does, else a value can stay unsent while the server's base moves on) and the
  readers. The theater demo reader keeps `(int)oldValue` (its old value is always whole). Every other
  `(int)(x + 9.313225746154785e-10)` (~200: colours, HUD, usercmd moves, `-86` net fields) still truncates; at
  most 1 off, and none is decoded against another side's rounding (Open items).
- **Checked**: release client, 7 managed bots, 130 s: 35 bodies, 32 within 5 units of the bot's last drawn
  position, 3 at 8-19 units, each exactly at the server's position (sprinting bots are drawn 50-75 ms behind
  the server, the original's interpolation); before the fix four such runs gave 7 of 29, 9 of 27, 1 of 23 and
  10 bodies at 18-876 units. Client and dedicated server as two processes over UDP (4 test bots, the kill loop): joins, plays, 12
  kills, no error. check_file.py natively and `--i386` (only the known GLEW/SDL artefacts), audit_splitlocals.py
  0, gen_db_save.py `--check`.
- **Tool**: `kb_shots <count> [<every>]` (sys_platform.cpp, both backends' KB_SCREENSHOT hook in gl_d3d9.cpp):
  with KB_SCREENSHOT, the next <count> screenshots every <every> presents from the moment the command runs, so
  a KB_CMDS entry films one kill at 30 per second; `KB_SCREENSHOT_EVERY=0` takes no periodic ones.

## Decisions (session 29)

**Only the crosshair (`kb_cleanHud`)**, the user's request (Status).
- **What the HUD is** (screenshots of a match with kills, `KB_SCREENSHOT`): the HUD menus that
  `Menu_PaintAll(cgDC)` paints (minimap, score/timer/emblem, ammo and killstreak wheel, kill feed with the bots'
  `[3arc]` clan tags, the victim's and the player's cards, the weapon pickup hint), the scripts' hud elements
  (`+250` from _rank.gsc's `hud_rankscroreupdate`; medals and challenges through _hud_message.gsc's
  notifyMessage at the top centre; the objective text; the spawn's perk list, which _hud_util.gsc's
  createLoadoutIcon/Text put in the foreground layer; 3D waypoints, `CG_AddDrawSurfsFor3dHudElems`), names
  over heads (`CG_UpdatePlayerNames`), chat, and the damage/grenade indicators. What the user meant by "the
  tags top left" is not certain (the minimap's grid labels, the clan tags, the player card at the top right
  of a 4:3 window?); everything but the crosshair is gone, so it is gone too.
- **How** (CG_Draw2DInternal, cg_draw_mp.cpp): with the dvar on, the names, chat, `Menu_PaintAll`, damage and
  grenade indicators, spectator message and vote are skipped; the crosshair, the flash-damage tint, screen
  burn/electrified, the scoreboard and the debug overlays (`cg_drawFPS`, which the user's own config turns
  on) still draw. The scripts' hud elements, both layers and the 3D waypoints, are skipped while a round is on
  (`CG_KbHideHudElems`, cg_newDraw_mp.cpp): the match flag `cg_drawSpectatorMessages` (matchUIVisibilityFlags
  0x800) is set by Callback_StartGameType and cleared by displayRoundEnd/displayGameEnd/endGame, so the
  outcome texts show. The foreground layer (an audit of all 280 MP scripts) holds only the loadout icons, the
  killcam captions, the pregame title, Copycat, the guided missile overlay and RC-XD/minigun/rocket HUDs,
  powerups, tutorial tips and dev elements: hidden with the rest. The dvar is read each frame (cg_main_mp.cpp,
  next to kb_noVersion); 0 gives the original HUD at once.
- **Checked** (release client, listen server on mp_nuked, 4 test bots, `scr_playerwarp enemies_host` kills,
  scratch profiles, KB_SCREENSHOT 3/s): with the dvar on, from the spawn on only the crosshair and the FPS
  overlay through kills (no +XP, medal or challenge popups, cards, kill feed, hints, minimap, ammo); `set
  kb_cleanHud 0` mid-match brought the full HUD back at once ("Assist", "Longshot", "Avenger" popups, +300,
  cards, kill feed, minimap), `set kb_cleanHud 1` removed it again; at the 1-minute time limit "VICTORY / Time
  limit reached / 1400 300 / Match Bonus 280 XP" showed, then the After Action Report, the next map and the
  class menu. A round-based mode's round outcome (S&D) is from the code, not tried. check_file.py natively and
  `--i386`, audit_splitlocals.py 0, gen_db_save.py `--check`.

**The launcher's signing** (tools/launcher/build.sh): installing session 29's build stopped after the
binary, at `codesign` (its error was sent to /dev/null, and `set -e` skipped the shader-library and DLC-zone
steps): the game folder's new `Black Ops Launcher.app` already carried `com.apple.FinderInfo` (frFlags
0x2000, kHasBundle) and `com.apple.fileprovider.fpfs#P` (~/Documents is under a File Provider), and
codesign refuses Finder info ("resource fork, Finder information, or similar detritus not allowed").
build.sh now deletes FinderInfo/ResourceFork just before signing and prints codesign's error if it still
fails. The flag comes back right after (so `codesign --verify --strict` complains later; plain `--verify`
passes, and a local app without quarantine launches with it).

**Disk cleanup** (the user's request): the disk was 96% full. At the user's word, the Black Ops leftovers went
to the Trash and the user emptied it (68 GB: old sessions' scratchpads, 13-15 GB of logs in one, Instruments
traces from the gpu_trace runs, ~250 per-run `com.apple.gpuarchiver` caches with `kb_` shaders, the Metal
shader-validation caches); Counter-Strike work of the user's other sessions was left alone. Keep test logs and
screenshot sets small, and delete them once read.

## Decisions (session 28)

**No prone (`kb_noProne`) and no hit marker (`kb_noHitMarker`)**, the user's requests (Status).
- **Prone, where**: pmove decides the stance on the server and in the client's prediction from the
  command's buttons (8 prone, 9 crouch, 44 dive to prone; PM_CheckDuck, Dtp_IsDtp). PmoveSingle now turns
  a prone request into a crouch request and drops the dive bit before anything reads them, so nobody goes
  prone, bots included (they never choose prone anyway; only `sv_botsForceProne` asks for it, which now
  crouches them) and script commands that force the prone stance through the client (EV_STANCE_FORCE_PRONE).
  A player already prone when the dvar comes on gets up to crouch on the next move (from the code, not
  tried). Not covered: a
  turret's own prone stance (eFlags 0x100, turret.cpp; multiplayer has no mounted prone turret) and last
  stand's pose (`kb_noLastStand` has none).
- **Prone, the client's keys**: the client keeps its own stance state and toggles from it (cl_input_mp.cpp).
  With only pmove changed, Ctrl (`toggleprone`, the user's binding) after a crouch would have needed two
  presses to stand. CL_ToggleStance now treats the prone toggle as the crouch toggle (also while
  sprinting: no dive), and CL_AddCurrentStanceToCmd turns a prone or dive state (goprone, lowerstance,
  holding `+stance`, a forced prone) into crouch, so the next key press starts from what the player sees.
  `+prone` (held) is left to pmove. The dvar lives with the other pmove ones (bg_misc.cpp, read by both
  sides: a remote client's own value drives its prediction, as with `kb_noAds`).
- **Hit marker, what it is**: maps/mp/gametypes/_damagefeedback.gsc's `hud_damagefeedback`, a 24x48
  `damage_feedback` hud element centred on the crosshair, set to alpha 1 and faded over 1 s by
  `updateDamageFeedback` on each hit (Callback_PlayerDamage). The script patch (cscr_parser.cpp) skips
  only the `alpha = 1`, read at each hit (`GetDvarInt`), so the `mpl_hit_alert` sound still plays and a
  change takes effect at once. The other indicator in that script, `directionalHitIndicator`, is
  Tactical Mask Pro's flash/concussion feedback (blocked with `kb_noGrenades` anyway). The code's
  `CG_DamageFeedback` is the damage the player takes, not this.
- **Checked** (release client, listen server on mp_nuked, scratch profiles):
  - Stances by eye height (`viewpos` at -100 360 on the street, feet at -70): with `kb_noProne 1`,
    `toggleprone`, `goprone`, `lowerstance` twice and `+prone` all gave crouch (eye -30; standing -10),
    `toggleprone` again or from a crouch stood up, `-prone` stood up; with `kb_noProne 0` the same commands
    lay down (eye -59). Sprinting then `toggleprone`: the original game dives (eye up to +16, then down to
    -56), with the dvar the player crouches and keeps walking (eye -23).
  - Hit marker (temporary logging of human hits in finishPlayerDamage and of `damage_feedback` draws, removed):
    `+set developer_script 1 +set scr_testclients 4 +set sv_botsAllowMovement 0`, a KB_CMDS loop of
    `set scr_playerwarp enemies_host`, `setviewpos` 130 units back and fire. `kb_noHitMarker 0`: 12 hits,
    the marker drawn after each and faded over 1 s (screenshot: the X around the CS crosshair);
    `set kb_noHitMarker 1` mid-match: 15 hits, no marker drawn (screenshot right after a hit: the crosshair
    only). No script-patch warning; both dvars land in the profile's config_mp.cfg.
  - check_file.py natively and `--i386` on every changed .cpp, audit_splitlocals.py 0.

**Create a Class on the main menu (`kb_cacButton`) and the Create-a-Class lists by mouse** (Status).
- **Getting there**: the menus' own way is PLAY -> lobby -> CREATE A CLASS, and PLAY, OPERATIONS and THEATER
  only say "No network connection detected" offline (`isSignedInToLive` 0; `listzone patch_ui_mp menu
  main_text`). THEATER became the button: films are the least likely of the three to ever work offline.
  The menus come from the zones (patch_ui_mp), so UI_KbPatchMenu (ui_shared.cpp) changes `main_text` as
  UI_AddMenu takes it: the four items whose text expression is "@MPUI_THEATER_LOBBY_CAPS" (the button, its
  highlighted and dimmed labels, the flyout header) say "@MPUI_CREATE_A_CLASS_CAPS" ("CREATE A CLASS",
  the lobbies' own string), and the button's `action` handler runs one unconditional script, the
  lobbies' `exec "set ui_cac_ingame 0" ; setdvar invite_visible "0" ; open cac_main`. cac_main's Esc
  saves (`uploadstats`) and closes back to the main menu. UI_InitUIInfos adds main_text before
  UI_RegisterDvars, so `kb_cacButton` is registered at the top of UI_InitOnceForAllClients (registered
  later, the menu was added while the dvar was null: the first try crashed there).
- **The lists' bug**: a click in a drop-down list (modal listbox) runs its `doubleClick` at once:
  equipclasscurrentitem / equipclasscurrentattachment / equipclasscurrentoption with sharedUiInfo's
  current item, attachment or option. Upstream's struct rework (18948a32, "IDA got the struct wrong")
  renamed the stores of IDA's `modIndex` (the item) to `itemIndex` but left the next store, the row, as
  `itemIndex` too: in UI_Project_FeederCount (49/58/97), UI_FeederItemText_ItemInSlot,
  UI_FeederItemText_CustomPerksInSlot (every frame, for the highlighted row) and UI_Project_FeederSelection.
  The row now goes to `itemNum` (nothing reads it). Likewise feeder 57 (attachments) counted into
  `attachmentNum`, the picked attachment, which UI_FeederItemText_Attachments also read as the count:
  the count is `numAttachments` now and the item groups' count (feeders 49/58/97/59/65-67/100 and the item
  texts' cursor clamps) `numItemsInSlot`, the fields the struct's IDA cross-references give them (the
  original assert text still says numItemsInSlot). sharedUiInfo is runtime-only, so only the code's
  consistency matters, not the layout. "Item WEAPON_MP5K does not have attachment number 7" every frame
  in the attachment list is gone with it.
- **Tools**: `kb_mouse <x> <y>` / `kb_key <key>` (sys_platform.cpp, registered on the first IN_Frame)
  stand in for the mouse and keyboard in KB_MENU_CMDS/KB_CMDS; listzone's menus print each item's rect,
  visibility and text expressions and each script's condition (postfix; function names from
  expressions/functions.txt, which the dedicated server doesn't load into the evaluator).
- **Checked** (release client, scratch profile, 1024x768 window, KB_SCREENSHOT): the main menu shows CREATE A
  CLASS, highlighted under the cursor; the click opens Create a Class (Custom 1-10), Esc returns. Before
  the fix: Custom 1's Thompson row equipped nothing visible ("MAKAROV DUAL WIELD" label; the class's secondary
  showed that item afterwards), the Uzi row showed "MAKAROV". After: Thompson (`statReadDDL cacLoadouts
  customclass1 primary` 25, the screen THOMPSON with its stat bars), MP5K (15), Perk 1 Ghost Pro, the MP5K's
  Suppressor (primaryattachmentmuzzle 5; the others grey out, Clear Attachments appears), Tiger camo
  (primarycamo 8); the stats file is written on Esc and a restart reads them back; a listen match spawns
  the player with that class (MP5K, suppressor, Tiger camo). check_file.py natively and `--i386` (only the
  known SDL/TargetConditionals artefacts in sys_platform.cpp), audit_splitlocals.py 0, gen_db_save.py
  `--check`.

## Decisions (session 27)

**DLC weapons, stage 3: Create a Class** (Next steps, first item; stages 1-2 in Decisions sessions 25-26).
- **The tables live in the zone**: kb_dlc_weapons.ff carries whole copies of three multiplayer string
  tables with the guns' rows: `mp/statsTable.csv` (the items: Create a Class, the weapon table, the
  class scripts, the bots), `mp/attributesTable.csv` (Create a Class's damage/range/accuracy/mobility/
  fire-rate bars, looked up by reference in column 1) and `mp/weaponOptions.csv` (the client looks
  a weapon's parent up for its camo textures; missing, it printed "Couldn't find weapon parent ..." once
  per gun). String tables have a default asset, so a zone of higher priority replaces one
  (DB_LinkXAssetEntry: the delayed clone, "Redundant asset: 'stringtable','mp/statstable.csv'" in the
  log, then DB_SwapXAsset): flag 1024's priority is 60, code_post_gfx_mp's (1) 51, patch_mp's 59. Off
  (or without the file) the original tables are untouched. Patching the table at load in the engine
  would have tied the rows to code instead of the zone that holds the weapons; the menus' and scripts'
  `tableLookup`s read the asset, so the rows must be in it either way.
- **The writer, new** (db_save_tool.cpp; make_dlc_weapons.sh's last merge passes them):
  `damage <weapon>=<multiplayer weapon>` (DB_SaveTool_MatchDamage: damage, minDamage, playerDamage,
  minPlayerDamage, fMaxDamageRange, fMinDamageRange, the 19 hit location multipliers, iMeleeDamage,
  penetrateType and, between two shotguns, shotCount; fire rate, clip, recoil, spread stay the gun's),
  `rows <table>=<file>` (rows by index into empty rows: refuses a row in use), `addrows <table>=<file>`
  (rows appended; one the table has is left out), `strings <file>` (localized strings, `NAME text`; a
  source zone's string of the name takes the text). The replaced table is written whole although
  multiplayer has the name (`s_fullAssets`, skipped by DB_SaveTool_Resolve's stubbing); `all` keeps a
  source zone's string table that differs from multiplayer's, so the finished zone written again from
  itself is still the same bytes (under the same file name: the cut bank is named after the file). The
  cell index (`cellIndex`) is rebuilt sorted by hash then column, as StringTable_FirstHashIndex's binary
  search reads it; 16-bit cell numbers, so a table stays under 32768 cells. `listzone` prints a weapon's
  damage fields, localized strings' text (`listzone - localize [prefix]`) and a menu's items with the
  scripts its events, keys, items and animation states run (`listzone ui_mp menu cac_weapon`): how the
  Create-a-Class screens were driven from the console below.
- **The data** (tools/): `dlc_weapons_stats.csv` (16 rows in empty rows below 99: thompson 25, type100
  58, mp40 59 (SMG); stg44 36, m1garand 60, gewehr43 61, m1carbine 79 (assault); fg42 41, bar 80,
  bipodbar 81 (LMG); kar98k 46, scopedkar98k 82, type99 83 (sniper); trenchgun 52, doublebarrel 11,
  sawedoff 84 (shotgun, `weapon_cqb`); sort keys after multiplayer's guns of the group, cost 2000, slot
  primary, image the HUD icon, name the weapon's display name, description our KB_DLC_*_DESC; unlock
  level the matched gun's, the FG42's 20 (the Stoner63 is classified)). Groups follow World at War where
  Black Ops has the group (SMG, LMG, shotgun, bolt-action as sniper) and put WaW's semi-automatic rifles
  with Black Ops' semi-automatics (M14, FN FAL: assault). `dlc_weapons_attributes.csv` (the matched gun's
  bar values, the gun's own magazine and fire mode; the slow MP40 and BAR get a lower fire-rate bar),
  `dlc_weapons_options.csv` (the launchers' neutral camo rows: the WWII models have none of
  multiplayer's camo textures, so camo leaves them as they are; bots pick a random camo, no effect),
  `dlc_weapons_strings.txt` (16 descriptions in multiplayer's style; the sawed-off's name shortened to
  "Sawed-Off Shotgun", the WaW "Sawed-Off Double-Barreled Shotgun w/ Grip" was cut off in the lists).
- **Damage** (the closest multiplayer gun by kind, fire mode and rate; make_dlc_weapons.sh's list):
  Thompson, MP40 -> MP5K (40-20); Type 100 -> AK74u (35-20); STG-44 -> AK47 (40-30); M1 Garand -> M14
  (50-40, head x1.5); Gewehr 43, M1 Carbine -> FN FAL (50-40); FG42 -> Stoner63 (40-40, same 64 ms);
  BAR, bipod BAR -> M60 (50-40); Kar98k, scoped Kar98k, Type 99 -> L96A1 (70, x1.5 upper body);
  trench gun -> Stakeout (8 pellets 40-10); double-barrel, sawed-off -> Olympia. The zombie values were
  100-300 a bullet with 2.7-10x head multipliers.
- **Loading** (common.cpp Com_KbAddDlcWeaponsZone, Com_KbDlcWeaponsLoaded): `kb_dlcWeapons` (archived;
  read when the common zones load, so a change takes effect at the next start; not latched: a latched
  dvar changed from the console is not saved, Dvar_SetVariant marks no archive change for it) is
  registered where the common zones load: DB_LoadFastFilesForPC (startup, client and
  dedicated: common_mp 64, then kb_dlc_weapons 1024, then the client's ui_viewer_mp 2048) and
  Com_LoadCommonFastFile (when a level load finds common_mp unloaded, 256). Loading flag 1024 frees the
  low zones >= 1024 (ui_viewer_mp), so it must come before ui_viewer_mp in the same call; its localized
  partner (2048, `en_kb_dlc_weapons`) doesn't exist (a warning line). The unlockable items are parsed
  in LiveStats_Init, before the common zones load: after the load the database is synced and
  BG_UnlockablesReloadItems parses them again (BG_UnlockableItemsInit's BG_InitUnlockables, keeping the
  menus' list). itemInfo keeps pointers into the table, which is why the zone never unloads while the
  game runs (nothing frees flag 64 or lower but shutdown).
- **No dual wield, the menu** (bg_unlockable_items.cpp): session 26's `ItemHasDualWield` 0 did not
  remove the option: the CZ75's attachment list (feeder 57) still offered Dual Wield. The menus count
  and map an item's attachments through BG_UnlockablesGetNumItemAttachmentsWithAttachPoint and
  BG_UnlockablesGetItemAttachmentNumWithAttachPoint; with `kb_noDualWield` both skip `dw` (the list's
  numbers stay the item's own; the weapon table's BG_UnlockablesGetNumItemAttachmentsAtPoint is
  unchanged, so the variants still register). The ASP (only `dw`) has no attachments to choose now.
- **Engine facts found**: Create a Class (cac_main, menu_cac_custom, cac_weapon) is driven by dvars
  (`ui_custom_name`, `selected_loadout_slot`, `selected_group`, `ui_inside_popup`,
  `cac_weapon_current_list`), `setCurrentItemIndex`, `buildItemListForSlotName(AndGroup)`,
  `setlocalvarint ui_weaponButton` and the anim_controller item's `ui_animate ... OpenDropDownList /
  CloseDropDownList` states; KB_MENU_CMDS can run all of them (the sequences are in the checks below).
  The bots skip a weapon row without an unlock level (they take it for a dual wield,
  bot_weapon_dual_wield_unlocked). _class.gsc falls back to the M1911 for a primary whose row is gone
  (`kb_dlcWeapons 0` with a DLC gun in a class).
- **Checked** (scratch game folder: links to the game's data plus the new zone as
  zone/Common/kb_dlc_weapons.ff; scratch profiles):
  - The zone: two extractions give the same bytes; the finished zone written again from itself under
    the same name gives the same bytes; `listzone kb_dlc_weapons stringtable mp/statsTable.csv` shows
    the 16 rows, `listzone kb_dlc_weapons weapon` the matched damage. 4,260,413 bytes, 466 roots.
  - Dedicated server, mp_nuked: the weapon table has 1670 weapons (1654 with `+set kb_dlcWeapons 0`);
    a saved `seta kb_dlcWeapons "0"` in the game folder's players/config_mp.cfg skips the zone at
    startup, "1" loads it (the config runs before the common zones load).
  - Create a Class (the client, KB_MENU_CMDS; screenshots): Custom 1 with the Thompson (icon, name),
    cac_weapon with the Type 100 (bars filled from its attributes row), the group list, the five groups'
    weapon lists with the 16 guns after multiplayer's; the CZ75's attachment list without Dual Wield
    (before the fix: with it, as with `kb_noDualWield 0`). The console sequences: `openmenu cac_main`,
    `equipClassItem customclass1 <row>`, `openmenu menu_cac_custom`; the Primary button:
    `setCurrentItemIndex <row>;set selected_loadout_slot primary;buildItemListForSlotName primary;set
    ui_menu_selection_title @MPUI_PRIMARY_WEAPON;set uiViewer_equipWeapon primary;closemenu
    menu_cac_custom;openmenu cac_weapon`; the group list: `set ui_inside_popup select_weapon_group;set
    cac_weapon_current_list weapon_group_list;setlocalvarint ui_weaponButton 1;ui_animate cac_weapon
    anim_controller OpenDropDownList 0`; a group's weapons: `ui_animate cac_weapon anim_controller
    CloseDropDownList 0;set selected_group weapon_smg;set cac_weapon_current_list weapon_list;
    buildItemListForSlotNameAndGroup primary weapon_smg;set cac_weapon_feeder_count 12;set
    ui_next_inside_popup select_weapon`; attachments (secondary slot): `set ui_inside_popup attachments;
    set cac_weapon_current_list attachments_list;set cac_weapon_feeder_count 5;setlocalvarint
    ui_weaponButton 2;ui_animate cac_weapon anim_controller OpenDropDownList 0`.
  - A listen match, mp_nuked TDM, 7 managed bots, 2 minutes (temporary logging of every script
    `giveWeapon` and `finishPlayerDamage`, removed): the scripted player's Custom 1 (`equipClassItem
    customclass1 25`) spawned with the Thompson (firing, 20-round magazine, 13/180 in the screenshot);
    the bots were given 16 DLC guns of ~44 primaries (Type 100, STG-44, sawed-off, M1 Garand and FG42
    twice; Thompson, MP40, M1 Carbine, Arisaka, Scoped Kar98k, Deployable BAR) and hit with them at
    multiplayer damage (Thompson and MP40 40, 55 to the head; STG-44 40/55; M1 Garand 50, 75 to the
    head; BAR 50; FG42 40; sawed-off 40 a pellet; Arisaka 75 to the lower torso, as the L96A1 in the
    same match); the kill feed shows their icons. `kb_dlcWeapons 0` with that class: the M1911
    (_class.gsc's fallback), no error.
  - Every map (tools/bench_views.py --map all --bots --views overview): all 26 load the zone and
    draw (250-590 fps at the overviews with 7 bots); no pool limit, weapon mismatch or missing parent
    (the effects pool is 1024 since session 26); bots took DLC guns 85 times over the 26 matches.
  - A dedicated server and a client as two processes: both load the zone, the client stays in the
    match for a minute next to bots using DLC guns, no "Weapon index mismatch".
  - ASan client (build_asan): the Create-a-Class screens above, `vid_restart`, then a listen match
    (mp_nuked, TDM, 7 managed bots, `scr_tdm_timelimit 1`, Custom 1 with the MP40) through three match
    ends and rotations (mp_array, mp_cracked, mp_crisis): the player spawned with the MP40 on each map,
    the bots were given DLC guns 36 times; no report. The zone stays loaded across maps.
  - check_file.py natively and `--i386` on every changed .cpp, audit_splitlocals.py 0, gen_db_save.py
    `--check`.

## Decisions (session 26)

**DLC weapons, stage 2: every weapon in one zone** (Next steps, first item).
- **The scope, changed by the user during the session**: first all of session 24's list (WWII guns,
  their Pack-a-Punch versions, the MP40, the wonder weapons); then "i dont want any ray guns (or
  wonder weapons?) nor any crossbows either" (asked: all wonder weapons go) and "drop the pack a punch
  versions too". Left: the 12 WWII guns, the scoped Kar98k, sawed-off, bipod BAR and the MP40.
- **Where they are** (`listzone <map>_patch,<map> weapon`): prototype: Thompson, BAR, trench gun
  (`zombie_shotgun`), double-barrel and sawed-off, Kar98k and scoped, M1 Carbine; asylum: + bipod BAR,
  Gewehr 43, STG-44, M1 Garand; sumpf: + Type 100, Type 99, `_upgraded` versions of most; factory: + FG42
  and the 9 `_upgraded` ones; common_zombie: `mp40_zm`, `ray_gun_zm` (+ upgraded) and BO's guns as
  `*_zm`. Wonder weapons, for the record: tesla_gun_zm (sumpf, factory, coast), thundergun_zm
  (prototype, theater, cosmodrome), freezegun_zm (asylum, pentagon), humangun_zm and sniper_explosive_zm
  with its bolt (coast), shrink_ray_zm (temple), microwavegundw_zm with its left hand and the
  microwavegun_zm alt mode (moon). The FG42 is only in factory, the Type 99 only in sumpf, the scoped
  Kar98k only in prototype, the bipod BAR only in asylum: 4 map runs. Sources chosen: factory for the
  Thompson, Type 100, STG-44, FG42, Gewehr 43, Kar98k, M1 Carbine, trench gun, double-barrel (and the
  MP40 from common_zombie); asylum for the BAR, bipod BAR, M1 Garand, sawed-off; prototype for the
  scoped Kar98k; sumpf for the Type 99. Each run loads `<map>_patch,<map>,common_zombie`, patch first
  (the first zone's asset wins, as the patch overrides in the game).
- **Names**: `<ref>_mp`; the references are no prefix of one another, of any of the 194 statsTable
  references or of any of the 1717 MP weapon names, and none of those is a prefix of ours (checked
  with a script: `scavenger` would have hit `scavenger_item_mp`, the perk's item, back when the
  Scavenger was in). parentWeaponName is the reference, as MP's weapons have it (`ak47_mp` -> `ak47`,
  `cz75dw_mp` -> `cz75`, bolts -> the launcher's); the client's CG_SetBaseWeaponForStats Com_Errors on a
  registered weapon whose parent the table lacks, so `--no-parents` writes them empty for tests before
  stage 3's rows exist. The zombie weapons' ammo and clip names are empty (each has its own ammo).
- **The writer, new**: `weapon <old>=<new>[:<ref>]` (DB_SaveTool_RenameWeapons: the internal name, the
  alt, dual-wield and spawned-grenade names a weapon holds, parentWeaponName; every weapon of the zone
  needs a name once one is renamed); the weapons a weapon names come along as roots
  (DB_SaveTool_AddLinkedWeapons); an asset name in several source zones is written once, the first
  zone's (DB_SaveTool_Resolve), stubs too (they were written once per source zone); effects named by
  effects load before the effects naming them (the writer records each name with the root it was
  written under, DB_SaveNameRef; DB_SaveTool_OrderRoots: sorted, then each root's named roots first,
  so the order settles, cycles or not). Before, an effect named by one earlier in the zone got a
  default entry that the real one then overwrote ("Redundant asset", DB_DelayedCloneXAsset): it worked,
  but the loader's own zones don't do that. A written asset of a type without a default (Com_Error
  "Attempting to override asset" if a later zone has the name) prints a warning: none in this zone.
- **Sound notetracks** (`sndnt#<alias>`): Black Ops' viewmodel animations name sounds as notetracks
  that CG_PlayClientSoundNoteTracks plays without the 6-character prefix (WaW's guns use plain alias
  names or the weapon's notetrack sound map). The writer saw the notetrack strings but matched them
  whole: the MP40's and factory STG-44's magazine sounds (`fly_mp40_*`, `fly_mp44_*`) were missing, the
  reloads silent (-34 and -50 dBFS under the `_plr` solo). DB_SaveTool_WantString strips the prefix
  now; every `sndnt#` name of the zone is in its bank. The other prefix the plan named,
  PhysPreset::sndAliasPrefix, is never read by the MP executable (Ragdoll zeroes it); weapon bounce
  sounds are full names (`bounceSound[surface]`), impact sounds MP's own tables.
- **Merging**: the map zones (0.4-5 MB) merge 3 at a time (`all`: every asset but the banks, sorted;
  the bank is cut again from what the zone names), the flag pairs being 3. More pairs exist
  (0x40000/0x10000/0x4000, localized 0x80000/0x20000/0x8000), but the unload frees the low ones in the
  order 0x40000, 0x10000, 0x4000 and each `DB_LoadXAssets` frees low flags >= its own: they would have
  to load in the order 0x4000, 0x10000, 0x40000 after the high ones; not needed. Source zones load by
  name from `<install path>/zone`, so the script makes a game folder of its own (links to the game's
  `main` and zone files, its own `zone/Common` for the zones it writes, a scratch fs_h) and runs the
  build's dedicated server there with `+exec` (the command line is 2 KB and a startup command 1 KB; a
  cfg line may be 8 KB and 512 tokens). The extraction takes ~10 s.
- **Tools**: `listzone` (db_save_tool.cpp; tool mode, dedicated server): a zone's asset counts, a
  type's assets (weapons with variant count, display name, type/class, alt/dual/parent, ammo, models,
  icons, fire and reload sounds, the notetrack sound map), a string table's cells or a script (packed:
  length, packed length, zlib), with `-` multiplayer's loaded ones.
- **The effects pool** (POOLSIZE_FX, db_registry.cpp): 500 in the original; `listassetpool` counts on a
  dedicated server: mp_nuked 360, mp_stadium 418, mp_cracked 420; the mp_nuked client used 381 before
  the zone. The wonder weapons' 102 effects overflowed it ("Exceeded limit of 500 'fx' assets", a
  Sys_Error); the 16 guns' 26 would leave little room on the busiest maps. 1024 now (a few KB).
  Others on the mp_nuked client with the first zone (48 weapons, wonder weapons included): xanim 3737
  of 5100, image 4005 of 4608, material 2839 of 4096, xmodel 691 of 1000, weapon 1770 of 2048, asset
  entries 24561 of 32768; the dedicated server's busiest map had 4098 images (mp_stadium) and 3372 materials
  (mp_cracked): images are the next pool to watch (STREAM_MAX_IMAGES must match it).
- **Late weapons** (`give` of a weapon outside the weapon table, `loadzone`d): one per map. The second
  ends the match with "Weapon index mismatch for '<an MP weapon>'" (CG_SetupWeaponConfigString; the
  engine asserts "We do not support modified weapon files" on that path); the weapons' own data was
  checked (variant count 0, no name-hash collision with the 1717 MP names). Stage 3 registers them
  through the weapon table.
- **Seen in passing**: the zone fills 7 assets that loaded zones referenced but no MP zone holds
  (`mc_unlitdecalblend_add`, `fxt_env_dust_mote_pcloud_2`, `fxt_env_water_droplets` and their
  materials, `gfx_fxt_smk_def_1_z120`, `gfx_fxt_smk_gen_add`): "Redundant asset" as the defaults get
  the real assets. `kb_noGrenades` blocks any projectile weapon with a bolt explosion (it would have
  blocked the Scavenger).
- **Checked** (scratch game folder, mp_nuked listen match, `+set kb_godMode 1`; the `--no-parents` zone;
  one gun per map load, KB_CMDS: team, class, `loadzone kb_dlc_weapons`, `give` (a `w<N>` dvar chain
  run with `vstr`, which also sets `snd_solo_alias_substring` to the gun's player fire alias), fire 0.5
  s, solo `_plr`, reload, `map mp_nuked`; screenshots 2/s, OpenAL Soft's wave writer): all 16 raise,
  fire (ammo counter, muzzle flash) and play their reload animations (contact sheets); fire peaks -1.2
  to -6.9 dBFS after -60 to -67 dBFS of solo'd silence, reload windows -0.3 to -6.6 dBFS (MP40 and
  STG-44 after the `sndnt#` fix: two magazine peaks each, -4 to -7). No error, `[mtl] BUG` or missing
  asset from the zone. The extraction twice: identical bytes; the zone written again from itself: identical
  bytes; ASan: the extraction (same bytes as the release build) and the client (two guns, two map
  loads) report nothing. check_file.py natively and `--i386`, audit_splitlocals.py 0, gen_db_save.py
  `--check`.

**No dual wield** (the user, mid-session: "also remove ANY and ALL dual weilds", "from stock game
too"). `kb_noDualWield`, archived, default 1, like the other changes. Multiplayer has 10 (ASP, CZ75,
M1911, Makarov, Python, Kiparis, MAC11, PM63, Skorpion, HS10), each `<ref>dw_mp` with a `<ref>lh_mp`
left hand; their parentWeaponName is the single gun's reference.
- **Classes**: `_class.gsc`'s getFullWeaponName makes `<ref>dw_...mp` when the bottom attachment is
  "dw"; a script patch (Scr_KbPatchSource) makes `is_attachment_excluded` count "dw" as excluded, as a
  custom game's `attachment_exclusion_<n>` dvars do, so the class gets the single gun with its other
  attachments. Bots: `_bot.gsc`'s `bot_weapon_dual_wield_unlocked` (a 20% roll on the DW rows) returns
  false.
- **Server**: G_GivePlayerWeapon refuses a dual-wield weapon (either hand); PlayerCmd_giveWeapon and
  PlayerCmd_setSpawnWeapon take one as its single gun (G_KbSingleForDualWield: parentWeaponName +
  `_mp`, with the class's camo and options; the developer console prints "kb_noDualWield: X comes as
  Y" once per weapon), for a class that stores the DW item itself (row 7, CZ75 DW) and any other path.
- **Menus**: the dvar is registered in BG_UnlockableItemsInit (startup: the menus run before any map),
  and BG_UnlockablesItemHasDualWield (the menus' `ItemHasDualWield`) answers 0. The Create-a-Class
  attachment screens weren't reached: whether the Dual Wield option is gone is for stage 3's
  screenshots. The item's attachment list keeps "dw": classes store attachment positions in it.
- **Checked** (mp_nuked listen match): a custom class with PM63 + Dual Wield and CZ75 + Dual Wield
  (`statWriteDDL cacLoadouts customclass1 primaryattachmentbottom 3`, ...) is given `pm63_mp` and
  `cz75_mp` (one gun in the hand, 20/60 and 12/36) with the dvar, `pm63dw_mp` and `cz75dw_mp` (two
  guns) without; a class storing the CZ75 DW item gets `cz75_mp` ("cz75dw_mp comes as cz75_mp"); 7
  managed bots for 90 s were given 12 dual wields without the dvar (ASP, CZ75 x3, Kiparis, M1911 x2,
  Makarov, PM63, Python x3) and none of 205 weapons with it (temporary logging of every script
  `giveWeapon`, removed). No script patch warning.

## Decisions (session 25)

**DLC weapons, stage 1: the zone writer** (Next steps, first item; the design and its reasons are in
Decisions session 24).
- **The generator** (`tools/gen_db_save.py`, rerun when db_load.cpp changes; `--check` fails when the
  output is stale): parses db_load.cpp's Load_* bodies (a small statement parser for the decompiled
  subset), drops the asserts and the loader's side effects (textures, shaders, vertex declarations and
  buffers, water picmip) and emits Save_* that walk the loaded objects in the loader's order. Patterns:
  `Load_Stream` -> `Save_Stream` (copy the source bytes at the loader's block position); `if (p) { p =
  AllocLoad_X(); ... }` -> the field becomes -1 and the data follows; `if (p == -1) {...} else
  DB_ConvertOffsetToPointer` -> an alias to an already written range holding [p, p + size) (the size from
  the callee's first Load_Stream), else -1; asset pointers (the DB_InsertPointer functions) -> a template:
  -2 and a 4-byte slot in block 4 the first time, an alias to the slot after (what
  DB_ConvertOffsetToAlias reads), and `DB_SaveResolveAsset` may substitute the header (stubs). The
  generated code lives in `namespace dbsave` with its own `var*` globals. Hand-written (db_save_stream.cpp):
  Save_XString (inline bytes or an alias), the zone's header (XAssetList, the script string list, the root
  asset array; Load_XAssetListCustom's order), the file (36-byte XFile with the blocks' high-water marks,
  "IWffu100", 473, one zlib stream, blocks 2-3 after the stream as Load_DelayStream reads them).
- **The runtime**: block positions, the push/pop stack and the temp block's rewind exactly as db_stream.cpp
  (offsets; blocks are 4096-aligned, so aligning offsets is aligning addresses); every written source range
  is recorded (source address -> block, offset, file offset); a pointer field is patched in the newest copy
  of the range holding it (the closest start at or before it, 8 steps back at most, else the write fails
  with "pointer field ... was not written"); aliases never point into the temp block. Script strings: a
  dry run collects them (the list precedes the assets), entry 0 is null. Dry runs repeat while the tool
  adds roots.
- **Tool mode** (db_save_tool.cpp, `g_dbSaveToolMode`, only on a dedicated server: a client would create
  the sources' shaders and world vertex buffers): DB_AddXAsset keeps each loaded header in a private copy
  instead of linking it (no pool limits, no clashes, the world types never reach `cm`/`comWorld`/...;
  their headers are left in temp memory), Load_SndBankAsset/SndPatchAsset skip SND_AddBank/AddPatch,
  Load_FxEffectDefFromName keeps the name, Load_Texture copies the load definition with its pixels (they
  sit in temp memory). Up to 3 source zones load together, each with its own flag pair (0x4000000,
  0x1000000, 0x400000; DB_LoadXAssets inserts each localized zone with the pair's lower flag right before
  its zone, so with one pair for all, the unload freed them out of stack order: "free does not match
  allocation"). The tool loads common_mp normally first (a map-less dedicated server has only
  code_pre_gfx_mp, code_post_gfx_mp and patch_mp) to compare against the zones that stay loaded in a
  game.
- **What goes in the zone**: the zombie zones refer to shared assets by stub (",name": techsets, effect
  materials, `viewmodel_hands_no_model`, `$identitynormalmap`, `weapon/shellejects/fx_pistol`), held by
  common_zombie. A stub whose name multiplayer has stays a stub; otherwise the tool writes the real asset
  from the source zones (`gfx_fxt_fx_spark_field`, the muzzle flash's, was a default material without it:
  "non effect material ... used for effect"); one in neither is listed as a warning. A full asset
  multiplayer has is written as a stub (zeroed header, ",name"), so the zone never overrides a
  multiplayer asset. Weapons bring their szXAnims; effects named by effects become roots; every string the
  zone holds (XStrings, script strings: weapon sound fields, notetrack sound maps, an alias's secondary
  name, display names) that names a sound alias or a localized string of the sources, and that
  multiplayer lacks, brings it: the aliases go into one bank named after the zone (`<zone>.all`; its index
  is SND_FindInIndex's coalesced hash: slot id % count, chains through free slots, every slot used), the
  localized strings (the English zone's) as roots. `writezone <out> <zones> all` writes every asset but the
  banks and what multiplayer has, sorted by type and name: the writer's coverage test.
- **Engine facts found**: model index and vertex buffers are built only when a map's client game starts
  (CL_FinishLoadingModels -> R_OptimizeAllModels); a zone loaded during a match drew without them
  (r_draw_xmodel.cpp:332 asserts, Metal "indexed draw without indices"), so `loadzone` now builds them
  (DB_UpdateDebugZone, not on a dedicated server). `vid_restart` doesn't. A zone loaded with `loadzone`
  dies at the next map start (Com_LoadLevelFastFiles frees 0x4000000's group, which spans 0x200000 up).
  The Thompson's textures stream from main/iw_38.iwd (Rezurrection's iwd: `w_tommy_n`, `~-gw_tommy_c`,
  ...), not from the zone: session 24's "the WaW guns' textures are not there" was wrong for it. `give`
  of a weapon outside the weapon table registers it late (cg_servercmds_mp.cpp:811, "We do not support
  modified weapon files", ignored). The command line splits at '+', so the zone list uses ','.
- **Checked**: the Thompson zone (prototype + common_zombie, 22 roots, 86 script strings) in an mp_nuked
  listen match: viewmodel, raise, fire (muzzle flash in the frames at the shots, tracers, impacts),
  reload with the magazine animation, the name "Thompson", ammo; with `snd_solo_alias_substring
  thompson`, fire -1.4 dBFS peak (RMS -14), the magazine sounds as two peaks in the reload. Loading a
  written zone in tool mode and writing it again gives the same bytes: the Thompson zone, and all of
  zombie_cod5_prototype (1760 roots, 16 MB, 690 images with their pixels, 21 weapons, 249 localized
  strings, rawfiles, destructibles, physics presets and constraints); the client loads the latter at the
  main menu in 1.2 s with no error, and in a match. ASan: the tool (identical output to the release
  build) and the client (loadzone, give, fire, reload, a second loadzone) report nothing. Swapping a
  `loadzone` zone whose weapon is in the player's hands for another left its models without buffers
  (the default stubs): `loadzone`'s limit, not the zone's. A tool run takes ~4 s.
- `tools/ff_info.py <zone.ff>` prints a zone's header, block sizes, script strings (`--strings`) and
  asset types (`--assets`).

## Decisions (session 24)

**Reflections and anisotropic filtering** (the user: "all textures look glassy or glossy, or like
covered by ice", the M16 "extremely shiny", on GL too; their reference: a 2010 console-video frame of
Nuketown's backyard holding the M16, dark gunmetal where ours was chrome).
- **Shared code**: Metal, Metal with `KB_MTL_GLCOMPAT=1` and GL give the same look at mp_nuked's views.
- **Ruled out**: the texture setting (the user's config: `r_picmip 2` for colour, normal and specular
  maps, i.e. "Low", most likely the recommended set for an unknown GPU; full quality changes little:
  mean |Laplacian| 19.7 -> 20.5 at view c, 29.1 -> 30.0 at the overview); streaming (temporary logging:
  ~500 high-mip loads replace the in-zone mips, a 512x512 file loads as 128x128 at picmip 2 and 512x512
  at 0; `r_streamLog`'s "allocation complete. bytes=0" is the request before its read; `r_streamTaint`
  is registered but unused); the shader translator's maths (log/exp base 2, abs in pow/rsq/log, the
  source modifiers); the reflection probe choice (drawsurf bits 25-27 as the readers take them).
  `$blacktransparent`, `$g16r16` and `$r5g6b5` are missing from the PC zones (data, as on Windows), so
  `r_specularRoughnessMap 1` ("Matte Smooth") binds garbage: don't use it to judge specular.
- **The M16's shading** (temporary logging of each pass's arguments): technique
  `pimp_technique_lprobe_*`, pixel shader `pimp_shader_lprobe_*` (translated with
  tools/shader_test.sh from bytecode cut out of common_mp). The specular map's alpha is roughness
  (specular power 2^(13 - 13a), probe LOD 8a); the probe sample is decoded as rgb^2 x 8 and scaled by
  the model lighting volume (rgb^2 x 31.875), the `_e` map's RGB (reflection mask) and the material's
  `envMapParms` (1, 1, 1, 0.5 on the M16). The viewmodel's sun light is scaled by the vision file
  (mp_nuked.vision: `r_primaryLightUseTweaks 1`, diffuse 2.9825, specular 1.5): its brightness is data.
- **Cause**: `FillCaps` (mtl_device.mm, after the GL layer's `FillDefaultCaps`) reported `TextureCaps`
  and `TextureFilterCaps` 0. `R_StoreDirect3DCaps` derives `canMipCubemaps` from
  D3DPTEXTURECAPS_MIPCUBEMAP; without it `Image_CreateCubeTexture_PC` makes one level and
  `Image_UploadData` drops the rest (mp_nuked's probe: 256x256 DXT1, 9 levels in the zone, 1 used), so
  the shaders' `texldl` at LOD 8 x roughness read level 0 and every reflective surface was a mirror.
  Without D3DPTFILTERCAPS_MIN/MAGFANISOTROPIC, `R_SetTexFilter` turns the materials' 2x/4x anisotropic
  sampler states into plain linear. Both bits are reported now. The GL layer had seen the symptom
  (`GLCubeTexture::maybeGenMips` builds a chain for single-level uncompressed cubes; the probes are
  DXT1) and samples unmipmapped by design (session 13's deviations): left as is, and compat mode ignores
  anisotropy so it still draws like GL.
- **Checked**: the M16 at the reference spot: carry handle's mean luminance 105 -> 106, mean
  |Laplacian| 34.9 -> 31.0 (the mirror reflections gone, satin metal); ground at view a 51.8 -> 54.8,
  overview 30.9 -> 32.3. fps at views a/c/overview, two runs each: before 552/755/496, 559/762/497;
  after 577/751/516, 558/727/501 (each bit alone in the same range). API and GPU validation quiet;
  compat vs GL within the noise; all 26 maps' overviews drawn (234-773 fps), no `[mtl] BUG` or error.

**DLC weapons** (the user: "make the DLC weapons available in classes/custom"; "I know Thompson is one
of the DLC weapons"). Investigated and designed; nothing built yet (Next steps, first item).
- **What they are**: the map packs added MP maps only. The Thompson and the other WWII guns are
  Rezurrection's remade World at War zombie maps' (zombie_cod5_prototype/asylum/sumpf/factory.ff):
  `zombie_thompson`, `zombie_type100_smg`, `zombie_stg44`, `zombie_fg42`, `zombie_bar` (+`_bipod`),
  `zombie_m1garand`, `zombie_gewehr43`, `zombie_m1carbine`, `zombie_kar98k`, `kar98k_scoped_zombie`,
  `zombie_type99_rifle`, `zombie_shotgun` (M1897), `zombie_doublebarrel` (+`_sawed`); factory also has
  their Pack-a-Punch `_upgraded` versions. Wonder weapons per map: tesla_gun (Wunderwaffe),
  thundergun, freezegun, humangun (V-R11), sniper_explosive (Scavenger), shrink_ray (31-79 JGb215),
  microwavegundw (Zap Guns); common_zombie has ray_gun_zm and mp40_zm. No MP zone has any of them
  (models, anims, sounds). The MP scripts register an MP40 care-package weapon (`mp40_blinged_mp`,
  `mp40_drop_mp` in _supplydrop.gsc) whose weapon def is in no MP zone either (only iw_00.iwd's text
  files, like `ks23_mp`). statsTable (code_post_gfx_mp) offers every MP primary; empty rows: 11, 25, 36,
  41, 46, 52, 58-61, 66, 72, 79-87.
- **Feasibility**: SP and MP share the zone format (one XAssetType enum, `GAMEWORLD_SP` included).
  `loadzone zombie_cod5_prototype` at the main menu parsed in step through weapons, GfxWorld, ComWorld
  and clipmap until "Exceeded limit of 1 'game_map_sp' assets"; `loadzone common_zombie` stopped at
  "Exceeded limit of 5100 'xanim' assets" (MP's 3355 + its 1882). Zone memory (XFile blocks):
  prototype ~62 MB, asylum ~76, factory ~102, sumpf ~105; factory alone has 9 of the 12 guns. A build
  would load the chosen zombie zones at map start keeping only the weapons (world types not linked),
  give them MP names, add statsTable rows, and retune damage if the user wants.
- **The user's answers**: all of them (Next steps, first item), damage like MP, bots too.
- **Why not load the zombie zones themselves**: zone blocks come from one 328 MiB pool
  (physicalmemory.cpp:26, 0x14800000; `DB_AllocXBlocks` -> `_PMem_AllocNamed`), of which a match uses
  ~195 MB (code_post_gfx_mp 3.4, patch_mp 10.3, common_mp 101.6, a map ~80); one zombie zone fits, the
  ~7 needed (prototype, asylum, sumpf, factory, coast, temple, moon, + common_zombie) don't, and the pool
  sits in the 1536 MiB in-image Ptr32 region. Their world types map to the live globals (`cm`,
  `comWorld`, `gameWorldMp`, `s_world`: a second world memcpy's over the live one; game_map_sp has no
  pool) and can't be unloaded while a world is in use (Sys_Error); a skipped GfxWorld still creates its
  vertex buffers (db_load.cpp:7982). Pools (db_registry.cpp:45-145): xanim 5100, xmodel 1000, material
  4096, techset 2048, image 4608 (the streamer's arrays match it), sound 32, weapon 2048, localize 10240,
  fx 500, stringtable 576. Images, materials and models must be in the pools (streaming indexes the pools,
  model buffers are made for pooled models), anims are found by name (`szXAnims` -> BG_CreateXAnim ->
  DB_FindXAssetHeader), sounds by alias hash (SND_BankAliasLookup, newest bank first, 32 banks), display
  names through localize entries.
- **Weapon table** (bg_weapons_def.cpp): built at map init from statsTable, not from the asset pool:
  BG_LoadWeaponTable("_mp") (:458) loads `<reference>_mp` and every attachment combination of column 8
  for the groups smg, assault, cqb, lmg, sniper, pistol, launcher, special, shotgun, explosive, grenade,
  specialgrenade, inventory; a missing base weapon is a hard error (:567). Other weapons register only
  while `level.initializing` (precacheItem; the cheat `give` sets it, g_cmds_mp.cpp:209). The client
  rebuilds the same table and checks every weapon config string (2836+, 256 of them, "index name
  [variants]") by name and index (cg_weapons.cpp:7118-7180). 2048 variants, 11-bit network indices.
  statsTable columns (bg_unlockable_items.cpp:808-852): 0 index, 1 passive, 2 group, 3 name, 4 reference,
  5 count, 6 image, 7 desc, 8 attachments, 9 sortKey, 10 unlock level, 11 default classes, 12 cost, 13
  loadout slot, 14 unlock prestige, 15 unclassify-at; 256 items (uint8 in the class stats). The CAC menus
  list a group by column 2 with cost >= 0 and the slot (buildItemListForGroupName /
  buildItemListForSlotNameAndGroup), sorted by column 9; a cost < 0 ends the group's list
  (bg_unlockable_items.cpp:3031-3058). `_class.gsc`: a custom class's weapon is `reference` + `_` +
  attachments + `mp` (getFullWeaponName); weapon types (primary/side arm) only for indices < 99
  (`max_weapon_num = 99`); bots (`_bot.gsc`) pick any weapon row with the slot and a cost other than -1.
- **Zone flags** (db_registry.cpp:2094, 3054-3232): bare ints, a localized `<lang>name` zone with each;
  zones persisting across maps have a low flag below 0x4000 (code 1, dev 4, patch 16, common_mp 64 on
  the client / 256 dedicated, 1024 unused); loading low flag F unloads the low zones >= F; zones must
  unload in reverse load order (physicalmemory.cpp:214); max 4 zones a call, 32 in all.

## Decisions (session 23)

**`FxBoltAndSortOrder`** (fxprimitives.h; session 22's open item). Every `#if 0` IDA copy that
touches `boltAndSortOrder` (FX_SpawnEffect's bolt setup, FX_SpawnRunner, FX_UpdateEffectBolt,
FX_ShouldProcessEffect, FX_UpdateSpotLightEffect, FX_RewindTo, FX_SpawnElem's decal case,
FX_FirstEffectIsFurther) packs the word as: sortOrder bits 0-7; dobjHandle 8-18 (`0xFFF800FF`,
`>> 8 & 0x7FF`); temporalBits 19 (`0xFFF7FFFF`, `>> 19 & 1`); boneIndex 20-28 (`0xE00FFFFF`,
`>> 20 & 0x1FF`, `|= 0x1FF00000`, 511 meaning no bone); type 29-30 (0 none, 1 DObj
0x20000000, 2 dynent 0x40000000, 3 mark entity 0x60000000); rewindKill 31 (the `< 0` test).
absDynEntId overlays bits 8-28 (`& 0x1FFFFF`).
- **The struct**: `temporalBits:2, boneIndex:8` stored bone 511 as 255 and dropped the top bit of
  bones 256-510. The widths are now 1 and 9, in the same 32-bit word. clang's record dump gives the
  IDA bit ranges for i386 (`-mms-bitfields`) and natively; FxEffect's size is unchanged.
- **Two more slips in the rewritten code**: FX_SpawnEffect's dynent branch set `_type = 1` (a DObj
  bolt) where IDA writes 0x40000000 (type 2). FX_UpdateEffectBolt's dynent case passed
  `dobjHandle` (11 bits) where IDA passes `bolt >> 8` to the u16 `DynEnt_Valid` and
  `DynEnt_GetClientPose` (absDynEntId). Both fixed. FX_SpawnEffect also clears the temporal bit for
  every type now, as IDA does: the mark-entity branch kept the slot's old value, which was harmless
  because only DObj bolts read it.
- **The other live uses** match IDA and needed no change: FX_ShouldProcessEffect (`type != 0`),
  FX_UpdateSpotLightEffect (type, dobjHandle), FX_RewindTo (dobjHandle, rewindKill), the decal case
  (type 3 -> dobjHandle as the mark entity), FX_FirstEffectIsFurther (sortOrder). Nothing addresses
  the word through a raw offset.
- **What it broke** (temporary logging, mp_nuked listen server, 7 managed bots with grenades, the
  scripted player firing an RPG, 100 s per build). Every runner child of an unbolted effect went
  down FX_SpawnRunner's bolted branch (bone 255 is not 511), and FX_SpawnEffect(2047, 255) took the
  dynent branch, which stored type 1, dobj 255, bone 0. So the child was a DObj bolt to entity 255,
  killed or stopped on its first update (or, had entity 255 a DObj, glued to its bone 0). Lost this
  way: grenade explosions' flash and lingering smoke (`fx_grenade_flash`, `fx_grenade_smk_ling_w`),
  electric boxes' spark showers, and the ambient runners found on 6 of the 14 maps: tree snow
  (mp_array, mp_duga), shore-break waves (mp_crisis, mp_villa), blinking and shooting stars
  (mp_hanoi), spark showers (mp_mountain). Impacts (mark entity 1022, the world, or the entity hit)
  passed their mark entity on as a DObj handle with bone 255, so every impact's distant-dust child
  (`fx_distant_grain`/`_conc`) was killed. Effects bolted to an entity without a tag (bone 511:
  FX_SpawnBoltedEffect for defs that need no bolt update, CG_PlayBoltedEffect without a tag) failed
  FX_GetBoneOrientation on any model with fewer than 256 bones: silenced world muzzle flashes, glass
  hits on cars, thrown grenades' and tomahawks' trails (stopped mid-flight), Nightingale. Over 500
  bolts were lost in 100 s.
- **After**: children inherit the parent's bolt as in IDA (unbolted stays unbolted, a mark entity
  stays the same mark entity, a DObj bone the same bone, a dynent becomes unbolted). They spawn
  within 4.4 units of the parent's frame. The 63 bolts lost in 100 s are all entities that went
  away (dead players, exploded missiles, landed grenades). Screenshots at 2/s, same scripted throw:
  the grenade's dark smoke cloud is there half a second after the explosion, where the old build
  shows thin dust only. A sweep of the other 13 maps (45 s each) lost no bolts.
- **Not run in game**: the dynent bolt (FX_PlayBoltedEffect_DynEnt): burning debris
  (`destructibles/fx_dest_fire_trail_sm`, from a burning destructible's pieces or
  DynEntCl_FlameDamage) and CG_EntityEvent's dynent FX. A car blown up by a grenade threw no
  burning pieces, and `give ft_ak47_mp` gives no flamethrower (the `ak47_ft_mp` alt mode needs the
  class's action slot). Before the fix those were DObj bolts with the dynent id's bits as handle
  and bone: stopped at once, or attached to an unrelated entity. The fix follows IDA's masks.
- **Checks**: check_file.py natively and `--i386` (fx_system, fx_update, fx_sort: clean);
  audit_splitlocals.py: 0; a final 100 s run of the release build (7 bots, grenades, RPG): no
  assert, crash or `[mtl] BUG`.

**No launchers or killstreaks** (the user's request: "I don't want ANY nades, RPGs, grenade
launchers, or even airstrikes". Grenades, grenade launchers, the explosive crossbow and equipment
were already off: `kb_noGrenades`, Decisions session 16). The user's answers: all killstreaks, not
only the explosive ones; a launcher in a class becomes an AK-47; everyone, bots included. Both are
`kb_*` dvars, archived, default 1.
- **kb_noLaunchers**: `G_KbNoLauncherWeapon` (g_weapon.cpp, beside `G_KbNoGrenadeWeapon`) makes
  `G_GivePlayerWeapon` refuse every `WEAPCLASS_ROCKETLAUNCHER` weapon (and such an alt weapon), on
  every path. The script command `giveWeapon` (`PlayerCmd_giveWeapon`), which class loadouts
  (custom and default, `_class.gsc`) and the bots' random loadouts (`_bot.gsc`) use, gives
  `ak47_mp` in place of a launcher that isn't a carried killstreak weapon
  (`bIsCarriedKillstreakWeapon`). `G_InitializeAmmo` gives it starting ammo, and the scripts' later
  ammo calls on the launcher do nothing for a weapon not held. China Lake is grenade-class, so
  `kb_noGrenades` already covers it. The developer console prints "kb_noLaunchers: not giving X"
  once per weapon.
- **kb_noKillstreaks**: a `Scr_KbPatchSource` patch on `_globallogic.gsc`'s
  `if( getDvarInt( "custom_killstreak_mode" ) == 1 )`, the custom game mode's "killstreaks off",
  run after the gametype's start. It clears `level.killstreaksenabled` (and `hardpointsenabled`),
  which every award checks (`_hardpoints.gsc`: `giveKillstreakForStreak`,
  `giveKillstreakInternal`, `takeKillstreak`). The care package is a killstreak itself, so its
  contents never come either (`_supplydrop.gsc` gives some, like the carried weapons, with
  `GiveWeapon` directly). Wager modes already turn killstreaks off this way. The HUD's killstreak slots stay empty, as at any spawn.
- **Checked** (temporary logging of every script `giveWeapon`; mp_nuked listen server, 7 managed
  bots, the scripted player fighting, 180 s per run). With both dvars 0, the bots got RPG x8,
  Strela-3 x5 and M72 LAW x4, and killstreaks from 40 s on: RC-XD x10, the care package's sentry
  gun x4, Spy Plane x3. With the defaults, all 15 launcher picks (LAW 7, Strela 4, RPG 4) came as
  `ak47_mp`, with no killstreak in 174 s and no script patch warning. No assert or crash in either
  run. check_file.py natively and `--i386` (g_weapon, g_client_script_cmd_mp, g_main_mp,
  cscr_parser): clean; audit_splitlocals.py: 0.
- **Not changed**: the Create-a-Class menus still offer grenades, equipment and launchers (a pick
  is refused or swapped at spawn); destructible cars and barrels still explode.

## Decisions (session 22)

An audit of the code on the job-queue workers for session 21's pattern: data written with plain
stores, then a pointer, flag or count stored plainly, read by another thread without a lock or an
acquire. Fixes are `std::atomic_thread_fence` release before the publishing store and acquire after
the unlocked read that found it (no code on x86), commented once per file.

- **The primitives are full barriers** (disassembly of build_macos): `_InterlockedCompareExchange`,
  `_InterlockedExchangeAdd` and `InterlockedExchange` (`__sync_lock_test_and_set`, documented as
  acquire only) compile to LSE `casal`, `ldaddal` and `swpal`, both acquire and release; so
  `tlAtomicMutex::Unlock`'s exchange on a local before its plain `ThreadId = 0` is a release, and
  the minspec, tlAtomic and tlAtomicReadWrite mutexes are sound. A plain store that releases a
  hand-rolled lock is not (`DObjUnlock` was `str wzr`).
- **Clean**: the job queue (queue nodes under tlAtomicMutex, interlocked counts, session 5's fences
  in `jqPoll`/`jqFlush`, which also end `jqAssistWithBatches`' waits; conditional batches are never
  used). Physics: the phases are separated by `IW_task_manager_flush` (= `jqFlush`), and inside a
  phase the env-query prolog merges under a mutex, the cluster list, pair lists, contact lists and
  wheel list are CAS pops/pushes, the transient and slot-pool allocators CAS (tagged stack), the
  GJK cache, bpei database and auto rigid bodies take read/write locks, auto-activation is
  double-checked under `g_bp_auto_activate_mutex` (the unlocked test only skips work), the
  constraint solver works per island (shared static bodies copied into per-solver temps). Streaming
  (r_stream's IO-thread fences complete; update batches serialised by the executing count; the
  static-model/surface modules are never submitted), sound (the worker's queues, streams under
  per-stream locks, the OpenAL backend under its lock, the occlusion thread under
  `CRITSEC_SOUND_OCCLUSION`), demo save (`demo_save_smp` 0: synchronous), cinematic (Bink stub),
  dpvs static/dynmodel/dynbrush, r_model_skin, r_skin_cached_staticmodel, dyn_smodel_drawstate,
  water (try-lock).
- **Scene-entity bounds** (r_model_pose.cpp `R_UpdateSceneEntBounds`): the thread that wins the
  0->1 CAS on `cull.state` (a dobj_skel/dobj_skin/r_dpvs_entity/r_spot_shadow_ent batch or the main
  thread) computes the pose (`skel.mat`, bones), `cull.lods`, mins/maxs and the radius, then stores
  `cull.state = 2` plainly; the others spin on it and skin with `I_dmaGetDObjSkel(obj)`. A stale
  `skel.mat` (last frame's, or 0) or half-written bones went to the skinner. Release before the
  store, acquire after the spin; acquire in the two unlocked readers of the bounds
  (`R_InitialEntityCulling` in r_dpvs.cpp, the sceneent batch in r_dpvs_sceneent.cpp).
- **`DObjUnlock`** (dobj_utils.cpp): `locked = 0` releases the CAS spinlock that the bounds threads
  (`CG_DObjCalcPose`), FX bolt workers and main-thread traces take on a DObj; the next owner could
  see the skeleton's `timeStamp` and bone bits current with the bones not written. Release fence.
- **Glass renderer lock** (glass_renderer.cpp): the six live `rendererLock.lock = 0` (Reset,
  RemoveGlassShards, Update, GenerateVerts, DoMaintenance x2) are plain; fx_draw's
  `GlassCl_GenerateVerts` (Update, GenerateVerts, DoMaintenance: `smpGlass` 0) and the main
  thread's `RemoveGlassShards` (snapshot parsing) contend for it over std::list shard groups.
  Release fences.
- **Effects** (EffectsCore):
  - The exclusive iterator (`FX_RunGarbageCollectionAndPrioritySort`, `FX_SortEffects`) ends with
    plain `firstActiveEffect`/`iteratorCount = 0` stores after rewriting `allEffectHandles` and
    freeing effects (`def = 0`); the last cooperative iterator runs the GC on its own thread (often
    a worker) and `FX_SortEffects` runs in the ppu job, so the next iterator could read a stale
    handle list and draw a freed effect. Release fences (3).
  - `FX_SpawnEffect` takes the handle the GC wrote into a freed slot before advancing
    `firstActiveEffect`: acquire before reading it (only a nearly full ring reaches those slots).
  - A spawner (any thread) fills the effect, then CASes `firstNewEffect`; every loop over
    [`firstActiveEffect`, `firstNewEffect`) read the effect's `def`/`owner`/status ordered only by a
    control dependency (arm64 may satisfy them first). Acquire at the top of each loop body
    (draw, sort, stop/kill, stale-attached, rewind, profile) and after the GC's and
    `FX_RunPrioritySorting`'s snapshots: the GC read a just-published effect's reference count, and
    a stale 0 would have freed a live effect. `FX_Update`'s snapshot is read before its Begin CAS
    (a full barrier) and needs none.
  - Elements and trails are linked into an effect with plain stores and the drawers walk them
    without the effect's lock (the updaters take it): release before `firstElemHandle`
    (`FX_SpawnElem`) and `firstTrailHandle` (`FX_SpawnEffect_AllocTrails`), acquire in
    `FX_DrawNonSpriteEffect`, `FX_DrawSpriteEffect` and `FX_DrawTrailsForEffect`. The trail head was
    published before the trail's own fields (wrong on x86 too); it is now stored last.
  - The spot light's count was incremented before its handle was stored (effect and element; wrong
    on x86 too: a reader could see the count and the old handle): handle first, then the
    interlocked increment, and acquire where a count check is followed by the handle
    (`FX_DrawSpotLight`, `FX_UpdateSpotLight`, `FX_UpdateSpotLightEffectPartial`,
    `FX_FreeSpotLightElem`).
- **`SND_CommandPush`** (snd_public_async_q.cpp, session 4's fences): the acquire fence sat before
  the tail compare it pairs with, so the slot test in the following assert could read a stale
  command; moved after the compare, as `SND_NotifyPush` has it.
- **Not fixed, benign or unreachable**: `FX_ImpactMarkWithinRange` reads `camera.origin` after
  `isValid` (the writer orders them with an interlocked exchange; a stale origin moves one
  mark-range test by a frame); `g_loadingAssets`/`g_zoneNames[].loaded` (readers then take the
  DBHASH or stream locks; ui_viewer only); `g_snd.init` read by the stream thread (first
  `SND_Init` only); the lost-device rendezvous (`g_isRecoveringLostDevice`/`g_mayRecoverLostAssets`,
  plain flags spun on both ways) is unreachable, no backend returns `D3DERR_DEVICELOST`.
- **Testing**: none of these was reproduced (nothing stresses them like `KB_PHYS_STRESS` does the
  broad phase). Release client, 7 managed bots, scripted player sprint-firing: `KB_PHYS_STRESS=2500`
  300 s (mp_nuked, then mp_array after a 3-minute TDM) and 330 s of 1-minute TDM (mp_nuked,
  mp_array, mp_cracked, mp_crisis): no crash, assert or `[mtl] BUG`. fps (bench_views.py --bots,
  mp_nuked's 8 views, HEAD and new alternating, geometric means): HEAD 633.8, 646.5, 631.0; new
  620.9, 642.2, 649.1. check_file.py natively and `--i386`: the 11 files clean;
  audit_splitlocals.py: 0. Races that are races on x86 too are under Open items.

## Decisions (session 21)

The physics crash seen once in session 20 (Decisions session 20, "Found, not fixed").

- **Reading the crash**: the crash handler's `backtrace()` starts after `_sigtramp` with the
  interrupted link register, not the faulting pc: a fault inside a frameless leaf shows its caller
  (lldb on a later crash: pc in `set_local_query_info`, the handler's first frame its caller).
  Session 20's two frames at `bp_env_jq_batch_function2+516` were that link register and the frame
  record x29 pointed at, `collide_bpg_environment`'s own: the fault was inside
  `collide_bpg_environment` before its first call, walking the query's environment list, not a
  register corrupted across its return.
- **The cause**: double-checked locking without memory ordering. In the broad phase's environment
  pass each thread's `standard_query::query` looks brushes, terrain partitions, user bodies,
  entities, dynents and glass up in the bpei database; the first thread to need one builds it
  under that entry's mutex in the transient collision memory (reset every step, so it holds last
  step's objects) and stores the pointer in `m_data`; the others test `m_data` without the lock
  and use the object. x86 makes stores visible in order; arm64 may show `m_data` before the
  object's fields, so the reader got last step's contents: a garbage environment object in
  `collide_bpg_environment` (session 20), an `entity_bpi_header::m_mat` of -1 (the stress below).
  A query box covering the map makes all three threads (two workers and the flushing main thread)
  build the same hundreds of objects in the same step, hence the leaf-list overflow just before.
- **The fix** (phys_collision.cpp): `std::atomic_thread_fence` release before each `m_data` store
  and acquire after an unlocked read that found it set, at the five sites (brushes, partitions and
  user bodies in `standard_query::query`, `create_entity_bpi`, `query_create_prolog` and
  `query_create_epilog`); no code on x86, like session 19's fences. `bp_env_jq_batch_function2`'s
  loop reads the shared `g_bpb_list_cur` once per pop (its condition re-read it; the compiled code
  happened to read it once).
- **Reproducing it**: the ASan client with forced map-wide queries (three 180 s runs: queries only,
  queries and group boxes, and every dynent woken) never failed; ASan's slowdown hides the race.
  The release client did: `KB_PHYS_STRESS=<units>` (phys_broad_phase.cpp, CLAUDE.md) grows the
  query and own box of every cluster holding a group (ragdoll, vehicle, compound dynent) by
  <units> (a quarter of that vertically) and wakes every dynent such a group reaches without the
  per-body test. With 2500 on mp_nuked (7 managed bots, the scripted player; ~1,200 environment
  objects per query) the old code crashed in 3 runs of 3, at 35, 54 and 65 s
  (`create_entity_bpi` -> `set_local_query_info` each time, twice on the main thread helping the
  flush); the fixed
  build ran 5 x 240 s (about 1.3 million map-wide queries) without a crash, and the fixed ASan
  client 180 s of it without a report.
- **`dynent_hits`** (phys_main.cpp): `dynamic_ent_aa::auto_activate` appended to the 10-entry list
  with no bound (`destructible_ent_aa` has one). On arm64 entry 10 is the low half of
  `num_dynent_hits`: the 11th hit in a step replaced the count with a dynent id, the next ones wrote
  2-byte ids across the physics globals after it (the task manager's batch, `g_physics_system`...),
  and `process_dynent_hits` then damaged that many garbage dynents. Bounded like the
  destructibles; a dynent past the tenth is woken by the next step's fresh callback.
- **ASan's `detect_stack_use_after_return=1`** (frames on a heap with their own redzones) found two
  stack bugs at map load that the normal layout hides: `R_InterpretSunLightParseParamsIntoLights`
  took the sun colour's green and blue from `sunParse[1].sunSettings[0].skyColor`, 136 bytes past
  the struct (now `sunDiffuseColor[1..2]` times the intensity, as the diffuse colour below it;
  `sunLight->color` and `sunColorFromBsp` feed no shader, so nothing renders differently), and
  `CG_InitExposure` wrote a terminator 255 bytes into `CG_Init`'s 64-byte `mapname` (`r[255] = 0`
  before copying; now `I_strncpyz` into its own buffer). With both fixed, the ASan client with that
  option over 330 s of 1-minute TDM (mp_nuked, mp_array, mp_cracked, mp_crisis): no report.
- check_file.py natively and `--i386`: the five files clean; audit_splitlocals.py: 0.
- **The crash handler** (linux_main.cpp, after the above): installed with `sigaction` and
  `SA_SIGINFO`, it prints the signal, its code and fault address (`si_addr`), the interrupted
  pc, lr, fp and sp from the context (pc and lr symbolized by `backtrace_symbols_fd`), and an
  `atos -o <exe> -l <load address> <pc> <lr>` line, before the backtrace (CLAUDE.md). The load
  address is looked up with `dladdr` when the handler is installed (not async-signal-safe).
  Linux i386 prints eip, ebp and esp from `uc_mcontext.gregs` and the image's path and base;
  that branch has never been compiled (the `--i386` check defines `__APPLE__` and skips it).
  Checked on the dedicated server: `+crash` (`Com_Crash_f` is a frameless `brk`) shows
  `Com_Crash_f + 0` as the pc while the backtrace starts at its caller; a temporary write to 0x10
  in it gives SIGSEGV, fault address 0x10 and its pc, and the printed atos line resolves both to
  file and line; `kill -SEGV` on a running match gives the main thread's pc in
  `__semwait_signal`.

## Decisions (session 20)

Stage 1 of the session-19 plan: session 19's commit (8794ab5c) verified.

- **Screenshots, default mode**: bench_views.py --map all --shots, 78d18742 (a worktree build) twice
  and the new build once, the screen awake (`caffeinate -u` every 50 s): 26 of 33 images within
  the two reference runs' noise (pixels over 24); the other 7 traced to what varies between runs,
  not to rendering: the team emblem (`autoassign`), the fps counter, the objective text, and the
  match clock. The new build reaches the map ~2 s sooner, so at the screenshot its match clock
  is 2 s further (9:51 against the references' 9:53): mp_cracked's drifting smoke (1.6% with the
  HUD masked), mp_stadium's flag, mp_kowloon's rain. mp_area51's one wall swaps between a
  blurred and a sharp, seamed version from run to run: texture streaming; four more runs with
  30 s at the view (two per build) put the sharp one in the reference runs this time.
- **Screenshots, `KB_MTL_GLCOMPAT=1` against two GL runs**: the differences over the GL-vs-GL noise
  are the HUD (emblem, fps counter, score box) and particles, which step per frame (Metal 300-750
  fps, GL 80-120): 0.1-1.8% with the HUD masked (mp_mountain's gondola 1.8%).
- **The validation layer** (`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`): mp_array, mp_kowloon and
  mp_villa with 7 bots, mp_cosmodrome, mp_radiation and mp_zoo: no message, no `[mtl] BUG`.
- **ASan client** (`MTL_DEBUG_LAYER=1`, 7 managed bots, the scripted player sprint-firing and
  meleeing, 1-minute TDM): 330 s, mp_nuked -> mp_array -> mp_cracked -> mp_crisis: no report, no
  BUG, no assert.
- **fps** (bench_views.py --bots, mp_nuked's views A-F, sky, overview; reference and new
  alternating, the display state the same in all four, presented = shown): reference
  521/677/695/744/648/704/740/415 and 520/651/701/719/655/685/682/415, new
  524/651/650/704/657/605/681/405 and 520/664/691/724/651/686/725/418: the same. Default-mode
  screenshot runs, every view's geometric mean: reference 496 and 507, new 506.
- make_portable.sh rerun (the game folder's copy and launcher).

The user's requests at the start of the session (in their words: "remove the version text at
the bottom right, and also, the crosshair still isn't static, it should be like my
counterstrike one, as much as possible"):

- **What "still isn't static" was**: session 17's crosshair kept the original's hiding (weapon
  raise, drop and switch, reload, melee), its red colour over enemies, and a size in degrees,
  so its pixels changed with the FOV; it was drawn through the 640x480 placement (filtered
  texture edges).
- **The user's CS settings**: their CS:GO port's config (~/Documents/game/csgo/cfg/config.cfg
  and ~/Documents/Games/csgo/csgo/cfg/config.cfg, both written that night): cl_crosshairstyle 4
  (classic static), size 2.5, gap -1, thickness 1, colour 5 = 255 255 255, alpha 255,
  usealpha 0, no dot, no outline. (Steam's CS2 config from July has style 4, size 3, gap -2,
  green, usealpha 1: older, not used.)
- **kb_staticCrosshair** draws `CWeaponCSBase::DrawCrosshair` of the user's CS port
  (~/Documents/CounterStrike_AA, game/shared/cstrike15/weapon_csbase.cpp) for style 4: the gap
  `(int)(4 + cl_crosshairgap)` pixels, the lines `RoundFloatToInt(YRES(size))` long and
  `max(1, RoundFloatToInt(YRES(thickness)))` thick (YRES: x * height / 480; RoundFloatToInt:
  lrintf, halves to even), the same rectangles, the dot and outline as CS; screen pixels from
  the view's real viewport (`R_AddCmdDrawStretchPic`). At 1280x720: lines 4 pixels long and 2
  thick, an 8-pixel gap. The look is archived dvars named after CS's: `kb_crosshairSize`,
  `kb_crosshairGap`, `kb_crosshairThickness`, `kb_crosshairColor` (r g b a), `kb_crosshairUseAlpha`,
  `kb_crosshairDot`, `kb_crosshairOutline`, `kb_crosshairOutlineThickness`.
- **Additive like CS**: with usealpha 0 CS adds its colour at alpha 200 (`whiteAdditive`). BO's
  `white_additive` is a 3D material (the 2D path refuses it: it uses the depth buffer); the 2D
  materials with additive blending all have textures (a probe of every loaded material). Of
  those, `light_corona` (2d technique, ONE/ONE) is pure white at its centre: a quad whose four
  UVs are its centre adds exactly the colour (read back on black: 0.25, 0.5, 200/255, 1 gave 64,
  128, 200, 255; on grey 0.25 + 200/255 gave 255). The crosshair is that, with the colour times
  200/255; `kb_crosshairUseAlpha 1` blends `white` instead, as CS does.
- **When it shows**: wherever the original drew a crosshair at all (alive, a weapon, the HUD on,
  not hardcore, `cg_drawCrosshair`, not in the killstreak map, not flashbanged; vehicles and
  turrets keep theirs), but also during weapon raise, switch, reload and melee (the original
  hid it there); it hides while aiming down the sights (CS hides it scoped; only with
  `kb_noAds 0`). White always (no enemy red), no fading, no movement.
- **The shots**: stage B locked a human player's spread to the crosshair (1 degree, the old
  crosshair's gap). The CS crosshair's gap is in pixels: its lines start 4 pixels from the centre
  at the user's 1280x720 and FOV 80 (tanHalfFovY 0.63), atan(4 / 360 * 0.63) = 0.40 degrees, so
  KB_CROSSHAIR_SPREAD is 0.4 (5 pixels at FOV 65, still inside the lines). Shotguns keep their
  pellet spread. At 1 degree shots landed ~10 pixels out, past the 8-pixel ends of the CS lines.
- **kb_noVersion** (archived, default 1): `CG_DrawVersion` (the "Call of Duty Multiplayer -
  Release COD_T5_R MP build 7.0.61 ..." line at the bottom right of menus and matches) is not
  drawn. The main menu's build number (`UI_DrawBuildNumber`, top right) is untouched.
- **Checked**: the client on mp_nuked with 7 bots and the scripted player firing, reloading,
  switching weapons, sprint-firing, meleeing and leaning, 75 s, a screenshot every 150 frames:
  every one of the 245 after the spawn has the crosshair's exact pixels (each bar pixel >= 199
  in every channel, the pattern above), none before it (loading screen, menus); no version text.
  With `kb_staticCrosshair 0 kb_noVersion 0`: the original crosshair and the version text.
  fps with bots: 518/687/703/743/664/711/727/444 and 515/673/696/724/658/711/744/422 (the
  same). ASan client (`MTL_DEBUG_LAYER=1`, bots, 250 s over three maps): no report. Every
  changed file through check_file.py natively and `--i386`; audit_splitlocals.py 0.

Leftover 4 (CPU at view A):

- **Profile** (bench_views.py --views a --sample 10, no bots, the user's settings: the game
  folder's config, 1280x720 fullscreen, no AA, r_picmip 2, with r_vsync 0 and com_maxfps 0; two
  runs): the render thread busy 98%; `RB_PatchModelLighting` 9.6-10.2% inclusive: the engine
  locks the whole model lighting volume (256x256x4 A8R8G8B8, 1 MB) without DISCARD every frame
  and rewrites a few 4x4x4 blocks; the lock allocated and zero-filled a 1 MB vector (`__bzero`
  1.4%), copied the kept bytes into it (`lockBox` 3%), and the unlock copied it into a new
  MTLBuffer (`newBufferWithBytes`, `copyToTexture` 6.2-6.9%). The occlusion-query polling
  (`GetData`, `RB_HW_ReadOcclusionQuery`) was <0.1% at view A (session 13's 7% is gone; at the
  light view D it is ~4% with `pthread_self` from `onRenderThread`, but there the render thread
  waits 72% of the time: the engine's frame pacing), so it stays. The shader constants'
  `setVertexBytes` copies (3.3%) are the D3D9 constant model's.
- **The fix**: a lock of a level whose bytes are kept (last written through the GPU) returns the
  kept buffer itself (no allocation, no copy in; the lock's data is uploaded from it at unlock),
  and `copyToTexture` copies into a staging buffer per frame in flight (reused after the frame's
  semaphore wait, grown as needed; a level over 4 MB, a streamed mip, gets its own buffer as
  before) instead of a new MTLBuffer per upload. One 1 MB copy a frame is left (the GPU reads the
  staging copy while the engine patches the next frame's).
- **Measured**: `RB_PatchModelLighting` 2.4% (two runs), view A sampled 553/549 -> 577/582 fps.
  bench_views.py --bots, alternating with 0a363601: reference 570/689/652/701/678/684/696/442 and
  545/709/704/688/688/716/723/456, new 595/706/734/732/657/715/760/471 and
  591/698/700/729/670/703/725/487 (view A +6%, overview +6%; the screen locked in all four).
- **Checked**: screenshots of mp_nuked's views and the overviews of mp_array, mp_hanoi,
  mp_kowloon, mp_villa, mp_radiation, mp_cracked and mp_mountain against two runs of 0a363601:
  with the HUD masked the scene is the same (mp_hanoi and mp_cracked 0.00-0.03%; the rest the
  score box and fps counter past the mask, mp_kowloon's rain, particles), so the lighting volume
  is intact. `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`, mp_nuked's views with bots: no message.
  ASan client (bots, the scripted player, 1-minute TDM over mp_nuked, mp_array, mp_cracked):
  no report.

Leftover 3 (frames not shown without vsync) is not measured: the screen was locked (above). To
do with the screen unlocked and the user away: instrument (temporarily) the time from the input
sample (`CL_CreateNewCommands`) through the frame's hand-off (`R_IssueRenderCommands`, per
`s_backEndData` slot) and Present to the drawable's `presentedTime`, and count frames rendered
without a drawable; then compare 3 drawables (now), 2, presenting from the render queue without
the second queue's copy, and `com_maxfps` caps, and put the numbers to the user in plain words.

Stage 3 (the pipeline cache):

Stage 3 of the session-19 plan: the self-filling pipeline cache (stage 2, the shader file, waits for
the Metal Toolchain download, which needs the user's final yes).

- **What it is**: `players/metal_pipelines/<map>.bin` under the home path (fs_h; test runs with a
  scratch fs_h never touch the user's), one MTLBinaryArchive per map (and `menus.bin`) with the
  compiled code of the pipelines that map drew and of their pixel functions specialised for the
  alpha test (`MTLFunctionDescriptor.binaryArchives`, the pipeline descriptor's `binaryArchives`).
  At startup every file is opened for lookups (`...FailOnBinaryArchiveMiss` first, then the
  usual compile on a miss). Everything a map draws (from the files or compiled) is listed, and at
  the map's end (`CL_ShutdownCGame` -> `g_kbRendererLevelEnd(cgs mapname)`, the render thread's next
  Present, or the device's destructor at a quit) the list goes to a writer queue that adds it to a
  new archive and writes it as that map's file (a temporary file renamed: a crash mid-write
  leaves the old one), 0.8-1.5 s for ~300-370 pipelines, off the render thread. `stamp.txt`
  holds the macOS build and the GPU; files made on another are deleted at startup (log line) and
  refill as the maps are played; a file that can't be read is deleted (log line). Log lines:
  `[mtl] pipeline cache: <dir>, N files, X MB` and `wrote <map>.bin, N pipelines (H built from
  the files, C compiled; F pixel functions from the files), X MB, T ms`. `KB_FPS`'s pipeline part
  adds `, N from the cache file`; `KB_MTL_SHADERLOG`'s frame lines (`p`) add the frame's pipelines
  and their build ms.
- **Why one file per map, rewritten**: Metal can't write an archive that was read from a file
  once something is added to it (`serializeToURL` fails: "cannot pack ... expecting 'fragment'
  stage in pipeline no. N", reproduced in a 60-line test with one pipeline added; then "Nothing to
  serialize"), so the files are read-only and each map's is rebuilt from what it drew. A pipeline
  that came from a file can be added to a new archive (0.1 ms). File count is bounded (the maps
  played), and each file holds what its map drew the last time. A map's file is ~15-17 MB
  (~300-370 pipelines, ~50 KB each): every map played, ~400-450 MB.
- **Measured** (mp_nuked, 7 managed bots, the scripted player, 2-minute TDM; the same build with
  its files deleted before each run as "before"; frames over 20 ms in the match with more than
  10 ms of pipeline builds in them):
  - System cache warm (mp_nuked the last map compiled): ~0.4 ms a pipeline either way (302
    compiled in 124 ms; 291 of 317 from the file in 97-220 ms): no hitches from pipelines in both.
  - After another map (mp_array with a shader set not compiled before, which evicts mp_nuked's
    from the system cache): before 4 and 5 frames (21-386 ms; 15-99 pipelines each, 533-721 ms of
    builds); after 1 frame (22 ms: one pipeline not in the file) and none, 311 of 364 and 322 of
    344 pipelines from the file.
  - Strictly cold (the measured map's own shaders in batches of a new size, so the system cache
    has none of them): before 2 and 2 (30-82 ms); after 2 (29-33 ms: with another batch size some
    functions differ from those in the file, 57 misses) and none; the map's first frame 0.77 s
    instead of 1.24-1.33 s (157 pipelines from the file; the rest of it is shader batches still
    compiling: stage 2's part).
  - Does a lookup go to the compiler service? Little: in the match the compiler service retired
    1.1-2.0 G instructions without the files and 0.3-1.2 G with them (the misses); the hitches were
    mostly waiting on its round trips (0.5-0.7 s of stalled frames against ~0.1 s of its CPU for
    the whole match).
- **Checked**: screenshots of mp_nuked's views and the overviews of mp_array, mp_hanoi,
  mp_kowloon, mp_villa, mp_radiation and mp_cracked (bench_views.py `--home`, a profile kept
  between runs: one pass filling the files, one with them, one of 5305e7d7 without): with the
  HUD masked within the reference noise except Nuketown's random mannequin and mp_radiation's
  steam. The second pass took nearly everything from the files (mp_kowloon 377 of 378, mp_villa
  343 of 345; a map's pipelines are mostly in the other maps' files too). `MTL_DEBUG_LAYER=1` with
  the files: no message. ASan client (`MTL_DEBUG_LAYER=1`, bots, the scripted player, 1-minute
  TDM rotations), 300 s writing mp_nuked.bin and mp_array.bin at the maps' ends, then 200 s
  reading them (344 of 373 from the files): no report. fps with bots, alternating with 5305e7d7
  in four pairs: 5305e7d7 569/695/715/744/687/710/741/456, 562/684/710/747/677/750/741/458,
  552/707/708/718/670/739/713/472, 554/707/686/722/696/707/696/450; the cache
  550/707/687/702/645/754/697/444, 528/704/700/699/699/711/742/435, 558/664/679/700/710/716/701/475,
  552/712/703/703/675/703/732/442. View D was 2-6% lower in each pair, the rest mixed: samples
  of view D show the same profile in both (the pipeline lookups 0.2-0.3% of the render thread,
  which waits 72% of the time: that view runs into the engine's frame pacing), so no cost found.
  check_file.py natively and `--i386` (cl_cgame_mp.cpp; the .mm files are macOS-only).
- **Metal's shader validation can't read binary archives**: with `MTL_SHADER_VALIDATION=1`, opening
  any archive written by `serializeToURL` crashes in `-[_MTLBinaryArchive
  getScriptsWithAIRNTObject:]` (macOS 26.6.2 25G83; a one-pipeline archive in a test does it too),
  and an archive written under it holds instrumented pipelines that normal runs miss. So the
  cache is off under `MTL_SHADER_VALIDATION` (a log line says so); `MTL_DEBUG_LAYER` alone reads
  and writes it fine.
- **Found, not fixed**: the release client crashed once (SIGSEGV, ~53 s into an mp_nuked match
  with bots, build 5305e7d7, before this stage's code) on a physics worker thread in
  `bp_env_jq_batch_function2` (phys_broad_phase.cpp:2446, the loop node's callee-saved register
  bad after `collide_bpg_environment`), right after the only `colgeom_visitor_t::intersect_box:
  leafList overflow (max 2048)` of the night: an object with a huge query box. Not reproduced;
  a task card was left for it. Fixed in session 21 (Decisions session 21).

Stage 2 (the shader file), work in progress: all but the compile (the Metal Toolchain download
waits for the user's yes).

- **Names**: `MSL_TranslateNamed` (mtl_msl.cpp) translates with a placeholder name, hashes the
  prelude and the source (FNV-1a 64), and names the entry and its structs `kb_<16 hex>`; the
  backend names every shader so (runtime batches included) and the per-sample variant too, so
  one library holds them all and a function of a shader's name is exactly its code: a changed
  translation (or `KB_MTL_GLCOMPAT`'s raw shadows) looks up a name the library doesn't have.
- **The game**: `MetalShaderCompiler` opens `<fs_b>/kb_shaders.metallib` (the game folder) at
  device creation and keeps its function names; a shader in it gets an already compiled "batch"
  holding the library (no front-end compile; its function is specialised and built as before),
  one that isn't goes to the batches as before with a log line (`[mtl] shader kb_... isn't in
  kb_shaders.metallib: compiled at run time`, the first 10 and every 1000th); the per-sample
  variant likewise. Without the file one line says so and everything compiles at run time, as
  before (checked: mp_nuked's views within the reference noise, the ASan client over three maps
  quiet).
- **The collection** (`tools/make_shaderlib.sh`, step 1, once per game folder): the game with the
  GL layer's `KB_DUMP_SHADERS` in a folder of its own (links to `main/` and `zone/`, a scratch
  profile: the game folder's settings and profile untouched), `map` to each of the 26 maps 20 s
  apart from the menu (`KB_MENU_CMDS`), 9 minutes: 16,140 shaders (session 13's 8,864 were the
  menus and fewer maps) into `<game>/kb_shaders/bytecode` (101 MB).
- **The generator** (src/gfx_mtl/tools/shaderlib.cpp, built by the script with the backend's
  sm3.cpp and mtl_msl.cpp): every shader and every pixel shader's per-sample variant (the user's
  requirement: no graphics setting compiles at run time), 31,915 functions after merging equal
  translations, in 125 files of up to 256 (289 MB of MSL), `names.txt` and a stamp (a hash of the
  names). Checked against the game: KB_DUMP_MSL on mp_nuked with 4x MSAA (transparency AA): all
  3,204 functions the game made, the 28 per-sample variants among them, are in names.txt.
- **The compile** (step 3, untested: no toolchain): skipped when `kb_shaders.stamp` holds the
  stamp and the compiler's version; else `xcrun metal -c -fpreserve-invariance` per file (as the
  runtime's `preserveInvariance`), in parallel, and `xcrun metallib` into the game folder.
  Without the toolchain `xcrun metal` says "missing Metal Toolchain": the script prints the
  download command and stops (exit 1); make_portable.sh, which runs it last, says the game
  compiles at run time until then. Expected: ~5 T compiler instructions (~2-3 minutes on 10
  cores), a file of a few hundred MB.

## Decisions (session 19)

Metal leftover 1, the load with a cold system shader cache. Committed as work in progress: the
checks at the end of this section are not all done.

- **Measuring the compiler**: wall and CPU time of a compile vary 2x on this machine (another
  session's CS:GO port benchmarked and compiled shaders at the same time; P or E cores), so the
  measure is the instructions the MTLCompilerService process retired (`proc_pid_rusage`,
  `ri_instructions`; the instance serving a process is the one that appears at its first
  compile), stable to ~2%. The tool and the dumped MSL (`KB_DUMP_MSL`) were in the session's
  scratchpad, gone with it. A shader costs ~165 M instructions in a batch of 64 (10-17 ms of a
  P core); a library with functions has a fixed ~2.1 G (the stdlib templates it instantiates):
  25% of the cost at 64 a batch, 8% at 256 (an empty library: 38 ms). 76% of the time is LLVM's
  optimization (InstCombine 22%), 24% the front end.
- **Tried and dropped**: `preserveInvariance` off and `MTLLibraryOptimizationLevelSize`: no
  change. `#pragma clang optimize off` in the prelude: the front end -27%, but the backend
  compile at pipeline creation (synchronous, at the draw) got much slower: the first frame
  1.6-2.1 s and 5 s of pipeline builds in the match with 1.6-1.9 s frames. Batches of
  52/100/128 instead of 64: the first frame 1.06/1.01/1.19 s, the startup 0.18/0.54/0.46 s:
  64 stays. No two shaders translate to the same MSL (3,176 bodies of 3,181).
- **The queue** (the main cost): Metal compiles ~10 libraries at once (two MTLCompilerService
  processes of ~5 threads on the M4) and queues the rest first in, first out, and the
  synchronous requests of a draw wait behind them: the function specialization
  (`newFunctionWithName:constantValues:`, the alpha test) and the pipeline build. With 56
  batches pending one specialization took 1.36 s and one pipeline 2.96 s; with 8 or fewer,
  1-15 ms. A map's ~50 batches are all submitted during the load, so its first frame spent
  2.3-2.6 s specializing (4-9 s while the other app compiled too). MetalShaderCompiler now
  gives Metal cores - 2 batches at a time (`KB_MTL_COMPILES=<n>`) and keeps the rest in its
  own queue; a batch a draw needs goes ahead of the queue. A function with function
  constants can't skip specialization (Metal refuses the unspecialized function even when
  every constant is optional).
- **Shadow maps by name**: the `kb_pcf` branch (session 13) cost 16% of the compile. The
  game's shaders keep their constant tables (the CTAB comment); sm3::Parse reads the sampler
  names. The shadow maps are `shadowmapSamplerSun` and `shadowmapSamplerSpot` (registers s0-s3,
  per shader), and 12,576 of the 12,582 compare branches were on them. Such a sampler is now a
  `depth2d` read with `sample_compare` (texldp: z/w, texldl: z), and the device's sampler for
  a depth texture compares LessEqual (lit where z <= depth, as kb_pcf; session 13 matched the
  two to 0.0001): -21% of the compile, one cheaper lookup on the GPU. On mp_nuked (7 bots, all
  views, shadows on, and with `sm_enable 0`) the engine bound depth textures to exactly those
  samplers; a mismatch is a `[mtl] BUG` line and the draw is skipped (a depth2d can't take a
  colour texture). `KB_MTL_GLCOMPAT=1` reads raw depth through a texture2d as before. The
  per-draw shadow bit (buffer 21's y) is gone.
- **Numbers** (cold front end, the backend's cache warm, mp_nuked with 7 bots, nothing else
  compiling; bench_views.py --views a): HEAD's first frame 2.62 and 3.16 s (specializing 2.57
  and 2.30), the startup 0.41/0.33 s; now 1.05 and 1.20 s (waiting for batches 0.70/0.84,
  specializing 0.29/0.30), the startup 0.24/0.20 s. The 1.06 s frame at the end of every run is
  the `quit`.
- **What a map draws**: mp_nuked creates 3,181 shaders (220 vertex, 2,961 pixel); a 2-minute
  scripted match draws 462 (372 pixel), bench_views.py's views 368. The technique types don't
  predict it: only 183 pixel shaders belong to types that never drew (the deferred `light spot`
  and `light omni`, NVIDIA-only `nvintz`, debug); the rest spread over the lit variants, picked
  per surface by its lights.
- **Prototypes, not committed**: compiling the shaders on a list first (417 from two scripted
  matches; a second queue Metal gets first) made the cold first frame 0.36 s, with 6 of the
  330 drawn shaders not on the list. Compiling the unlisted ones only when drawn: each miss
  pulled in a 64-shader batch, frames up to 2.4 s: no.
- **Warm cache**: the first frame built 173 pipelines in 26 ms in one run and 222 ms in the
  next (specializing ~1.2 ms each when the shared system cache has dropped them); in a match a
  few frames take 25-65 ms when 20-54 pipelines are first used at once (specialization plus
  3-20 ms backend compiles).
- **The user's answers** (to plain questions at the end of the session): a cache file in the
  game folder for the warm hitches, yes (a binary archive); leftovers 3 and 4, both; for the
  load (a shader list) and the offline metallib (the toolchain download) they asked for the
  "by the book" way of each to be explained first: Apple's way is the offline .metallib (and a
  binary archive for pipelines); the list is a workaround that only reorders runtime compiles.
  They then chose Apple's way: the offline .metallib, not the list, built and kept up to date
  by make_portable.sh with the transparency-AA variants in it; and the stutter cache filled by
  the game itself (not pre-built). They need only M4 support.
- **Diagnostic**: `KB_MTL_SHADERLOG=<file>` (CLAUDE.md) logs each shader's creation and batch,
  batch submits and completions, a draw's waits, first uses and frames over 15 ms.
- **Checked**: the validation layer (API and shader) on mp_nuked, 7 bots, every view, cold:
  no message, no BUG (twice, the second on the final code). Screenshots of mp_nuked's views
  against HEAD in default mode: mean |d| 0.05-0.20 (HEAD against itself 0.26-0.84). fps level
  within this machine's noise (7 bots, alternating, last pair HEAD 473/597/643/681/563/563/623/
  402, new 482/597/645/654/599/618/631/416). check_file.py natively for the changed files,
  `--i386` for sm3.cpp and mtl_msl.cpp (the .mm files are macOS-only); audit_splitlocals.py 0.
- **Not checked yet** (done in session 20, Decisions session 20): every map's overview against HEAD~1 (default) and
  against GL (compat; GL's own noise as the tolerance), the validation layer on a few other
  maps, the ASan client (`MTL_DEBUG_LAYER=1`, bots, the scripted player, map rotations), fps
  pairs in the normal display state; then make_portable.sh. A run of all 26 maps was started
  and stopped: the user was at the computer, and the fullscreen test window takes the keyboard
  (one run ended with "Host ended game": an Esc and End Game). Run the long checks when the
  user is away, or ask first.

## Decisions (session 18)

The user's gameplay change 1, the lean (stage C), as in stages A and B: an archived dvar,
`kb_aaLean`, default 1; 0 gives the original game. It applies to every player (bots don't lean;
the body and eye code reads only the player state, so no `BG_KbHumanPlayer` test).

- **The source**: OpenMoHAA (github.com/openmoh/openmohaa, commit a2f34019 of 2026-04-24), a
  shallow clone (124 MB) in session 18's scratchpad, read only; it is gone with the session (ask
  before downloading it again). Its lean: `PmoveSingle` (fgame/bg_pmove.cpp) eases `fLeanAngle`
  with the numbers fgame/player.cpp and cgame/cg_predict.c set per game: Allied Assault (1.11)
  leanMax 40, leanAdd 10, leanRecoverSpeed 15, leanSpeed 4, leaning while moving allowed;
  Spearhead/Breakthrough 45, 6, 8.5, 2, no leaning while moving without a dmflag.
  `CG_OffsetFirstPersonView` (cgame/cg_view.c) turns the eye by the angle about a point 28.7
  units below it, around the view's forward axis (pitch and yaw), traces a 6-unit box from the
  unleaned eye down to the new height and across to it (MASK_PLAYERSOLID), and rolls the view
  0.3 of the angle (`CG_CalcViewValues` adds 0.1); `CG_CalcViewModelMovement` drops the gun
  `vm_lean_lower` (0.1) a degree; `PmoveAdjustAngleSettings(_Client)` rolls the bones. The
  client sends its camera position as the eye the server shoots from (`CL_EyeInfo`). Nothing
  else in its pmove looks at the lean: no slowdown, no trace.
- **The user's answers** (session 18): AA's lean; AA's lengths scaled to BO's soldier (eye 60
  units high against AA's 82, so x60/82: the pivot 21.0, the box 4.39, the gun's drop 0.073 a
  degree); the same 40 degrees prone (AA has no prone; BO halves it there).
- **The state**: `ps->leanf` holds the angle / 80 (`KB_LEAN_DEGREES`), so AA's 40 degrees is BO's
  full lean, 0.5: the network, the entity state's lean and every BO reader keep their range, and
  the dvar can change in a match. Constants in bg_misc.h (`KB_LEAN_*`).
- **The motion** (`PM_KbUpdateLean`, bg_pmove.cpp, in `PM_UpdateLean`'s on-foot branch): AA's
  arithmetic in floats, frame time = msec / 1000. AA's quirks kept: the right lean takes only the
  eased step (both arms of its test assign it), the left one at least 4 degrees/s, so a left lean
  reaches exactly -40 (544 ms) and a right one creeps up to it. AA's rules: it leans in the air
  (BO only on the ground), and dead or on a ladder the angle is 0 at once (AA's PM_CLIMBWALL).
  BO's no-lean states stay (weapon state 35, deployed; pm_flags 0x800, which masks the buttons;
  turrets and vehicle seats), and a mantle still freezes it (BO skips the lean while mantling).
  No trace limits the angle (BO cut it with an 8-unit box 20 units out).
- **Movement** (`PM_LeanWalks`): leaning no longer makes the player walk (40% speed, walk leg
  animations, the quieter footsteps, the ladder bob) or ends or prevents a sprint.
- **The eye** (`BG_AddLeanToEye`, bg_misc.cpp; `AddLeanToPosition` with the dvar off): AA's
  rotation, used by every reader of the leaning eye: the camera, `BG_GetPlayerViewOrigin` (the
  server's shots through `G_GetPlayerViewOrigin`, the client's `CG_GetPlayerViewOrigin`), the
  snapshot's visibility point, the bots' aim at a leaning enemy, CSC `getlocalclienteyepos`, the
  viewmodel's lighting origin, turret placement and the plant check. At 40 degrees, level: 13.50
  sideways, 4.91 down (BO's full lean: 14.67 and 3.12). Looking down it moves less sideways, as
  in AA (the axis is pitched).
- **Collision** (`BG_KbClipLeanEye`): AA's two traces, ±4.39 box, the player's mask 0x2818011,
  in the camera (cgame's trace) and in `G_GetPlayerViewOrigin`/`CG_GetPlayerViewOrigin`, so the
  shots start where the camera is. Walls stop the eye, not the angle: the roll and the body stay
  at 40 (the body can lean into a wall, as in AA).
- **The camera** rolls 0.4 of the angle (16 degrees at 40; BO's doesn't roll). **The gun** rolls
  0.3 of it on top of the camera (AA's arms roll 0.7) and drops 2.9 units at 40 (BO: -1.5 degrees
  and a 1.6-unit shift); both fade into the sights like BO's offset (`kb_noAds` keeps ADS off).
- **The body** (`BG_Player_DoControllersInternal`; third person and the server's hit locations):
  AA's rolls: the pelvis's 0.8 of the angle on the lowest spine bone (rolling BO's pelvis would
  tilt the legs), 0.22 on back_mid (AA's Spine1), -0.02 on back_up (Spine2), -0.4 on the head
  (it ends at 0.6), nothing on the neck, no shift or tilt of the whole model; the upper back ends
  at the full angle, as in AA (BO: ~17 degrees, shifted 2.5-5 units). Standing and crouched; prone
  keeps BO's prone lean (a torso twist), now to 40 degrees, ~1.7x its usual extent. Seen from the
  front the left lean looks a little smaller than the right (BO's rifle pose is one-sided).
- **Checks** (temporary logs of `leanf`, the camera's angles and eye, and the server's eye,
  marked and removed; mp_nuked, release client unless said): the client and server eye 13.50 /
  4.91 at 40 degrees, 13.12 / 4.60 and roll 15.47 at 38.67 (the formulas give the same); the ease
  follows AA's recurrence (3.2, 6.144 degrees at 8 ms frames); 90% in 215-224 ms (AA's formula:
  224), -40 at 542 ms (544), released to 10% in 151 ms (152). Running while leaning 178-183
  units/s (dvar off: 72), sprinting while leaning 272 with the sprint flag kept (off: the lean
  ends it). Into a hedge: the eye stops at 12.51 of 13.50 with the angle and roll at 40 (off: the
  lean cut to 20.5 degrees); camera and server agree (11.59 at another spot). Prone (eye held at
  BO's 8-unit floor), crouched, jumping (the lean holds in the air). Screenshots: the view rolls
  toward the lean, the gun a little further; third person against the dvar off. A client process
  on a dedicated server with `cg_showmiss 1`: one prediction miss, at the `setviewpos` teleport,
  none while leaning, moving, jumping or switching sides. ASan client (`kb_godMode 0`, 7 managed
  fu bots, 1-minute TDM, the scripted player leaning standing, running, sprinting, jumping,
  crouched and prone in a loop) over mp_nuked, mp_array, mp_cracked and mp_crisis (three match
  ends and rotations, 7 deaths): no report, no assert. Every changed file through check_file.py
  natively and `--i386`; tools/audit_splitlocals.py: 0 sites.
- **fps**: unchanged. bench_views.py --bots, the reference (HEAD) and this build alternating,
  three runs each, all in the normal display state (presented = shown, 530-539 a second): ref A-F,
  sky, overview 535/675/652/746/655/701/740/424, 532/663/685/726/660/697/735/439,
  519/670/696/702/641/680/709/426; new 537/676/697/731/664/702/703/430,
  519/675/700/720/667/687/739/432, 520/662/661/722/644/702/735/425. The views don't lean (noclip
  skips it), so view A standing against holding a lean (no noclip, two runs each): new 563/561
  and 557/551 fps, ref 566/568 and 558/565: no cost beyond run noise.

## Decisions (session 17)

The user's gameplay changes, stage B (items 7, 8, 9 of the list in Next steps), as in stage A:
archived dvars that default to 1, 0 gives the original game. All three apply to human players
only (bots keep their spread, ADS and sprint): `BG_KbHumanPlayer` (bg_misc.cpp) is
`!com_sv_running || !SV_IsTestClient(clientNum)`, the test `kb_infiniteReserve` used, now shared.
pmove and the spread run on the server and in the client's prediction; bots run only on the
server, and a client predicts only its own player, so both sides agree.

- **The user's answers** (session 17): the hip-fire spread is locked to the crosshair, not only
  drawn small; shotguns keep their own pellet spread (their hip spread is the pellet cone).
- **The crosshair** (the weapon table dumped at load, 1705 variants): every weapon that draws
  side reticles draws the same material, `reticle_side_small`, 8 units, `iReticleMinOfs` 0,
  `fHipReticleSidePos` 0; crosshairs differ only by the gap, which is the spread
  (`240 * tan(spread) / tanHalfFovY` in the 640x480 space). The smallest a real weapon draws is
  the Python's prone minimum, 1.0 degree (6.7 of 480 units at the default FOV; the dev
  `defaultweapon_mp`'s 0.4 doesn't count); standing minimums start at 2.0 (SMGs, G11, Python).
- **kb_staticCrosshair**: `CG_DrawReticleSides` draws `reticle_side_small` (registered in
  `CG_Init`, `cg_kbReticleSide`) at 8 units, the gap from 1.0 degree by the game's own formula
  (so it matches the bullet cone at any FOV), at full alpha (no fading with `aimSpreadScale`),
  for every weapon, the launchers, Death Machine and killstreak items too (they had a centre
  reticle or none); no centre reticle. Where the original hides the crosshair (weapon raising
  or switching, reload, melee, hardcore, flashbang) it still does. `BG_GetSpreadForWeapon`
  clamps a human player's min and max spread to 1.0 degree, except shotguns
  (`WEAPCLASS_SPREAD`); a smaller spread (vehicle guns, Steady Aim prone) stays. The server's
  `FireWeapon` (bullets and rockets), the client's predicted impacts (`CG_DrawBulletImpacts`)
  and the crosshair all go through it. Measured per shot: MP5K standing, walking and
  sprinting 1.0 (dvar off: 1.3-3.25), Dragunov 1.0 (10-15), RPG/M72/M202 1.0, Stakeout 4-7
  (unchanged), Death Machine 0 (its own); bots 2-8, 0 in ADS (unchanged).
- **kb_noAds**: `PM_UpdateAimDownSightFlag` never allows ADS for a human player outside a
  vehicle seat. The weapons that raise the sights to fire (`adsFireOnly`: RPG, M72 LAW, China
  Lake, M202, M220 TOW, Death Machine) fire from the hip, without the raise or its delay
  (`PM_UpdateAimDownSightLerp`, `PM_Weapon_StartFiring`); sniper rifles have no scope. The
  heat-seeking lock-on (Strela, M202, M72) runs only while `PlayerStingerAds()` in
  `_heatseekingmissile.gsc`, which wanted `playerads() == 1.0`: a `Scr_KbPatchSource` patch
  also accepts a human player with `kb_noAds` (`!self istestclient()`), so it runs while the
  launcher is held. The Strela (`requireLockonToFire`) locked on the dev attack helicopter
  (`set scr_spawnenemyheli 1`, `developer_script 1`, the view turned to it by a temporary
  hack) from the hip within ~1 s and fired locked; with no lock it refuses to fire, as before.
- **kb_sprintFire**: the fire button (and a dual-wield left gun's, bit 0x18) no longer ends or
  prevents a sprint (`PM_SprintEndingButtons`, `PM_SprintStartInterferingButtons`), and
  `PM_Weapon_CheckForSprint` never enters the sprint weapon states (27-29: raise, loop, drop),
  so the weapon stays ready and fires at sprint speed. The viewmodel's lowered sprint pose
  came from those states, so the gun stays up; the sprint bob stays. Reload, melee, jump,
  crouch/prone, offhands and lean still end a sprint; the automatic reload of an empty clip
  doesn't. Bots see the third-person sprint animation while the player fires. Measured
  (mp_nuked, MP5K): sprint + fire at 290-330 units/s, weapon state 6 (firing) at the press,
  sprint flag on throughout; dvars off: states 27/28, the press drops the weapon (29) and
  fires 0.3 s later at 203 units/s.
- **Checks**: release client, 7 managed bots, the scripted player standing, walking, sprinting
  and holding the ADS button while firing an SMG, a sniper rifle, a shotgun, the launchers,
  the Death Machine and the Strela, with temporary per-shot logs on the server and of the
  client's predicted impacts (removed): all 705 bot shots kept their spread and ADS, none
  fired sprinting. A client process on a dedicated server (prediction with no server in the
  process) predicted the same 1.0 as the server. Screenshots: the crosshair keeps one size
  standing, walking, sprinting and firing (the original's grows and fades). The ASan client
  (`kb_godMode 0`, fu bots, 1-minute TDM, the scripted player sprint-firing and holding the
  ADS button in a loop) over mp_nuked, mp_array, mp_cracked and mp_crisis (three match ends
  and rotations): no report, no assert, no script patch warning. Every changed file through
  check_file.py natively and `--i386`.
- **fps**: unchanged. bench_views.py --bots, the reference (HEAD) and this build alternating, four
  runs each. Two pairs in the normal display state (`KB_FPS` presented = shown, ~400-600):
  ref A-F, sky, overview 533/624/596/684/644/667/596/437 and 526/616/644/687/578/669/602/416;
  new 535/653/717/700/646/679/723/438 and 547/645/659/686/644/666/660/425. The first pair ran
  with the display asleep or locked (120 presented, 0 shown a second), and there the new build
  was 7-10% lower at D, E, F and sky (630/628/656/628 against 686/673/708/697), level elsewhere;
  not seen in the three other pairs, and the change adds only a few dvar tests per pmove and one
  `tan` per frame.

## Decisions (session 16)

The user's gameplay changes, stage A (items 2, 3, 4, 5, 6, 10 of the list in Next steps).
Every change is a dvar, default 1, archived (flag 1, so `seta kb_... "0"` in a profile's
config_mp.cfg keeps it off; a test profile keeps what a run `+set`); 0 is the original game.
Test runs that need the player to die pass `+set kb_godMode 0`.

- **Where each lives**: the game's scripts are in the fastfiles, so script behaviour is
  changed from the engine: by engine paths the scripts call (giving weapons, damage, last
  stand), by sound groups, and for the death view by patching the script text as it loads.
  The scripts were read from the zones (decompress the .ff's zlib stream after the 12-byte
  header, find RawFile assets: name pointer -1, length, buffer -1, the name, then for
  .gsc/.csc an 8-byte header and a zlib stream; patch_mp's copies override common_mp's).
- **kb_godMode** (human players only: not `SV_IsTestClient`): `G_KbGodMode` (g_combat_mp.cpp)
  ends `G_DamageClient` before the script's damage callback, so no shellshock, flinch, burn
  or hit marker either, and `finishPlayerDamage` treats it like the `god` cheat. Falls and
  kill triggers can't kill you; `kill` (cheats are on in map games) still does, and a team
  switch suicides as before.
- **kb_noGrenades** (everyone): `G_KbNoGrenadeWeapon` (g_weapon.cpp) makes
  `G_GivePlayerWeapon` refuse the offhand slots lethal (frag, semtex, tomahawk), tactical
  (flash, concussion, Willy Pete, Nova gas, decoy) and equipment (claymore, C4, jammer,
  motion sensor, camera spike, tactical insertion), projectile weapons of class grenade (the
  `gl_*` underbarrels, China Lake) and the explosive-bolt crossbow; the gun of a `_gl_`
  variant is still given, without its launcher (`G_GivePlayerWeapon`'s alt loop stops at a
  refused alt). Every path gives through that function (class loadouts, bots, `give`,
  pickups, scavenger), and the script commands that follow (`switchToOffhand`, ammo) return
  false for a weapon not held. From the weapon table (1656 variants dumped at load): the
  slots are exactly the user's three categories, killstreak items (care package marker) are
  in none of them, and the crossbow is the only `WEAPPROJEXP_BOLT` weapon (the ballistic
  knife is `NONE`). `CG_AltWeaponToggleIndex` now returns only an alt weapon the player
  holds, so the d-pad no longer shows a launcher with 0 rounds (the game itself always gives
  both). The developer console prints "kb_noGrenades: not giving X" once per weapon. The
  bots' engine AI throws only what it holds: no grenade sounds in a 160 s match with 7 bots
  apart from the care package's smoke marker.
- **kb_noLastStand** (everyone): `finishPlayerDamage` skips the Second Chance entry
  (`perks[1] & 0x20`, specialty_pistoldeath); the player dies instead. Checked with the
  bots' dev hook `set devgui_bot laststand` (developer_script 1): off, the bots went down;
  on, they died.
- **kb_noDeathCam**: `Scr_KbPatchSource` (cscr_parser.cpp) replaces one exact line in a
  script as it loads (found exactly once, else a warning and the script unchanged; no line is
  added, so script errors keep their line numbers; both the fastfile and the loose-file
  path). `_killcam.gsc` init turns `level.killcam` and `level.finalkillcam` off (read at the
  map's start); `_globallogic_player.gsc` Callback_PlayerKilled skips its 0.25 s and 1.75 s
  death-view waits (one 0.05 s frame stays so the spawn never runs inside player_die). The
  replacements test `GetDvarInt( "kb_noDeathCam" )` when they run. Measured (god mode off):
  death to respawn 6.55 s with it off (death view and killcam), 0.1 s on; the final killcam's
  match flag set at the match end only with it off. (`setArchive`, which the killcam init
  calls, is a nullsub in this engine.)
- **kb_noMusic, kb_noAnnouncer** (client sound): `SND_GetBaseLevel` returns 0 for voices of
  the music category (the options' music slider's category: `mus_*`, the menu's
  `mus_mp_frontend`) and of the group `voice_announcer` (`vox_<faction>_<line>`: match start,
  time, killstreaks, the leader dialog); battle chatter is the group `voice_player`
  (`vox_<faction>_<n>_...`) and stays. Muting, not skipping, keeps voices, streams and the
  scripts that wait for them as before. Wave writer: menu music -14..-25 dBFS -> -120;
  `mus_` soloed in a match -7 dBFS -> -120; `_tdm_start` soloed -7.5 -> -120; `_inform_`
  (chatter) with the announcer off still -12.
- **kb_infiniteReserve** (human players only): `PM_ReloadClip` fills the clip without taking
  from the reserve (and isn't capped by it). pmove runs on the server and in the client's
  prediction: `BG_KbInfiniteReserve` excludes bots by `SV_IsTestClient` when a server runs in
  this process, and a client predicts only its own player, so both sides agree. A weapon
  whose reserve is 0 still can't reload (One in the Chamber keeps working). Checked: the
  player's MP5K reloaded 10 times from a reserve that stayed 90; bots' reserves drained.
- **Checks**: release client matches (mp_nuked, 7 managed bots, scripted player); ASan
  client, `kb_godMode 0`, fu bots, 1-minute TDM over mp_nuked, mp_array, mp_cracked (two
  match ends and rotations): no report, no assert; a dedicated server with 4 test clients;
  every changed file through check_file.py natively and `--i386`.
- **fps**: unchanged. bench_views.py --bots, the reference (HEAD) against this build:
  ref A-F, sky, overview 532/676/710/733/656/689/771/415 and 527/659/725/721/666/692/709/412;
  new 509/673/700/720/664/661/705/406 and 506/666/655/694/646/674/702/416; without bots the
  two match. Two earlier runs of the new build were 15-25% lower at every view, and in both
  the window was being composited from the player's spawn on (`KB_FPS`: exactly 240
  presented and shown a second, against ~480 presented/~400 shown otherwise): session 15's
  unexplained compositor state, intermittent (0 of the other 13 runs, both builds); not
  the code (the same build ran normally twice after). If it recurs, look at what is on
  screen above the fullscreen window. With bots, instant respawn means more live bots and
  more fighting (19-28 kills a run against 12-16), which can cost a few percent at view A.

## Decisions (session 15)

- **Metal is the default** (the user's decision, 2026-10-10): `KB_UseMetal()`
  (mtl_window.mm) picks Metal unless `KB_RENDERER=gl`; another value prints a line and
  uses Metal. Checked with bench_views.py and no environment (mp_nuked, numbers in
  Metal backend), then make_portable.sh updated the game folder's copy and rebuilt the
  launcher (it passes its environment on, so its matches run Metal). The GL-only
  diagnostics (`KB_SPEC`, `KB_DUMP_SHADERS`, `KB_TRACEFRAME`, `KB_SCREENSHOT_WINDOW`) need
  `KB_RENDERER=gl` now.
- **Display rate, screen unlocked** (the user's main display, a 240 Hz Dell G2524H,
  fullscreen 1280x720): with vsync 240.0 fps and 240 frames shown a second (`KB_FPS`'s
  `shown/s`), one 8.3 ms frame every second or two; without vsync 360-470 presented and
  330-395 shown a second at 500-800 fps: the fullscreen window's drawables flip without
  waiting for the refresh (tearing, as GL's swap interval 0), and exactly 240 when the
  compositor composites the window (in one run from ~26 s on, without a cause found).
  Session 13's 120 a second was the locked screen. So no CAMetalDisplayLink.
- **The system shader cache** (`$(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal`, one for
  all of the user's apps) keys a library by its source, and keeps about one map's set
  of our batches: compiling another set evicts the previous one (mp_nuked, mp_array,
  mp_nuked again: the second mp_nuked partly cold; batches of 63 after 64: the 64s cold
  again). Not per executable, as session 13 thought: the same binary at another path
  ran warm, and a new build ran warm when the shaders were unchanged; session 13's cold
  runs had other compiles in between. Every shader is created on one thread in a fixed
  order, so a run's batches are the same each time (source hashes logged in two runs).
  Cold, mp_nuked with bots: ~3,300 shaders, ~59 s of CPU in MTLCompilerService (~17 ms a
  shader), 7-10 cores busy from the load's first shader to ~2 s after its last; warm,
  1-2 ms a batch. `KB_MTL_BATCH=<n>` (default 64) sets the shaders per batch: a size not
  used before gives every batch a new source, a cold cache to measure with (clearing the
  system's would cost every other app its cache too).
- **The spawn stall**: `KB_FPS`'s pipeline line now splits the time into waiting for
  batches, the function lookups (specialization) and the longest build. With a cold
  cache it showed three waits: ~0.45 s before the main menu (the startup batches), 0.6-1.6
  s at the map's first frame (the load's backlog: the loading screen stays up that much
  longer), and ~0.6 s at the player's spawn, ~15 s after the load: the load's last batch
  (45 shaders) was neither full nor needed by a draw, so it stayed open until the
  spawn's first draw and then compiled cold (9 shaders, 0.1 s, with batches of 61).
  Now the open batch is submitted once no shader was created for 0.5 s (checked at
  Present): gaps inside mp_nuked's load were at most 0.25 s, the pause after the startup
  shaders 1.45 s. Cold after the change: no wait after the first frame, the worst
  in-match frame 30 ms; warm: no waits. The first two remain (Next steps 0).
- **Checked** (the final build): `KB_MTL_GLCOMPAT=1` screenshots of mp_nuked's views and
  all 26 overviews against session 13's two GL runs: within the GL-vs-GL noise of the
  closer one once the team emblem (`autoassign`) and the fps counter are masked, except
  mp_array (0.62%; session 13's Metal 0.73%: particles, Decisions session 13); against
  session 13's Metal run 0.0-0.6% (no drawing changed). `MTL_DEBUG_LAYER=1
  MTL_SHADER_VALIDATION=1`, mp_nuked's views with 7 bots and a cold cache: no message.
  ASan client with `MTL_DEBUG_LAYER=1`, 7 bots, the scripted player, 1-minute TDM over
  mp_nuked, mp_array, mp_cracked (240 s, cold batches): no report. fps with no
  environment: A 542, B 677, C 703, D 714, E 650, F 699, sky 703, overview 479.
  make_portable.sh rerun with this build.

## Decisions (session 14)

- **Split locals, every build** (the rest of session 13's CanDamage kind): IDA turns an
  array into scalar locals and the code relies on the i386 frame: the first scalar's
  address passed as a vector or buffer, `*(&x + i)`, or an inlined `strcat` that starts
  at `(char *)&prevLocal + 3` (the byte before the buffer). Nothing keeps them adjacent
  on arm64, and the recompiled i386 builds don't promise it either.
  `tools/audit_splitlocals.py` finds them (each `&scalar` or `&pointer` local passed where
  the callee, followed through its calls, reads past element 0; arrays passed or cast past
  their size; pointer arithmetic on a local's address and walks from it; scalar struct
  members; `#if 0` skipped): 146 sites before, 0 now plus 46 reviewed and listed in the
  tool. Each fix is an array rebuilt from IDA's stack offsets. Those reached in play: the
  player use list's bounds, the HUD stance icon and prone text colour (read past
  `deltaTime`), view angles after a missile or gunner camera, the attack dogs'
  `CM_CheckForTraps` (start point; `maxs` was `[2]`), missile attractors (`perp = delta -
  forwardDist * forward`, normalized), script vehicles' path move direction, the
  underwater bullet impact normal, the spot dlight's attenuation constant (a vec3
  uploaded as a vec4), the generic filter's clear colour, the sound proximity trace, the
  compass player arrow, the stats-milestone ticker text, the water simulation (two
  16-byte stores 4 bytes short of a float4; a temporary now, its lanes copied where i386
  put them). Menus: `setDvarFromLocString` (4 KB into a `char`), the server browser's
  filter name (two pointers written through an `int`) and refresh time (64 bytes into a
  `long double`), the Create-a-Class item colour (read `itemNum` and 3 floats, not
  `itemColor`), "equipped in classes", playlist rotations, emblem layers (the UVs were
  split across four locals; the hand rewrite transformed `layer->pos` in place each draw
  and used a transformed UV as the outline size). Mods and raw-file loaders: merged weapon
  files (split pointer arrays), the merged config parser, sun parsing, layered technique
  sets, xanim part counts, collmap bounds, brush edge partitions, menu `#include`. The
  rest are debug draws and dev tools. Where i386 read a neighbour, the value stays: the
  compass friendlies' unknown z is `clientPos[0]` as before. Checked: the ASan client,
  7 managed bots and the scripted player, 330 s over three maps (1-minute TDM rotations),
  and 240 s more after the last fixes; the Create-a-Class, barracks, playercard and
  server browser menus: no report.
- **memfile LZO lengths**: `lzo_uint` is `size_t`, 8 bytes on 64-bit; the 4-byte lengths
  passed by address overflowed, and the compressor's final length write zeroed the first
  4 bytes of each block. Temporaries now (no MP memfile compresses).

## Decisions (session 13)

- **Spec first** (src/gfx_gl/gl_spec.cpp, `KB_SPEC`): hooks in every D3D9 entry point
  of the GL layer record each distinct call shape (method, state and value class,
  format, pool, usage, lock flags, primitive type, vertex declaration, shader pairing
  and the sampler kinds each pixel shader reads) with a count, per thread. Recorded on
  the menus, mp_nuked's views, six overviews and a 4x MSAA run: ~70 methods, 59
  render-state values, 30 sampler states, 23 texture kinds, 180 vertex declarations,
  ~1,500 shader pairs. The Metal device implements that list; anything else prints
  `[mtl] BUG` once. It never printed in this session's runs.
- **One shader parser** (src/gfx_shader/sm3.cpp): the SM3 bytecode parser
  gl_shader.cpp had is now its own IR (declarations, defs, instructions), shared by
  the GLSL emitter (gl_shader.cpp, byte-identical output over all 8,864 shaders the
  game loads: tools/shader_test.sh with the old translator) and the MSL emitter
  (src/gfx_mtl/mtl_msl.cpp; all 8,864 compile with the system's Metal compiler).
  `xcrun metal` needs the Metal Toolchain download, not installed (not asked for);
  the test compiles through `MTLDevice newLibraryWithSource`, the same compiler.
- **D3D9 semantics in MSL**: the half-pixel offset in the vertex shader (as the GL
  layer), `vPos` = floor of the pixel position, `vFace` from `front_facing` with
  clockwise front faces, `rsq`/`log`/`pow` of |x| (D3D9's definitions), alpha test as a
  function constant (a pipeline per compare function; the reference is a per-draw
  value), hardware shadow maps (below), relative constant reads that land on `def`'d
  constants (four vertex shaders; the GL layer reads the uniform array there),
  `[[position, invariant]]` with `preserveInvariance` (the depth prepass's EQUAL test).
- **Shadow maps compare in the shader** (session 19: by the sampler, `sample_compare`; Decisions
  session 19): D3D9 drivers return a filtered comparison
  when a depth-format texture is sampled (lit where the coordinate's z <= the stored
  depth, bilinear over the 4 texels). The game reads its shadow maps only with
  `texldl` and `texldp` (the spec: 398 and 202 sampler bindings). So those lookups on
  2D samplers carry a branch on a per-draw bit (buffer 21, set when a depth texture
  is bound): `kb_pcf` gathers the 4 depths, compares and filters, matching
  `sample_compare` to 0.0001 (tested). A depth texture on a sampler read any other way,
  or with a filter other than bilinear, is a BUG line. The first version declared
  the sampler `depth2d` in a variant compiled at first draw: 95-145 ms each for these
  large lighting shaders, the in-match stalls with bots (up to 0.7-2 s). The branch
  costs ~20% more compile time per pixel shader with a cold cache (8.6 -> 10.3 ms, in
  the background batches).
- **Bindings**: vertex buffers 0-3 are the streams; buffer 20 holds the shader
  constants (`setVertexBytes`/`setFragmentBytes`, only the range the shader reads),
  21 the per-draw values, 22 a buffer of D3D9's defaults for attributes the
  declaration lacks ((0,0,0,1), COLOR (1,1,1,1), constant step function); texture
  and sampler N are sampler register N (vertex texture samplers 257-260: 16-19).
- **DISCARD renames, no per-frame ring** (a deviation from the agreed rule, by the
  spec): dynamic buffers get NOOVERWRITE locks frames after their last DISCARD (7.4M
  such locks in a run), appending to contents earlier frames wrote, so a ring reset
  each frame would hand out storage still holding live data. Each buffer keeps a
  small pool of shared MTLBuffers; DISCARD while the GPU may read the current one
  swaps in one whose last command buffer has completed (or a new one), and bumps a
  global generation so the draw rebinds. Locks return a pointer into the buffer
  (unified memory); a plain lock of a buffer in use would wait (a BUG line; never seen).
- **Texture uploads**: shared-storage textures; a level not yet used by the GPU is
  written with `replaceRegion` at unlock, one already used goes through a staging
  buffer and a blit in the frame's command buffer (earlier draws keep the old data).
  A non-DISCARD relock returns the previous contents: `RB_PatchModelLighting`
  rewrites part of the lighting volume each frame, and zeroed memory there turned
  foliage and models black.
- **Shaders compile in batches**: `newLibraryWithSource` takes ~70 ms per shader
  cold (the menus ran at 5 fps when each compiled at first draw), ~8 ms each in a
  library of 64, ~0 with the system's shader cache warm. CreateVertex/PixelShader
  translate at once and add to an open batch of 64, compiled asynchronously; a draw
  that needs a shader of an unfinished batch submits it and waits. Pipelines are
  built at first draw (cached by a key of shaders, declaration, formats, samples,
  blend and alpha test), and so is the per-sample variant (transparency AA, its own
  library). `KB_FPS` prints the pipelines built per interval and the time. The
  system cache keeps libraries and pipelines between runs; with it cold (the first
  run of a new build) the first match can stall up to ~0.7 s where a draw waits for a
  batch still compiling; warm, the worst frames with 7 bots were 26-55 ms (GL: 39).
- **One command buffer a frame** (plus readbacks): render encoders open lazily at the
  first draw into a target, clears become load actions (partial clears draw a quad),
  StretchRect resolves MSAA in a render pass or blits; occlusion queries use one
  32768-slot accumulate visibility buffer (Metal limits offsets to 256 KB) and are
  read when their command buffer completes; event queries complete by serial.
  Three frames in flight behind one semaphore, waited for at the frame's first
  command buffer. Metal calls only on the render thread; the query FLUSH flag
  commits only there.
- **Presentation**: CAMetalLayer (framebufferOnly, 3 drawables). Rendering must not
  wait for the display: `nextDrawable` blocks until the compositor returns a drawable,
  and presenting from the render queue capped the GPU at ~360 fps. So a presenter
  thread waits for drawables, the render queue scales the back buffer into one of
  three present textures, and a second queue (waiting on an MTLEvent) draws it into
  the drawable. Without vsync a frame with no drawable ready is not shown (as Apple's
  GL drops them); with vsync Present waits. `nextDrawable` delivered 120 drawables a
  second here, with sync on or off and in a bare test app, but the screen was locked
  for every run of this session (the user away; locked at 01:35, the tests from 01:52):
  nothing was composited (`KB_FPS`'s `shown/s`, frames the display reported showing,
  stayed 0). Whether an unlocked 240 Hz display gets 240 a second is unmeasured;
  `KB_FPS` prints `presented/s` and `shown/s` to check it (Next steps 0).
- **Autorelease pools**: the engine's render thread has none, and Metal autoreleases
  per encoder (96 KB each under the validation layer: the ASan run ran out of memory).
  A pool is pushed when a command buffer begins and popped after it commits. Pools
  are LIFO, so no `@autoreleasepool` may span the commit of a command buffer begun
  outside it ("Invalid or prematurely-freed autorelease pool").
- **Formats**: D24S8/D24X8/D24FS8 -> Depth32Float_Stencil8 (Apple GPUs have no 24-bit
  depth), A16B16G16R16 -> RGBA16Unorm (D3D9 clamps it; the GL layer used a float
  target), X8R8G8B8/L8/A8L8 by swizzles, DXT1/3/5 -> BC1/2/3.
- **Transparency AA** (the `ADAPTIVETESS_Y` ATOC/SSAA hack on alpha-tested draws into
  MSAA) runs the pixel shader per sample, its inputs interpolated at the samples
  (`sample_perspective`), so the alpha test cuts each sample. Metal's
  alpha-to-coverage instead left orange fringes on foliage (the colour these textures
  keep under low alpha). The GL layer's fast mode (ATOC) shades half the samples
  (`glMinSampleShading(0.5)`), which Metal can't express: both modes cost what SSAA
  does (numbers in Metal backend). The first version read `[[sample_id]]` but left
  the inputs at the pixel centre: every sample passed or failed together, i.e. no
  transparency AA; the MSAA screenshots against GL found it.
- **Where the GL layer differs from D3D9** (found by the spec and the screenshots;
  not fixed, GL stays the fallback and Linux's renderer): it ignores
  `D3DSAMP_MIPFILTER` (gl_state.cpp's min filter is never mipmapped, so only level 0 is
  sampled, however small the surface); it samples
  shadow maps as raw depth, so the sun casts no shadows on the world; it ignores
  separate alpha blending and all stencil states (the game sets EQUAL/NEVER stencil
  tests, two-sided); it drops vertex buffer uploads made from the worker threads, so
  particle clouds (mist, smoke, dust sheets) never draw; relative constant reads miss
  `def`'d constants; A16B16G16R16 targets are float. `KB_MTL_GLCOMPAT=1` reproduces
  the first three on Metal, for screenshot comparisons.
- **Engine**: sentient.cpp's global `glob` (a struct) had external linkage, and the
  symbol `_glob` is libc's `glob()` too: `FindFirstFileA`'s call to `glob()`
  (win_kernel.cpp) bound to that data in every macOS build. The Metal build's larger
  layout put it out of branch range and the link failed. It is `static` now.
- **CanDamage's cone check** (g_combat_mp.cpp, every build): the decompile split each
  direction vector into three float locals and passed the first to `Vec3Normalize`,
  relying on the i386 stack layout; ASan caught the stack overflow on the server thread
  (a grenade's `G_RadiusDamage` with bots). They are `float[3]` now. The rest of the
  kind: Decisions session 14.
- **KB_CMDS_GAMETIME=1** (linux_main.cpp; bench_views.py sets it): KB_CMDS times its
  entries by the client's server time instead of the wall clock, so two runs that
  load at different speeds take their screenshots at the same moment of the map.

## Decisions (session 12)

- **Pre-tessellated indices never reset** (r_pretess.cpp, every build):
  `gfxBuf.preTessIndexBuffer->used` only grew, so the 1M-index buffer filled a few
  frames into a map and every later `R_AllocPreTessIndices` failed. The BSP and static
  model surfaces it merges then drew one by one, each with its own index upload
  (`R_SetIndexData`, ~3800 uploads a frame at view A), and `RB_UpdateDynamicBuffers`
  copied the stale 2 MB every frame. Nothing in the decompile resets it (IDA's
  `R_BeginPreTess` has only the wait). `R_BeginPreTess(1)` now empties the buffer the
  first time a frame locks it (a static records the frame per pool buffer; a second
  view appends), after waiting for the backend, which publishes with a release fence.
  View A: 6950 -> 4400 draws.
- **Empty skinned cache** (rb_backend.cpp, every build): `RB_UpdateDynamicBuffers`
  locked the skinned-vertex buffer with `used` bytes even at 0, and a D3D9 lock of
  size 0 means the whole buffer: the GL device copied and orphaned 8 MB every frame
  without skinned models. It skips the lock like the other dynamic meshes. Uploads
  11.5 MB -> 0.6 MB a frame at view A.
- **Model lighting volume** (gl_resources.cpp): `RB_PatchModelLighting` patches the
  256xHx4 volume every frame and the device re-uploaded it whole; Apple's GL first
  waited for the GPU to finish with the texture (a submission and a stall, 7% of the
  render thread at A). A single-level volume rewritten after creation now rotates
  through three textures (`KB_GLRetargetTexture` moves the bound stages to the new
  name). The whole volume is still copied (~0.2 ms); a dirty box from the engine would
  cut that.
- **Render targets** (gl_d3d9.cpp, gl_surface_ops.cpp): one auto depth-stencil
  renderbuffer per target size (`selectAutoDepth`) instead of reallocating one for each
  size change (the bloom chain; each reallocation submitted a command buffer), and
  StretchRect reuses two framebuffers instead of creating and deleting two per call.
- Rendering at the 7 views matches the unmodified build (0.2-0.4% of pixels differ by
  more than 24/255: the weapon's idle sway and the fps counter). ASan client: the bot
  benchmark and a 5-minute soak with 1-minute TDM matches (7 hard bots, scripted
  player; mp_nuked, mp_array, mp_cracked), no report.

## Decisions (session 11)

- **DLC maps against the 7.0.61 executable**: the map packs' data was built for later
  executables. mp_area51 spawns a `trigger_damage` from script (the apple): it moved
  from `s_bspOnlySpawns` to `s_bspOrDynamicSpawns`, and `SP_trigger_damage` takes
  `spawn( classname, origin, flags, radius, height )` as a box (the bullet test,
  `SV_SightTraceCapsuleToEntity`, uses `CM_TempBoxModel` for an entity without a
  brush model). The failed spawn had left the server spinning in "potential infinite
  loop". `wait` shorter than half a frame was a zero-frame wait (the decompile
  read the x87 `setp` as `isnan`); it lasts a frame now, as in the original, which
  fixed mp_gridlock's `while (IsTouching) wait 0.01;` air pumps.
- **xWMA decoder** (src/audio_openal/wma/, README.md there): FFmpeg n7.1's WMA v2
  decoder (wmadec.c, wma.c, wma_common.c, the tables of wmadata.h, wma_freqs.c and
  aactab.c's scale factor VLC), downloaded with the user's OK and ported: LGPL-2.1+,
  FFmpeg's headers and COPYING.LGPLv2.1 kept. FFmpeg's bit reader, VLC builder (same
  multi-level tables), av_tx IMDCT (a DCT-IV through an n/2-point FFT, av_tx's sign
  and scale), float DSP and sine windows are local code; the VLC tables, windows and
  MDCTs are built once and shared. xWMA specifics from libavformat/xwma.c: decoder
  flags 0x1F (no extradata in xWMA) and its bit rate fix-ups; the engine's
  `nAvgBytesPerSec` (6000 x channels) and `nBlockAlign` (2230 mono, 4096 stereo)
  match every asset seen. The user preferred this to converting the sounds, which
  would mean rebuilding the fastfiles (the sounds are inside them).
  - `ALSourceVoice` decodes a packet at a time into a per-voice buffer (decoder
    ~170 KB plus up to 15 frames of PCM per playing xWMA voice), reads in order and
    restarts the decoder for a loop, another buffer or an earlier frame. It drops
    the codec delay (2 frames, as FFmpeg's `skip_samples`; gun shots then start at
    frame 0) and adds the last half frame at the end (FFmpeg's flush): the total is
    the asset's frame count and the dpds table's. It clips to the 16-bit range:
    XAudio2 decodes xWMA to 16-bit PCM (the dpds table counts 16-bit bytes), and
    weapon shots overshoot to +6 dBFS (128 of 352 assets go over full scale).
  - Checked against FFmpeg 8 (OBS.app's libavcodec/libavformat, loaded with dlopen
    by a scratch harness that built RIFF XWMA files from assets dumped out of the
    game): 352 assets, same lengths, 134 dB SNR, no decode errors; ~2400x realtime.
    The harness and the dump code were temporary; README.md says how.
- **Finding what plays** (temporary code, removed): `[snd-tmp]` prints in
  `SND_PlaySoundAlias` (each early `return -1` with a reason), `iSND_CreateVoice`
  (alias, stream, format, channels, rate, bus, flags) and `SD_StopVoice` (the
  voice's maximum dry/wet levels, matrices and LPF from `SDXA2_UpdateVoiceSends`,
  plus the backend's measured peaks after decode, after the per-voice effect and
  into each send). Result: 2168 of 19065 voices in 110 s were xWMA, all
  `wpn_`/`fly_`/`prj_`/`dst_` aliases, with normal engine levels (weapons' dry level
  median 0.24 vs. voices' 0.16) and zero decoded signal.
- **Hearing specific sounds**: `snd_solo_alias_substring <s>` (a cheat dvar; map
  games keep `sv_cheats` on) zeroes every alias whose name lacks `<s>`, so a wave
  writer capture holds only that group. Set it from `KB_CMDS`/`KB_MENU_CMDS` in
  timed segments separated by `zzz_none` (silence), and timestamp the game's stderr
  (pipe it through `perl -MTime::HiRes=time -ne 'printf "%.3f %s", time-$t0, $_'`;
  the WAV starts at the `[al] output` line) to cut the WAV per segment. The wave
  writer's clock stalls during loading, so line segments up from the stderr times,
  not from the first frame.
- **Vehicles** (found while testing; every build unless noted): a bot's RC-XD
  crashed or froze the game.
  - `G_ClearVehicleInputs` (64-bit): walked the vehicle list as `T_internal_base`
    nodes and used i386 offsets: `i[70].m_next_T_internal` (m_owner),
    `[43].m_prev_T_internal` (gentity_s::scr_vehicle), `memset(&i[100], 0, 0x34)`
    (mVehicleController.m_cmd), i.e. 52 bytes cleared past the vehicle on 64-bit.
    The list's iterator and named fields now (docs/64bit.md rule 17).
  - `colgeom_visitor_inlined_t<200> proximity_data` had no vtable, and
    `intersect_box`'s `visit()` calls crashed: `G_SpawnVehicle` and
    `Actor_InitMove` memset the struct and then assigned the static `dummy` (C++
    assignment never copies the vtable pointer; the original copied the vftable);
    `CG_Vehicle_PreControllers` left the copy commented out on `MT_Alloc`'d memory.
    Copy-constructed in place from `dummy` now.
  - `collide_vehicle_wheels`: the wheel's hit point `v22` was an `int` that
    `Vec3Lerp` wrote 12 bytes into (the collision context next to it on clang's
    stack; ASan). A `float[3]` (its i386 slot is 12 bytes).
  - `VEH_TouchEntities`, `GlassSv_PredictTouch`: IDA split `float[3]` bounds into
    three floats (`mins`, `v10`, `v11`...) passed by address; MSVC's stack kept them
    together, clang's doesn't (inverted bounds asserts, ASan). Arrays now. The same
    pattern is likely elsewhere in the decompile; ASan's stack checks find it when
    the code runs.
  - `CG_Vehicle_DoControllers`: upstream's rewrite passed `&steerAnglesYaw[2]`,
    `&bodyAngles[2]`, `&spin`... as 3-float angles, i.e. MSVC's stack order; on clang
    the vehicle's bones went NaN (~400k asserts in a 4-minute match, effects bolted
    to them too). Rewritten from the original's semantics (the `#if 0` version):
    body/turret/barrel/gunner/minigun angles, and the wheels' suspension, steering
    and child-bone spin that the rewrite had dropped (session 6's note).
  - `SpectatorThink`/`ClientThink_real` cleared `pmove_t` with `memset(pm, 0,
    0x258)`: up to `m_gjkcc_input` on i386, 8 bytes short of it on 64-bit;
    `offsetof(pmove_t, proximity_data)` now (also covers i386's padding).
- **Texture streamer state** (`StreamFrontendGlob`, r_stream.h; every build):
  rebuilt from the i386 offsets the code used. The original puts the arrays on
  128-byte lines and sizes the image arrays for 4224 images (132-word bit arrays,
  `aux_buffer[2113]`, `imagePartIndex < 4224`); the decompiled struct had
  4096-image arrays and gap fields, and reached the real fields through neighbours
  (rule 9): `*(float *)&imageImportanceBits[i - 4064]` was `imageImportance[i]`
  (0x6A80), `dynamicImageImportanceBits[(i >> 5) - 4064]` was
  `imageImportanceBits` (0xAC80), `imageLoading[w - 8]` and `imageUseBits[w - 4]`
  the arrays at 0xF384 and 0xF594 (after a 128-aligned `dummy`),
  `*(int *)((char *)&sortedImages[i] + 2)` plain `sortedImages[i]` (0x10000; the
  struct had it at 0xFFFE), `totalBytesWanted` `sortedImageCount` (0x14200) and `syncThing`
  `calculateTotalBytesWanted`. Named fields now, without the alignment gaps, the
  image arrays sized by `STREAM_MAX_IMAGES` = 4608 = `POOLSIZE_IMAGE` (upstream's
  "accurate to retail" pool sizes, commit 7c5565a, had raised it from 4224);
  static_asserts in db_registry.cpp tie the image, material and xmodel pools to
  the streamer's arrays. With the old sizes, an image at pool index 4224-4607
  indexed 1-12 words past each array into the next field: the touch bits'
  `[1][132]` is `activeImageTouchBits` (touching image 4230 while buffer 1 is
  active makes the double-buffer index 65, and the touches and clear that follow
  go ~16 KB past the struct), the loading bits' word 132 is the use bits' word 0
  and the use bits' word 132 the force bits' word 0 (another image looks loaded or
  forced), the importances overwrote the importance bits of images 0-255. A run
  with a temporary log showed indices 4224-4231 (glass decals) reaching the
  streamer right after mp_mountain -> mp_array, as in the report; the old build's
  crash itself was not rerun, so which of these paths led to the reported read
  (and to the stream thread loading "images/.iwi", an image with no name) is
  inferred, not observed.
- `SP_light` (g_misc_mp.cpp, every build) packed a 3-float color with
  `Byte4PackRgba`, which reads 4 (ASan stack-buffer-overflow at mp_hanoi's load,
  whose map has such lights; the 4th byte is the exponent, written right after). The
  client's copy in cg_scr_main.cpp already had a 4-float array.

## Decisions (session 10)

- **A software XAudio2** (src/audio_openal/al_audio.*): the old backend played raw
  PCM only, one AL source per voice with the largest matrix entry as its gain, no
  effects or routing; the game's sounds are MS-ADPCM (streams, most loaded sounds)
  and xWMA, so nothing played. The backend now runs the graph the engine builds, in
  480-frame passes at 48 kHz: each started source voice decodes (PCM 8/16, float,
  MS-ADPCM; xWMA silent), resamples (Catmull-Rom; source rate x frequency ratio, at
  most 2), runs its effect chain (the engine's per-voice LPF/futz XAPO) and mixes
  through its output matrices into the reverb bus (4 channels) and the master or
  "novoice" bus; the buses run their XAPOs (reverb, compressor, EQ/limiter) in
  processing-stage order into the stereo mastering voice. XAudio2 behaviour kept:
  matrix changes ramp over a pass (set at once before a voice is first heard);
  effects are AddRef'd and LockForProcess'd while their voice exists (the engine
  picks free per-voice DSPs by reference count); IXAPOParameters by its real IID;
  buffer queue with loop regions; OnBufferStart/End, OnLoopEnd, OnStreamEnd on the
  mixing thread; GetState's BuffersQueued/SamplesPlayed; Stop keeps the position.
  One recursive lock covers every voice call and each pass (0.1-0.3 ms per 10 ms
  pass in a match, 3.6 ms at most under ASan). Output: one stereo float32 AL source
  with AL_DIRECT_CHANNELS_SOFT (the engine pans; no HRTF on the mix), pulled by an
  AL_SOFT_callback_buffer callback, or (OpenAL Soft < 1.22, `KB_SND_QUEUE=1`) fed by
  a mixer thread that keeps four passes queued. The device (the system default)
  opens at CreateMasteringVoice: `[al] output: <device>, ...` in the log.
- **MS-ADPCM rounding**: the predictor is `>> 8`, as Microsoft's reference decoder
  and CoreAudio (bit-identical over the menu music); `/ 256` (Wine, FFmpeg)
  differed in 14% of the samples, by up to 284.
- **Stream thread** (every portable build): linux_main.cpp stubbed `Stream_Init`, so
  `Stream_Thread` (stream reads for sound, `R_StreamUpdate_ReadTextures`) never ran;
  src/win32/win_stream.cpp is compiled in now (portable.cmake).
- Engine bugs met on the way:
  - `Snd_StreamInit` sized the stream and stream-buffer tables with i386 byte counts
    (0xFA0, 0x15E0); the structs are 440/288 bytes on 64-bit (400/280), so loading
    buffer 19 wiped stream 0's filename and the stream starved. `sizeof` now (the
    same numbers on i386).
  - `SND_RvFrame` (reverb) took its frame count as `Ptr32_Decode(480)` (the image
    base + 480) and counted it down as its loop counter: ~4 billion frames written
    over the globals (job queue, console font) as soon as the reverb ran. The count
    is an unsigned int, and the decompile's 32-bit byte offsets between the eight
    channel arrays (two through `Ptr32_Encode`) are frame indices: the bus buffer
    and the effect's own can be far apart on 64-bit.
  - `SND_PatchValue` (every build): a float patch value is a 16-bit code over the
    field's [minimum, maximum]; the decompile divided by 65535 only, so every map's
    patch set the master presets' compressor makeup gain to 1/16 (-28 dB: the mix
    ~20 dB too quiet) and their attack/release times, EQ gains and limiter
    thresholds wrong. dB SPL values (group attenuations, alias volumes) go through
    `SND_dBSPLToLinear`, which had lost its -100 (`SND_LinearToDbSpl` adds it): every
    patched attenuation and volume became full. Both checked against the unpatched
    values that the patches re-apply (equal within the 16-bit code).
  - `Scroll_Slider_ThumbFunc` read the captured item as `((itemDef_s **)p)[6]`, byte
    48 on 64-bit instead of `scrollInfo_s::item` at 24: dragging a slider (the
    volume sliders) crashed in `Scroll_Slider_SetThumbPos`.
- `tools/wavstat.py`: peak/RMS per window of a WAV (the wave writer's float32 too).

## Decisions (session 9)

- **MSAA** (gl_d3d9.cpp): the engine draws the scene straight into the back buffer
  (`R_RENDERTARGET_SCENE` shares `R_RENDERTARGET_FRAME_BUFFER`), so the back-buffer
  FBO's colour and depth-stencil renderbuffers are multisampled
  (`D3DPRESENT_PARAMETERS::MultiSampleType`, at CreateDevice and Reset). A
  multisampled buffer only blits 1:1 into a single-sample one of the same format
  and refuses glReadPixels (probed on the M4), so its readers go through a resolve:
  `resolvedBackbufferFbo()` (Present, screenshots, `KB_SCREENSHOT`) and
  `blitToBackbuffer()` for StretchRect into it. StretchRect from it into an RGBA8
  texture of the same rectangle (the scene resolves, six a frame) resolves
  directly: going through the copy cost 81 fps vs 90. Render-target textures
  stay single-sample, with their own depth, as before. `CheckDeviceMultiSampleType`
  refuses counts above `GL_MAX_SAMPLES` once a context has existed; the first
  device creation (before any context) clamps, and logs `[gl] N samples asked`.
- **Transparency AA** (`r_aaAlpha`): the engine asks NVIDIA's driver for it with
  the FOURCC 'ATOC' or 'SSAA' in `D3DRS_ADAPTIVETESS_Y` (after
  `CheckDeviceFormat('SSAA')`, now accepted). The GL device turns it into sample
  shading (`glMinSampleShading` 0.5 / 1.0) on alpha-tested draws into the
  multisampled back buffer (`commitSampleShading`, per draw beside the blend
  commit): the alpha-test discard runs per sample. Engine (every build):
  `R_ChangeState_0` keeps the last blend's bits in the state while blending is off,
  and `R_SetAlphaAntiAliasingState` tested them (`& 0xF00`), so after the first
  blended draw it was always off. It now judges the requested blend and re-checks
  when blending toggles; the `r_aaAlpha` change handler likewise, and switches it
  off when set to 0.
- **Fullscreen fills the display** (replaces session 8's black bars): D3D's
  fullscreen mode switched the display to the back buffer's size, and the
  engine's automatic aspect ratio in fullscreen is the monitor's
  (`R_StoreWindowSettings`), so a 4:3 back buffer on a 16:9 display holds a
  picture squeezed to be stretched (seen in the 1024x768 back buffer). A window
  keeps the back buffer's shape when they differ (moved to a display of another
  density).
- **Resolution list**: `R_EnumDisplayModes` ran before anything had started SDL
  video, so the GL adapter offered its 1920x1080@60 fallback alone. The adapter
  queries start video (`Sys_EnsureSDLVideo`). The list is the display's SDL mode
  sizes plus its own size in pixels (a Retina display doesn't list it), all at
  the desktop's refresh rate, as fullscreen never changes the display mode: other
  rates did nothing (`r_displayRefresh` "60 Hz" in the user's config falls back to
  240 Hz).
- **One adapter per display** (gl_d3d9.cpp, sdl_window.cpp): an HMONITOR is the
  SDL display index + 1 and so is `GetAdapterMonitor`. `R_ChooseMonitor` picks
  `r_monitor` (1-based) in fullscreen, else the display holding
  `vid_xpos`/`vid_ypos` (MonitorFromPoint; the defaults 3,22 are the main display),
  and the window opens centred on it. Engine fixes (every build):
  `R_GetDeviceType` (the PerfHUD probe right before CreateDevice) reset
  `dx.adapterIndex` to 0, which lost that choice and left
  `R_CreateDeviceInternal`'s fall-back-to-adapter-0 dead; `R_MonitorEnumCallback`
  stored the monitor 4 bytes into `GfxEnumMonitors`, whose pointer is 8 bytes in
  on 64-bit. Monitor rectangles are reported at 0,0 (the screenshot code compares
  them with window-relative positions), sized in pixels; `MonitorFromWindow` is
  the game window's display.
- **Retina** (macOS): the window has `SDL_WINDOW_ALLOW_HIGHDPI` and is sized in
  points to give one pixel per back-buffer pixel (`fitWindow`, at creation,
  resize and leaving fullscreen); resolutions, monitor and desktop sizes are in
  pixels. SDL's drawable size is wrong on a 1x display in a mixed setup (Open
  items), so the pixel size is points x the display's density, re-read on SDL's
  size, move and display-change events. The cursor maps points -> pixels
  (`KB_GLWindowToPixels`) -> back buffer.
- **Bots for the launcher**: the PC game's managed bots, `bot_spawner_Once` in
  maps/mp/gametypes/_bot.gsc, which a PC online game (our `map` sets `onlinegame`)
  runs instead of Combat Training's: `scr_bots_managed_spawn 1`,
  `scr_bots_managed_allies`/`_axis` (or `_all` for free-for-all, split between
  the teams) and `scr_bot_difficulty` (easy, normal, hard, fu), checked every 10 s.
  The engine registers `sv_botsAllowMovement`, `sv_botsPressAttackBtn` and
  `sv_botsPressMeleeBtn` off and only the scripts' developer blocks turn them on
  (which is why `developer_script 1` was needed for `scr_testclients` bots), so the
  launcher sets them. Friendly bots are allies and the launcher joins the player to
  allies through `KB_CMDS` (`openscriptmenu team_marinesopfor allies`; the team
  menu is that on every map). The bot AI doesn't play objectives: in Domination,
  CTF, S&D and the like the bots only fight. 16 bots at most (18 slots: the host
  and the demo recorder take two).
- **`cg_drawArms`** (the user's request; archived, default 1): 0 hides the view
  model's arms and keeps the weapon. The arms are the view model DObj's first model,
  the one the weapon hangs from (`ChangeViewmodelDobj`), so the model stays and its
  bones are hidden on top of the weapon's own hidden parts
  (`CG_UpdateViewModelHidePartBits`, every frame from `CG_AddViewWeapon`, so a change
  applies at once). The weapon still animates (reload, weapon switch checked). The
  launcher's "Show arms" checkbox passes it on every start.
- A fresh profile's resolution comes from `configure_mp.csv` (1024x768, 4x AA for
  an unknown GPU such as the M4), as on Windows; the engine's own `r_mode`
  default (the smallest mode) is unchanged.

## Decisions (session 8)

- **Everything maxed out = the original's own switches, on by default.**
  `allItemsUnlocked`, `allItemsPurchased` (bg_unlockable_items.cpp) and
  `allEmblemsUnlocked`, `allEmblemsPurchased` (bg_emblems.cpp) default to 1 (on
  every build). `BG_UnlockablesAllItemsUnlocked/Free` check them first, in every
  game mode: the original honoured them only in public online games and basic
  training, and the main menu at startup is neither (a client's `onlinegame` is 0
  until it hosts a map). Everything that asks whether an item, attachment, weapon
  option, pro perk, clan tag feature or emblem part is locked or bought goes
  through these, as do the server's `purchasedItems` bits (`isItemPurchased` in
  the scripts: challenges, pro perk tracking) and "classified" weapons (declassify
  after enough purchases). They keep the cheat flag (`Dvar_SetCheatState` resets
  them to their default, 1). Turning the mode off: `+set allItemsUnlocked 0 +set
  allItemsPurchased 0 +set allEmblemsUnlocked 0 +set allEmblemsPurchased 0`
  (session 7's progression; the rank stays at what was last saved).
- **Rank** (`LiveStorage_SetTopRank`, at sign-in, while allItemsUnlocked): RANKXP
  = `CL_GetMaxXP()` (1262500), RANK = its rank (49, shown as 50), PLEVEL =
  `CL_GetMaxPrestige()` (15) in the global buffer, copied into the CAC copy; a
  listen server takes them with the host's stats, and the scripts cap XP at the
  top rank. Max prestige also hides prestige mode, whose `prestige_reset.cfg`
  resets every custom class. CP is left alone (nothing costs anything; matches
  still add to it).
- **Pro perks**: `BG_ReplaceItemsWithPurchasedProItem` at sign-in, as buying a pro
  perk did, so saved classes upgrade; the menus list the pro versions in place of
  the base ones. A base perk equipped from the console stays base until the next
  start.
- **No CAC validation** while `allItemsUnlocked` or `allItemsPurchased` is set,
  as `CL_CACValidateRequest_f` did (git history before the Demonware removal):
  `SV_ValidateClientCAC` would reject classes holding items the stats never
  recorded as bought. Mid-match class edits are then valid at once. As in the
  original, a prestige request made with only allItemsPurchased set would be lost.
- **`sv_cheats`**: `SV_Map_f` only turns it on (devmap, or developer 2 with
  thereisacow 1960), so every server, listen or dedicated, keeps the default 1.
  sv_cheats is write-protected and read-only (0x10|0x40 from two registrations),
  which lets the console set it only to its default: `sv_cheats 1` works, `0`
  doesn't; `disableCheats` (a UI command, nothing in the MP menus runs it) turns
  cheats off until a devmap or `sv_cheats 1`. Systeminfo goes out only in
  gamestates (configstring 1 is rebuilt for each), so `SV_PreFrame` clears the
  modified flag after a mid-match change instead of asserting every frame; the
  release build ignored it.
- `KB_MENU_CMDS` (linux_main.cpp): KB_CMDS's syntax, timed from startup and run
  once, connected or not, for the main menu (`openmenu cac_main`, ...).
- **Portable**: on macOS/Linux `Sys_DefaultInstallPath` (zones) and the `fs_b`
  default (base path; `fs_h`, where everything is written, defaults to it) are
  the executable's directory when `main/` and `zone/` are there, else the current
  directory as before (Windows unchanged: the original used the current
  directory, where Windows starts a program). The base path dvar is `fs_b`
  (`fs_basepath` never existed here) and doesn't move the zones, which load
  before the filesystem starts. `make_portable.sh` copies the build and every
  non-system library it links (plus SDL3, which sdl2-compat loads from next to
  itself), rewrites the references to `@executable_path/lib/`, and re-signs
  (ad hoc). Rerun it after each build; the dev workflow (build_macos/blackops
  started in the game folder, Homebrew libraries) is unchanged.
- Writes that ignored the game folder: the `screenshot` command's
  `FS_BuildOSPath(fs_gamedir, 0, ..)` had lost its fs_homepath argument (IW3 has
  it) and wrote to `<current directory>/main/screenshots` (every build);
  `LiveSteam_Init` deleted `steam_appid.txt` in the current directory (Windows
  only now; Steam is stubbed elsewhere). Debug-only writers still use the current
  directory: `quickprint.log` (cg_draw_debug), `dx.log` (`r_logFile`), and the
  `KB_*` dumps (their own paths). Bink's video path (`r_cinematic.cpp`) is the
  current directory too, but Bink is stubbed.
- **Main-thread deadlocks (macOS)**: the render thread asks the main thread for
  window work (`Sys_RunOnMainThread`: context creation, resize) and SDL3 asks it
  for GL context updates in the buffer swap after a window change
  (`dispatch_sync` to the main queue), while the main thread can be blocked
  waiting for the render thread. Applying graphics settings hung both ways:
  `R_CheckResizeWindow` waits in `Sys_WaitD3DDeviceOKEvent` for the reset whose
  `GLDevice::Reset` resizes the window, and in a match the main thread waits for
  each frame (`Sys_WaitRenderer`) whose swap dispatches the update. Fixes:
  `WaitForSingleObject` on the macOS main thread waits on events and semaphores
  in 2 ms slices and runs posted main-thread work between them (a signal still
  wakes it at once); `SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH` 1, SDL's documented
  switch for a GL render thread whose main thread waits on it.
- **Fullscreen/vsync in the GL backend**: `D3DPRESENT_PARAMETERS::Windowed` and
  `PresentationInterval` are honoured at CreateDevice and Reset. Fullscreen is
  SDL's desktop fullscreen (covers the display, no mode switch; no Spaces
  animation, `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES` 0); Present scales the back
  buffer to the window keeping its aspect ratio (black bars) when their sizes
  differ, and `IN_Frame`/`IN_SetCursorPos` map the cursor through the same
  rectangle. The window's pixel size is read on the main thread (creation,
  resize, fullscreen switch, SDL size events: `KB_GLNoteWindowSize`) and logged
  as `[gl] window WxH [fullscreen]`. Vsync: `SDL_GL_SetSwapInterval` (it was
  never set: always off).
- **Field of view 80** (the user's request): `cg_fov_default` (the options slider,
  65-80) and `cg_fov` default to 80 instead of 65, so fresh configs and the
  options' reset give 80. The view took `cg_fov_default` only at each spawn
  (`CG_SetThirdPerson`); `CG_GetViewFov` applies a change at once now. The PC
  scripts never send `cg_fov` in play (`setThirdPerson` returns unless
  `level.console`), so nothing else overrides it.
- **Sound lock order** (every build): `SND_FindAliasFromId` took
  `CRITSECT_SOUND_LOOKUP_CACHE` (the sound log's name cache) around a bank
  lookup, then `CRITSECT_SOUND_BANK`; `SND_AddBank` holds the bank lock and
  re-applies patches through `SND_FindAliasFromId`. A menu sound while a
  fastfile added a bank deadlocked the render and loader threads at startup
  (seen once). The lookup no longer takes the log's lock.

## Decisions (session 7)

- **Offline stats storage** (live_storage_win.cpp): one file `players/mpstats.dat`
  (header + the CAC, global and basic-training buffers, each with its checksum;
  written to `.tmp`, previous save kept as `.bak`, an unreadable file renamed
  `.bad`). `PC_InitSigninState` signs the local player in on a client (XUID = the
  Steam/stub SteamID, now 64-bit; gamertag = `name`), which reads the file or
  creates fresh stats (`mp/stats_init.cfg` gives the default custom classes).
- **Buffers**: DDL permission 1 = client (CAC loadouts, weapon attachment bits),
  2 = server (PlayerStatsList: RANKXP/CODPOINTS/PLEVEL, challenges, item
  `purchased`/`new` flags - but the client keeps those in its CAC buffer), 3 =
  both. The client reads currency/prestige from the CAC copy of PlayerStatsList;
  the global buffer is authoritative and copied over it at every commit.
- **Listen server**: the host's stats are copied into `client_t` once per
  connection (`SV_SendClientGameState`, `statPacketsReceived`); from then on the
  server's copy is newer (kept across map changes) and reaches the client as the
  `N` command. The host's custom classes are read live from the client buffer
  (`SV_GetClientCACStats`). Bots and remote clients get zeroed stats; a client
  ignores `N` from any server but its own (`com_sv_running`), so a dedicated or
  someone else's server can't overwrite it. `SV_IsLocalStatsServer` makes
  `IsGlobalStatsServer()` true (scripts: `level.rankedMatch`) and `SV_Map_f` sets
  `onlinegame 1` plus the XP/CP rates `default_xboxlive.cfg` would (scr_xpscale 1,
  scr_codpoints*scale 0.1) when unset. Without onlinegame the scripts read custom
  classes from the gamer profile (a stub returning 0).
- **Commit** (`LiveStorage_UploadStats`: the menus' `uploadstats`, the join+1 s `Q`,
  map loading; `CL_UploadStatsForController` on disconnect/quit): ports the ranked
  server's `SV_ValidateClientCAC`, charging new purchases/prestige to the global
  stats against the last validated classes. While a local match holds the stats
  it only previews (purchases charged when the host leaves; class edits that
  charge nothing are accepted at once). `N` changes to PlayerStatsList are
  mirrored into the CAC copy as deltas, so CP earned mid-match is spendable.
- Restored from the strip/decompile: `GScr_GetDStat`, `LiveStats_SpendCurrency`,
  the AAR comparison (`LiveStats_CompareStatsVsStableBuffer` on disconnect and
  in challenge sorting), `CL_PrestigeRequest`'s commit. Removed `iassert(0)` from
  `SV_GetClientDIntStat/DInt64Stat`; the 64-bit getters return 64 bits.
- Decompile bugs found on the way (every build): ~50 `DDL_MoveTo` calls whose last
  path names were dropped (count > args, varargs read garbage: item
  purchased/new flags, weapon attachment/option bits, emblems, item stat
  iteration, combat record; one combat-record name, `"used"`, is a guess);
  `LODWORD(v) = Live_GetXuid()/DDL_GetInt64()` kept only the low half (18 sites);
  `Dvar_SetFromLocalizedStr_f` (`setFromLocString`) had its buffer split into
  `char combined; char pszInputBuffer[4099]` (ASan stack overflow).
- 64-bit: `expressionEntry` is 24 bytes, allocated as 16 (`Expression_Alloc`):
  runtime `if ( ... )` commands (cfgs, menu `execNow if`) crashed in `MakeRPN`.
  Sizes use `sizeof` now and the command buffers scale with it.

## Decisions (session 6)

- **POSIX UDP** (`sys_net.cpp`) ports win_net.cpp's IP socket: `net_ip`/`net_port`
  (3074, tries the next 9 ports), non-blocking, SO_BROADCAST, `getaddrinfo` for
  names, `getifaddrs` for the local addresses that `Sys_IsLANAddress` compares,
  `net_noudp` and `net_restart`. Not ported: the SOCKS proxy, the client socket pool
  (`cl_socketpool_enabled` defaults to 0) and the remote script-debug sockets.
- **Offline connect without Steam** (`stubs_online.cpp`): the handshake carries a
  Steam ticket (getchallenge) that the server checks with `Steam_CheckClientTicket`.
  Without Steam the client sends the placeholder ticket "offline" and the stub
  user's SteamID (the same for every client), and a non-Steam server accepts any
  nonzero SteamID: there is nothing to verify against (LAN/offline play). A Windows
  Steam server rejects these clients. `CL_CDKeyValidate` calls
  `Steam_UpdateClientAuthTicket` on every platform (the stub returns true).
- `CG_Vehicle_DoControllers` is upstream's simplified rewrite of the decompiled
  function (kept under `#if 0`); it lost the `boneIndex < 0xFE` guards on the gunner
  turret, wheel and extra-wheel tags, so a vehicle without those tags (255) indexed
  `partBits[7]` of a 5-word stack array and could call `DObjSetLocalTagInternal` on
  bone 255 (writes past the bone matrices). Guards restored (every build). The rewrite
  also drops wheel suspension, steering and child-bone rotation (vehicle visuals);
  session 11 rewrote it from the original (its angle vectors made the bones NaN).
- `Com_Init`: when a startup command fails (`+connect` to a bad address), the error
  cleanup unloads ui_mp right after `Com_InitUIAndCommonXAssets` loaded it, and the UI
  started without `ui_mp/menus.txt` ("Could not find menu 'main'" every frame, no
  menu). ui_mp is now reloaded right after that cleanup, as `Com_AssetLoadUI` does.
- `UI_RunMenuScript`'s "stop refresh" branches wrote `dc[1].localVars.table[65].name`:
  on i386 that is `uiInfo_s::nextFindPlayerRefresh` (offset 10080), on 64-bit 3312
  bytes past `uiInfoArray`, i.e. 8 zero bytes into another global every time the
  main menu opened. Found by the ASan client started at the main menu.

## Decisions (session 5)

- arm64 memory ordering: acquire/release fences (`std::atomic_thread_fence`, no
  code on x86) where a thread reads data another thread published through a plain
  variable: the dvar read lock (after the writeCount spin), job-queue completion
  (`jqPoll`'s "done" result, the end of `jqFlush`), the stream IO thread's
  request status (texture buffers), deferred server->client packets and the glass
  action ring, on top of session 4's sound queues. Hand-offs through events,
  critical sections or the Interlocked wrappers (full barriers) need nothing.
- ASan: under `-fsanitize=address` the region allocator poisons a guard granule
  after each allocation and freed ranges; the small-block heap adds a 16-byte
  redzone and poisons slack and freed blocks. The first run found the physics
  debug buffers allocated with their i386 size.
- `KB_CMDS` scripted console commands (linux_main.cpp) for unattended play.
- "Tried to perform a stats operation before the stats have been fetched" prints
  once (it fired every frame); the underlying issue is next step 2.
- The session-1 cast rewrite also produced `Ptr32_Decode(<size>)` comparisons
  (glass shard Defrag assert) and hid out-of-bounds reads into the next global
  (live_contracts display order); FindCycleBFS's stack-local queue is a struct now.
- qsort comparators over arrays of native pointers that read the element as a
  32-bit value and decoded it sorted by garbage (the low half of a 64-bit pointer;
  it only works when the image base's low 32 bits are 0, as under lldb): HUD
  elements (`compare_hudelems`), unlockable items, and the **network Huffman tree**
  (`nodeCmp`), which came out ASLR-dependent, so two 64-bit processes could not
  read each other's packets (the listen server shares one tree, so it never showed).
- `GfxCmdBufState` stats pointers (`prim.viewStats/primStats/backupPrimStats`)
  point into the state's own `frameStats`; R_DrawCall's stack copies left the global
  pointing into a dead frame (later draws added counts into other functions'
  locals). `R_RebaseCmdBufStats` re-points them after each copy (every build).
- `R_SetDLightsConstants` (upstream reconstruction): the spot light def was stored as
  8 bytes into `diffuseColor[0]` (clobbering an omni light's colour, or the pointer
  read back with an omni's colour in its high half), and the spot matrix transform
  read a 4th component past `lightAttentuation` (now w 0, then `[3][3] = 1`).
- `cdl_proftimer::reset` sorted `mx[5]` with `i < 5` (read and swapped one past).

## Decisions (session 4)

- **The GL backend renders in D3D's row order.** The translated vertex shaders (and
  the built-in XYZRHW program) negate clip-space Y, so row 0 of every target is
  D3D's top row: render targets sample the right way up with D3D texture
  coordinates, StretchRect/viewport/scissor rects need no flip, and gl_FragCoord.y
  counts from the top like vPos. The back buffer is an offscreen FBO
  (`GLDevice::backbufferFbo`, colour + depth-stencil renderbuffers); Present blits
  it to the window upside down. Before this every post pass that read a render
  target flipped the image, and the cull mapping/viewport flips were compensating
  for the window only.
- Cull: GL's front face is always CCW (= D3D's CW), D3DCULL_CW culls GL_FRONT, so
  `gl_FrontFacing` matches D3D's vFace.
- `SetRenderTarget(0, ..)` resets the viewport (depth 0..1) and scissor to the new
  target, as D3D9 does; the renderer relies on it for its downsample chains (the
  480x270/240x135 passes drew with a 1920x1080 viewport).
- D3D9 pixel centres are at integer coordinates: vertex shaders add half a pixel
  (`kbPosFixup`, per viewport size, as Wine does) and vPos is `floor(gl_FragCoord)`.
- Sampler `sN` is bound to texture unit N at a program's first use on native GL
  (it was only done in the web build's verified-bind path; every sampler read unit 0
  and draws mixing 2D and cube samplers failed with GL_INVALID_OPERATION: the whole
  lit world, and the HUD icon that drew the font atlas).
- Sound command/notify queues: acquire/release fences (`std::atomic_thread_fence`,
  no-ops on x86) around the lock-free consumer side.
- Stride audit: the struct sizes of all 1560 IDA-annotated header structs were
  probed in both ABIs (557 differ); byte arithmetic that multiplies or divides by
  one of their i386 sizes found devgui (40), XAnimClientNotify (24, the crash),
  UILocalVar (12), static_model_leaf_t (8), the fx_marks point-group limit (68)
  and a corpse clientInfo_t lookup (`tools/audit_stride.py`); a grep for byte
  pointer differences divided by a literal found a pointer array counted in 4-byte
  slots in phys_collision.cpp. The remaining audit_stride hits are file formats,
  bytecode, script memory nodes and asset structs with the same layout.
- `Ptr32_Encode` slow path (the old "performance pass" item): measured ~16K calls
  per map load and none during play; not a hot spot. SV_LinkEntity's NaN check
  decoded float bits as a pointer (fixed).

## Decisions (session 1)

- **Ptr32 + 32-bit linear window** instead of retyping the engine to native
  64-bit pointers: thousands of sites keep pointers in ints, and bytecode, assets
  and network fields rely on 4-byte pointers. docs/64bit.md has the details.
- **Heap inside the executable image** (1.5 GB zero-fill array): globals and heap
  share one linear window. ~1.9 GB is the most macOS loads below the dyld shared
  cache; 2 GB fails.
- **Encode passes values below 4 GB through; Decode of an unissued handle returns
  the value**: decompiled ints typed as pointers, and the loader's raw on-disk
  offsets, survive.
- **Asset, networked and offset-addressed structs keep the i386 layout**
  (`Ptr32` fields): the 732 asset structs, playerState_s/entityState_s/etc.,
  centity_s, WeaponFullDef, vehicle_info_t, VariableStackBuffer, GfxModel*Surface.
- **Decompiled structs larger than the original allocations**: allocations use
  max(original literal, sizeof) so 32-bit builds never shrink (client_t, IKState,
  vehicle_cache_t, cg_s).
- Networked structs match the i386 layout (layout_diff), so 64-bit clients can
  stay wire-compatible with 32-bit builds. Not tested.
- Five parallel agents fixed the compile errors per module (script, UI, game,
  physics, renderer); their reports' open items are above.

## Decisions (session 2)

- `Scr_Settings` honours `developer_script` (it compiles the `/# #/` blocks);
  `developer` and `abort_on_error` stay forced off as upstream had them.
- `VM_Execute_0` is built unoptimized on clang/GCC (`SCR_VM_SETJMP_SAFE`) rather
  than making its locals volatile: the error path resumes the loop with them.
- `-fno-strict-aliasing -fwrapv` on macOS and Linux.
- Struct sizes in allocation budgets follow the allocations (`sizeof`, or
  max(original literal, sizeof) where the original literal was larger).

## Decisions (session 3)

- **Stats buffer = the installed data's size**: `STATS_BUFFER_SIZE` 40548
  (live_storage.h; the 7.0.61 exe had 40168). The stat upload and stats file are
  stubbed in this codebase, so only the server->client modified-stats message (4-hex
  offsets, fine) depends on it; `MODIFIED_STATS_BYTE_SIZE` derives from it.
  `persistentStats` had a stale IDA size and its flags are named fields now.
- Linux/macOS `main` runs WinMain's pre-`Com_Init` steps (Com_InitParse, Dvar_Init,
  InitTiming, Sys_FindInfo): without Dvar_Init `set`/`seta`/`toggle` were unknown
  commands (saved configs never loaded) and the zero timer scale broke the resource
  wait (fastfile loads 4-8x slower).
- POSIX `Sys_GetInfo` reports real memory/CPU (sysctl, /proc/cpuinfo) with the same
  "2.4 GHz swag" times core factor as Windows' stubbed benchmark; with no dialog,
  changed hardware or configure_mp.csv re-runs autoconfigure (Windows asks).
- GL backend on desktop: `g_kbPresentEnter` counts presents (the per-frame program
  link budget never reset, so only 4 programs ever linked); NOOVERWRITE/DISCARD
  buffer ranges upload through an unsynchronized `glMapBufferRange` (Apple's GL made
  each in-flight `glBufferSubData` a Metal blit: 6 fps -> 100).
- SDL events are pumped only on the main thread (the render thread reached it via
  `Sys_LoadingKeepAlive` during map loads; AppKit throws).
- `tools/audit_memlit.py` looks through casts on the destination and matches
  `literal * count` sizes (found the scene, flame, huffman and graph clears).
