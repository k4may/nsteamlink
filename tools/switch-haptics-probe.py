#!/usr/bin/env python3
"""Deploy a self-contained NRO using nxlink, then verify its recorded trace.

Never opens an empty TCP connection to netloader. No SD files are required.
Reads a separate pinned reference executable from check-switch-haptics.py.
Reports packet/value/timing/IPC evidence separately from physical motor fidelity
and loader return, which this probe cannot observe.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess


def close_values(a, b):
    return len(a) == len(b) and all(
        math.isclose(x, y, rel_tol=1e-5, abs_tol=1e-6)
        for x, y in zip(a, b))


def analyze(records, reference):
    errors = []
    stages = {r.get("stage"): r for r in records if r.get("stage") in
              ("begin", "device", "joined", "stopped", "end")}
    for required in ("begin", "device", "joined", "stopped", "end"):
        if required not in stages:
            errors.append(f"missing {required} stage")
    commands = [r for r in records if r.get("stage") == "command"]
    for command in commands:
        wire = bytes.fromhex(command["wire"])
        if not wire or (wire[0] == 13 and
                        (len(wire) != 69 or wire[1:6] != bytes([10, 0, 0, 0, 16]))):
            raise ValueError("Malformed recorded native command")
    sends = [r for r in records if r.get("stage") == "send" and r["kind"] == 2]
    reads = [r for r in records if r.get("stage") == "readback"]
    all_sends = [r for r in records if r.get("stage") == "send"]
    native = [r for r in commands if bytes.fromhex(r["wire"])[0] == 13]
    if len(native) != 13:
        errors.append(f"expected 13 native fixture commands, received {len(native)}")
    if len(commands) != 14 or [r["seq"] for r in commands] != list(range(1, 15)):
        errors.append("missing, duplicated or reordered fixture commands")
    if any(r.get("rc", 1) or r.get("slot", -1) != 0 for r in commands):
        errors.append("fixture command rejected or routed to the wrong device")
    device = stages.get("device", {})
    if device.get("activate", 1) or device.get("permission_rc", 1):
        errors.append("device activation/permission query failed")
    if not device.get("permitted", False):
        errors.append("system vibration permission is disabled")
    if device.get("devices") != 2:
        errors.append("two actuators are required to verify both fixture sides")
    expected_order = [(r["seq"], i) for r in native for i in range(3 if r["seq"] <= 12 else 1)] + [(14, 0)]
    if [(r["seq"], r["sample"]) for r in sends] != expected_order:
        errors.append("missing, duplicated or reordered output across fixture packets")
    for previous, frame in zip(all_sends, all_sends[1:]):
        if frame["sent_ns"] < previous["completed_ns"]:
            errors.append("native output timestamps are not serial")
            break
    expected = {}
    for motor in range(2):
        payload = b"".join(bytes.fromhex(r["wire"])[7+motor*4:11+motor*4] for r in native)
        rows = subprocess.run([str(reference.resolve())], input=payload,
                              capture_output=True, check=True).stdout.splitlines()
        if len(rows) != len(native):
            raise ValueError("Independent reference omitted records")
        held = [0, 160, 0, 320]
        for command, line in zip(native, rows):
            fields = list(map(float, line.split()))
            count = int(fields[0])
            values = [fields[i:i+4] for i in range(1, len(fields), 4)]
            if len(values) != count:
                raise ValueError("Invalid independent reference result")
            expected.setdefault(command["seq"], [None, None])[motor] = values or [held]
            if values:
                held = values[-1]
    jitter, late, source_errors, matched, outputs = [], [], [], 0, 0
    playback_origin = None
    values_ok = len(native) == 13
    devices = stages.get("device", {}).get("devices", 2)
    if devices not in (1, 2):
        raise ValueError("Invalid actuator count")
    style = stages.get("device", {}).get("style", 0)
    for command in native:
        seq = command["seq"]
        bands = expected[seq]
        frames = [r for r in sends if r["seq"] == seq]
        count = max(len(bands[0]), len(bands[1]))
        if len(frames) != count:
            values_ok = False
            errors.append(f"sequence {seq}: {len(frames)} outputs for {count} samples")
            continue
        for index, frame in enumerate(frames):
            outputs += 1
            wanted = [bands[m][min(index, len(bands[m])-1)] for m in range(2)]
            if frame["sample"] != index or frame["rc"] or frame.get("slot") != 0 or any(
                    not close_values(frame["values"][m], wanted[m]) for m in range(2)):
                errors.append(f"sequence {seq} sample {index}: parameter/order mismatch")
                values_ok = False
            if not command["received_ns"] <= command["apply_ns"] <= frame["sent_ns"] <= frame["completed_ns"]:
                errors.append(f"sequence {seq} sample {index}: invalid timestamp order")
            late.append(max(0, frame["sent_ns"] - frame["due_ns"]))
            if playback_origin is None:
                playback_origin = frame["sent_ns"] - command["received_ns"]
            expected_time = command["received_ns"] + playback_origin + index * 5000000
            source_errors.append(abs(frame["sent_ns"] - expected_time))
            if index:
                interval = frame["sent_ns"] - frames[index-1]["sent_ns"]
                jitter.append(abs(interval - 5000000))
            # A readback cannot certify a sample after ANY newer output,
            # including a different packet or the ordinary stop.
            end = min([r["sent_ns"] for r in all_sends if r["sent_ns"] > frame["sent_ns"]] +
                      [frame["sent_ns"] + 5000000])
            physical_values = [wanted[1] if devices == 1 and style & 16 else wanted[0]]
            if devices == 2:
                physical_values.append(wanted[1])
            if any(frame["completed_ns"] <= r["ns"] <= r["end_ns"] < end and
                   all(r["rc"][m] == 0 and close_values(r["values"][m], physical_values[m])
                       for m in range(devices)) for r in reads):
                matched += 1
    joined = stages.get("joined", {})
    counters_ok = joined.get("starts") == joined.get("joins") == 1 and not joined.get("running", True)
    counters_ok &= all(joined.get(key, 1) == 0 for key in
                       ("failures", "io_rc", "rejected", "dropped", "trace_lost", "stream_rejected", "stream_dropped"))
    counters_ok &= joined.get("decoded") == joined.get("scheduled") == joined.get("emitted") == 37
    counters_ok &= joined.get("queued", 0) == joined.get("applied", -1) == len(commands) == 14
    counters_ok &= joined.get("sends", -1) == len(all_sends)
    if not counters_ok:
        errors.append("worker, queue or trace counters are incomplete or report failures")
    cleanup = [r for r in records if r.get("stage") == "send" and r["kind"] == 3]
    cleanup_ok = bool(cleanup) and all(
        r["rc"] == 0 and all(v[0] == 0 and v[2] == 0 for v in r["values"])
        for r in cleanup)
    if not cleanup_ok:
        errors.append("missing or failed native zero-output cleanup")
    stop = [r for r in sends if r["seq"] == 14]
    stop_commands = [r for r in commands if r["seq"] == 14]
    stop_ok = len(stop) == len(stop_commands) == 1
    if stop_ok:
        wire = bytes.fromhex(stop_commands[0]["wire"])
        stop_ok = wire == bytes([1]) + bytes(len(wire)-1) and len(wire) in (9, 21)
        stop_ok &= stop[0]["rc"] == 0 and all(v[0] == v[2] == 0 for v in stop[0]["values"])
        stop_ok &= stop_commands[0]["received_ns"] <= stop_commands[0]["apply_ns"] <= stop[0]["sent_ns"]
    if not stop_ok:
        errors.append("missing or failed explicit ordinary zero-output command")
    stopped = stages.get("stopped", {})
    stopped_ok = len(stopped.get("amplitudes", [])) == 4 and all(
        a == 0 for a in stopped["amplitudes"]) and stopped.get("rc") == [0, 0]
    if not stopped_ok:
        errors.append("missing or nonzero system parameter readback after worker join")
    software_ok = not errors and counters_ok and stages.get("end", {}).get("software_ok", False)
    return {
        "software_path_verified": software_ok,
        "native_commands": len(native), "native_samples": outputs,
        "independent_reference_values_match": bool(outputs) and values_ok,
        "max_interval_error_ns": max(jitter, default=0),
        "max_submission_lateness_ns": max(late, default=0),
        "max_source_timeline_error_ns": max(source_errors, default=0),
        "sample_timing_within_500us": bool(jitter) and max(jitter) <= 500000 and
                                       bool(source_errors) and max(source_errors) <= 500000,
        "ipc_readback_matched_samples": matched,
        "ipc_readback_verified": bool(outputs) and values_ok and matched == outputs,
        "zero_output_cleanup_verified": cleanup_ok,
        "explicit_stop_verified": stop_ok,
        "stopped_ipc_readback_verified": stopped_ok,
        "vibration_permitted": stages.get("device", {}).get("permitted", False),
        "physical_fidelity_verified": False,
        "loader_return_verified": False,
        "errors": errors,
        "boundary": "Original DeviceWrite/native worker/HID IPC trace and parameter readbacks; "
                    "no proof of electrical/mechanical response or hbmenu screen state",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nro", type=Path)
    parser.add_argument("--address", default="10.10.10.17")
    parser.add_argument("--log", type=Path, default=Path("build/haptics-device.log"))
    parser.add_argument("--reference", type=Path, default=Path("build/haptic-reference/reference"))
    parser.add_argument("--output", type=Path, default=Path("build/haptics-device.json"))
    args = parser.parse_args()
    if not args.reference.is_file():
        raise SystemExit("Run tools/check-switch-haptics.py to prepare the independent reference first")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    collection = None
    if args.nro:
        nxlink = shutil.which("nxlink") or "/opt/devkitpro/tools/bin/nxlink"
        try:
            run = subprocess.run([nxlink, "-a", args.address, "-r", "1", "-s", str(args.nro.resolve())],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
            args.log.write_bytes(run.stdout)
            collection = {"returncode": run.returncode, "timed_out": False}
        except subprocess.TimeoutExpired as error:
            args.log.write_bytes(error.stdout or b"")
            print("nxlink collection timed out; analyzing available records")
            collection = {"returncode": None, "timed_out": True}
    try:
        records = []
        for line in args.log.read_text(errors="replace").splitlines():
            match = re.search(r"HAPTIC (\{.*\})", line)
            if match:
                records.append(json.loads(match[1]))
        report = analyze(records, args.reference)
    except (KeyError, ValueError, IndexError, TypeError, subprocess.SubprocessError) as error:
        report = {"software_path_verified": False, "physical_fidelity_verified": False,
                  "loader_return_verified": False, "errors": [str(error)]}
    if collection is not None:
        report["collection"] = collection
        report["collection_complete"] = not collection["timed_out"] and collection["returncode"] == 0
        report["nro_sha256"] = hashlib.sha256(args.nro.read_bytes()).hexdigest()
    report["reference_sha256"] = hashlib.sha256(args.reference.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if collection and (collection["timed_out"] or collection["returncode"] != 0):
        raise SystemExit(1)
    if not report["software_path_verified"]:
        raise SystemExit(1)
    if not report["sample_timing_within_500us"] or not report["ipc_readback_verified"]:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
