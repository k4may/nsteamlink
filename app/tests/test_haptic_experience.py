#!/usr/bin/env python3
"""Execute real SDK/worker stimuli with a simulated HID terminal.

Verify independently that the intended distinctions reach the output, rather
than merely checking a demo's success flag. This does not certify physical feel.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess


def verify(data):
    records = [json.loads(line[7:]) for line in data.splitlines() if line.startswith("HAPTIC ")]
    checker_path = Path(__file__).resolve().parents[2] / "tools/check-feedback-matrix.py"
    spec = importlib.util.spec_from_file_location("matrix", checker_path)
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    matrix = checker.analyze(records)
    assert matrix["software_path_verified"], matrix
    groups = []
    for r in records:
        if r["stage"] == "demo-scene":
            groups.append(dict(note=r, frames=[], stops=[]))
        elif groups and r["stage"] == "demo-send":
            assert r["scene"] == groups[-1]["note"]["scene"]
            groups[-1]["frames"].append(r)
        elif groups and r["stage"] == "demo-stopped":
            assert r["scene"] == groups[-1]["note"]["scene"]
            groups[-1]["stops"].append(r)
    cases = [g["note"] for g in groups]
    assert [r["scene"] for r in cases[:7]] == list(range(1, 8))
    assert all(r["ok"] and r["joined"] and not r["trace_lost"] and
               0 <= r["commands"] <= 200 and r["skipped_steps"] <= 20 and
               (r["interrupted"] or r["commands"] >= 180) for r in cases), cases
    assert all(not r["interrupted"] for r in cases[:7])
    stops = [g["stops"][0] for g in groups if len(g["stops"]) == 1]
    assert len(stops) == len(groups) and all(r["rc"] == [0, 0] and r["amplitudes"] == [0]*4 and r["settled"] for r in stops)
    # The host terminal delays stop visibility by 40ms. Source six has no
    # trailing authored silence and must wait instead of aborting scene seven.
    if matrix["platform"] == "host-simulation":
        assert stops[5]["samples"] > 1 and stops[5]["after_join_ns"] >= 35000000, stops[5]
    outputs = {g["note"]["scene"]: g["frames"] for g in groups[:7]}
    # Replays and early navigation are legitimate; retain their separate stop /
    # join evidence instead of merging repetitions into a single monotonic ramp.
    for g in groups:
        frames = g["frames"]
        assert frames and all(r["rc"] == 0 for r in frames)
        if not g["note"]["interrupted"]:
            assert len(frames) >= 180
        assert all(a["sent_ns"] <= b["sent_ns"] for a, b in zip(frames, frames[1:]))
        assert frames[-1]["kind"] == 3 and all(v[0] == v[2] == 0 for v in frames[-1]["values"])
    # Linear numerical ramp really has many amplitude levels, rises then falls.
    levels = [r["values"][0][0] for r in outputs[2] if r["kind"] == 2]
    assert len(set(round(x, 5) for x in levels)) >= 90
    peak = max(range(len(levels)), key=levels.__getitem__)
    assert 70 < peak < 130 and levels[0] < .02 and levels[-1] < .02
    assert abs(levels[peak] - .5571934 * .85) < .01
    frequencies = {round(v[b+1]) for r in outputs[3] for v in r["values"] for b in (0, 2) if v[b]}
    assert frequencies == {80, 160, 320, 640}, frequencies
    # Position is genuinely lateral, independent of ordinary layout rendering.
    positions = {tuple(any(v[b] for b in (0, 2)) for v in r["values"]) for r in outputs[4]}
    assert positions == {(True, False), (False, True), (False, False)}, positions
    runs = []
    start = None
    for r in outputs[5]:
        active = any(v[0] or v[2] for v in r["values"])
        if active and start is None:
            start = r["sent_ns"]
        elif not active and start is not None:
            runs.append((r["sent_ns"] - start) / 1e6)
            start = None
    assert len(runs) == 8 and sum(25 <= d <= 80 for d in runs) == 6 and sum(550 <= d <= 660 for d in runs) == 2, runs
    engine = [r["values"][0][1] for r in outputs[6] if r["values"][0][0]]
    assert len(set(engine)) >= 60 and all(a <= b for a, b in zip(engine, engine[1:]))
    assert min(engine) <= 81 and max(engine) >= 300
    pairs = {tuple(round(v[b+1]) for b in (0, 2)) for r in outputs[7] for v in r["values"] if v[0] and v[2]}
    assert pairs == {(80, 640), (160, 320)}, pairs
    end = [r for r in records if r["stage"] == "demo-end"]
    assert len(end) == 1 and end[0]["completed_scenes"] == sum(not r["interrupted"] for r in cases) and end[0]["ok"]
    print(json.dumps(dict(software_path_verified=True, scenes=len(groups),
                         completed_scenes=end[0]["completed_scenes"],
                         interrupted_scenes=sum(r["interrupted"] for r in cases),
                         physical_fidelity_verified=False, game_source_verified=False)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    if args.probe:
        run = subprocess.run([str(args.probe.resolve())], capture_output=True, text=True, timeout=50)
        assert run.returncode == 0, run.stdout + run.stderr
        verify(run.stdout)
    elif args.log:
        verify(args.log.read_text())
    else:
        parser.error("--probe or --log required")
