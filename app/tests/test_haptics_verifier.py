"""Negative controls for the device evidence verifier; no hardware claims."""
import copy
import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("verifier", ROOT / "tools/switch-haptics-probe.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)
ZERO = [0, 160, 0, 320]
PULSE = [.0441941738, 160, 0, 320]
ABSOLUTE = [.255474287, 80, .255474287, 640]


def reference_run(argv, *, input, **kwargs):
    """Handwritten golden values for the fixture, independent of production C."""
    rows = []
    for offset in range(0, len(input), 4):
        word = int.from_bytes(input[offset:offset+4], "little")
        values = {0x40400100: [ZERO], 0xc0050000: [ZERO, PULSE, ZERO],
                  0x60208180: [ABSOLUTE]}[word]
        rows.append(str(len(values)) + " " + " ".join(str(x) for v in values for x in v))
    return subprocess.CompletedProcess(argv, 0, ("\n".join(rows)).encode(), b"")


def fixture():
    records = [dict(stage="begin"), dict(stage="device", devices=2, style=1,
               permitted=True, activate=0, permission_rc=0)]
    for seq in range(1, 14):
        words = [0x40400100, 0xc0050000] if seq <= 6 else [0xc0050000, 0x40400100]
        bands = [[ZERO, ZERO, ZERO], [ZERO, PULSE, ZERO]] if seq <= 6 else [
                  [ZERO, PULSE, ZERO], [ZERO, ZERO, ZERO]]
        if seq == 13:
            words, bands = [0x60208180] * 2, [[ABSOLUTE], [ABSOLUTE]]
        wire = bytes([13, 10, 0, 0, 0, 16, 0]) + b"".join(w.to_bytes(4, "little") for w in words) + bytes(54)
        start = seq * 50000000
        records.append(dict(stage="command", slot=0, seq=seq, rc=0, wire=wire.hex(),
                            received_ns=start-100000, apply_ns=start-50000))
        for i in range(len(bands[0])):
            sent = start + i * 5000000
            values = [bands[0][i], bands[1][i]]
            records.append(dict(stage="send", kind=2, slot=0, seq=seq, sample=i, rc=0,
                                due_ns=sent, sent_ns=sent, completed_ns=sent+10000, values=values))
            records.append(dict(stage="readback", ns=sent+100000, end_ns=sent+200000,
                                rc=[0, 0], values=values))
    records.extend([dict(stage="command", slot=0, seq=14, rc=0, wire=(bytes([1])+bytes(8)).hex()),
                    dict(stage="send", kind=2, slot=0, seq=14, sample=0, rc=0, values=[ZERO, ZERO]),
                    dict(stage="send", kind=3, slot=0, seq=0, rc=0, values=[ZERO, ZERO]),
                    dict(stage="stopped", rc=[0, 0], amplitudes=[0]*4),
                    dict(stage="joined", starts=1, joins=1, running=False, failures=0, io_rc=0,
                         rejected=0, dropped=0, trace_lost=0, queued=14, applied=14, sends=39),
                    dict(stage="end", software_ok=True)])
    return copy.deepcopy(records)


class VerifierTests(unittest.TestCase):
    def run_report(self, rows):
        with patch.object(verifier.subprocess, "run", side_effect=reference_run):
            return verifier.analyze(rows, Path("unused-reference"))

    def test_valid(self):
        report = self.run_report(fixture())
        self.assertTrue(report["software_path_verified"])
        self.assertTrue(report["ipc_readback_verified"])
        self.assertTrue(report["sample_timing_within_500us"])
        self.assertEqual(report["native_samples"], 37)
        self.assertFalse(report["physical_fidelity_verified"])
        self.assertFalse(report["loader_return_verified"])

    def test_missing_wrong_reordered_and_failed(self):
        for case in ("end", "sample", "parameter", "order", "ipc", "cleanup", "joined", "drop", "stopped"):
            with self.subTest(case=case):
                rows = fixture()
                frames = [r for r in rows if r["stage"] == "send" and r["seq"] == 1]
                if case in ("end", "cleanup"):
                    rows = [r for r in rows if r["stage"] != "end"] if case == "end" else [
                        r for r in rows if r.get("kind") != 3]
                elif case == "sample":
                    rows.remove(frames[1])
                elif case == "parameter":
                    frames[1]["values"][1][0] = .5
                elif case == "order":
                    frames[1]["sample"] = 2
                elif case == "ipc":
                    frames[1]["rc"] = 123
                else:
                    r = next(r for r in rows if r["stage"] == ("stopped" if case == "stopped" else "joined"))
                    r[{"joined": "joins", "drop": "dropped", "stopped": "amplitudes"}[case]] = [1]*4 if case == "stopped" else 2
                self.assertFalse(self.run_report(rows)["software_path_verified"])

    def test_bad_readbacks_do_not_prove_ipc(self):
        for case in ("missing", "wrong", "overlap"):
            rows = fixture()
            if case == "missing":
                rows = [r for r in rows if r["stage"] != "readback"]
            else:
                for r in rows:
                    if r["stage"] == "readback":
                        if case == "wrong":
                            r["rc"] = [123, 123]
                        else:
                            r["end_ns"] += 5000000
            report = self.run_report(rows)
            self.assertTrue(report["software_path_verified"])
            self.assertFalse(report["ipc_readback_verified"])

    def test_collapsed_sample_times_fail(self):
        rows = fixture()
        for r in rows:
            if r["stage"] == "send" and r.get("seq") == 1 and r["sample"] == 1:
                r["sent_ns"] -= 4000000
        self.assertFalse(self.run_report(rows)["sample_timing_within_500us"])


if __name__ == "__main__":
    unittest.main()
