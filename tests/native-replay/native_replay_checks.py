import copy
import unittest

from probe_native_replay import DOMAINS, check_overlay
from run_native_regression import validate_report


class NativeRegressionChecks(unittest.TestCase):
    def report(self):
        return {"status": "pass", "domain_names": sorted(DOMAINS), "archive_bytes": 4000,
                "game_copies": 0, "filesystem_mounts": 0,
                "legs": [{"name": name, "exit_code": 0} for name in ("record", "replay1", "replay2")]}

    def test_complete_report(self):
        validate_report(self.report())

    def test_fail_closed_for_incomplete_or_bloated_runs(self):
        for key, value in (("status", "incomplete"), ("legs", []), ("domain_names", []),
                           ("archive_bytes", 65536), ("game_copies", 1), ("filesystem_mounts", 1)):
            with self.subTest(key=key):
                report = self.report()
                report[key] = value
                with self.assertRaises(RuntimeError):
                    validate_report(report)

    def test_failed_process_is_not_a_pass(self):
        report = self.report()
        report["legs"][1]["exit_code"] = 1
        with self.assertRaises(RuntimeError):
            validate_report(report)

    def frames(self):
        return [{"kind": "X", "data": [
            {"effective_mode": 3, "success": True, "load_segment": 2000,
             "declared_relocations": 1, "relocation_read_bytes": 4,
             "payload_read_sha256": identity} for identity in ("a", "b", "a")]}]

    def test_overlay_slot_reuse(self):
        self.assertEqual(check_overlay(self.frames())["loads"], 3)

    def test_missing_overlay_is_not_a_pass(self):
        frames = self.frames()
        frames[0]["data"].pop()
        with self.assertRaises(RuntimeError):
            check_overlay(frames)

    def test_overlay_identity_relocation_and_address_regressions(self):
        for key, value in (("success", False), ("load_segment", 2001),
                           ("declared_relocations", 0), ("relocation_read_bytes", 0),
                           ("payload_read_sha256", "a")):
            with self.subTest(key=key):
                frames = copy.deepcopy(self.frames())
                frames[0]["data"][1][key] = value
                with self.assertRaises(RuntimeError):
                    check_overlay(frames)


if __name__ == "__main__":
    unittest.main()
