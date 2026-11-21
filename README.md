# Catherine Classic: native Steam Deck port

Runs Catherine Classic natively on the Steam Deck, without Proton. The installer translates the game's Windows code
into a Linux program on your own Deck, from your own copy.

**You need to own the game on Steam.** This repository contains no game files, and it never will. The built result
is made from Atlus's game and Microsoft's DLLs, so please don't share it with anyone: everyone builds their own.

Unofficial fan project, not connected to Atlus or Sega. Use it at your own risk.

## Technical highlights

- **Static recompilation, not emulation.** Ghidra decodes `Catherine.exe` (36,288 functions, 1.85 million x86
  instructions) and two Microsoft DirectX DLLs; a Python lifter (`tools/recomp/lift.py`) turns them into C, which GCC
  compiles to native x86-64. No Wine, no Proton, no interpreter at run time.
- **All 777 Windows imports reimplemented** in ~6,300 lines of C/C++ (`runtime/host/`): Win32, the MSVC C/C++
  runtime, input, GDI text on FreeType, movies on FFmpeg, the CRI sound mixer on SDL3, Direct3D 9 to Vulkan through
  DXVK, and a bridge to the real Steam client (achievements, leaderboards, Steam Cloud saves shared with Windows).
- **Correctness checked by differential testing:** each translated function runs from identical random states both
  as C and as the original x86 bytes in the Unicorn emulator, and registers and memory are compared. The movie code
  passes 680/680; the SIMD, x87 and random-sample suites are held to a fixed baseline of known differences, so a
  lifter change that breaks anything shows up at once.
- **Problems found by measuring:** keeping CPU registers in C locals and fixing a sound-mixer lock that stalled the
  main thread every frame took the bar scene from 33 to 70 FPS; per-frame cursor warps leaked a file handle each in the Deck's compositor and would have frozen the game after
  ~2 hours. Both fixed; the game runs at 60-90 FPS on a Steam Deck.
- **One-command, owner-only build:** the installer checks the player's game by hash and builds everything from it in
  a throwaway container (~45 minutes on a Deck). No game code or anything derived from it is ever distributed.

## Install

You need: the game installed from Steam, about 10 GB free while building, and the charger plugged in.
It takes about 45 minutes on a Deck, and you don't have to do anything while it runs. If the Deck goes to sleep,
the build pauses and carries on when you wake it.

1. Switch to Desktop Mode (Steam button > Power > Switch to Desktop) and open **Konsole**.
2. Paste this and press Enter:

   ```bash
   git clone https://github.com/ManuelRiosRomero/catherine-linux-port.git catherine-port-src && bash catherine-port-src/install.sh
   ```

3. When it says **Done**, go to Steam > Catherine Classic > gear icon > Properties > General, and paste the line it
   shows into **Launch Options**. It looks like this:

   ```
   /home/deck/catherine-port/catherine-launch.sh %command%
   ```

4. Press Play. You can go back to Game Mode and launch it there as usual.

**To go back to the normal game:** clear the Launch Options line. **To uninstall:** also delete `~/catherine-port`.

## Good to know

- **Only the current Steam version works.** The installer checks the exact `Catherine.exe` and stops if it differs.
- **Saves:** if you've played on Proton on this Deck, the port uses that same save, so Steam Cloud keeps syncing it with
  your PC, and it keeps a copy of the last 30 saves in `~/catherine-port/save-backups`. Otherwise it keeps its own save
  in `~/.local/share/catherine-port`, and that one isn't synced.
- **Fills the 16:10 screen.** The original draws 16:9 with black bands on the Deck; the port shows a little more
  of the scene above and below instead (nothing is stretched). Movies keep their 16:9 shape. Set Resolution to
  1280x800 in the game's Display options.
- **Steam Deck only so far.** It has only been tested on SteamOS.
- **Game stopped or crashed?** The log is `~/catherine-port/last_run.log`. Its last `catherine:` lines say why.

## Status

The whole story, all 8 endings, Babel and saving have run on a Deck. Most puzzle stages were skipped by the test tools,
not played by a person, so later nights have had little real play. 60-90 FPS.

Seen once each, not reproduced: a frozen picture from a lost GPU connection in the Night 9 fifth sphere, and a black
screen at the very end of an ending. If either happens: quit the game and load your last save.

## How it works

1. **Ghidra** (the NSA's free reverse-engineering tool) reads `Catherine.exe` and two Microsoft DirectX DLLs into a
   plain list of CPU operations (`build/ghidra_all.sh`).
2. **`tools/recomp/lift.py`** turns every function in that list into C, and GCC compiles it to native x86-64 code.
3. **`runtime/host/`** replaces what Windows would provide: files, threads, windows, input, sound, movies, fonts, and
   a bridge to the real Steam client (ownership and achievements are Steam's own, nothing is faked).
4. **DXVK** turns Direct3D 9 graphics into Vulkan.

`install.sh` runs all of that (`build/build_all.sh`) in a throwaway Ubuntu container, so nothing gets installed on
SteamOS. It downloads Ghidra, Java, SDL3, DXVK, FFmpeg and, if Steam doesn't have them, Microsoft's DirectX
redistributable. Afterwards it deletes the ~8 GB build folder (`KEEP=1` keeps it).

## License

The code here is MIT licensed (see `LICENSE`). That covers none of the game's content or anything built from it.
