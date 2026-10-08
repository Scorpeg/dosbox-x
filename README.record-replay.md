# Experimental native record/replay fork

This branch (`record-replay`) belongs to Scorpeg/dosbox-x. It imports the
reviewed replay/agent instrumentation on upstream `826dfc42`, then adds native
archive loading and in-process recording/playback. A separate remote-debug fork
is not required. Upstream CPU/FPU upgrades remain a separate qualification step.

Build with SDL2, the heavy debugger and the agent enabled:

```sh
./configure --enable-sdl2 --enable-debug=heavy --enable-dosbox_agent
make -j4
```

The debugger is an implementation dependency, **not a terminal requirement**.
Native mode does not open an agent socket/pipe or invoke Python.

```sh
dosbox-x --record /absolute/path/route.drp --game-dir /absolute/path/game --target GAME.EXE
dosbox-x --replay /absolute/path/route.drp --game-dir /absolute/path/game
dosbox-x --replay-info /absolute/path/route.drp
```

Optional recording arguments: `--directory SUBDIR` (DOS-relative, `/` between
components), `--sample-ticks 1000` (20–5000), `--ticks 5000` (test endpoint).
Normal recording ends on window close, Stop recording or selected DOS exit.
Existing files are never overwritten. Interrupted/unsealed files are not
successful recordings. Replay is a separate operation; it returns nonzero on
corrupt/incomplete data, unsupported settings, input/history/hash differences or
a boundary timeout. `--replay-info` validates and exposes decoded JSON frames.

The archive embeds the machine profile, launch/clock and entry evidence, then
compressed input deliveries, loader lifecycle records and observation hashes.
See [wire format](docs/native-replay-format.md). No RGB/PCM/RAM movies, game files
or backend executables are stored. The supplied original directory is used
directly as DOSBox C:, without OS mounts or copies. Game writes persist. Only
the executable is fingerprinted before launch; other changed files can cause a
later mismatch. Do not run concurrently against the same game directory.

Native v1 uses one explicit profile: normal/Pentium, fixed 10000 cycles, 16 MiB,
SVGA S3, INT33 mouse, SB16/DBOPL OPL3/PC speaker and isolated host audio.
It supports protected-mode execution, but does not yet expose every DOSBox-X
machine/input/device option. Guest sound stays enabled even if a host audio
device is unavailable. Replay removes host throttling, not the guest cycle
budget. Host configs, mapper files, autoexec and config environment overrides
are excluded; UTC and the initial RTC are selected inside the process.

Entry/sample hashes cover declared integer/RAM, FPU/SIMD, mouse, raster,
mixer/SB/DMA, OPL and PIT/speaker storage/history contracts. Loader observations
include DOS EXEC overlays; arbitrary custom overlays still need game-specific
module identity/semantic adapters. These are not complete machine snapshots or
proof of unobserved behavior. Another engine must expose comparable behavioral
observations; its host memory need not match DOSBox's backing bytes.

Qualification is currently Linux SDL2, with short native NPA and frontend-input
routes. The portable archive and C++ code do not imply Windows/macOS or arbitrary
build compatibility has been proven. Different FPU implementations may fail
entry checks. Compatibility is a versioned emulation/evidence contract, not
equality of platform executable files. Existing DRINPUT archives require their
legacy launcher; this distinct format does not silently reinterpret them.
