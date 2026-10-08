"""Short native-process qualification; no sockets, PTYs, mounts or game copies."""
import argparse
import copy
import hashlib
import json
import os
import resource
from pathlib import Path
import subprocess
import struct
import tempfile
import time
import zlib
from replay_test_common import ROOT, TMP, TESTS, run


DOMAINS = {
    "dos-exec-rows-v1", "fp-simd-storage-v2", "input-deliveries-v1",
    "integer-backing-v1", "mixer-rendered-v1", "mouse-int33-v2",
    "opl-dbopl-state-v1", "pit-speaker-state-v1", "raster-submitted-v1",
}
OVERLAY_RESULT = b"OVL1" + struct.pack("<4H", 0xaaaa, 0xbbbb, 0xaaaa, 49)


def require(condition, detail):
    if not condition:
        raise RuntimeError(str(detail))


def check_overlay(frames):
    rows = [row for frame in frames if frame["kind"] == "X" for row in frame["data"]]
    loads = [row for row in rows if row.get("effective_mode") == 3 and row.get("success") is True]
    require(len(loads) == 3, "Expected three successful overlay loads")
    require(len({row["load_segment"] for row in loads}) == 1, "Overlay slot was not reused")
    require(all(row["declared_relocations"] == 1 and row["relocation_read_bytes"] == 4 for row in loads),
            "Overlay relocations were not observed")
    identities = [row["payload_read_sha256"] for row in loads]
    require(identities[0] == identities[2] and identities[0] != identities[1],
            "Overlay A/B/A payload identities were not preserved")
    return {"loads": len(loads), "reused_segment": loads[0]["load_segment"], "identities": identities}


def main():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "src/dosbox-x")
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--target", default="CAPTURE.COM")
    parser.add_argument("--directory", default="")
    parser.add_argument("--ticks", type=int, default=3000)
    parser.add_argument("--frontend-input", action="store_true", help="Inject SDL events only in the recording process, then close its window")
    parser.add_argument("--faults", action="store_true", help="Check corrupt archives, unsupported settings and a rehashed false observation")
    parser.add_argument("--dos-exit", action="store_true", help="Check a small speaker/selected-process-exit fixture")
    parser.add_argument("--overlay", action="store_true", help="Self-checking x87/VGA and relocated A/B/A overlay fixture")
    parser.add_argument("--output-dir", type=Path, help="New artifact directory beneath .tmp (never overwritten)")
    args = parser.parse_args()
    if args.dos_exit and (args.game_dir or args.frontend_input):
        parser.error("DOS exit fixture cannot be combined with game-dir or frontend-input")
    if args.overlay and (args.game_dir or args.frontend_input or args.dos_exit):
        parser.error("Overlay fixture cannot be combined with game-dir, frontend-input or dos-exit")
    (TMP / "runs").mkdir(exist_ok=True)
    if args.output_dir:
        directory = args.output_dir.resolve()
        if TMP.resolve() not in directory.parents:
            parser.error("output-dir must be beneath .tmp")
        directory.mkdir()  # Exclusive: never overwrite a previous run.
    else:
        directory = Path(tempfile.mkdtemp(prefix="native-replay-", dir=TMP / "runs"))
    binary = args.binary.resolve(strict=True)
    source = args.game_dir.resolve(strict=True) if args.game_dir else directory / "fixture"
    if not args.game_dir:
        source.mkdir()
        if args.dos_exit and args.target == "CAPTURE.COM":
            args.target = "ENDPROBE.COM"
        fixture = "native_overlay" if args.overlay else "native_exit" if args.dos_exit else "mouse_capture"
        run("nasm", "-f", "bin", TESTS / f"probes/{fixture}.asm",
            "-o", source / args.target)
        if args.overlay:
            for filename, marker in (("ONE.OVL", "0xaaaa"), ("TWO.OVL", "0xbbbb")):
                run("nasm", "-f", "bin", "-DMARKER=" + marker,
                    TESTS / "probes/native_overlay_image.asm", "-o", source / filename)
    archive = directory / "input.drp"
    if args.frontend_input:
        run("c++", "-std=c++14", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
            "-I", "/usr/include/SDL2", TESTS / "native_frontend_input.cpp", "-ldl", "-o", directory / "frontend-input.so")
    report = {"status": "incomplete", "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "runtime": "native-no-controller", "game_copies": 0, "filesystem_mounts": 0, "legs": []}
    print("Native qualification:", directory, flush=True)
    try:
        for name in ("record", "replay1", "replay2"):
            cwd = directory / name
            cwd.mkdir()
            # Recorded settings must override inherited config and timezone.
            (cwd / "dosbox-x.conf").write_text("[cpu]\ncycles=max\n[autoexec]\necho INHERITED_CONFIG_MUST_NOT_RUN\n")
            env = {**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
                   "DOSBOX_PROOF_AUDIO_OUTPUT": "none", "DOSBOX_CPU_CYCLES": "max",
                   "TZ": "Pacific/Honolulu" if name == "replay2" else "Europe/Moscow"}
            for key in ("LD_PRELOAD", "NATIVE_REPLAY_PROBE_ARCHIVE", "NATIVE_REPLAY_PROBE_ABORT"):
                env.pop(key, None)
            for key in ("XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME"):
                target = cwd / key.lower(); target.mkdir(mode=0o700); env[key] = str(target)
            command = [str(binary), "--record" if name == "record" else "--replay", str(archive), "--game-dir", str(source)]
            if name == "record":
                command += ["--target", args.target, "--directory", args.directory]
                if args.frontend_input:
                    env["LD_PRELOAD"] = str(directory / "frontend-input.so")
                    env["NATIVE_REPLAY_PROBE_ARCHIVE"] = str(archive)
                else:
                    command += ["--ticks", str(args.ticks)]
                if args.overlay:
                    command += ["--sample-ticks", "20"]
            # Remove prior output so every leg must write its own result.
            if args.overlay and (source / "RESULT.BIN").exists():
                require((source / "RESULT.BIN").read_bytes() == OVERLAY_RESULT, "Unexpected synthetic output")
                (source / "RESULT.BIN").unlink()
            start = time.monotonic()
            with (cwd / "emulator.log").open("wb") as log:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                        cwd=cwd, env=env, timeout=60)
            text = (cwd / "emulator.log").read_text(errors="replace")
            report["legs"].append({"name": name, "exit_code": result.returncode,
                                   "seconds": round(time.monotonic()-start,3)})
            require(result.returncode == 0, text[-5000:])
            require(("NATIVE RECORDING SAVED" if name == "record" else "NATIVE REPLAY MATCH") in text, text[-3000:])
            require("INHERITED_CONFIG_MUST_NOT_RUN" not in text, "Inherited autoexec ran")
            if args.overlay:
                require((source / "RESULT.BIN").read_bytes() == OVERLAY_RESULT, "ASM oracle output differs")
                report["legs"][-1]["asm_oracle"] = "pass"
        report.update(status="pass", archive_bytes=archive.stat().st_size,
                      archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        dump = subprocess.run([str(binary), "--replay-info", str(archive)], capture_output=True, text=True, check=True, timeout=10)
        frames = [json.loads(line) for line in dump.stdout.splitlines()]
        rows = [row for frame in frames if frame["kind"] == "I" for row in frame["data"]]
        report["inputs"] = len(rows)
        report["input_kinds"] = sorted({row["kind"] for row in rows})
        last = [frame["data"] for frame in frames if frame["kind"] == "O"][-1]
        report["end_tick"] = last["tick"]
        report["domain_names"] = sorted(last["domains"])
        require(set(last["domains"]) == DOMAINS, "Observation domain coverage changed")
        if not args.game_dir and args.ticks <= 3000:
            require(archive.stat().st_size < 64 * 1024, "Short synthetic archive exceeds 64 KiB budget")
        if not (args.game_dir or args.frontend_input or args.dos_exit or args.overlay):
            require(last["tick"] == args.ticks, "Recording did not stop at requested tick")
        if args.overlay:
            require(last["exit"]["exit_code"] == 0, "ASM self-check failed")
            report["overlay"] = check_overlay(frames)
            # Changing an overlay must fail replay even when the selected COM is unchanged.
            original = (source / "TWO.OVL").read_bytes()
            (source / "RESULT.BIN").unlink()
            try:
                run("nasm", "-f", "bin", "-DMARKER=0xcccc",
                    TESTS / "probes/native_overlay_image.asm", "-o", source / "TWO.OVL")
                with (directory / "changed-overlay.log").open("wb") as log:
                    changed = subprocess.run([str(binary), "--replay", str(archive), "--game-dir", str(source)],
                                             cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                             stdout=log, stderr=log, timeout=60)
                text = (directory / "changed-overlay.log").read_text(errors="replace")
                require(changed.returncode == 1 and "DOS EXEC mismatch" in text
                        and "NATIVE REPLAY MATCH" not in text, text[-3000:])
                report["overlay"]["changed_dependency"] = "rejected"
            finally:
                (source / "TWO.OVL").write_bytes(original)
        if args.dos_exit:
            require(last["exit"]["exit_code"] == 7 and last["tick"] < 1000, last)
            report["exit"] = last["exit"]
        if args.frontend_input:
            require({"key", "motion", "button", "wheel", "capture"} <= set(report["input_kinds"]), rows)
            require(any(row["source"] == "api" and row["kind"] == "key" and row["key"] == "b" and not row["pressed"] for row in rows), rows)
        if args.faults:
            def encode(values):
                result = bytearray(b"DRNATV01")
                chain = hashlib.sha256(result).hexdigest().encode()
                for frame in values:
                    raw = json.dumps(frame["data"], sort_keys=True, separators=(",", ":")).encode()
                    payload = zlib.compress(raw)
                    prefix = struct.pack("<cI", frame["kind"].encode(), len(payload))
                    chain = hashlib.sha256(chain + prefix + payload).hexdigest().encode()
                    result.extend(prefix + payload + chain)
                return bytes(result)

            raw = archive.read_bytes()
            variants = {"truncated": (raw[:-1], "truncated archive"),
                        "trailing": (raw+b"x", "trailing data"),
                        "corrupt": (raw[:-1]+bytes([raw[-1]^1]), "hash chain differs")}
            for name, key, value, reason in (
                    ("compatibility", "compatibility", "unsupported", "compatibility contract"),
                    ("previous-backend", "compatibility", "native-replay-experimental-v1/826dfc42/48fe7645/normal-fixed10000/silent-entry2-seed0", "compatibility contract"),
                    ("settings", "machine", {}, "unsupported machine profile"),
                    ("executable", "target_identity", {}, "executable identity differs")):
                changed = copy.deepcopy(frames)
                changed[0]["data"][key] = value
                variants[name] = (encode(changed), reason)
            changed = copy.deepcopy(frames)
            next(frame for frame in changed if frame["kind"] == "O")["data"]["domains"]["mouse-int33-v2"] = "0"*64
            variants["false-observation"] = (encode(changed), "observation mismatch")
            report["faults"] = {}
            for name, (data, expected) in variants.items():
                bad = directory / (name + ".drp"); bad.write_bytes(data)
                with (directory / (name + ".log")).open("wb") as log:
                    failed = subprocess.run([str(binary), "--replay", str(bad), "--game-dir", str(source)],
                                            cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log, timeout=60)
                text = (directory / (name + ".log")).read_text(errors="replace")
                require(failed.returncode == 1 and expected in text and "NATIVE REPLAY MATCH" not in text, text[-3000:])
                report["faults"][name] = "rejected"
            if args.frontend_input:
                partial = directory / "aborted.drp"
                crash_env = {**env, "LD_PRELOAD": str(directory / "frontend-input.so"),
                             "NATIVE_REPLAY_PROBE_ARCHIVE": str(partial), "NATIVE_REPLAY_PROBE_ABORT": "1"}
                with (directory / "aborted.log").open("wb") as log:
                    aborted = subprocess.run([str(binary), "--record", str(partial), "--game-dir", str(source),
                                              "--target", args.target, "--directory", args.directory],
                                             cwd=cwd, env=crash_env, stdin=subprocess.DEVNULL, stdout=log, stderr=log, timeout=60)
                require(aborted.returncode == 73, "Fault injector did not terminate recording")
                check = subprocess.run([str(binary), "--replay-info", str(partial)], capture_output=True, text=True, timeout=5)
                require(check.returncode == 1 and "incomplete archive" in check.stderr, check.stderr)
                report["faults"]["abrupt-stop"] = "unsealed-incomplete"
    except BaseException as error:
        report.update(status="failed", error=str(error)[-5000:])
        raise
    finally:
        (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report,indent=2),flush=True)


if __name__ == "__main__":
    main()
