# Native replay archive, experimental v1

This is a new native contract, not a reinterpretation of legacy DRINPUT
recordings. The archive codec alone does not implement game playback.

Eight-byte magic: `DRNATV01`. Frames: one ASCII type byte, unsigned little-endian
32-bit compressed length, a single zlib member, 64 lowercase ASCII SHA-256
hex characters. Initial chain = SHA256(magic). Each next chain =
SHA256(previous ASCII digest || five-byte frame prefix || compressed payload).
This detects corruption; it is not authentication.

Decoded payloads are canonical agent JSON. Maximum decoded header size is 2 MiB;
other frames 64 KiB. Compressed lengths are bounded by zlib's compressBound of
that limit. Concatenated members, trailing bytes, duplicate/noncanonical JSON,
missing footers and repeated headers are rejected. Readers validate the entire
chain before launch and check it again during streaming. Memory is bounded by a
frame, not playthrough length. Writers exclusively create a new file and flush
every frame before acknowledging inputs. Power loss can still lose OS buffers;
missing/truncated data must fail validation, never become a successful replay.

Frame types: H header/config, I input deliveries, X DOS EXEC lifecycle rows,
O observations, F final completion. H must be first and unique; F last and
mandatory. The runtime additionally validates semantic order, counts, timeline
and compatibility. Hashes of memory/raster/audio are retained, not their raw
bytes. No game files, host directory snapshots or backend executable are stored.

The standalone test is `tests/native-replay/archive_test.cpp`; the parent
harness also checks the wire format independently with Python hashlib/zlib,
every truncation of a small fixture and malformed canonical/compressed data.
