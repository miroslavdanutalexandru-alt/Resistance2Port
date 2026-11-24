# Resistance 2 PC port: community beta release notes

**Version:** 0.3.0-beta.1 (GitHub pre-release). **Release date:** 2026-11-21.

A native PC port of *Resistance 2* (PS3) built by static recompilation: the game's PowerPC and SPU code is translated to C/C++ and compiled for Windows. Graphics go through RPCS3's Vulkan renderer, built into the game as a library.

> **No game files and no prebuilt game are included.** This release is the source code plus Setup. You need your own legally dumped copy of the game (see *Install*); Setup builds the game on your PC from that copy.

This is a **beta**. The campaign has been played through the opening Iceland levels on one PC (Ryzen 5 5600X, GeForce RTX 5070, Windows 11). Later levels are untested. Please report what you find (see *Reporting problems*).

## 0.3.1-beta (unreleased)

### Controls
- **Weapon wheel on keyboard & mouse: the mouse picks a slot** (as in the Resistance 3 port, where a video review found the Marksman slot unreachable). About 0.2 s after the wheel opened the port took it for a menu (WASD then pressed the d-pad and the stick dropped to centre), and the mouse did nothing in the wheel. Now, while the weapon wheel key (Q, Triangle (Q)) is held, moving the mouse toward a slot selects it and the selection stays when the mouse stops; WASD diagonals are full length; the view holds still; and the choice is still there in the frame you let go of the key, when the game takes it. A quick tap works as before. `PORT_KBM_WHEEL=0` restores the old behaviour. *Not yet tested in play; please report how it works.*
- **No more split-screen from a second controller or a virtual copy of your controller (reported on Resistance 3, Resistance3Port #36, #37).** With DSX (or DS4Windows / Steam Input) running next to the default SDL3 controller support, the game saw both the real DualSense and the tool's virtual controller, treated them as two players, and pressing START could start split-screen. Now:
  - **Local co-op is off by default.** The game sees one player however many controllers are connected, and player 1 is **the controller you used last**: pick up any connected controller and press a button. To play split-screen, set launcher > Controls > **Local co-op (split-screen)** to **On** (`PAD_COOP=1`). The *Player 2 controller* and *Player 2 uses keyboard and mouse* options apply when it is on. Keyboard and mouse work as before.
  - **Better duplicate detection:** a controller that connects while another one is already playing (for example DSX's virtual DualShock 4 or Xbox pad, or a Steam Input pad) only joins once it is used on its own; if it copies several different inputs of the other controller, it is ignored (two real controllers simply held the same way are not mistaken for copies, and an ignored controller comes back when you press several different buttons on it while the other controller is untouched; it is then still watched, so a copy that comes back is ignored again). `PAD_DEDUP=0` turns this off. *Not yet tested with DSX; please report how it works.*
- **DualSense / DualShock 4 not detected: clearer `logs\run.log` (Resistance3Port #6).** You don't need DSX or DS4Windows: with *Controller support* on **SDL3** (the default), the game reads them directly over USB or Bluetooth. If one isn't seen, the log now lists controllers SDL3 found but can't use, notes when HidHide (used by DSX and DS4Windows to hide the real controller) is installed or a Steam setting may be hiding controllers, and says `controllers: XInput only` when the game was built without SDL3. If your controller only works with DSX, close DSX completely (also from the tray) and check that HidHide isn't still hiding the controller.
- **A real second co-op controller is ignored?** Unplug it and plug it back in (`PAD_DEDUP=0` turns the duplicate check off).

### New
- **Aim assist option.** Launcher > Controls > Aiming and sticks > **Aim assist**, or in game with F9 or *PC SETTINGS* > *PC CONTROLS & HUD*, where a change takes effect at once and is saved for the launcher too: *Auto* (the default) turns the game's aim assist off while you play with keyboard & mouse and keeps it with a controller; *On* keeps it always, as on PS3; *Off* turns it off for everyone. Aim assist is the slowdown when your aim passes over an enemy and the pull toward it. In split-screen, *Auto* only turns it off for the keyboard & mouse player. Enemies aim as before. `PORT_AIM_ASSIST=on` / `off` (unset = Auto). *Not yet tested in play; please report how it feels.*
- **Cheats (single player, off by default)**, contributed by @Jishnuraj07 (PR #14): **Ctrl+F1** toggles *God mode*, **Ctrl+F2** toggles *Infinite ammo* (`R2_CHEAT_GOD=1` / `R2_CHEAT_AMMO=1` turn them on at start). Once a cheat has been on, trophies are not unlocked until you restart the game. The keys do nothing in photo mode, in the port's menus or while F1 / F2 is bound in `controls.txt`, and cheats stay off in benchmark runs. Co-op is untested.
- **Stutter reports name their cause.** Frames of 30 ms or more (up to 10 every 5 seconds) now add a `[spike] cause:` line to `logs\run.log` with what the game actually waited on (for example GPU occlusion results, the Reflex sleep of Frame Generation, game code or SPU jobs), and every 5 seconds a `[spike] causes 5 s:` line counts all of them by cause. If you notice stutters, play a couple of minutes where they happen and send `logs\run.log`. `PORT_SPIKE_CAUSE=0` turns it off.

### Fixes
- **Cutscenes without dialogue (subtitles only) with Windows or the launcher's *Game language* set to Russian or Simplified Chinese.** For Russian, Korean and Chinese the game's movie player plays the English voice-over itself, but the port's language fallback (new in 0.3.0) still renamed the English voice tracks to the language the disc lacks, so the player found no voice track and the cutscene played music and effects only. Those languages now keep the movies as they are (`PS3_LANG_TITLE_ENGLISH`, default `8-11`; `none` = the 0.3.0 behaviour). English (UK), Polish, Portuguese (Brazil) and Turkish, for which the game really asks for a voice track the disc lacks, still get the English one. `logs\run.log` now has a `[dialogue]` line for every movie (language, the disc's audio tracks, any renamed track) and audio port (how it is mixed to stereo); `PORT_DIALOGUE_DIAG=0` turns them off.
- **Audio > Speaker layout now takes effect (#12).** The game picks its mix layout from the audio output's channel count, which was always reported as 5.1, so choosing **Stereo** (or 7.1) in the launcher changed nothing and every game mixed in 5.1, folded down to stereo by the port. The setting now sets that channel count: with **Stereo** the game makes its own stereo mix instead of the port down-mixing its 5.1 mix. `logs\run.log` shows the layout the game configured (`AudioOutConfigure(audioOut=0, 2 ch, ...)`). The port also zeroes any invalid (NaN / out-of-range) samples before output and reports them in `logs\run.log`.
- **Judder and doubled frames on screens where G-Sync / FreeSync isn't really working** (for example a 165 Hz laptop panel). The game's own pacing was even, but 60 or 120 frames a second on a 165 Hz screen that refreshes at a fixed rate are shown for an uneven number of refreshes. New launcher option Display > Frame rate > **Display sync** (`R2_DISPLAY_SYNC`): *Auto* (the default) measures whether VRR follows the game's frames (`[display] VRR effective: yes / no` in `logs\run.log`) and, when it doesn't, switches to VSync and a frame rate that divides the refresh rate (165 Hz: 55 fps, or 41.25 rendered -> 82.5 shown with Frame Generation; 144 Hz: 72), so every frame stays on screen equally long. *VRR* keeps everything as before; *Fixed refresh* always paces that way. *Not yet tested on a fixed-refresh screen; please report how it looks and include `logs\run.log`.*
- **Keyboard keys now follow the controller layout chosen in the game's own options** (#19). If the game's Controls screen used another layout (for example aim on R2 and alternate fire on L1) or crouch separate from sprint (crouch on L3), keys did the action of the new button instead of the action their launcher row names: the Aim key fired the alternate mode, the Crouch keys only sprinted. Every key now does what its row says, whatever layout the game is set to; controllers are unchanged. If you swapped keys in `controls.txt` to work around this, press **Restore default controls** on the launcher's Controls page (or swap them back). `PORT_KBM_LAYOUT=old` brings the previous behaviour back.
- **Launcher Controls page:** the last key of a game action can no longer be cleared by accident (right-click shows a message instead), and right-clicking an empty row puts its default key back. The Crouch, Sprint and L3 rows explain how they follow the game's layout.
- **Mouse look no longer reverses itself in fights.** The mouse's turn direction was learnt from how the view moved, so recoil, hits, aim assist and scripted turns could flip it mid-fight (`[mouselook] yaw direction now 1` / `-1` in `logs\run.log`). It now follows the game's own *Invert* options directly, every frame, and `mouselook_r2.cfg` is no longer used for it. `R2_ML_LEARN=1` brings the old learning back, `R2_ML_LEARN=0` fixes the direction (from `mouselook_r2.cfg`, or normal).
- **On a fresh install the game could close right after the main menu** (`fios read archive error ... global_cached.psarc` in `logs\run.log`). On its first start the game copies some of its archives to its data folder, and that copy was written over the archive with the PC settings menu (`mods\.cache\`), cutting it short. Mod and PC-menu files are now only ever used for reading; the game's own copies go to the game's own files. A damaged copy in `mods\.cache\` is detected and rebuilt automatically on the next start, so there is nothing to delete by hand.
- **Frame Generation: less HUD ghosting, and ghosting diagnostics.** On frames where the HUD could not be separated from the 3D scene, the HUD was interpolated together with the scene and left trails while turning. The HUD area of the last separated frame (under 2 s old) is now kept as a fixed UI layer on such frames (`R2_DLSS_FG_HUD_HOLD=0` turns this off; `R2_DLSS_FG_HUD_DEBUG=1` shows it in yellow). `R2_DLSS_FG_STATS=1` adds `[fg-stats]` lines to `logs\run.log` (HUD split rate, frame pairing, motion-vector statistics) and `R2_DLSS_FG_FX_MV=1` / `2` tries different motion for smoke, sparks and muzzle flashes. *Not yet tested in play; if you see ghosting, play a minute with `R2_DLSS_FG_STATS=1` and send `logs\run.log`.*
- **Frame Generation: fewer stutters and doubled-looking frames in play.** When the upscaler had to skip a single frame (`[r2aa] not applied: scene surfaces were replaced before the upscaler ran` in `logs\run.log`), Frame Generation switched off for three frames and came back with a reset, which showed as a hitch with repeated frames, several times a minute in some levels. A gap of one or two frames in the middle of play now resumes on the next frame (`frame generation: N-frame scene gap bridged` in `logs\run.log`); loading screens, movies and menus still pause it as before. `R2_DLSS_FG_GAP=0` restores the old behaviour.
- **Bryce Canyon, *Control Room*: the Swarm no longer drops the game to ~5 fps (#15).** The SPU job that runs the Swarm (and a few other effects) loads small code modules at run time; jumps through those modules' `switch` tables landed on code that had not been translated, and from there the whole rest of the job ran in the slow SPU interpreter (66 ms per run instead of under 1 ms; `unlifted job code at LS 0x07E58 ... -- interpreting` in `logs\run.log`). The code generator now translates those `switch` targets too. `[spu-jobs]` lines in `logs\run.log` now show `INTERPRETED X ms` for any job that still falls back to the interpreter. `R2_PIC_SWITCH=0` when generating the game code restores the old behaviour.
- **AMD Radeon: stretched "ghost" copy of the 3D scene (#16), still being investigated.** Reproduced on a Radeon 780M; it also happens with the upscaler off and at 1280x720. `logs\run.log` now has a `[r2rsx] gpu:` line with the GPU properties the AMD and NVIDIA render paths differ on (subgroup size, D24S8, framebuffer loops, attribute interpolation), and `RSX_BARY_MANUAL=1` makes AMD interpolate shader inputs the way NVIDIA does. The ghost comes from the 32-bit float depth buffer AMD uses in place of the PS3's 24-bit one (an NVIDIA card shows it too with `R2_HIGHP_Z=1`); The float buffer kept depth values beyond the far end of the PS3 range, which the 24-bit buffer caps, so the sky failed the depth test and the previous frame's content showed through. These values are now capped the same way, while geometry outside the game's depth range is still cut off as on the PS3 (`RSX_D32_UNORM_Z=0` turns this off; `run.log` shows a `[r2rsx] RSX_D32_UNORM_Z:` line when it applies). `RSX_D32_EMULATE_CMP=0` switches off the shader depth-compare emulation. Please report whether the ghost is gone.
- **Black screen for ~20 s after dying (Bryce Canyon), being investigated.** After a death the game showed no frame for about 20 s and went on the moment the window lost focus (alt-tab). When the game shows no frame for 5 s without being paused, `logs\run.log` now gets a `[noflip]` line saying what the game waits on (thread stacks in `logs\noflip_<tick>.txt`), and the port then releases all input for half a second, as an alt-tab does (only if the game is reading the pad and not loading files), and logs whether that ended the wait. If it happens to you, please send `logs\run.log` and the `noflip` file. `PORT_NOFLIP_WATCHDOG=0` turns the check off, `PORT_NOFLIP_NUDGE=0` only the input release.
- **Holar Tower: elevator checkpoint that never triggered.** In the Holar Tower elevator lobby (level `iceland2`) the crew could fail to move to the elevator, so the next checkpoint never came; RPCS3 has the same problem with its PPU LLVM recompiler but not with its interpreter. Two places where the game's PowerPC code was translated less faithfully than on a PS3 are fixed: the `fres` / `frsqrte` reciprocal estimates now return the PS3's own table values instead of exact results (`PS3_PPU_HW_ESTIMATE=0` restores the old ones), and `PS3_PPU_FMADDS_INTERP=1` makes `fmadds` round like RPCS3's interpreter, for testing. The level scripts' progress messages now go to `logs\run.log` as `[lua]` lines (`R2_LUA_LOG=0` turns them off), so the last one shows where a level is stuck. The elevator no longer waits forever: if the crew is not on it 20 s after the script sends them, the order is given again; if they are still missing once you have stood on the platform for a while, they are moved onto it, and as a last resort the elevator starts as the script would start it (`R2_HOLAR_ASSIST=0` turns this off; each step shows as a `[holar-assist]` line in `logs\run.log`). If Holar Tower still gets stuck, please send `logs\run.log`.
### Setup
- **Progress for every step, with time left.** Generating the game code now reports how far it is (stage by stage, function by function), and building the game shows *game code file X of N*, the file compiling right now and how long it has been at it, and a time-left estimate from how fast this PC really is. The game code files take very different times to compile, so the estimate now uses each file's measured compile time instead of counting build steps (no more "about a minute left" with a quarter of an hour to go). A *still working, last activity N s ago* note tells a slow step apart from a stuck one. One overall bar covers the whole install. The status line while the game code is generated no longer repeats "Generating the game code", and its first time-left estimate comes from how long each generator stage took on this PC.
- **Setup remembers how long each step took** (`logs\setup_timings.json`, and a summary in `logs\setup_script.log`) and uses it for the next estimates. `logs\setup.log` and the build logs can now be opened while Setup is running.
- **Faster updates.** When an update changes the code generator, files that come out the same as before are left untouched, so the build only recompiles what really changed. This version's code generator also translates more SPU code (the Swarm fix above), so Setup generates the game code again after this update; only the SPU files that changed are recompiled.
- **Add the optional upscaler / Frame Generation SDKs after installing**: run `Setup.bat -GetSdks` (or run `Setup.exe` again with the SDK box ticked). It downloads the XeSS / Streamline / DLSS files Setup can fetch and rebuilds only what uses them. The RSX core build now also notices SDK folders that were copied in by hand.
- **RPCS3 source download gets through more networks** (#13). When the secure connection to GitHub fails (antivirus HTTPS scanning, company proxies, unreachable certificate revocation servers), Git is tried again with Windows' own certificate store and without the revocation check, and the source archives are downloaded with `curl` as a second try. A proxy set in Windows is passed on to Git and to the archive downloads. If nothing gets through, Setup prints the exact links to download in a browser and the folder to put the `.zip` files in (`tools_cache\rpcs3_download\`); the next run uses them.
- **Building with an older Windows SDK works.** 0.3.0 only built with the Windows 11 24H2 SDK (10.0.26100); with 10.0.22621 the Direct3D 12 renderer failed to compile (`D3D12_DEPTH_STENCIL_DESC2`, `D3D12_FEATURE_DATA_D3D12_OPTIONS14`, `ID3D12GraphicsCommandList8` undeclared). It now builds with any Windows SDK from 10.0.20348 on and the game works the same: the driver, not the SDK, decides whether the newer D3D12 features are used. An SDK older than that now stops the build at once with a message saying which one to install, and the build log names the SDK used (`Using Windows SDK ...`).

## 0.3.0-beta.1 (hotfix)

Setup fix only; the game itself is unchanged from 0.3.0.

- **Updating over a 0.2.x install no longer fails** with `'Emu/RSX/port_texdiag.h' file not found` while building the RSX core (Resistance2Port #17, Resistance3Port #24). Setup kept the RPCS3 source folder from 0.2.x (`ps3recomp\third_party\rpcs3`) and never applied 0.3.0's patch to it. It now detects a source patched by an older version and patches it again automatically (the old changes are kept in git's stash there; a copy that is not a git checkout is downloaded again). `build.bat` also checks the patch before building.

If you installed 0.3.0 fresh and it built, you don't need this update.

## 0.3.0-beta

The first update focused on performance and stability. Every option below is in the launcher or `settings.bat`; the names in brackets are the variables (useful for bug reports). Run Setup again after updating so the game code is regenerated.

### Fixes
- **Permanent freezes:** fixed a lost wake-up in the runtime's wait slots (the compiler turned a 16-byte atomic read into a plain load), which could leave game threads waiting forever.
- **DLSS Frame Generation no longer crashes at boot:** if it can't start on your GPU or driver, the game starts with Frame Generation off. With Frame Generation on, the Reflex frame cap is no longer multiplied by the generated-frame factor (`R2_DLSS_FG_CAP=0` removes the cap).
- **Ambient occlusion now works with DLSS, FSR and XeSS** (it runs on the upscaler's input). `RSX_SSAO_DLSS=0` turns it off under DLSS only.
- **Garbage textures on bullet holes and rubble** fixed (`RSX_AV_NEIGHBOUR_SMALL_FLUSH`, default on; `0` = old).
- **Microstutter** from texture copies reduced (`RSX_NV0039_PREINVAL`, default on; `0` = old).
- Striped / purple textures fixed by turning on RSX memory tiling, as in RPCS3.
- A black screen when the game ran out of thread slots after a cutscene is fixed (`PS3_THREAD_SLOT_FIX`, default on).
- **DualSense / DualShock 4 controllers work again without Steam Input or DS4Windows** (GitHub issue #6): the prebuilt SDL3 library (`ps3recomp\third_party\SDL3\lib\SDL3-static.lib`) was missing from the release, so builds fell back to XInput only. It is now included.
- **Garbled or partly black start-up splash and other linear textures** fixed: linear (non-swizzled) textures are byteswapped on the CPU instead of the GPU compute decoder (`RSX_TEXDECODE_LINEAR_CPU`, default on; `0` = old).
- **DLSS / Frame Generation motion fixes:** the camera's motion is now found in every frame instead of only among the first 8 view matrices, so Frame Generation and DLSS see the real camera movement (less doubling and smearing while turning); a confirmed camera motion is no longer thrown away by the motion check; the de-jitter pass keeps signed depth values intact. Each is on by default and can be turned off with `0` for comparison: `RSX_AA_EXACT_ALL`, `RSX_AA_CONF_TRUST_EXACT`, `RSX_AA_DEJIT_SIGNED`.
- Sound comes back after the audio device is unplugged or changed (`PORT_AUDIO_REOPEN`, default on), and no longer stays slowed or garbled after alt-tab or the F10 menu (`PORT_AUDIO_FOCUS_REPRIME`, default on).
- **Freeze in fights fixed:** an occlusion result the game waits for could sit unsent while the GPU was idle; it is now submitted after 200 µs of idle (`RSX_ZCULL_IDLE_FLUSH`, default on). A second hang, where the GPU command position was reported past a label before it was written, is also fixed (`R2_FIFO_GET_AT_LABEL`, default on).
- **Crash fix:** game threads opening and closing files at the same time could corrupt the file table and crash.
- **DLSS / FSR image fixes:** boiling and shimmer on vegetation and fine detail when standing still (`RSX_AA_POST_STABLE`, default on), purple walls and ladders (`RSX_AA_DS_LATCH`, default on), flashing branches and overly bright bloom are fixed.
- **AMD:** a driver crash while shaders were preloaded in the background (GitHub issue #10) is avoided: the preload skips MSAA pipelines that can't be used, uses one thread on AMD, and a pipeline that crashed the driver is caught and skipped on the next start (`PORT_PRELOAD_ALL_MSAA`, `PORT_PRELOAD_THREADS`, `PORT_PRELOAD_QUARANTINE`, `PORT_PRELOAD_SEH`); shadows and decals get a proper depth bias on 32-bit depth (`RSX_ZBIAS_D32`, auto).
- **Closing the window ends the game** at once, and quitting no longer runs the full PS3 shutdown path that could hang (`PS3_EXIT_CRT=1` = old).
- **Mouse aim learning is less eager**, so the mouse no longer gets mis-detected as a stick (`R2_ML_LEARN=0` off, `legacy` = old); delete an old `mouselook_r2.cfg` if aiming felt wrong.
- **Triggers** no longer flicker between pressed and released near the threshold (`PAD_TRIGGER_HYST=0` = old).
- **60 fps by default:** *Auto-detect* now recommends 60 fps (120 only on the fastest GPUs), because left-stick movement misbehaves above about 75 fps (`R2_AUTOCONFIG_120=1` = old recommendation).
- The build log is no longer flooded with compiler warnings.

### New features
- **Online (experimental, off by default):** `PORT_ONLINE=1` plus your own RPCN account in `online.ini` (copy `online.ini.example`). Groundwork only: signing in and getting a ticket from RPCN works; online play itself is untested.
- **PC SETTINGS inside the game's own Options menu** (`R2_MENU_ROWS`, default on), next to the F10 PC menu.
- **HD UI pack (experimental):** launcher › Saves & tools › *Create HD UI pack* AI-upscales the menu / HUD bitmaps from your own game files on your PC, on the graphics card in one Real-ESRGAN run (the upscaler comes with the port; the official `realesrgan-x4plus` model is downloaded the first time, see `THIRD_PARTY_NOTICES.md`); if the graphics card fails, the upscaler is stopped at once and the rest is made on the CPU, and later runs use the CPU (delete `tools\upscaler\gpu_fault.txt` or run `HDUIPack.exe --use-gpu` to retry the GPU); turn it on with `RSX_HD_UI=1`. Nothing game-derived ships with the port.
- **Save backups:** before the game overwrites a save, the previous version is copied to `saves_backup\` (newest 5 per save; `PORT_SAVE_BACKUP=0` off, `PORT_SAVE_BACKUP_KEEP=N`).
- **Import saves from RPCS3** (launcher › Saves & tools), including saves from other regions; your saves are backed up first.
- **Language fallback:** languages without a voice-over on the disc (for example English (UK)) play the English voices and keep their own text (`PS3_LANG_VOICE_FALLBACK`, default `1`; `0` = off).
- **Photo mode (F7)** can now also hide the game's HUD (`R2_PHOTO_HIDE_HUD`, default on; needs DLSS, FSR or DLAA).
- **PS3-style trophy pop-ups:** "You have earned a trophy." with the trophy's real icon from your game files (`PORT_TROPHY_STYLE=old` = the classic pop-up).
- **Cutscene skip hint follows Skip unwatched movies** (*not yet tested in game*): *Show cutscene skip hint* **Auto** (default) shows it when *Skip unwatched movies* is on, since every story movie can then be skipped. Holding the button skips either way; `PORT_SKIP_HINT=1` always shows the hint, `0` never.
- **Skip unwatched movies** (*not yet tested in game*): holding to skip now also skips pre-rendered movies you have not watched yet. While the hold fires, the game's own "already watched?" check in its movie skip code answers yes, so the game ends the movie through its normal skip path. Launcher / PC menu *Skip unwatched movies* (`R2_CUTSCENE_SKIP_ALL`, default on, `0` = off; needs *Hold to skip movies*). `logs\run.log` says how each movie ended (`[r2x] movie closed (...)`).
- **Game language option** in the launcher (PC page): Auto (the Windows language) or a fixed language (`PS3_LANG`, e.g. `en`, `en-gb`).
- Gore and adaptive difficulty options (`PORT_GORE`, `PORT_DYNDIFF`), a Steam Deck preset, a benchmark key (`R2_BENCH_KEY`, default F11), and a frame-time report in `logs\` at exit.
- **Save location** (launcher Advanced › Saves): game folder (default), *Saved Games\Resistance 2* or a folder you pick (`PS3_GAMEDATA_ROOT`).

### Setup and updates
- **In-place updater:** the launcher's *Updates* option (Advanced page): **Notify** (default) offers *Update now*, which downloads the release (SHA-256 checked), replaces the port's files, runs Setup in update mode and restarts the launcher; **Auto-install** asks once, then updates; **Off** (`PORT_NO_UPDATE_CHECK=1`). Settings, saves, game copy and builds are kept; a failed update is rolled back (`logs\update.log`). `PORT_UPDATE_NOTIFY_ONLY=1` = the old notice. Git checkouts and RecompLauncher installs only get the notice.
- **Portable build tools (experimental):** Setup lists the missing build tools, explains each one and asks first: **S** installs them system-wide, **P** puts them only into `tools_cache\` in the port folder (nothing is installed in Windows; `Setup.bat -PortableTools`). `Setup.bat -RemoveTools` removes again exactly what Setup installed (`tools\installer\buildtools.ps1`).
- **LLVM 22 or newer is now needed:** the committed PGO profile (`pgo\r2.profdata`) is in a format older LLVM cannot read. Setup updates an older LLVM (or fetches a portable one), and `build.bat` stops with a clear message if clang-cl is older (`build.bat nopgo` builds without the profile).
- **Build repair:** `tools\build_doctor.ps1` resets a broken build configuration (a moved folder, `build.ninja still dirty`), shows the first error of a failed build (`-FirstError`) and how much space each part takes (`-DiskUsage`); Setup uses it by itself.
- **Game copy check:** Setup now also checks that `game.psarc` (the game's data archive) was copied, not only the program file; a copy without it started to a black screen. The PS3 menu extras and the disc's PS3 system update are no longer copied.
- The launcher checks that `game\EBOOT.elf` and the built game are there before it starts the game, and says what to run if not.
- **More reliable RPCS3 source download** (GitHub issue #13): every download is retried with a growing pause, then falls back to a full fetch and finally to GitHub's source archive of the exact pinned commit; a run that stopped half way continues where it stopped, exFAT / FAT32 / network drives and Git's "dubious ownership" error are handled, and Git's messages plus a plain-English hint go to `logs\fetch_rpcs3.log`.
- **Build fix for folders with spaces:** the build no longer fails on the FFmpeg headers when the port folder's path contains spaces.
- **Setup help:** if the compiler runs out of memory, Setup retries with fewer files at a time and continues where it stopped; it suggests excluding the port folder from Microsoft Defender and warns about slow USB / exFAT / network drives.

### Controls
- **Round-to-square stick mapping** like a DualShock 3, so diagonals reach full speed (`PAD_SQUIRCLE`, default `4000`; `0` = old).
- New stick and button options, all off / as the game by default: outer deadzone (`PAD_OUTER_DEADZONE`), anti-deadzone (`PAD_ANTI_DEADZONE`), trigger deadzone (`PAD_TRIGGER_DEADZONE`), turn boost (`PAD_TURN_BOOST`), crouch and sprint toggle / hold (`PAD_CROUCH_MODE`, `PAD_SPRINT_MODE`, experimental).
- Vibration strength (`PAD_RUMBLE`, default 100), Circle-to-confirm (`PS3_ENTER_BUTTON=circle`), controllers in the background (`PAD_BACKGROUND_INPUT`, default Auto), all controllers drive player 1 (`PAD_MERGE=1`, default off for co-op) and Nintendo button prompts (`R2_PAD_GLYPHS=nintendo`).

### Graphics
- **Direct3D 12 renderer: for testing only, not for playing** (`R2_RSX=d3d12`). It has many graphics bugs and runs much slower than the default Compatible (Vulkan) renderer. Upscaler there via `RSX_FSR` (`3` = FSR 3.1 native AA using the AMD FidelityFX Direct3D 12 DLLs in `tools\ffx_dx12\`, `2` = DLAA, `4` = XeSS native AA; unverified in game).
- **AMD Anti-Lag 2 and a single latency option** (`PORT_LATENCY=off|reflex|antilag|auto`; unset = the Reflex setting alone decides, as before). Off while Frame Generation is on.
- Native-resolution shadow maps option (`RSX_SHADOW_NATIVE=1`, default off), remembered window position (`RSX_WINDOW_REMEMBER`, default on) and many DLSS / FSR image fixes (less ghosting, smearing and shimmer on foliage and effects).
- **DLSS preset** option (`RSX_DLSS_PRESET`); Auto picks the CNN preset E on RTX 20 / 30, where the transformer model costs much more (`PORT_DLSS_AUTO_ARCH=0` = NVIDIA's default everywhere).
- **Frame generation backend** option: Auto (DLSS-G, else FSR 3), DLSS only or FSR 3 (`R2_FG_BACKEND`).
- **Brightness** (`PS3_DISPLAY_GAMMA`), **texture detail / LOD bias** (`R2_LOD_BIAS`) and **Ultrawide HUD** (`RSX_HUD_FIX=1`, experimental: HUD in a centred 16:9 area) options.

### Audio
- Native code for the SPU audio mixer job (`R2_NATIVE_AUDIO_JOB`, default on; `0` = old).
- **Speaker layout follows your playback device:** the recommended (default) choice is now **Stereo** on headphones / stereo speakers and 7.1 on 8-channel outputs, instead of always 5.1 (`PORT_SPEAKERS`; pick 5.1 / 7.1 / Stereo in the launcher).
- Master volume and mono audio (`PORT_VOLUME`, default 100; `PORT_AUDIO_MONO=1`).
- Steadier sound under load: clock-drift correction (`PORT_AUDIO_DRIFT`), an adaptive buffer (`PORT_AUDIO_ADAPTIVE`), and silence instead of looping sound while the game is paused or stalled (`PORT_AUDIO_PAUSE_FREEZE`, `PORT_AUDIO_STALL_FREEZE_MS`); all on by default.

### Performance
- **Native SPU job kernels** (`R2_NATIVE_JOBS`, default on; `0` = the translated SPU code only): hand-written, bit-exact versions of the hottest geometry and zlib routines, plus the water / ocean jobs (`R2_NATIVE_JOBS_WATER`, default on).
- Native copies of the game's own memcpy / strcmp family (`R2_HOT_NATIVE`, default on).
- **PGO-optimised build:** `build.bat` uses the committed profile `pgo\r2.profdata` automatically (`set PORT_PGO=0` or `build.bat nopgo` to opt out).
- **CPUs without AVX2:** Setup also builds a variant without AVX2 (SSE4.1 minimum) and the launcher picks it on those CPUs.
- Lower CPU use while waiting (`PORT_NOSPIN`, default on), a menu frame cap (`PORT_MENU_FPS`, default 90), better behaviour on 4-core CPUs and many renderer fast paths.
- **Faster big fights:** scaled depth copies are one GPU blit instead of a slow colour round trip (`RSX_DEPTH_BLIT_DIRECT`, default on), the DLSS feature is created once and reused when the render size changes (`RSX_AA_DYNRES`), and an occlusion wait is capped at 50 ms, with objects drawn visible on timeout.
- **Thread placement:** on Intel hybrid CPUs the busy game threads prefer P-cores (`PORT_CPU_PCORES`, on); optional raised priority (`PORT_THREAD_PRIORITY`, off); game threads opt out of Windows power throttling (`PORT_ECOQOS=1` keeps the Windows default).
- **Reduce detail when the CPU is slow** (`R2_GOVERNOR_MS`): the game's own governor at its original 36.6 ms, earlier (30 ms, weak CPUs) or never.

### Settings
- `RSX_GPU_TIME` (GPU timing in the hitch log) is now off by default; `RSX_GPU_TIME=1` turns it on.
- **Overlay:** the F3 overlay draws a frame-time graph (`PORT_OVERLAY_GRAPH`, on); the overlay, notifications and PC menus scale with the window (`PORT_UI_SCALE`, Auto; 75–200 %).
- `PS3_THREAD_SLOT_FIX`, `RSX_AV_NEIGHBOUR_SMALL_FLUSH`, `RSX_NV0039_PREINVAL`, `R2_NATIVE_JOBS`, `R2_NATIVE_AUDIO_JOB` and `R2_HOT_NATIVE` are on by default; set one to `0` in `settings.bat` to compare with the old behaviour when reporting a bug.
- Quieter `logs\run.log`: diagnostic logs and profilers are now off by default (shadow / depth diagnostics, renderer micro-stats, the RSX CPU profiler `PORT_RSX_CPU_PROF`, the fall detector `PORT_FALL_M`); file opens are logged for the first 200 only (`PS3_FSLOG=1` logs all) and the upscaler's summary is written every 60 s instead of every 5 s.

### Known issues
- **Frame Generation (DLSS-FG) is still experimental** and has remaining artifacts: jitter on grass / vegetation and around doors and entrances, and some ghosting. These will be fixed in an upcoming hot patch. Frame Generation is off by default.

### Support
This port is free and always will be. If you enjoy it and feel like buying me a coffee, it's genuinely appreciated: [buymeacoffee.com/damiro98](https://buymeacoffee.com/damiro98)

## 0.2.3-beta (last hotfix)

This is the last hotfix. The next updates focus on performance and stability, based on your reports and playthroughs.

- **Setup works without winget.** If winget is missing or fails on your PC, Setup downloads the official installers of the build tools (Visual Studio Build Tools, CMake, Ninja, LLVM, Python, Git) directly, checks them (SHA-256 or Microsoft's signature) and installs them.
- **RecompLauncher support.** The port can be installed, set up and played from [RecompLauncher](https://recomplauncher.com): it checks your copy, builds the game, shows the main video settings on the game's page and starts the game. Your own Launcher.exe and Setup.exe work as before.
- The release zip is now named `...-Windows-x64.zip`.
## 0.2.2-beta (setup hotfix)

- **Fixed: the build could fail right away with a Ninja/CMake error** ("build.ninja still dirty", or CMake re-running in a loop) for players in time zones behind ours. Files from the download could be dated a few hours in the future. The installer and zips now carry correct times, and the build resets any future-dated files before it starts.

If your setup failed this way: install 0.2.2 (or wait a few hours) and run Setup again; it continues where it stopped.

## 0.2.1-beta (setup hotfix)

Setup fixes only; the game itself is unchanged from 0.2.0.

- **Progress bar during the build.** The bar no longer stops partway while the game compiles; it follows the build to the end, so Setup no longer looks stuck.
- **Setup shows that it is working:** a running clock for each step, the game copy in GB with the time left, the time left while the game compiles, and a "still working" note during steps that print nothing for several minutes.
- **The installer now opens the Setup window** (with the progress bar and your game already filled in) instead of a console window. Click **Install** there.
- **Much smaller build logs.** The compiler was set to report every possible warning, which made `logs\build.log` grow past 150 MB and slowed the build. It now reports normal warnings only.
- **Low-RAM PCs:** if the build runs out of memory, Setup again detects it and retries with fewer parallel jobs (this check could not read the build log in 0.2.0).

If 0.2.0 is already built and runs, you don't need this update.

## What's in this beta

### Launcher
- `Launcher.exe` holds all settings and key bindings. **Auto-detect** recommends settings for your PC.
- Pages: Display, Graphics, Upscaling & Frame Gen, Performance, Controls, Audio, Interface & Notifications, Advanced, **Trophies**, **Saves & tools** and About (`Launcher.exe --page N` opens one, 0–10).
- **Updates:** the launcher checks the Releases page at start and can update in place (*Update now*: downloads, replaces the port's files, runs Setup and restarts; a failed update is rolled back). The *Updates* option on the Advanced page picks Notify (default), Auto-install or Off (`PORT_NO_UPDATE_CHECK=1`).

### Graphics
- **Upscaling and anti-aliasing:** NVIDIA DLSS on GeForce RTX cards, AMD FSR 3.1 on any GPU, or Off (`RSX_FSR` = 2 / 1 / 0). If DLSS can't start, the port uses FSR 3.1.
- **Intel XeSS 2 (experimental):** pick *Intel XeSS* in the launcher (Compatible renderer; `RSX_FSR=4`). It runs on any GPU. If it can't start, or on the native renderer, the port uses FSR 3.1. Setup downloads the XeSS SDK after you accept Intel's licence.
- **Render resolution** up to 3840x2160 (`RSX_RES_SCALE`), plus an upscaling ratio (`RSX_UPSCALE`) and sharpening.
- **Frame rate targets:** **60** (default), 30, 120, 144, *Match display*, *Just under refresh* (VRR) and Uncapped (`R2_FPS`).
  - Physics runs at a fixed 60 Hz step.
  - Game speed stays correct at every rate.
- **VRR / G-Sync fix:** VSync *Auto* and *Adaptive* now present with FIFO or FIFO relaxed instead of MAILBOX, which G-Sync / FreeSync doesn't follow. The *Presentation mode* option (`R2_PRESENT=immediate|mailbox|fifo`) overrides this.
- **Refresh rate switch:** in exclusive fullscreen, the display switches to the refresh rate that best fits the frame rate cap (`PORT_REFRESH_SWITCH=1`, experimental, off by default: every switch blanks the screen and can glitch audio).
- **Ultrawide** 21:9 and 32:9 (`R2_ASPECT`), with an optional centred 16:9 HUD (experimental).
- **Shader precompile:** with *Compile shaders before playing* (default on), the cached pipelines are built before the first frame, with a **Compiling shaders N / M** panel. Before Play, the launcher also runs the offline shader scan (shaderscan) with its own progress panel. Shader caches are built on your PC and never shipped.
- **Frame Generation** (experimental, off by default; Upscaling & Frame Gen page, `R2_DLSS_FG`, `R2_FG_BACKEND=auto|dlss|fsr3`): NVIDIA DLSS Frame Generation 2x on an RTX 40, up to 4x on an RTX 50 (needs NVIDIA DLSS); other GPUs get AMD FSR 3 2x.
- **Optional extras:**
  - HDR output (`R2_HDR`)
  - NVIDIA Reflex (`R2_REFLEX`)
  - toggles for screen blur, camera shake and cinematic black bars
  - colour-blind filters

### Controls
- **Mouse and keyboard:** raw mouse look and rebindable keys (`controls.txt`, `mouselook.cfg`).
- **Controllers through SDL3** (the default; `PAD_BACKEND=xinput` is still available): Xbox, PlayStation, Switch Pro and others. Optional gyro aiming, light bar and Nintendo button layout.
- **Button prompts follow your device** (`R2_PAD_GLYPHS=auto|xbox|ps|keys`): keyboard keys, or Xbox / PlayStation buttons.
- **Local split-screen co-op.**

### PC features
- **PC menu:** F10, or hold View + Menu. It has Resume, PC Options and **Quit to Desktop** (with confirmation; it waits for a save in progress to finish).
- **PC wording:** text that refers to the console is changed to PC terms (`R2_PC_TEXT=0` turns this off).
- **Screenshots** with F12 or PrtScn. **F3** toggles the performance overlay.
- **Menus, overlay and notifications are drawn into the game image** (`PORT_OSD`), so they also show in exclusive fullscreen. The movie-skip hint can be turned off (*Show cutscene skip hint*, `PORT_SKIP_HINT=0`).
- Alt+Enter toggles windowed mode. You can choose the monitor, and pause or mute when the game is in the background.
- **Trophies** are recorded locally. There is a **mods folder**, and **crash / hang reports** go to `logs\crash\`.

### Audio
- **Time stretching** (SoundTouch, `PORT_AUDIO_STRETCH=1`, default on): when the frame rate drops, audio slows slightly instead of crackling or gapping.

### Diagnostics
- **Hitch logger** (`PORT_HITCH_LOG=1`, default on; `PORT_HITCH_MS`, default 25 ms): each slow frame writes one `[hitch]` line to `logs\run.log`.

## What you download

- **`Resistance2Port-Setup-<version>.exe`** from the GitHub release: an installer with the port's source, scripts and `Launcher.exe`. It installs the files and starts Setup.
- or the **source code** (the release's source archive or `git clone` at the tag), then `Setup.bat`.

Either way, the game itself is built on your PC; no built game program is published. Setup downloads the build tools, RPCS3's source, FFmpeg (an LGPL build), shaderscan and, if you accept their licences, the NVIDIA DLSS, Streamline (DLSS Frame Generation) and Intel XeSS SDKs.

## Requirements

- Windows 10 or 11, 64-bit.
- A CPU with **AVX2** (Intel 4th generation or newer, or any AMD Ryzen) is recommended. On an older CPU with SSE4.1, Setup also builds a slower variant without AVX2 (`build_baseline\`) and the launcher uses it; below SSE4.1 the game says so and exits.
- A **Vulkan 1.3** GPU. DLSS needs a GeForce RTX card. FSR works on AMD, NVIDIA and Intel. Only NVIDIA has been tested so far.
- The Microsoft Visual C++ 2015–2022 x64 runtime, version 14.40 or newer.
- Your own copy of **Resistance 2, EU disc (BCES00226), version 1.00**. Other regions, digital copies and patched versions are not supported yet.
- About **35 GB** of free disk space: about 22 GB for the game files plus the build and the shader cache.
- For setup: an internet connection and the free build tools. Setup installs the tools itself.

## Install

1. **Dump your own game.** Use a decrypted ISO, or a game folder containing `PS3_GAME` (the same kind of folder RPCS3 uses), from your own disc.
2. **Install the port.** Either:
   - run `Resistance2Port-Setup-<version>.exe`, which installs to `C:\Games\Resistance2Port` by default and asks for your game; or
   - download the source code (the release's source archive, or `git clone` at the release tag) into a folder without spaces and double-click `Setup.bat`.
3. **Setup** finds or asks for your game (`Setup.bat -Game "<ISO or folder>"` skips the question). It then:
   - checks the copy and copies it to `gamefiles\PS3_GAME`;
   - decrypts `EBOOT.BIN` to `game\EBOOT.elf` with the public keys from RPCS3's source;
   - downloads RPCS3's source at a fixed version and applies the port's patch;
   - generates and builds the game-specific code. This takes 20–60 minutes the first time.
4. **Play** with `Launcher.exe`.

**How the port finds your game:**
- `run.bat`, which the launcher starts, runs `build\Resistance2.exe` with `game\EBOOT.elf` and points the game's virtual disc at `gamefiles` (`PS3_VFS_ROOT`).
- Saves go in `gamedata\dev_hdd0\home\00000001\savedata` (`PS3_GAMEDATA_ROOT` moves them).
- Settings and controls are also kept in `%USERPROFILE%\Saved Games\Resistance 2\config`. An empty `portable.txt` turns that off.
- The shader cache backup is in `%LOCALAPPDATA%\Resistance2Port\shadercache`.

**First launch:** before the game starts, the launcher prepares the shader cache from your game files (a progress panel; **Skip for now** or Esc skips it). The game then compiles its known shaders behind a **Compiling shaders N / M** panel. Later starts are fast.

## Known issues

- Occasional freezes. One cause, an SPU job jumping to an empty command slot, has a workaround; there may be others.
- Above 60 fps, physics still updates at 60 Hz without interpolation, so motion can look slightly juddery.
- Lighting and bloom can look brighter in some spots at 60 fps (also reported on RPCS3). If it bothers you, use 30 fps.
- With DLSS, blob shadows under moving pickups can trail a little, and some foliage and wire fences can still shimmer.
- Heavy fights are CPU-bound (high 30s in the biggest ones on a mid-range CPU). Frame Generation (launcher → Upscaling & Frame Gen) can help here.
- **Frame Generation is experimental and barely tested** (DLSS on RTX 40/50, FSR 3 elsewhere; off by default). Expect ghosting and HUD or menu artifacts; reports are welcome.
- **Changing Frame Generation in-game** can double the sound and then leave constant static. Change it in the launcher, or restart the game after changing it.
- "open FAIL" lines in the log for streaming files the EU disc doesn't have are harmless.
- Exclusive fullscreen can give black F12 screenshots, and the screen goes black briefly at start and when switching windows. Borderless avoids both.
- Everything else, including levels past the opening Iceland levels: **TBD after testing**.
- What's planned next: [ROADMAP.md](ROADMAP.md).

## Reporting problems

Please read **[REPORTING.md](REPORTING.md)**. Include how to reproduce the problem, the session's log from `logs\` (the launcher has a button for it), and any report from `logs\crash\`.

## Licences

The port is MIT-licensed (see `LICENSE`). Its RPCS3 patch is GPL-2.0, and the game you build links RPCS3, so that build is GPL-2.0; don't redistribute it with the DLSS files. See `THIRD_PARTY_NOTICES.md`. This port uses NVIDIA DLSS. *Resistance 2* is a trademark of its owners. This project is not affiliated with or endorsed by them.
