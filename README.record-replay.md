# DOSBox-X record/replay

Record a DOS game session, then replay it and check that the recorded results
match. Use this fork's `record-replay` branch and your original game files.

## Start

Build once (dependencies: [BUILD.md](BUILD.md)):

```sh
./autogen.sh
./configure --enable-sdl2 --enable-debug=heavy --enable-dosbox_agent
make -j4
```

Record, replay, or inspect a recording:

```sh
./src/dosbox-x --record session.drp --game-dir /path/to/game --target GAME.EXE
./src/dosbox-x --replay session.drp --game-dir /path/to/game
./src/dosbox-x --replay-info session.drp
```

Play normally; close the window or press **Stop recording** to finish.
Existing recordings are never overwritten. Add `--directory SUBDIR` for a game
in a subfolder. Successful verification prints `NATIVE REPLAY MATCH`; failure
returns a nonzero exit code.

## How it works

Recording begins before the selected EXE/COM's first instruction. The replay
stores settings, timed inputs, loader events and state/output hashes—not RGB,
audio or RAM dumps. Playback applies those settings automatically; no external
controller is needed.

Game files are used directly, without copies or OS mounts. **Game writes persist**;
changed saves, settings or assets may prevent a matching replay.

## Debugging and identity

`--replay-info` prints JSON lines:

- `H`: settings, compatibility version, executable identity and entry hashes.
- `I`: timed keyboard/mouse deliveries.
- `X`: DOS loads/exits, including overlay addresses, content and relocation hashes.
- `O`: tick/position and CPU/RAM, FPU, mouse, graphics, sound and event-history hashes.
- `F`: completion evidence; missing means an unfinished recording.

The executable fingerprint is a block-chain hash, **not ordinary `sha256sum`**.
Overlay identity uses load events and content hashes: an address can be reused.
Custom loaders bypassing DOS EXEC need additional tracing.

## Tests and limits

```sh
python3 -B tests/native-replay/run_native_regression.py --binary ./src/dosbox-x
```

Experimental: Linux SDL2 and one machine profile are currently qualified.
Matching captured evidence is not proof of every internal operation or of another
engine's gameplay. See [tests](tests/native-replay/README.md) and
[archive format](docs/native-replay-format.md).
