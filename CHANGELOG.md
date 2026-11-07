# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: [SemVer](https://semver.org/).

## [Unreleased]

### Linux runtime (branch `linux-runtime`)

- the runtime builds and runs on Linux (`tools/build_linux.sh`): a Win32 compatibility layer
  (`runtime/plat/`, POSIX + SDL2 + software GDI + stb_truetype fonts) so the game code stays
  single-source, with `#ifdef __linux__` only in the genuinely platform-bound spots
  (coroutines in SysV AMD64, the TinyCC JIT on `libtcc.so`, SDL2 audio, the Vulkan Xlib
  surface, libpng/libjpeg codecs, the translator on the system Python, mods as `plugin.so`);
- Direct3D 11 does not exist on Linux: the game falls back to Vulkan or the software renderer;
- known gaps, all logged: the update check is unavailable, exclusive fullscreen is borderless,
  and the DWM vblank clock is replaced by the nominal frame grid;
- `tools/build_linux.sh` works out of the box on a plain system: the dependencies resolve
  through `pkg-config` (a missing one is named, with the package line to install), `stb_image.h`
  and `stb_truetype.h` are vendored in `runtime/plat/stb/`, TinyCC's `libtcc.so` (not packaged
  by any distribution) is compiled once from the official 0.9.27 source by
  `tools/build_tcc_linux.sh`, each source prints a `[n/N]` progress line, and, like the Windows
  build, the repository root becomes a ready-to-run install (`TagForceRecomp`, `tcc/`,
  `include/psp_rt.h`);
- the JIT compiles the translated modules on Linux: the paths that reach TinyCC use real
  separators, `psp_rt.h` includes the system's libc headers (the Windows bundle carries its
  own), and `__declspec(dllimport)` is Windows-only;
- the PSP's 1 GiB address space is a `PROT_NONE` reservation with `mprotect()` commits (a Linux
  `mmap()` never overlays an existing mapping, so the first `MEM_COMMIT`s landed elsewhere and
  every start died with `falha ao mapear memoria`), and the `gen/jitcache/` object cache
  round-trips: after 0.9.27 TinyCC renumbered `TCC_OUTPUT_OBJ`, so the runtime now asks the
  library itself which output type writes an ELF `ET_REL`, validates every object it writes
  and skips the cache when the disk is nearly full;
- `libtcc1.a` (which every module link needs) is searched in the bundle, next to the loaded
  `libtcc.so` and in the usual distribution spots (`/usr/lib/tcc` on Arch): TinyCC 0.9.27 opens
  `<tcc_set_lib_path>/libtcc1.a` while 0.9.28rc looks for a bare `libtcc1.a` in the library
  paths only (both checked). No more hand-made symlink in `/usr/lib` to start the game;
- `tools/build_linux.sh` bundles ONLY the tested TinyCC 0.9.27 (the version the Windows release
  uses): a system `libtcc.so` of any other version - Arch's 0.9.28rc, whose code generator this
  game was never verified with (a first real boot crashed inside the recompiled BOOT) - is
  refused and the official source is built instead; a stale `gen/linux/tcc` from an older build
  is rebuilt the same way. `tools/build_tcc_linux.sh` now also builds `libtcc1.a` (without
  `bcheck.o`, whose 0.9.27 source does not compile against glibc >= 2.34) and fails loudly when
  it is missing instead of silently shipping a bundle nothing can link. The JIT logs the TinyCC
  version and where it came from: `[jit] TinyCC 0.9.27 (tcc/libtcc.so)`;
- the crash report says what actually happened: an access inside the 1 GiB PSP window prints as
  a guest address, a NULL prints as a native null pointer (the old `end PSP 0xffff8073...` for
  `fa - g_mem` was meaningless), the RIP is resolved to a named function of the executable
  (`-rdynamic`) or to the JIT range of a module (the linked code is mmapped by the runtime now),
  a correct `addr2line` hint with the real load base is printed, the native call chain above
  the fault is listed, and **the syscall in flight is named with the arguments the game passed**
  (`syscall em execucao: <lib>::<funcao>(a0..a3) ra=...`) - `psp_call` notes it before running
  the (static, unnameable by `dladdr`) HLE body, so a crash inside one no longer needs
  `addr2line` or a re-run with `HLE_TRACE=1` (the first full-data boot crashed exactly there:
  a native NULL with `RIP <sem simbolo>+0xda671` right under `psp_call`).
  `GetModuleHandle(nullptr)` returns the executable's load address (it used
  to return a `dlopen` handle, so `exe+offset` was garbage); `TF_CRASH_TEST=1` triggers a
  controlled crash to check all of this;
- blocking syscalls (sema/event-flag/vblank waits, delays, thread waits, FPL allocation) called
  without a current thread - a game routine run in interrupt context (the vblank of the
  scheduler's idle point) or a native module init - return the PSP's context error
  (`0x8002019a`) and log which call it was, instead of dereferencing the null current-thread
  pointer (that was a `(nil)` crash with the RIP inside the syscall's body); the checks that
  can be satisfied without blocking (a sema already signalled, event-flag bits already set)
  still succeed;
- the watchdog only counts missing frames once the scheduler starts (a 14 s JIT compile on a
  laptop is not a hang), and the `[iso] arquivos do jogo` line is logged once (the installed
  folder was opened twice: the background reader thread started twice too);
- the file installer (`--install`) and the command line work with real paths (argv values are
  no longer rewritten as Windows paths, and directory creation uses forward separators);
- the launcher builds the game: the Build page runs `tools/build_linux.sh` (the `[n/N]` lines
  are its progress bar) and Play starts the game from `<workspace>/source` - the Tools page
  now checks what the build actually needs (SDL2, the FFmpeg libraries, PNG/JPEG, Vulkan;
  CMake and Ninja are no longer required), and the CI artifact carries only the
  release tarball;
- **the payload carries what the player's machine must not need to build** (`prebuilt/`): the
  Vulkan shaders, already compiled to SPIR-V (`gpu_spv.h`, regenerated by
  `tools/build/spv2h.py`; `glslc` is only needed to recompile them after an edit), and
  TinyCC 0.9.27 (`tcc/`, `libtcc.so` with `libtcc1.a` and its headers), built at package time
  by `tools/build_tcc_linux.sh` from the official source - on the CI's oldest supported
  runner, so the `.so` loads on any newer glibc. `tools/build_linux.sh` prefers that copy
  over compiling TinyCC locally (a first Build on a machine whose TinyCC is another version,
  or where the source download stalls, simply no longer compiles or downloads anything);
  `tools/launcher/package_linux.py` builds and verifies it into the tarball, refuses to
  package a `libtcc.so` that is not the tested 0.9.27, warns if one needs a glibc newer
  than 2.35, and the Tools page now reports both as bundled.

### Added
- **Linux launcher** (`tools/launcher/`): an SDL2 + Dear ImGui window in the style of the DQ8Recomp launcher, with the pages **Your disc, Tools, Options, Build, Play**. It recognises the European disc (ULES-00600) from the ISO itself, checks the tools and libraries the build needs, copies the game files, translates the game's code through the same Python tools as the Windows release and compiles the runtime; Play starts the game. `tools/launcher/package_linux.py` builds the Linux release - an executable plus a `payload/` folder with the source - as a tar.gz, with SDL2 bundled; `.github/workflows/linux-launcher.yml` builds, tests (synthetic disc, smoke test, screenshots of every page) and publishes it as a prerelease on pushes to master. See `docs/LINUX.md`.

## [0.6.0] - 2026-11-02

### Added
- **Controls page** in the launcher: every action has a keyboard field and a controller field, assigned by pressing the key or button (LT/RT included). Generic DirectInput controllers are supported too, with numbered buttons you can remap. Run follows Cancel / Circle. Escape cancels a capture, Delete clears a slot, "Restore defaults" resets the layout. Changes apply after restarting the game.
- Cheats: **Remove banlist** (three copies of any card) and **Unlimited card copies** (owned cards can fill the deck in the editor; the save collection is unchanged).
- Cheats: "Unlock everything" also unlocks the Recipe Viewer entries and story parts 1-3.
- Plugin API 8: a mod can ask which 2D pieces the game drew over a box, so its sprites can hide under game windows.
- **Monsters on the field** shows the opponent's monsters too, fits each one in a small box and steps aside when a large game window covers it. It is listed in the launcher again (still experimental).
- **TF3 Cards** (experimental): many more card effects (Slifer the Sky Dragon, Twister, Graveyard and hand effects, equipment and continuous Spells, destroy-all effects, battle-destruction triggers, draw and life point effects and more). Some cards are plain data without a duel-tested effect.
- The launcher checks GitHub once at startup for a newer release and shows an "Update available" button on the bottom bar. It opens the release notes (the GitHub release text) with "Update now" (opens the download page) and "Later". The About page shows the current version. Turn it off with `verificar_atualizacao=0` under `[geral]` in `tagforce_recomp.ini`.

## [0.5.2] - 2026-10-28

### Added
- **TF3 Cards** mod (experimental, switched off by default): adds the cards that exist only in Tag Force 3, with art and texts in five languages, three copies of each in the trunk and a 72-card deck limit guard. Some cards already have their effect (passive ATK/DEF, equipment bonuses, draw and life point effects, destroy-all, summon, FLIP and battle damage/kill triggers); the rest are plain monsters and spells. See the mod's README.

### Fixed
- 120 FPS: map rooms no longer freeze between real frames, and floor shadows no longer jump.
- The deck save screen reads the system clock instead of failing.
- A Field Spell added by a mod without a background file of its own uses the default background instead of hanging the duel.

### Changed
- Better AI, Better Decks, CPU vs CPU and TF3 Cards are now labelled experimental in the Mods page and in the README; only the randomizers, Booster box and Story Duel LP are considered reasonably stable.

### Removed
- The **Yugi** mod (an extra Free Duel opponent in an empty character slot) is no longer part of the repository or the Mods download: it did not work well enough.

## [0.5.1] - 2026-10-25

### Added
- **Mouse Menus** mod (experimental, `MouseMenus`): hover a row of the game's own menu lists to move the cursor, click to confirm. Switched off by default.
- The card text box in duels scrolls with the mouse wheel (over the box), PageUp / PageDown or the right stick.
- Cheats: "Unlock everything" also fills the DP to the game's cap and marks the duelists as fully owned, so the Recipe Viewer and Character Album show them.
- **Recipe Viewer tabs redrawn**: the "User" and "Character" tabs are now drawn from scratch at 8x in Enhanced Textures (tab outline and bevels from geometry, a simple sheet-of-paper icon, new lettering), not upscaled.
- **Texture cache is prepared on the first install**: when the Enhanced Textures pack is next to the program, "Install and play" also fills the BC7 texture cache (about 5 GB, a few minutes) so the first session starts without stutter. A pack that is switched off is only prepared if you agree. `TagForceRecomp.exe --prepare-textures` does the same from the command line.

### Fixed
- The HD opening movie no longer appears a moment late: the file is read and the first frames are decoded at startup, so the blurry original frame is not shown first.
- Weaker PCs: texture budgets are sized to the hardware, Vulkan pipelines are compiled ahead of time and the automatic resolution is capped.
- Audio no longer stutters while an HD texture loads; the BC7 texture cache is also warmed in the background to avoid stalls on the first open.
- Stability: GPU fallback state after a lost device, a clean exit on fatal errors, and clashes between plugin hotkeys.
- Deck randomizers find your deck in any Tag Duel slot.
- The interaction balloon above the player and NPCs (Room 204, Store) is drawn again.

### Changed
- The Enhanced Textures pack is regrouped by theme (`cards`, `characters`, `menus`, `duel`, `deck`, `text`, `world`, `intro`, `extras`) instead of ~120 folders; redraws live in a `pc/` sub-folder of their theme. `tools/mods/reorganizar_texturas.py` migrates an old layout.

## [0.5.0] - 2026-10-25

### Added
- **Four downloads** (see the README): the plain runtime, the mods, the Enhanced Textures pack and a Full pack with everything. Every mod and the textures start switched off.
- **Enhanced Textures pack** (HD menus, Deck Edit, duel screens, HD opening movie, EU card art), split into parts below 2 GB each because of the release size limit.
- **Card style** option (TCG / Anime) on the launcher, right below Textures.
- Launcher: the Textures row shows where the pack lives and has a Browse button; the Mods page has a Disable all / Enable all button.
- Cheats: save states (F1 save, F12 load, 4 slots). F4 cycles the frame rate 30/60/120 FPS.
- On-screen notices are drawn as an overlay panel, so they show in fullscreen.
- Recovery from a lost GPU device (the renderer is rebuilt; software fallback after repeated losses).
- Folder mode caches the game files in RAM in the background (no stall at the start of a duel).
- Plugin API 7: `set_bgm` replaces the next looping music; the Yugi mod plays its own theme only in the duel against him.

### Fixed
- Frame interpolation: sliding menu panels, the 3D summon cut and face-down defense cards no longer run at the base rate or flicker; steadier frame pacing after a late frame.
- Duel win screen painted orange (triangles beyond the far plane); seams between axis-aligned 3D pieces at 4x/8x.
- Hand-redrawn HD UI in the textures pack: Deck Edit, duelist list, Duel Result, menu frames and pills, HD digits.

### Changed
- **Cheats is now part of the recomp, not a mod.** It lives in the exe (`runtime/mods/cheats.cpp`) and no longer needs `mods/Cheats`; the launcher's Cheats page and the in-game keys are unchanged. Settings moved to `cheats.ini` next to `tagforce_recomp.ini` (the old `mods/Cheats/config.ini` and its on/off switch are imported on the first start; an old `mods/Cheats` folder is ignored).

### Added
- **Story Duel LP** mod (`StoryLP`): story duels start with 4000 Life Points for both duelists instead of 8000 (idea from a community comment). The value is `lp=` in the mod's `config.ini`; Free Duel keeps the LP chosen on its rules screen.
- **Exclusive fullscreen** (Direct3D 11). The launcher's Window mode now has three choices: Windowed, Fullscreen (exclusive) and Fullscreen borderless (the previous "Fullscreen"; saved settings keep working). If the driver refuses exclusive mode, or with Vulkan, the game falls back to borderless. Alt+Tab leaves exclusive mode and comes back with the window.

## [0.4.1] - 2026-10-22

### Added
- Modding: a minimal `HelloMod` template (`plugins/examples/HelloMod`), a list of the game functions mods can hook (`docs/HOOKS.md`), a `TFMOD_REQUIRE` version check for plugins, and `tools/test/sdk_check.sh`, which freezes the plugin ABI and checks that every mod still builds and loads after an SDK change.

### Changed
- Mod folders have English names: `BoosterBox`, `BetterDecks`, `BetterAI`, `MonstersOnField` (were `CaixaDeBoosters`, `DecksAprimorados`, `IAAprimorada`, `MonstrosNoCampo`). An older folder is renamed automatically at startup, keeping its settings and on/off state.

## [0.4.0] - 2026-10-21

### Added
- **Player Deck Randomizer** and **Opponent Deck Randomizer** mods: every duel, your deck and/or the opponent's becomes a random whole recipe of the game, and the deck's own name shows at the start of the duel. Each works alone or together.
- Card Randomizer mod: refills every duel deck with random cards from the whole card pool.
- Fast-forward cheat (game speed 1x to 8x): in game with L + R + Left / Right, or a starting speed on the launcher's Cheats page. The music is time-stretched, so it speeds up without changing pitch.

### Changed
- **The main download no longer contains any mods** (closest to the original game). All behavior mods are in a second file of the release, `TagForceRecomp-Mods-<version>.zip`; each starts switched off. Copy its `mods` folder next to the exe and turn them on in the launcher's Mods page.
- The launcher is more compact (smaller fonts and rows).

### Removed
- MSAA anti-aliasing (it cost performance and was barely visible).
- The game window's menu bar (File / View / Mods / Cheats / Help): every setting lives in the launcher now. F3, F5, F11, Alt+Enter and the in-game cheat combos still work. Plugin sliders (API v3 `add_slider`) are accepted but no longer shown.

### Fixed
- Deck Edit no longer freezes when moving a card from the Deck to the Trunk (a rewritten window helper ignored one of its arguments).

## [0.3.3] - 2026-10-21

### Fixed
- Audio on slower PCs: late frames no longer cause crackling or lost game time; the audio reserve adapts to the machine.
- Audio output rewritten (WASAPI with a continuous resampler) for cleaner, steadier sound.
- 60 FPS no longer stays suspended when a heavy scene is over; it backs off and recovers.
- Smoother frame pacing (D3D11 timing, three back buffers).
- Faster startup after the first run: compiled code is cached.
- Sharper application icon at every window size, correct at any display scaling.

## [0.3.2] - 2026-10-18

### Fixed
- Fixed the public build crashing when starting Story mode from Part 1.
- Fixed the public build crashing when entering the tutorial.

## [0.3.1] - 2026-10-18

### Added
- Yu-Gi-Oh! GX Tag Force logo as the application icon.

### Improved
- More stable 120 FPS pacing while exploring the map.
- Smoother audio transitions in demanding scenes.

### Fixed
- Fixed the public build closing immediately on startup.
- Fixed startup when the package is extracted to a path containing accented characters.

## [0.3.0] - 2026-10-18

### Added
- **Mods** page in the launcher: turn mods on/off and change their priority order (`mods/load_order.txt`).
- **Cheats** page in the launcher; "Unlock everything" also on the F2 key.
- Window mode (windowed / fullscreen) in the launcher and in the Graphics menu.
- Quick marks on the controller, no note box: L3 = graphics glitch, R3 = performance, Start+Select = audio (saves the last 8 s as a WAV).
- Continuous monitor recording to hunt flicker (`tools/test/jogar_gravando.bat`).

### Changed
- Cheats, CPU vs CPU and Monsters on the Field no longer appear in the mod list (they have their own menu).
- Smoother music/audio transition between areas.
- The mouse cursor hides after 3 s when a controller is on.
- The F8 mark shows the real FPS mode (120/60/30).

### Fixed
- 120 FPS: text letters appearing gray or sliding when a message changes; NPC head detached from the body; menu item bar bending; map character freezing or clipping when changing pose.

## [0.2.0] - 2026-10-17

### Added
- 120 FPS option (interpolation with measured pacing) and smoother 60 FPS during the whole session.
- HD textures compressed to BC7 (a quarter of the memory), with a disk cache and background loading.
- Title menu with Continue, New Game and Exit to Desktop (asks before leaving); player name typed inside the game.
- Graphics option inside the game and in the launcher, with an Original/Enhanced texture switch (F5).
- "Unlock everything" cheat: cards, packs and Free Duel duelists.
- Single problem mark F8 (or F7 / L3+R3): records graphics and CPU state with one note.
- `tools/test/tf.py`: plays the game by itself (buttons, captures, memory reads) for automated tests.
- Readable C++ decomp of the title, boot, story mode, map and duel result screen.
- Map of the 351 decks by duelist.

### Changed
- Menus, dialog boxes and the Duel Result screen redrawn in navy/gold in the Enhanced Textures package.
- The HD font is now part of Enhanced Textures (no option of its own).
- Only one instance per save folder.

### Fixed
- Lines and seams on the head of HD portraits; line through the middle of card art at high resolution.
- Polymerization cards disappearing; wrong deck in Free Duel (Zane got Jaden's).
- Flicker on message change and character entrance with 60/120 FPS; slowdowns on first run.
- Crash on the story map; wrong character schedule (player and director) on the map.

### Removed
- "Integer scale" and "Texture Upscaling" options (replaced by the Enhanced package).
