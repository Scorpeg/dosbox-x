# Native replay tests

From the fork root, test an existing replay-enabled build:

```sh
python3 -B tests/native-replay/run_native_regression.py --binary ./src/dosbox-x
```

Requires Linux, Python 3, NASM, C++, SDL2 headers (`/usr/include/SDL2`) and zlib.
No game files or outer harness repo needed; DOSBox is not rebuilt.

| Test | Checks |
| --- | --- |
| Mouse/input | Tick stop, keyboard/mouse delivery, window close with a key held. |
| DOS exit | Speaker output, exit code and `EXIT.COM` versus the shell command. |
| Overlays | Relocated A/B/A loads at one address, VGA/x87, known output; changed overlay rejected. |
| Archive | Corruption, incompatibility, false observations and incomplete recordings rejected. |

Each fixture is recorded and replayed twice in fresh processes. Failures return
nonzero. All nine observation domains are required; each normal replay must stay
below 64 KiB.

Artifacts: `.tmp/runs/native-regression-*`; codec build:
`.tmp/build/native-replay-tests`. Override the temporary root with
`DOSBOX_REPLAY_TEST_TMP=/absolute/path/.tmp`.

These tests cover the listed cases, not all games, devices or platforms.
