"""Small native codec tests; artifacts only in .tmp, no emulator/game copies."""
import hashlib
import json
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path
from replay_test_common import ROOT as BACKEND, TMP, run


def main():
    build = TMP / "build/native-replay-tests"
    build.mkdir(parents=True, exist_ok=True)
    fpu_binary = build / "fpu-regvalid-test"
    run("c++", "-std=c++14", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", BACKEND / "tests/native-replay", "-I", BACKEND / "include",
        BACKEND / "tests/native-replay/fpu_regvalid_test.cpp", "-o", fpu_binary)
    run(fpu_binary, timeout=10)
    print("FPU v2: validity/denormal bits, raw x87/SIMD/fallback and alias/padding checks PASS")
    binary = build / "archive-test"
    run("c++", "-std=c++14", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", BACKEND / "tests/native-replay", "-I", BACKEND / "include",
        BACKEND / "tests/native-replay/archive_test.cpp",
        BACKEND / "src/agent/protocol/agent_protocol.cpp",
        BACKEND / "src/agent/protocol/sha256.cpp",
        BACKEND / "src/agent/replay/native_archive.cpp", "-lz", "-o", binary)

    def call(*args, success=True):
        result = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stderr
        return result.stdout

    def frame(chain, kind, raw):
        payload = zlib.compress(raw)
        prefix = struct.pack("<cI", kind, len(payload))
        chain = hashlib.sha256(chain + prefix + payload).hexdigest().encode()
        return chain, prefix + payload + chain

    with tempfile.TemporaryDirectory(prefix="native-codec-", dir=TMP / "tmp") as folder:
        path = Path(folder) / "test.drp"
        call("write", path)
        original = path.read_bytes()
        call("write", path, success=False)  # Never overwrite an existing recording.
        assert path.read_bytes() == original
        call("check", path)
        assert len(original) < 500
        assert call("hash", "abc").strip() == hashlib.sha256(b"abc").hexdigest()
        assert call("hash", "").strip() == hashlib.sha256(b"").hexdigest()
        offset = 8
        chain = hashlib.sha256(original[:8]).hexdigest().encode()
        kinds = []
        while offset < len(original):
            prefix = original[offset:offset + 5]
            kind, size = struct.unpack("<cI", prefix)
            payload = original[offset + 5:offset + 5 + size]
            chain = hashlib.sha256(chain + prefix + payload).hexdigest().encode()
            assert original[offset + 5 + size:offset + 5 + size + 64] == chain
            json.loads(zlib.decompress(payload))
            kinds.append(kind)
            offset += 5 + size + 64
        assert kinds == [b"H", b"O", b"F"]
        bad = Path(folder) / "bad.drp"
        for length in range(len(original)):
            bad.write_bytes(original[:length])
            call("check", bad, success=False)
        for offset in (0, 8, 12, 16, len(original)-1):
            changed = bytearray(original); changed[offset] ^= 1
            bad.write_bytes(changed); call("check", bad, success=False)
        bad.write_bytes(original + b"x"); call("check", bad, success=False)
        for raw in (b'{"x":1,"x":2}', b'{ "x":1}', b'"' + b'x' * (2*1024*1024) + b'"'):
            chain = hashlib.sha256(b"DRNATV01").hexdigest().encode()
            chain, header = frame(chain, b"H", raw)
            chain, footer = frame(chain, b"F", b'{}')
            bad.write_bytes(b"DRNATV01" + header + footer)
            call("check", bad, success=False)
        partial = Path(folder) / "partial.drp"
        call("partial", partial); call("check", partial, success=False)
    print("Native archive: round-trip, independent SHA/zlib decoder, exclusive create, all truncations, corruption, trailing data, JSON and expansion limits PASS")


if __name__ == "__main__":
    main()
