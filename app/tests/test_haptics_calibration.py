"""Run the real calibration fixture offline; test diagnostic failure controls."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('calibration',ROOT/'tools/switch-haptics-calibration.py')
calibration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(calibration)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args = parser.parse_args()
    run = subprocess.run([str(args.probe.resolve())],capture_output=True,check=True,timeout=10)
    rows = [json.loads(m[1]) for line in run.stdout.decode().splitlines()
            if (m:=re.search(r'HCAL (\{.*\})',line))]
    report = calibration.analyze(rows)
    assert report['diagnostic_complete'],report
    assert report['platform'] == 'host-simulation'
    assert report['steady_public_201_206_and_worker_match']
    assert not report['strict_5ms_fidelity_verified'] and not report['physical_fidelity_verified']
    for stage in ('event','requested','worker','end','readback'):
        bad = copy.deepcopy(rows)
        bad.remove(next(r for r in bad if r['stage']==stage and (stage!='readback' or r['route']==0)))
        assert not calibration.analyze(bad)['diagnostic_complete'],stage
    # A system transform can be diagnosed consistently across routes without
    # claiming equality to requested values or strict native fidelity.
    transformed = copy.deepcopy(rows)
    for r in transformed:
        if r['stage']=='readback':
            for v in r['values']:
                if v[0] == .0441941731:
                    v[0] = .0515766852
    report = calibration.analyze(transformed)
    assert report['diagnostic_complete'] and report['steady_public_201_206_and_worker_match']
    assert not all(o['steady_matches_requested'] for o in report['observations'])
    assert not report['strict_5ms_fidelity_verified']
    print('PASS actual calibration fixture, public/private parameter equivalence, cleanup and negative controls; host simulation only')


if __name__ == '__main__':
    main()
