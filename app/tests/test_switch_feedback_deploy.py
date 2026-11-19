"""Deployment/collection guards for the real matrix checker; no device calls."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("feedback_deploy", ROOT / "tools/switch-feedback-probe.py")
deploy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(deploy)
checker = deploy.load_checker()


def fixture(platform="switch"):
    """Synthetic transport log; the production checker still validates it.

    Values come from the matrix's independent reference, not the renderer.
    This fixture tests uploader guards, not hardware or the numerical oracle.
    """
    records = [dict(stage="begin", platform=platform, matrix=True, physical_fidelity_verified=False),
               dict(stage="device", activate=0, permission_rc=0, permitted=True, devices=2, style=1)]
    commands, cases, sends, reads = [], [], [], []
    expected = checker.expected_outputs(1)
    for index, (name, first, last) in enumerate(checker.CASES):
        start = 1_000_000_000 + index * 100_000_000
        cases.append(dict(stage="matrix-case", name=name, first_seq=first, last_seq=last,
                          begin_ns=start, end_ns=start+95_000_000, write_ok=True))
        for seq in range(first, last+1):
            received = start + (1_000_000 if seq != 2 else 41_000_000)
            applied = start + (41_000_000 if index == 0 else 2_000_000)
            commands.append(dict(stage="command", slot=0, seq=seq, received_ns=received,
                                 apply_ns=applied, rc=0, unsupported={10:4, 11:2}.get(seq, 0),
                                 source=2 if seq in (4, 5, 6, 8) else 3 if seq in (9, 10, 11) else
                                        1 if seq in (12, 13) else 0,
                                 wire=checker.fixture_commands()[seq-1].hex()))
            for _, sample, source, values in [row for row in expected if row[0] == seq]:
                sent = start + (42_000_000 if seq == 1 else 82_000_000 if seq == 2 else
                                3_000_000 + sample * 5_000_000)
                sends.append(dict(stage="send", kind=2, slot=0, seq=seq, sample=sample,
                                  due_ns=sent, sent_ns=sent, completed_ns=sent, rc=0,
                                  source=source, values=copy.deepcopy(values)))
                reads.append(dict(stage="readback", ns=sent+100_000, end_ns=sent+200_000,
                                  rc=[0, 0], values=copy.deepcopy(values)))
    cleanup_time = sends[-1]["sent_ns"] + 1_000_000
    sends.append(dict(stage="send", kind=3, slot=0, seq=0, sample=0, due_ns=cleanup_time,
                      sent_ns=cleanup_time, completed_ns=cleanup_time, rc=0, source=5,
                      values=[checker.ZERO.copy(), checker.ZERO.copy()]))
    joined = dict(stage="joined", starts=1, joins=1, running=False, queued=14, applied=14,
                  sends=len(sends), decoded=4, scheduled=4, emitted=4, ordinary_commands=5,
                  trigger_commands=4, sony_commands=3, unsupported_effects=2, adapted_effects=7,
                  failures=0, io_rc=0, rejected=0, dropped=0, trace_lost=0, stream_rejected=0,
                  stream_dropped=0, stale_frames=0, discontinuities=0, desync_rejected=0,
                  superseded_samples=0, dropped_frames=0)
    records.extend(cases)
    records.extend([dict(stage="matrix-end", ok=True, cases=13, commands=14),
                    dict(stage="stopped", rc=[0, 0], amplitudes=[0, 0, 0, 0]), joined])
    records.extend(commands + sends + reads)
    records.append(dict(stage="end", software_ok=True))
    return records


def log_bytes(records):
    return b"nxlink progress \xff\n" + b"".join(
        ("HAPTIC " + json.dumps(record) + "\n").encode() for record in records)


class FeedbackDeployTests(unittest.TestCase):
    def run_tool(self, args, result=None, error=None):
        with contextlib.redirect_stdout(io.StringIO()), \
             patch.object(deploy.shutil, "which", return_value="/fake/nxlink"), \
             patch.object(deploy.subprocess, "run", return_value=result, side_effect=error) as run, \
             patch("socket.socket", side_effect=AssertionError("Python socket use is forbidden")), \
             patch("socket.create_connection", side_effect=AssertionError("TCP probe is forbidden")), \
             patch("socket.getaddrinfo", side_effect=AssertionError("address discovery is forbidden")):
            code = deploy.main(args)
        return code, run

    def test_deploy_uses_only_nxlink_and_preserves_exact_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            nro, output = folder / "matrix.nro", folder / "result.json"
            nro.write_bytes(b"self-contained NRO fixture")
            data = log_bytes(fixture())
            code, run = self.run_tool(["--ip", "10.10.10.111", "--nro", str(nro), "--output", str(output)],
                                      subprocess.CompletedProcess([], 0, stdout=data))
            self.assertEqual(code, 0)
            run.assert_called_once()
            command = run.call_args.args[0]
            self.assertEqual(command, ["/fake/nxlink", "-a", "10.10.10.111", "-r", "1", "-s", str(nro)])
            self.assertLessEqual(run.call_args.kwargs["timeout"], 30)
            self.assertEqual(output.with_suffix(".log").read_bytes(), data)
            report = json.loads(output.read_text())
            metadata = json.loads(output.with_suffix(".metadata.json").read_text())
            self.assertTrue(report["collection_complete"] and report["cleanup_evidence_complete"])
            self.assertTrue(report["ipc_submission_verified"])
            self.assertFalse(report["physical_fidelity_verified"] or report["loader_return_verified"])
            self.assertEqual(metadata["command"], command)
            self.assertEqual(metadata["nro_sha256"], hashlib.sha256(nro.read_bytes()).hexdigest())
            self.assertEqual(metadata["captured_log_sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(metadata["checker_sha256"], hashlib.sha256(deploy.CHECKER.read_bytes()).hexdigest())

    def test_failed_or_timed_out_nxlink_never_certifies_even_a_complete_log(self):
        for mode in ("exit", "timeout"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                folder = Path(directory)
                nro, output = folder / "matrix.nro", folder / "result.json"
                nro.write_bytes(b"NRO")
                data = log_bytes(fixture())
                result = subprocess.CompletedProcess([], 1, stdout=data) if mode == "exit" else None
                error = subprocess.TimeoutExpired(["nxlink"], 30, output=data) if mode == "timeout" else None
                code, _ = self.run_tool(["--ip", "10.10.10.111", "--nro", str(nro), "--output", str(output)],
                                        result, error)
                report = json.loads(output.read_text())
                self.assertEqual(code, 1)
                self.assertFalse(report["collection_complete"] or report["software_path_verified"] or
                                 report["ipc_submission_verified"] or report["cleanup_evidence_complete"])
                self.assertEqual(output.with_suffix(".log").read_bytes(), data)
                self.assertEqual(report["collection"]["timed_out"], mode == "timeout")

    def test_successful_exit_requires_real_matrix_and_full_cleanup(self):
        for mode in ("joined", "cleanup", "end", "stopped", "old-fixture", "simulation"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                folder = Path(directory)
                nro, output = folder / "matrix.nro", folder / "result.json"
                nro.write_bytes(b"NRO")
                records = fixture("host-simulation" if mode == "simulation" else "switch")
                if mode in ("joined", "end"):
                    records = [r for r in records if r["stage"] != mode]
                elif mode == "cleanup":
                    records = [r for r in records if r.get("kind") != 3]
                elif mode == "old-fixture":
                    records[0]["matrix"] = False
                elif mode == "stopped":
                    next(r for r in records if r["stage"] == "stopped")["amplitudes"][0] = 0.5
                code, _ = self.run_tool(["--ip", "10.10.10.111", "--nro", str(nro), "--output", str(output)],
                                        subprocess.CompletedProcess([], 0, stdout=log_bytes(records)))
                report = json.loads(output.read_text())
                self.assertEqual(code, 1)
                self.assertFalse(report["collection_complete"] or report["cleanup_evidence_complete"])

    def test_offline_has_no_subprocess_or_network_and_preserves_simulation_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source, output = folder / "original.log", folder / "result.json"
            data = log_bytes(fixture("host-simulation"))
            source.write_bytes(data)
            code, run = self.run_tool(["--ip", "10.10.10.111", "--log", str(source), "--output", str(output)])
            run.assert_not_called()
            report = json.loads(output.read_text())
            self.assertEqual(code, 0)
            self.assertTrue(report["software_path_verified"] and report["cleanup_evidence_complete"])
            self.assertFalse(report["ipc_submission_verified"] or report["ipc_readback_verified"] or
                             report["loader_return_verified"])
            self.assertEqual(source.read_bytes(), data)
            self.assertEqual(output.with_suffix(".log").read_bytes(), data)
            self.assertIsNone(report["collection"]["command"])

    def test_missing_nro_and_malformed_log_are_saved_as_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            output = folder / "result.json"
            code, run = self.run_tool(["--ip", "10.10.10.111", "--nro", str(folder/"missing.nro"),
                                       "--output", str(output)])
            run.assert_not_called()
            self.assertEqual(code, 1)
            self.assertTrue(json.loads(output.read_text())["collection"]["collection_error"])
            source = folder / "bad.log"
            source.write_bytes(b"HAPTIC {invalid json}\n")
            code, run = self.run_tool(["--ip", "10.10.10.111", "--log", str(source), "--output", str(output)])
            run.assert_not_called()
            self.assertEqual(code, 1)
            self.assertEqual(output.with_suffix(".log").read_bytes(), source.read_bytes())

    def test_cli_requires_explicit_ip_exclusive_source_and_bounded_timeout(self):
        for args in (["--log", "fixture.log"],
                     ["--ip", "10.10.10.111", "--nro", "a", "--log", "b"],
                     ["--ip", "switch.local", "--log", "fixture.log"],
                     ["--ip", "10.10.10.111", "--log", "fixture.log", "--timeout", "31"],
                     ["--ip", "10.10.10.111", "--log", "fixture.log", "--timeout", "nan"]):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()), \
                 patch.object(deploy.subprocess, "run") as run:
                with self.assertRaises(SystemExit):
                    deploy.main(args)
                run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
