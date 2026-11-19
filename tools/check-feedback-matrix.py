#!/usr/bin/env python3
"""Verify the fixed, self-contained feedback matrix without trusting its OK flag.

Inputs are the probe's machine-readable command/output/readback/cleanup stages.
Host simulations verify software only; even a Switch run cannot establish
mechanical fidelity or the hbmenu screen state. No device deployment occurs.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess

PEAK = 0.5571934
ZERO = [0.0, 160.0, 0.0, 320.0]
CASES = [
    ("ordinary-batch", 1, 2), ("body-start", 3, 3),
    ("trigger-start", 4, 4), ("trigger-stop", 5, 5),
    ("trigger-restart", 6, 6), ("body-stop", 7, 7),
    ("trigger-final-stop", 8, 8), ("sony-legacy", 9, 9),
    ("sony-led", 10, 10), ("sony-audio-handoff", 11, 11),
    ("nintendo-pulse", 12, 12), ("nintendo-bands", 13, 13),
    ("final-stop", 14, 14),
]


def fixture_commands():
    def ordinary(kind, low=0, high=0):
        # SDK queues the canonical 9-byte RumbleCommand from either a 9-byte
        # or 21-byte input envelope. The trace cannot certify ignored padding.
        return bytes([kind]) + low.to_bytes(2, "little") + high.to_bytes(2, "little") + bytes(4)

    sony = bytearray([13, 47, 0, 0, 0] + [0] * 64)
    sony[5], sony[7], sony[8] = 3, 64, 128
    led = bytearray(sony)
    led[6], led[49], led[50], led[51] = 4, 0xaa, 0x2a, 0x55

    def nintendo(left, right):
        return bytes([13, 10, 0, 0, 0, 16, 0]) + left.to_bytes(4, "little") + right.to_bytes(4, "little") + bytes(54)

    return [ordinary(1, 32768, 16384), ordinary(1), ordinary(1, 65535, 0),
            ordinary(6, 0, 32768), ordinary(6), ordinary(6, 16384, 0),
            ordinary(1), ordinary(6), bytes(sony), bytes(led),
            bytes([13, 47, 0, 0, 0] + [0] * 64),
            nintendo(0x40400100, 0xc0050000), nintendo(0x60208180, 0x60208180), ordinary(1)]


def uint(value, label, bits=64):
    if type(value) is not int or not 0 <= value < 1 << bits:
        raise ValueError(f"invalid {label}")


def values(array):
    if not isinstance(array, list) or len(array) != 2:
        raise ValueError("expected two logical actuator values")
    for motor in array:
        if not isinstance(motor, list) or len(motor) != 4 or any(
                type(n) not in (int, float) or not math.isfinite(n) for n in motor):
            raise ValueError("invalid actuator value")
        if not (0 <= motor[0] <= 1 and 0 <= motor[2] <= 1 and
                40 <= motor[1] <= 640 and 80 <= motor[3] <= 1280):
            raise ValueError("actuator parameter out of range")


def match(a, b):
    return len(a) == len(b) and all(len(left) == len(right) and
        all(math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-7) for x, y in zip(left, right))
        for left, right in zip(a, b))


def expected_outputs(style):
    """Independent numerical goldens; does not invoke the production renderer."""
    paired = bool(style & (2 | 4))  # libnx Handheld / JoyDual
    single = None if paired else 0 if style & 8 else 1 if style & 16 else None

    def render(low, high):
        both = [low / 65535 * PEAK, 160, high / 65535 * PEAK, 320]
        if paired:
            return [[both[0], 160, 0, 320], [0, 160, both[2], 320]]
        if single is not None:
            return [both if i == single else ZERO.copy() for i in range(2)]
        return [both.copy(), both.copy()]

    def compose(body, left=0, right=0):
        result = [v.copy() for v in body]
        trigger = [left, right]
        if single is not None:
            trigger[single], trigger[1 - single] = max(left, right), 0
        for i in range(2):
            result[i][2] = max(result[i][2], trigger[i] / 65535 * PEAK)
        return result

    quiet = [ZERO.copy(), ZERO.copy()]
    body = render(65535, 0)
    sony = render(128 * 257, 64 * 257)
    states = [render(32768, 16384), quiet, body, compose(body, right=32768), body,
              compose(body, left=16384), compose(quiet, left=16384), quiet,
              sony, sony, quiet]
    result = [(i + 1, 0, 2 if i + 1 in (4, 5, 6, 8) else 3 if i + 1 in (9, 10, 11) else 0, v)
              for i, v in enumerate(states)]
    result.extend([(12, 0, 1, quiet),
                   (12, 1, 1, [ZERO.copy(), [0.0441941738, 160, 0, 320]]),
                   (12, 2, 1, quiet),
                   (13, 0, 1, [[0.255474287, 80, 0.255474287, 640]] * 2),
                   (14, 0, 0, quiet)])
    return result


def analyze(records):
    try:
        return _analyze(records)
    except (KeyError, ValueError, IndexError, TypeError, OverflowError) as error:
        return dict(software_path_verified=False, ipc_submission_verified=False,
                    ipc_readback_verified=False, stopped_ipc_readback_verified=False,
                    physical_fidelity_verified=False, loader_return_verified=False,
                    errors=[str(error)])


def _analyze(records):
    if not isinstance(records, list) or any(not isinstance(r, dict) for r in records):
        raise ValueError("probe records must be objects")
    errors = []

    def one(stage):
        found = [r for r in records if r.get("stage") == stage]
        if len(found) != 1:
            raise ValueError(f"expected exactly one {stage} stage")
        return found[0]

    begin, device, joined, stopped, end, matrix_end = [one(s) for s in
        ("begin", "device", "joined", "stopped", "end", "matrix-end")]
    if records[0] is not begin or records[-1] is not end:
        errors.append("begin/end do not delimit the complete capture")
    platform = begin.get("platform")
    if platform not in ("host-simulation", "switch") or begin.get("matrix") is not True:
        errors.append("missing matrix=true begin or unknown platform")
    if begin.get("physical_fidelity_verified") is not False:
        errors.append("begin must not claim physical fidelity")
    if end.get("software_ok") is not True or matrix_end.get("ok") is not True:
        errors.append("fixture did not complete successfully")
    if matrix_end.get("cases") != 13 or matrix_end.get("commands") != 14:
        errors.append("incomplete matrix case/command count")
    for key in ("activate", "permission_rc", "devices", "style"):
        uint(device.get(key), f"device {key}", 32)
    if device["activate"] or device["permission_rc"] or device.get("permitted") is not True:
        errors.append("device activation or vibration permission failed")
    devices, style = device["devices"], device["style"]
    if devices not in (1, 2) or not style & 31:
        raise ValueError("invalid active actuator count/style")
    if devices != (2 if style & 7 else 1):
        errors.append("actuator count disagrees with controller style")

    commands = [r for r in records if r.get("stage") == "command"]
    wanted_commands = fixture_commands()
    if len(commands) != 14:
        raise ValueError("matrix requires exactly 14 fixed commands")
    stimulus_match = True
    for seq, (command, wire) in enumerate(zip(commands, wanted_commands), 1):
        for key in ("slot", "seq", "received_ns", "apply_ns", "rc", "source", "unsupported"):
            uint(command.get(key), f"command {key}")
        if command["seq"] != seq or command["slot"] != 0 or command["rc"]:
            errors.append(f"command {seq}: sequence, slot or acceptance mismatch")
            stimulus_match = False
        if not command["received_ns"] or command["received_ns"] > command["apply_ns"]:
            errors.append(f"command {seq}: invalid receive/apply timestamps")
        if bytes.fromhex(command["wire"]) != wire:
            errors.append(f"command {seq}: differs from the fixed matrix stimulus")
            stimulus_match = False
        if command["unsupported"] != ({10: 4, 11: 2}.get(seq, 0)):
            errors.append(f"command {seq}: unexpected unsupported-category flags")
        source = 2 if seq in (4, 5, 6, 8) else 3 if seq in (9, 10, 11) else 1 if seq in (12, 13) else 0
        if command["source"] != source:
            errors.append(f"command {seq}: wrong source classification")
    if any(a["received_ns"] > b["received_ns"] or a["apply_ns"] > b["apply_ns"]
           for a, b in zip(commands, commands[1:])):
        errors.append("command timestamps are reordered")

    cases = [r for r in records if r.get("stage") == "matrix-case"]
    if len(cases) != len(CASES):
        raise ValueError("missing or duplicated matrix case")
    if not records.index(device) < records.index(cases[0]) <= records.index(cases[-1]) < \
            records.index(matrix_end) < records.index(stopped) < records.index(joined):
        errors.append("matrix/cleanup stages are reordered")
    for case, (name, first, last) in zip(cases, CASES):
        for key in ("first_seq", "last_seq", "begin_ns", "end_ns"):
            uint(case.get(key), f"case {key}")
        if (case.get("name"), case["first_seq"], case["last_seq"]) != (name, first, last):
            errors.append("matrix case order or command range mismatch")
        if case.get("write_ok") is not True or case["begin_ns"] > case["end_ns"]:
            errors.append(f"case {name}: failed write or timestamp range")
        for command in commands[first-1:last]:
            if not case["begin_ns"] <= command["received_ns"] <= command["apply_ns"] <= case["end_ns"]:
                errors.append(f"case {name}: command outside its observation window")
    if any(a["end_ns"] > b["begin_ns"] for a, b in zip(cases, cases[1:])):
        errors.append("overlapping/reordered matrix observation windows")
    if cases[-1]["end_ns"] - cases[0]["begin_ns"] > 2_000_000_000:
        errors.append("matrix exceeded its two-second interactive budget")

    all_sends = [r for r in records if r.get("stage") == "send"]
    for send in all_sends:
        for key in ("kind", "slot", "seq", "sample", "due_ns", "sent_ns", "completed_ns", "rc", "source"):
            uint(send.get(key), f"send {key}")
        values(send.get("values"))
        if send["kind"] not in (2, 3) or send["slot"] != 0 or send["rc"]:
            errors.append("invalid/failed output kind or device slot")
        if not send["due_ns"] <= send["sent_ns"] <= send["completed_ns"]:
            errors.append("output deadline/submission/completion order is invalid")
    if any(a["completed_ns"] > b["sent_ns"] for a, b in zip(all_sends, all_sends[1:])):
        errors.append("output submissions overlap or are reordered")
    sends = [r for r in all_sends if r["kind"] == 2]
    expected = expected_outputs(style)
    output_match = len(sends) == len(expected)
    if not output_match:
        errors.append("missing/extra source output frames")
    for send, (seq, sample, source, wanted) in zip(sends, expected):
        if (send["seq"], send["sample"], send["source"]) != (seq, sample, source) or not match(send["values"], wanted):
            errors.append(f"sequence {seq} sample {sample}: wrong source, order or parameters")
            output_match = False
        command = commands[seq-1]
        case = next(c for c in cases if c["first_seq"] <= seq <= c["last_seq"])
        if command["apply_ns"] > send["due_ns"] or send["completed_ns"] > case["end_ns"]:
            errors.append(f"sequence {seq}: output outside its causal observation window")
    first = next((r for r in sends if r["seq"] == 1), None)
    second = next((r for r in sends if r["seq"] == 2), None)
    received_gap = commands[1]["received_ns"] - commands[0]["received_ns"]
    played_gap = second["sent_ns"] - first["sent_ns"] if first and second else 0
    interleaved = first is not None and first["completed_ns"] <= commands[1]["apply_ns"]
    batch_ok = first is not None and second is not None and received_gap >= 30_000_000 and \
        commands[0]["apply_ns"] >= commands[1]["received_ns"] and \
        interleaved and received_gap <= played_gap <= received_gap + 25_000_000
    if not batch_ok:
        errors.append("ordinary batch did not preserve its received on/off interval")
    pulse = [r for r in sends if r["seq"] == 12]
    native_spacing = len(pulse) == 3 and all(
        b["sent_ns"] - a["completed_ns"] >= 5_000_000 for a, b in zip(pulse, pulse[1:]))
    if not native_spacing:
        errors.append("Nintendo three-sample pulse was collapsed or shortened")

    counters = dict(starts=1, joins=1, queued=14, applied=14, sends=len(all_sends),
                    decoded=4, scheduled=4, emitted=4, ordinary_commands=5, trigger_commands=4, sony_commands=3,
                    unsupported_effects=2, adapted_effects=7, failures=0, io_rc=0, rejected=0,
                    dropped=0, trace_lost=0, stream_rejected=0, stream_dropped=0,
                    stale_frames=0, discontinuities=0, desync_rejected=0, superseded_samples=0, dropped_frames=0)
    for name, wanted in counters.items():
        uint(joined.get(name), f"joined {name}")
        if joined[name] != wanted:
            errors.append(f"joined {name}: expected {wanted}, got {joined[name]}")
    if joined.get("running") is not False:
        errors.append("worker was not joined")
    cleanup = [r for r in all_sends if r["kind"] == 3]
    cleanup_ok = len(cleanup) == 1 and cleanup[0]["slot"] == 0 and cleanup[0]["rc"] == 0 and \
        cleanup[0]["sent_ns"] <= cleanup[0]["completed_ns"] and match(cleanup[0]["values"], [ZERO, ZERO]) and \
        bool(sends) and cleanup[0]["sent_ns"] >= sends[-1]["completed_ns"]
    if not cleanup_ok:
        errors.append("missing, duplicated or nonzero native cleanup submission")
    explicit_stop = [r for r in sends if r["seq"] == 14]
    explicit_stop_ok = len(explicit_stop) == 1 and commands[13]["seq"] == 14 and \
        commands[13]["slot"] == 0 and commands[13]["rc"] == 0 and \
        bytes.fromhex(commands[13]["wire"]) == wanted_commands[13] and \
        explicit_stop[0]["slot"] == 0 and explicit_stop[0]["rc"] == 0 and \
        explicit_stop[0]["source"] == 0 and explicit_stop[0]["sample"] == 0 and \
        commands[13]["apply_ns"] <= explicit_stop[0]["due_ns"] <= explicit_stop[0]["sent_ns"] <= \
        explicit_stop[0]["completed_ns"] and match(explicit_stop[0]["values"], [ZERO, ZERO])
    if not isinstance(stopped.get("rc"), list) or len(stopped["rc"]) != 2 or \
            not isinstance(stopped.get("amplitudes"), list) or len(stopped["amplitudes"]) != 4:
        raise ValueError("malformed post-join readback")
    for result in stopped["rc"]:
        uint(result, "post-join readback result", 32)
    if any(type(a) not in (int, float) or not math.isfinite(a) for a in stopped["amplitudes"]):
        raise ValueError("malformed post-join amplitude")
    stopped_ok = stopped["rc"] == [0, 0] and stopped["amplitudes"] == [0, 0, 0, 0]

    reads = [r for r in records if r.get("stage") == "readback"]
    for read in reads:
        for name in ("ns", "end_ns"):
            uint(read.get(name), f"readback {name}")
        values(read.get("values"))
        if read["ns"] > read["end_ns"] or not isinstance(read.get("rc"), list) or len(read["rc"]) != 2:
            raise ValueError("malformed readback timing/result array")
        for result in read["rc"]:
            uint(result, "readback result", 32)
    matched = 0
    for frame in sends:
        end_ns = min([s["sent_ns"] for s in all_sends if s["sent_ns"] > frame["sent_ns"]] + [2**64-1])
        physical = [frame["values"][1 if devices == 1 and style & 16 else 0]]
        if devices == 2:
            physical.append(frame["values"][1])
        if any(frame["completed_ns"] <= r["ns"] <= r["end_ns"] < end_ns and
               all(r["rc"][i] == 0 and match([r["values"][i]], [physical[i]]) for i in range(devices))
               for r in reads):
            matched += 1
    software_ok = not errors
    readback_ok = software_ok and matched == len(expected)
    return dict(software_path_verified=software_ok, platform=platform, matrix=True,
                commands=len(commands), output_frames=len(sends), native_samples=len(pulse)+1,
                all_stimuli_and_outputs_match=stimulus_match and output_match,
                batched_ordinary_interval_preserved=batch_ok, ordinary_received_interval_ns=received_gap,
                owner_worker_interleave_verified=interleaved,
                ordinary_after_completion_interval_ns=second["sent_ns"]-first["completed_ns"] if first and second else 0,
                ordinary_played_interval_ns=played_gap, native_sample_spacing_verified=native_spacing,
                ipc_submission_verified=software_ok and platform == "switch",
                parameter_readback_values_match=readback_ok, ipc_readback_matched_frames=matched,
                ipc_readback_verified=readback_ok and platform == "switch",
                stopped_parameter_readback_matches=stopped_ok,
                stopped_ipc_readback_verified=software_ok and stopped_ok and platform == "switch",
                explicit_stop_verified=explicit_stop_ok,
                zero_output_cleanup_verified=cleanup_ok, actuators=devices,
                max_submission_lateness_ns=max((r["sent_ns"]-r["due_ns"] for r in sends), default=0),
                physical_fidelity_verified=False, loader_return_verified=False, errors=errors,
                boundary=("HID terminals are simulated. " if platform == "host-simulation" else
                          "Switch IPC submissions and optional parameter readbacks. ") +
                         "No mechanical fidelity, real-game compatibility or hbmenu screen certification.")


def parse_records(text):
    records = []
    for line in text.splitlines():
        if "HAPTIC " in line:
            payload = line.split("HAPTIC ", 1)[1]
            records.append(json.loads(payload))
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--probe", type=Path, help="run a local host-simulation fixture")
    source.add_argument("--log", type=Path, help="analyze an already collected probe log")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    collection_ok = True
    try:
        if args.probe:
            try:
                run = subprocess.run([str(args.probe.resolve())], capture_output=True, timeout=10)
                text = run.stdout.decode(errors="replace")
                collection_ok = run.returncode == 0
            except subprocess.TimeoutExpired as error:
                text = (error.stdout or b"").decode(errors="replace")
                collection_ok = False
        else:
            text = args.log.read_text(errors="replace")
        report = analyze(parse_records(text))
        if args.probe and report.get("platform") != "host-simulation":
            collection_ok = False
        if not collection_ok:
            report["software_path_verified"] = False
            for flag in ("ipc_submission_verified", "ipc_readback_verified", "stopped_ipc_readback_verified"):
                report[flag] = False
            report["errors"].append("host fixture failed, timed out, or lacks its simulation marker")
    except (OSError, ValueError, TypeError) as error:
        report = analyze([])
        report["errors"].append(str(error))
    output = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end="")
    if not report["software_path_verified"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
