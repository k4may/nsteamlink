"""Check the actual self-contained fixture/logger against the verifier on a host.

HID readbacks in this run are simulated, never real system IPC or motor proof.
An optional --reference runs the independently compiled decoder instead of
the handwritten CI golden table. No Switch connection or deployment occurs.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
from unittest.mock import patch
from test_haptics_verifier import verifier, reference_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    run = subprocess.run([str(args.probe.resolve())], capture_output=True, check=True, timeout=10)
    if args.log:
        args.log.write_bytes(run.stdout)
    rows = [json.loads(m[1]) for line in run.stdout.decode().splitlines()
            if (m := re.search(r"HAPTIC (\{.*\})", line))]
    if args.reference:
        report = verifier.analyze(rows, args.reference)
    else:
        with patch.object(verifier.subprocess, "run", side_effect=reference_run):
            report = verifier.analyze(rows, Path("handwritten-ci-reference"))
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    assert report["software_path_verified"], report
    assert report["independent_reference_values_match"] and report["native_samples"] == 37
    assert report["explicit_stop_verified"] and report["zero_output_cleanup_verified"]
    assert report["platform"] == "host-simulation"
    assert not report["ipc_readback_verified"]
    assert not report["stopped_ipc_readback_verified"]
    assert not report["physical_fidelity_verified"] and not report["loader_return_verified"]
    # Host scheduling/readback windows are still reported, not treated as a
    # portable real-time guarantee or used to certify the simulated HID API.


if __name__ == "__main__":
    main()
