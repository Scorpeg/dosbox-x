# Native replay regression tests

Everything needed for the synthetic suite lives in this fork. No outer harness
repository, game assets, sockets, controller process or game copies are needed.
From the DOSBox-X repository root, after building with SDL2, heavy debugger and
agent support:

```sh
python3 -B tests/native-replay/run_native_regression.py --binary ./src/dosbox-x
```

An out-of-tree binary can be selected with `--binary /absolute/path/dosbox-x`.
The suite tests that existing binary; it does not rebuild the emulator. Current
test dependencies are Linux, Python 3, NASM, a C++ compiler, SDL2 headers at
`/usr/include/SDL2`, and zlib development files. The frontend injector uses
`LD_PRELOAD`; this is a test dependency, not a replay-runtime dependency.

Each custom ASM fixture is assembled into a tiny temporary DOS directory, then
recorded and replayed in two fresh processes:

- `mouse_capture.asm`: tick endpoint and INT33 mouse polling; a second run
  injects SDL key, mouse motion/button/wheel/capture events and closes the window
  with a key held. Replay receives only the archive, not injected SDL events.
- `native_exit.asm`: PC speaker output and selected-process exit code 7, named
  `EXIT.COM` to exercise the collision with the shell's built-in command.
- `native_overlay.asm` and `native_overlay_image.asm`: load relocated MZ
  overlays A/B/A into the same address, write VGA memory, calculate an x87
  result, and produce a known 12-byte DOS output. Every leg must freshly produce
  the expected output; loader records must preserve A/B/A identities and
  relocations. Changing B afterwards must produce a DOS EXEC mismatch.

The suite also tests its own checkers and the archive codec, rejects corrupt or
incompatible archives and a rehashed false observation, and checks that abrupt
termination cannot produce a sealed successful recording. All nine observation
domains and a 64 KiB limit per normal synthetic replay are required. Any failure
or timeout returns nonzero.

Reports, tiny archives, fixture binaries and logs stay in
`.tmp/runs/native-regression-*`; the codec test binary is reused under
`.tmp/build/native-replay-tests`. No RGB, PCM or RAM movies are produced. An
integrating harness may set `DOSBOX_REPLAY_TEST_TMP` to another absolute `.tmp`
directory. Nothing under that directory is automatically deleted.

Individual checks:

```sh
python3 -B tests/native-replay/test_native_replay.py
python3 -B tests/native-replay/native_replay_checks.py
python3 -B tests/native-replay/probe_native_replay.py --binary ./src/dosbox-x --overlay
```

This is repeatability coverage for these fixtures, not universal CPU or gameplay
proof. The ASM oracle checks some independently known results. Protected/v86
transitions, additional sound devices and loader flows, physical desktop input,
and cross-host portability need further native regression cases.
