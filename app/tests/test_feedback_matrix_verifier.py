"""Independent matrix goldens and rejection cases; no real-device claims."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("feedback_matrix_verifier", ROOT / "tools/check-feedback-matrix.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)
ZERO = [0, 160, 0, 320]
PEAK = .5571934


def fixture(platform="host-simulation"):
    def wire(kind, low=0, high=0):
        return struct.pack("<BHHI", kind, low, high, 0)

    sony = bytearray(69)
    sony[0], sony[1], sony[5], sony[7], sony[8] = 13, 47, 3, 64, 128
    led = bytearray(sony)
    led[6], led[49], led[50], led[51] = 4, 170, 42, 85
    audio = bytearray(69)
    audio[0], audio[1] = 13, 47
    pulse = bytes([13, 10, 0, 0, 0, 16, 0]) + struct.pack("<II", 0x40400100, 0xc0050000) + bytes(54)
    bands = bytes([13, 10, 0, 0, 0, 16, 0]) + struct.pack("<II", 0x60208180, 0x60208180) + bytes(54)
    inputs = [wire(1, 32768, 16384), wire(1), wire(1, 65535), wire(6, 0, 32768), wire(6),
              wire(6, 16384), wire(1), wire(6), bytes(sony), bytes(led), bytes(audio), pulse, bands, wire(1)]
    quiet = [ZERO, ZERO]
    body = [[PEAK, 160, 0, 320]] * 2
    sony_body = [[.279689236, 160, .139844618, 320]] * 2
    # These are hand-calculated software outputs, not production renderer calls.
    outputs = {
        1: [[[.278600951, 160, .139300476, 320]] * 2],
        2: [quiet], 3: [body],
        4: [[[PEAK, 160, 0, 320], [PEAK, 160, .278600951, 320]]],
        5: [body],
        6: [[[PEAK, 160, .139300476, 320], [PEAK, 160, 0, 320]]],
        7: [[[0, 160, .139300476, 320], ZERO]], 8: [quiet],
        9: [sony_body], 10: [sony_body], 11: [quiet],
        12: [quiet, [ZERO, [.0441941738, 160, 0, 320]], quiet],
        13: [[[.255474287, 80, .255474287, 640]] * 2], 14: [quiet],
    }
    case_specs = [("ordinary-batch", 1, 2), ("body-start", 3, 3), ("trigger-start", 4, 4),
                  ("trigger-stop", 5, 5), ("trigger-restart", 6, 6), ("body-stop", 7, 7),
                  ("trigger-final-stop", 8, 8), ("sony-legacy", 9, 9), ("sony-led", 10, 10),
                  ("sony-audio-handoff", 11, 11), ("nintendo-pulse", 12, 12),
                  ("nintendo-bands", 13, 13), ("final-stop", 14, 14)]
    notes, trace, readings = [], [], []
    cursor = 1_000_000_000
    for name, first, last in case_specs:
        begin = cursor
        for seq in range(first, last + 1):
            source = 2 if seq in (4, 5, 6, 8) else 3 if seq in (9, 10, 11) else 1 if seq in (12, 13) else 0
            received = begin + 100_000 + (30_000_000 if seq == 2 else 0)
            apply = begin + (30_200_000 if seq == 1 else 30_210_000 if seq == 2 else 200_000)
            trace.append(dict(stage="command", slot=0, seq=seq, received_ns=received, apply_ns=apply,
                              source=source, unsupported={10: 4, 11: 2}.get(seq, 0), rc=0,
                              wire=inputs[seq-1].hex()))
            sent = begin + 60_230_000 if seq == 2 else apply + 10_000
            for sample, params in enumerate(outputs[seq]):
                sent_ns = sent + sample * 5_010_000
                completed = sent_ns + 10_000
                trace.append(dict(stage="send", kind=2, slot=0, seq=seq, sample=sample, source=source,
                                  unsupported=0, due_ns=sent_ns-5_000, sent_ns=sent_ns,
                                  completed_ns=completed, rc=0, values=copy.deepcopy(params)))
                readings.append(dict(stage="readback", ns=completed+100_000, end_ns=completed+200_000,
                                     rc=[0, 0], values=copy.deepcopy(params)))
                cursor = completed + 1_000_000
        notes.append(dict(stage="matrix-case", name=name, first_seq=first, last_seq=last,
                          begin_ns=begin, end_ns=cursor, write_ok=True))
        cursor += 1_000
    trace.append(dict(stage="send", kind=3, slot=0, seq=0, sample=0, source=0, unsupported=0,
                      due_ns=0, sent_ns=cursor+1_000_000, completed_ns=cursor+1_010_000,
                      rc=0, values=copy.deepcopy(quiet)))
    joined = dict(stage="joined", starts=1, joins=1, running=False, queued=14, applied=14,
                  sends=17, decoded=4, scheduled=4, emitted=4, ordinary_commands=5, trigger_commands=4,
                  sony_commands=3, unsupported_effects=2, adapted_effects=7, failures=0, io_rc=0,
                  rejected=0, dropped=0, trace_lost=0, stream_rejected=0, stream_dropped=0,
                  stale_frames=0, discontinuities=0, desync_rejected=0, superseded_samples=0, dropped_frames=0)
    return [dict(stage="begin", platform=platform, matrix=True, physical_fidelity_verified=False),
            dict(stage="device", id=0, style=1, devices=2, activate=0, permission_rc=0, permitted=True),
            *notes, dict(stage="matrix-end", cases=13, commands=14, ok=True),
            dict(stage="stopped", rc=[0, 0], amplitudes=[0, 0, 0, 0]), joined,
            *trace, *readings, dict(stage="end", software_ok=True, physical_fidelity_verified=False,
                                    loader_return_verified=False)]


def row(records, stage, seq=None, sample=0):
    return next(r for r in records if r.get("stage") == stage and
                (seq is None or r.get("seq") == seq and r.get("sample", 0) == sample))


class FeedbackMatrixVerifierTests(unittest.TestCase):
    def reject(self, records):
        report = verifier.analyze(records)
        self.assertFalse(report["software_path_verified"], report)
        self.assertFalse(report["ipc_submission_verified"])
        self.assertFalse(report["ipc_readback_verified"])
        self.assertFalse(report["physical_fidelity_verified"])
        self.assertFalse(report["loader_return_verified"])

    def test_valid_and_platform_boundary(self):
        for platform in ("host-simulation", "switch"):
            with self.subTest(platform=platform):
                report = verifier.analyze(fixture(platform))
                self.assertTrue(report["software_path_verified"], report)
                self.assertTrue(report["parameter_readback_values_match"])
                self.assertTrue(report["batched_ordinary_interval_preserved"])
                self.assertTrue(report["native_sample_spacing_verified"])
                self.assertEqual(report["ipc_readback_matched_frames"], 16)
                self.assertEqual(report["ipc_submission_verified"], platform == "switch")
                self.assertEqual(report["ipc_readback_verified"], platform == "switch")
                self.assertEqual(report["stopped_ipc_readback_verified"], platform == "switch")
                self.assertFalse(report["physical_fidelity_verified"])
                self.assertFalse(report["loader_return_verified"])

    def test_structure_and_stimulus_rejections(self):
        for case in ("matrix", "platform", "begin", "missing_command", "duplicate_joined", "case_order",
                     "missing_case", "duplicate_output", "wrong_wire", "wrong_slot", "early_end"):
            with self.subTest(case=case):
                records = fixture()
                if case == "matrix": records[0]["matrix"] = False
                elif case == "platform": records[0]["platform"] = "unknown"
                elif case == "begin": records.pop(0)
                elif case == "missing_command": records.remove(row(records, "command", 5))
                elif case == "duplicate_joined": records.insert(-1, copy.deepcopy(row(records, "joined")))
                elif case == "case_order": records[2], records[3] = records[3], records[2]
                elif case == "missing_case": records.remove(row(records, "matrix-case"))
                elif case == "duplicate_output": records.insert(-1, copy.deepcopy(row(records, "send", 3)))
                elif case == "wrong_wire": row(records, "command", 3)["wire"] = struct.pack("<BHHI", 1, 32768, 0, 0).hex()
                elif case == "wrong_slot": row(records, "send", 3)["slot"] = 1
                elif case == "early_end": records.insert(0, records.pop())
                self.reject(records)

    def test_parameter_and_source_rejections(self):
        for seq, field, value in ((3, "source", 2), (4, "source", 0), (12, "sample", 2), (10, "source", 0)):
            with self.subTest(seq=seq, field=field):
                records = fixture()
                row(records, "send", seq)[field] = value
                self.reject(records)
        for seq, sample, motor, field, value in ((1, 0, 0, 1, 149.933411), (3, 0, 0, 0, 1),
                                                (5, 0, 0, 0, 0), (7, 0, 0, 2, 0),
                                                (10, 0, 0, 0, 0), (11, 0, 1, 2, .2),
                                                (12, 1, 1, 0, 0), (13, 0, 0, 3, 320)):
            with self.subTest(seq=seq, sample=sample, motor=motor, field=field):
                records = fixture()
                row(records, "send", seq, sample)["values"][motor][field] = value
                self.reject(records)

    def test_timing_and_cleanup_rejections(self):
        for case in ("collapsed_batch", "not_batched", "short_native", "noncausal", "missing_cleanup", "nonzero_cleanup", "nonzero_stop"):
            with self.subTest(case=case):
                records = fixture()
                if case == "collapsed_batch":
                    out = row(records, "send", 2)
                    out["sent_ns"] = row(records, "send", 1)["completed_ns"] + 1_000_000
                    out["due_ns"], out["completed_ns"] = out["sent_ns"], out["sent_ns"] + 10_000
                elif case == "not_batched": row(records, "command", 1)["apply_ns"] = row(records, "command", 1)["received_ns"]
                elif case == "short_native":
                    out = row(records, "send", 12, 1)
                    out["sent_ns"] -= 2_000_000
                    out["due_ns"], out["completed_ns"] = out["sent_ns"], out["sent_ns"] + 10_000
                elif case == "noncausal": row(records, "send", 3)["completed_ns"] = 1
                elif case == "missing_cleanup": records = [r for r in records if r.get("kind") != 3]
                elif case == "nonzero_cleanup": next(r for r in records if r.get("kind") == 3)["values"][0][0] = .1
                elif case == "nonzero_stop": row(records, "send", 14)["values"][1][2] = .1
                self.reject(records)

    def test_loss_and_category_counts_cannot_be_ignored(self):
        for key in ("failures", "io_rc", "rejected", "dropped", "trace_lost", "stream_rejected", "stream_dropped",
                    "stale_frames", "discontinuities", "desync_rejected", "superseded_samples", "dropped_frames", "joins"):
            with self.subTest(counter=key):
                records = fixture()
                row(records, "joined")[key] += 1
                self.reject(records)
        records = fixture()
        row(records, "command", 10)["unsupported"] = 0
        self.reject(records)
        for malformed in ([None], [], "not records"):
            self.reject(malformed)

    def test_failed_stops_cannot_keep_their_own_verified_flag(self):
        for case in ("cleanup_slot", "cleanup_rc", "stop_rc", "stop_wire"):
            with self.subTest(case=case):
                records = fixture()
                cleanup = next(r for r in records if r.get("kind") == 3)
                if case == "cleanup_slot": cleanup["slot"] = 1
                elif case == "cleanup_rc": cleanup["rc"] = 123
                elif case == "stop_rc": row(records, "send", 14)["rc"] = 123
                else: row(records, "command", 14)["wire"] = struct.pack("<BHHI", 1, 1, 0, 0).hex()
                report = verifier.analyze(records)
                self.assertFalse(report["software_path_verified"])
                self.assertFalse(report["zero_output_cleanup_verified"] if case.startswith("cleanup") else
                                 report["explicit_stop_verified"])

    def test_readbacks_are_distinct_from_submission(self):
        records = fixture("switch")
        for r in records:
            if r.get("stage") == "readback": r["rc"] = [123, 123]
        row(records, "stopped")["amplitudes"] = [.1, 0, 0, 0]
        report = verifier.analyze(records)
        self.assertTrue(report["software_path_verified"], report)
        self.assertTrue(report["ipc_submission_verified"])
        self.assertFalse(report["ipc_readback_verified"])
        self.assertFalse(report["stopped_ipc_readback_verified"])
        self.assertFalse(report["physical_fidelity_verified"])

    def test_failed_or_fake_host_collection_never_succeeds(self):
        for case in ("failed", "timeout", "switch_marker"):
            with self.subTest(case=case):
                records = fixture("switch" if case == "switch_marker" else "host-simulation")
                data = "\n".join("HAPTIC " + json.dumps(r) for r in records).encode()

                def run(argv, **kwargs):
                    if case == "timeout": raise subprocess.TimeoutExpired(argv, 10, output=data)
                    return subprocess.CompletedProcess(argv, 7 if case == "failed" else 0, data, b"")

                with patch.object(sys, "argv", ["checker", "--probe", "fake-never-executed"]), \
                        patch.object(verifier.subprocess, "run", side_effect=run), \
                        contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as error:
                    verifier.main()
                self.assertEqual(error.exception.code, 1)


if __name__ == "__main__":
    unittest.main()
