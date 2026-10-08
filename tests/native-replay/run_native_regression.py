"""Game-independent native replay regression suite; custom ASM, compact evidence."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from replay_test_common import ROOT, TMP, TESTS
from probe_native_replay import DOMAINS, require


CASES = (
    ("tick-stop", ("--ticks", "1000")),
    ("frontend-close", ("--frontend-input", "--faults")),
    # EXIT.COM must bypass the shell's built-in EXIT.
    ("dos-exit", ("--dos-exit", "--target", "EXIT.COM")),
    ("overlay-reuse", ("--overlay",)),
)


def validate_report(report):
    require(report["status"] == "pass", "Probe did not pass")
    require([leg["name"] for leg in report["legs"]] == ["record", "replay1", "replay2"],
            "Every fixture needs a recording and two fresh-process replays")
    require(all(leg["exit_code"] == 0 for leg in report["legs"]), "An emulator leg failed")
    require(set(report["domain_names"]) == DOMAINS, "Missing observation domain")
    require(0 < report["archive_bytes"] < 64 * 1024, "Synthetic archive outside size budget")
    require(report["game_copies"] == report["filesystem_mounts"] == 0, "Unexpected game copy or OS mount")


def execute(command, log_path, timeout):
    env = dict(os.environ)
    # Keep asserts enabled regardless of the caller's Python settings.
    env.pop("PYTHONOPTIMIZE", None)
    start = time.monotonic()
    with log_path.open("wb") as log:
        with subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                              stdout=log, stderr=subprocess.STDOUT, start_new_session=True) as process:
            try:
                code = process.wait(timeout=timeout)
            except BaseException:
                # Stop the entire process group on failure.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                raise
    require(code == 0, f"Exit {code}; see {log_path}")
    return round(time.monotonic() - start, 3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "src/dosbox-x", help="Built native replay-enabled DOSBox-X")
    args = parser.parse_args()
    binary = args.binary.resolve()
    require(binary.is_file(), f"Build the replay-enabled fork first; binary missing: {binary}")
    (TMP / "runs").mkdir(exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="native-regression-", dir=TMP / "runs"))
    summary = {"status": "incomplete", "cases": {}, "codec": "not-run"}
    print(f"Synthetic regression evidence: {directory}", flush=True)
    try:
        execute([sys.executable, "-B", str(TESTS / "native_replay_checks.py")], directory / "checks.log", 30)
        print("PASS regression checker unit tests", flush=True)
        execute([sys.executable, "-B", str(TESTS / "test_native_replay.py")], directory / "codec.log", 180)
        summary["codec"] = "pass"
        print("PASS archive codec/corruption checks", flush=True)
        for name, arguments in CASES:
            seconds = execute([sys.executable, "-B", str(TESTS / "probe_native_replay.py"),
                               "--binary", str(binary), *arguments,
                               "--output-dir", str(directory / name)], directory / f"{name}.log", 180)
            report = json.loads((directory / name / "report.json").read_text())
            validate_report(report)
            summary["cases"][name] = report
            print(f"PASS {name}: record + 2 replays; {report['archive_bytes']} bytes; {seconds}s", flush=True)
        require(len({report["binary_sha256"] for report in summary["cases"].values()}) == 1,
                "Backend binary changed during the suite")
        summary["status"] = "pass"
    except BaseException as error:
        summary.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        summary["artifact_bytes"] = sum(path.stat().st_size for path in directory.rglob("*") if path.is_file())
        (directory / "report.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(f"{summary['status'].upper()}: {directory / 'report.json'}", flush=True)


if __name__ == "__main__":
    main()
