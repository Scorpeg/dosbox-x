"""Standalone test paths: no outer harness checkout or source lock required."""
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parents[1]
TMP = Path(os.environ.get("DOSBOX_REPLAY_TEST_TMP", ROOT / ".tmp")).resolve()
if TMP.name != ".tmp":
    raise ValueError("DOSBOX_REPLAY_TEST_TMP must name a .tmp directory")
(TMP / "tmp").mkdir(parents=True, exist_ok=True)
os.environ["TMPDIR"] = str(TMP / "tmp")
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)
