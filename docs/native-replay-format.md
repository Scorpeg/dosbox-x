# Native replay format — experimental v1

Contract: `dosbox-x:native-replay:v1`. Legacy DRINPUT needs its old launcher.
`dosbox-x --replay-info session.drp` emits JSON lines: `{"kind":…, "data":…}`.

## Layout

File: `DRNATV01` (8 ASCII bytes, once), then frames:

| Frame offset | Bytes | Encoding |
| --- | ---: | --- |
| 0 | 1 | ASCII kind: H/I/X/O/F |
| 1 | 4 | Compressed length N, unsigned little-endian |
| 5 | N | One zlib member containing canonical agent JSON |
| 5+N | 64 | Lowercase ASCII SHA-256 hex chain digest |

Chain: `d0 = hex(SHA256(magic))`;
`dn = hex(SHA256(d_previous + kind/length bytes + compressed payload))`.
`+` concatenates bytes; digests are lowercase ASCII hex. Detects corruption, not authentication.

## Frames

H is first and unique; F is last and mandatory; I/X/O occur between them.

| Kind | JSON | Contents |
| --- | --- | --- |
| H | Object | Compatibility, machine/launch settings, RTC/seed, sample interval, executable identity, entry observation |
| I | Array | Ordered input deliveries, timing, outcomes |
| X | Array | DOS EXEC requests/results, entry/exit, overlay addresses, content/relocation hashes |
| O | Object | Tick, position, exit, domain hashes, input/loader-event counts |
| F | Object | `end_tick`, `inputs`, `exec_events`, `observations`, `last_observation_sha256` |

## Rules and limits

Decoded H is limited to 2 MiB; other frames to 64 KiB. Compressed length is
1…`compressBound(decoded_limit)`. Memory is bounded per frame, not recording length.

Readers reject unknown kinds, missing/repeated/misplaced H, missing F, trailing
data, truncated/oversized/concatenated zlib and invalid/duplicate-key/noncanonical
JSON. The entire chain is checked before launch and again while streaming;
runtime checks also cover compatibility, semantic order, timeline, counts and observations.

Writers create files exclusively and flush each frame before acknowledging
inputs. Completion requires a valid F; flushing does not guarantee power-loss durability.

## Identity and coverage

Only settings, inputs, loader events and state/output hashes are stored—not raw
RGB/audio/RAM, game files, directory snapshots or the emulator binary.

`target_identity` contains file size and a `file-1mib-block-chain-v1` fingerprint,
not ordinary `sha256sum`. Preflight checks only the selected EXE/COM; dependencies
may diverge later. Ordered DOS loads and payload/relocation hashes distinguish
overlays at reused addresses; custom loaders bypassing DOS EXEC need extra tracing.

Compatibility identifies the emulation/evidence contract, not a host build hash.
Matching hashes verify captured evidence, not all internal behavior.

Tests: [C++ codec](../tests/native-replay/archive_test.cpp);
[independent Python hashlib/zlib, truncation and malformed-data checks](../tests/native-replay/test_native_replay.py);
[record/replay and rejection suite](../tests/native-replay/README.md).
