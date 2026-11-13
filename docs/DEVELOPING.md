# Mike's Game Room

<img src="Resources/Logo.png" width="160" align="right">

A native macOS game-room app for **Nintendo Switch 1 and Switch 2** games: a SwiftUI library
plus an emulation core written in portable C.

## Build and run

Requires macOS 13+ and Xcode or the Command Line Tools (`xcode-select --install`).

```
cd ~/Documents/GitHub/MikesGameRoom
./build.sh --open        # builds build/MikesGameRoom.app and launches it
./build.sh --install     # or: build, copy to /Applications and launch
```

Core tests (no Xcode project needed). These run real ARM64 test programs (`Tests/guest/`) for the
CPU, FP/NEON (compared against native output), threads and IPC, in every container format:

```
./Tests/run_tests.sh
```

## Native engine (nxnative)

MikesGameRoom now carries the nxnative runtime (MIT, in `Engines/nxnative`; see `NOTICE.md`). A game is
prepared once from the library (game details → **Prepare for Mac**): its code is rewritten to run directly
on Apple silicon, its shaders are translated and compiled for Metal, and it is packed into an app beside the
game file. **Play** then starts that app. Preparing needs Xcode 27.2, Python, .NET and Homebrew's
`xcodegen`, `zstd` and `glslang` (Settings → Native Engine checks them). The first preparation of a large
Switch 2 game takes about two hours and keeps the Mac busy. MikesGameRoom's own emulator stays available
from a game's menu (**Open in the Emulator**) for research.


### Playing a prepared game

MikesGameRoom is an nxnative front end (Engines/nxnative/docs/frontends.md). Play starts the prepared app's
`Contents/MacOS/nxnative --no-launcher` itself, so there is no separate settings window; the game opens in its
own window, because nxnative builds its host (dmhost) for each game and runs it as a process of its own.
The game's details show:

- **Running state** with Show Game and Quit Game, and what happened when it ended (quit, graphics lost, crash with the log's last lines).
- **Game Settings**, drawn from the app's `Resources/dm/launcher.json`, saved as `launcher.<VAR>` in the app's
  preferences domain (`org.nxnative.<title id>`): the same choices nxnative's own launcher reads.
- **Game Log**, **Save Folder**, **Back Up Save** (a zip in the hub's data folder, `Save Backups`) and **Cheats Folder**.

## What works today

| Area | Status |
|---|---|
| Game library | Grid with icons, Switch 1 / Switch 2 / Homebrew filters, search, drag-and-drop, watched folders |
| **NUP** (`.nup`) | Full: base + updates + DLC, zstd, metadata, icons. Reads both the spec metadata numbering and the off-by-one numbering written by current `nup` builds |
| Extracted folders | `exefs/` + `romfs/`, or `base/` + `updates/<ver>/` layouts |
| **NSP / XCI / NCA** | Decrypts with **your own** `prod.keys` (+ `title.keys` / in-NSP tickets): name, publisher, version, icon, ExeFS, RomFS |
| **NSO / NRO** | Loads directly; NRO homebrew icon + name from ASET |
| NSZ / XCZ | Recognised; decompress to NSP/XCI first (`nsz -D`) |
| Program loader | NPDM parsing, NSO segments in **LZ4 (Switch 1) or ZBIC (Switch 2, Nintendo's zstd variant)**, rtld → main → subsdk* → sdk, mapped per segment (R-X / R / RW) like the real loader |
| CPU | AArch64 interpreter: integer ISA, all load/store forms incl. LD1-LD4/ST1-ST4, exclusives, ARMv8.1 LSE atomics, system registers, **scalar FP and NEON vector math**. FP/NEON output is verified bit-for-bit against native execution |
| Kernel | Horizon HLE: **multiple guest threads** with a priority scheduler, mutexes, condition variables, address arbiters, events, timed sleeps/waits, memory and info SVCs |
| OS services (IPC) | HIPC/CMIF + **TIPC**, domains, handle passing. `sm:`, `fsp-srv` with **RomFS DataStorage** (real game data, also from encrypted NSP/XCI), applet (am), hid, vi, nvdrv, set, time, acc, pctl, audio, nifm and more; unknown services answer success and are logged |

## Real-game status

*The Legend of Zelda: Ocarina of Time* (Switch 2, extracted ExeFS/RomFS) currently:
loads all modules (62 MB `main` decompressed from ZBIC), runs rtld and SDK startup, brings up the
whole Switch 2 graphics stack (gsv, syncpoints, nx2:gpu address space and memory, vi2 display,
nvsrv NVIDIA driver: Ampere GPU characteristics, address space, TSG and GPFIFO channels), creates
its three 2560x1440 swapchain images, submits GPU command buffers that the new GPU front-end runs
(`gpu_nx2.c`: semaphores, copy engine, inline uploads, the MME64 macro interpreter in `gpu_mme.h`,
compute QMD scheduling and releases, firmware call 4), maps its VAMM heap, starts its 24 worker
threads and is **loading game data from RomFS** (6+ billion instructions, no errors).

Nothing is drawn yet: draws and shaders need the Ampere 3D engine, and the interpreter (about
40-60 million instructions a second) makes loading slow. Those are the next two big pieces.

Every stop prints a backtrace with module offsets (`mgrcli run`), and `--peek ADDR` dumps guest memory.

### Cross-checked against Ryujinx

Service replies are audited against Ryujinx (MIT, via its maintained forks), which documents the
Switch 1 OS interfaces. Every applet / fs / time command Zelda calls that Ryujinx knows now returns
the same kind of data (values, events, objects). The commands Zelda stops on (applet proxy 40/60,
self-controller 2000/2400, kernel info 29-33) do not exist in Ryujinx: they are Switch 2 only.

## What's next (the road to playing games)

Retail games now get past boot, thread setup and their first wave of service calls. The next
milestones, in order:

1. **GPU 3D engine**: the command front-end (`gpu_nx2.c`) already runs GPFIFOs, semaphores and copies;
   next are macros (MME), render targets, shaders (Ampere SASS to Metal) and presenting vi2 images
2. **Real controller + display plumbing**: hid shared memory (GameController.framework)
3. **Native CPU (in progress)**: `cpu_hvf.c` runs game code directly on Apple Silicon through
   Hypervisor.framework (the Mac and the Switch 2 are both ARM64); the interpreter stays as the
   fallback and for debugging (`MGR_CPU=interp`, needed for `MGR_WATCH` / `MGR_BREAK`)
4. **Audio** (audren to CoreAudio) and **saves** (save-data file systems)

Track a game's progress with `build/mgrcli run "/path/to/game.nup"`: the log shows every service
it opens and where it stops.

## Keys

NUP files, extracted folders, NSO and NRO need no keys. NSP/XCI/NCA need keys dumped from
**your own console**; import them in *Settings → Keys*. They are stored in
`~/Library/Application Support/MikesGameRoom/keys/` and never leave your Mac.
No keys are included with this project; `Tests/fixtures/enc/test.keys` are random test-only keys.

## Layout

```
Sources/MGRCore/          C core
  include/mgr.h           public API used by the app
  formats.c               NUP, folders, NSP/XCI/NCA, NSO/NRO front-ends
  containers.c            PFS0/HFS0, RomFS, NCA headers & sections, NACP
  crypto.c                AES-128 ECB/CTR/XTS
  module.c                NSO/NRO loaders, NPDM
  cpu.c, cpu_simd.h       AArch64 interpreter (integer, FP, NEON)
  kernel.c                Horizon HLE kernel: threads, scheduler, sync, memory
  ipc.c                   HIPC/CMIF/TIPC + HLE services
  romfs_build.c           builds RomFS images for DataStorage from loose files
  zstddeclib.c            Zstandard decoder (BSD, vendored) + MikesGameRoom's ZBIC mode
Sources/MikesGameRoom/    SwiftUI app (library, player, settings)
Tools/mgrcli.c            command-line tool:  mgrcli info|run <game> [--keys prod.keys]
Tests/                    regression tests + fixture generators
Resources/                app icon (SVG source + .icns)
```

`build/mgrcli run "/path/to/game.nup"` shows how far a game gets, which is handy for tracking
progress on each milestone.
