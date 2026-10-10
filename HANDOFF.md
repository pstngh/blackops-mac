# HANDOFF — native macOS (Apple Silicon) port

Branch `mac-port` of https://github.com/pstngh/kisakblack-mac. Local checkout:
`/Users/pstn/Documents/kisakblack-mac`. Game data: `/Users/pstn/Documents/Games/codbo`
(Steam Black Ops: `main/*.iwd`, `zone/Common`, `zone/English`). Build, run and
debugging commands are in CLAUDE.md; the 64-bit design and rules in docs/64bit.md.

## Goal

A native arm64 macOS build of the multiplayer executable: no Rosetta, no Wine.
Apple Silicon has no 32-bit mode, so this is also the engine's first 64-bit port.

## Status (2026-10-10, session 13)

- **Builds and links**: `build_macos/blackops`, Mach-O arm64, and the ASan build
  `build_asan`. Every changed file passes the i386 syntax check (gfx_gl/ and
  platform/sdl/ files: only the known GLEW/SDL header artefacts).
- **Dedicated server** with 4 bots: deaths, killstreaks, ragdolls, no asserts; also
  clean under ASan with heap redzones (120 s).
- **Client**: main menu, team menu over the blurred world, local matches that render
  on every MP map; 116-304 fps uncapped on an M4 at 1280x720 (Performance below). Keyboard, mouse and console work.
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
  clouds, stencil). GL stays the default until the user switches (Metal backend
  below, Next steps 0).
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

`KB_RENDERER=metal` makes `Direct3DCreate9` (src/gfx_mtl/mtl_window.mm) return a D3D9
device on Metal instead of the GL layer's; the engine's renderer (src/gfx_d3d) is
unchanged. Same binary (cmake/macos.cmake: Objective-C++ with ARC, Metal, QuartzCore,
Foundation; no new dependencies); GL stays the default and the game folder's portable
copy keeps GL (make_portable.sh not rerun) until the user switches. Design and the
reasons behind it: Decisions (session 13).

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

0. **Metal backend: the user's decision, then leftovers** (Metal backend above; built
   in session 13). The rules agreed with the user (2026-10-09) still hold for any work
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
   Leftovers:
   - Make it the default if the user wants: `KB_UseMetal()` in mtl_window.mm (GL with
     `KB_RENDERER=gl` then), and only then rerun `tools/make_portable.sh` for the game
     folder.
   - Check the display rate with the screen unlocked: every run of session 13 had the
     screen locked (nothing composited; `nextDrawable` paced at 120/s). `KB_FPS=1`
     prints `presented/s` and `shown/s` (frames the display reported showing). If an
     unlocked 240 Hz display shows ~120, try CAMetalDisplayLink with a 240 Hz
     `preferredFrameRateRange` (under the lock it starved, so it is untested).
   - The first match after a new build can stall (up to ~0.7 s seen) where a draw waits
     for a shader batch still compiling with a cold system cache; measure that with
     `KB_FPS`'s pipeline line before changing the batching.
   - CPU at view A, if a profile at the user's settings asks for it: occlusion-query
     polling (7%), the model lighting volume's staging buffer and relock copy each
     frame (~5%). The engine's frame pacing caps lighter views at ~730 fps.
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
6. Drive the main-menu menus by hand (keyboard/mouse) beyond the screens
   `KB_MENU_CMDS` reached (`cac_main`, `cac_weapon`, killstreaks): attachment/
   camo/reticle pickers, emblem editor, clan tag, barracks, combat record,
   after-action report; combat training (`xblive_basictraining`, its own buffer
   and rank, not maxed) is untested. To show a prestige other than 15,
   `LiveStorage_SetTopRank` is the place (a dvar would do).
7. Keep soaking with `KB_CMDS` (longer matches, other gametypes) and the ASan
   client, at the main menu too; each fix of this kind so far came from a run.
8. Rendering fidelity: compare against a Windows screenshot (shadows, reflections,
   gamma). `vFace`, `vPos` and the half-pixel offset follow D3D9 now; textures
   stream their high mips since session 10.
9. Later: wire compatibility with a Windows/Linux server, an .app bundle (a
   double-clickable app; today `codbo/blackops` opens in Terminal), controller
   support.

Reading game data: the scripts, menus and string tables (e.g. mp/statsTable.csv)
are inside the fastfiles; session 7-8 extracted them with throwaway scripts that
are not in the repo (decompress the .ff's zlib stream, then scan for RawFile and
StringTable assets; table strings shared with earlier assets are references
that weren't resolved). Item indices are listed in CLAUDE.md.

## Open items found but not fixed

Seen in session 13:
- Two more scalar locals passed as vectors (Decisions session 13, CanDamage):
  `g_missile.cpp` ~3499 and `cg_compass.cpp` ~2992.
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
- **Shadow maps compare in the shader**: D3D9 drivers return a filtered comparison
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
  (a grenade's `G_RadiusDamage` with bots). They are `float[3]` now. A scan found two
  more of the kind, not fixed (a separate task): `g_missile.cpp` ~3499 (missile
  attractors, `Vec3NormalizeTo(&perpDir[2], &perpDist)`) and `cg_compass.cpp` ~2992
  (`Vec2Length(&v22)`).
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
