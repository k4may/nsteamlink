#!/usr/bin/env python3
"""Collect one self-contained comparison of public and private HID output.

This diagnoses readback transforms/latency. It never certifies 5ms sample or
physical fidelity, nor changes switch-haptics-probe.py acceptance thresholds.
No network access occurs without --nro; --address is mandatory for deployment.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess

ZERO = [0, 160, 0, 320]
PULSE = [.0441941731, 160, 0, 320]
BANDS = [.255474287, 80, .255474287, 640]
ACTIVE_WORDS = [(0x46c00100, 0x40400100), (0x40400100, 0x46c00100),
                (0x60208180, 0x60208180), (0x46c00100, 0x40400100),
                (0x40400100, 0x46c00100)]
ACTIVE_VALUES = [[PULSE, ZERO], [ZERO, PULSE], [BANDS, BANDS], [PULSE, ZERO], [ZERO, PULSE]]


def close(a, b):
    return len(a) == len(b) == 2 and all(len(x) == len(y) == 4 and all(
        math.isfinite(v) and math.isclose(v, w, rel_tol=1e-5, abs_tol=1e-6)
        for v, w in zip(x, y)) for x, y in zip(a, b))


def analyze(records):
    stages = {r.get('stage'): r for r in records if r.get('stage') in
              ('begin', 'device', 'initial_zero', 'joined', 'stopped', 'end')}
    errors, observations = [], []
    if set(stages) != {'begin', 'device', 'initial_zero', 'joined', 'stopped', 'end'}:
        errors.append('missing required stages')
    begin, device = stages.get('begin', {}), stages.get('device', {})
    platform = begin.get('platform')
    if platform not in ('switch', 'host-simulation') or begin.get('schema') != 1:
        errors.append('unknown platform/schema')
    if device.get('activate', 1) or device.get('permission_rc', 1) or not device.get('permitted'):
        errors.append('activation/permission failed')
    if stages.get('initial_zero', {}).get('rc', 1):
        errors.append('initial zero failed')
    events = [r for r in records if r.get('stage') == 'event']
    requested = [r for r in records if r.get('stage') == 'requested']
    reads = [r for r in records if r.get('stage') == 'readback']
    workers = [r for r in records if r.get('stage') == 'worker' and r.get('kind') == 2]
    worker_values = [r for r in records if r.get('stage') == 'worker_values']
    expected_order = [(route, trial, step) for route in range(3) for trial in range(5) for step in range(3)]
    if [(r['route'], r['trial'], r['step']) for r in events] != expected_order or len(requested) != 45:
        errors.append('missing/duplicated/reordered trial events')
    if len(workers) != 15 or [w['seq'] for w in workers] != list(range(1,16)):
        errors.append('missing/duplicated/reordered worker submissions')
    steady = {}
    for e in events:
        key = e['route'], e['trial'], e['step']
        route, trial, step = key
        words = ACTIVE_WORDS[trial] if step == 1 else (0x40400100,)*2
        wanted = ACTIVE_VALUES[trial] if step == 1 else [ZERO, ZERO]
        q = [r for r in requested if (r['route'], r['trial'], r['step']) == key]
        if e['word'] != list(words) or e['rc'] != [0,0] or e['begin_ns'] > e['end_ns'] or len(q) != 1 or not close(q[0]['values'], wanted):
            errors.append(f'trial {key}: stimulus/submit mismatch')
        start, completed = e['begin_ns'], e['end_ns']
        if route == 2:
            ws = [w for w in workers if w['seq'] == e['seq']]
            vs = [v for v in worker_values if v['step'] == e['seq']]
            if len(ws) != 1 or ws[0]['rc'] or len(vs) != 1 or not close(vs[0]['values'], wanted):
                errors.append(f'trial {key}: worker output differs')
                continue
            start, completed = ws[0]['begin_ns'], ws[0]['end_ns']
            if not e['begin_ns'] <= start <= completed:
                errors.append(f'trial {key}: noncausal worker time')
        rr = [r for r in reads if (r['route'], r['trial'], r['step']) == key and
              completed <= r['begin_ns'] <= r['end_ns']]
        if not rr or any(r['rc'] != [0,0] for r in rr):
            errors.append(f'trial {key}: missing/failed readbacks')
        if step != 1:
            continue
        unique = []
        for r in rr:
            if r['values'] not in [v['values'] for v in unique]:
                unique.append(dict(first_observed_after_submit_ns=r['begin_ns']-start, values=r['values']))
        tail = [r for r in rr if r['begin_ns'] >= completed + 40000000] if trial < 3 else []
        tail_stable = bool(tail) and all(close(r['values'],tail[-1]['values']) for r in tail)
        settled = tail[-1]['values'] if tail_stable else None
        steady[route, trial] = settled
        observations.append(dict(route=route, trial=trial, requested=wanted, readbacks=len(rr),
                                 distinct_readbacks=unique, steady_readback=settled,
                                 late_window_stable=tail_stable,
                                 steady_matches_requested=settled is not None and close(settled, wanted)))
    joined, end = stages.get('joined',{}), stages.get('end',{})
    if joined.get('starts') != 1 or joined.get('joins') != 1 or joined.get('running',True) or joined.get('emitted') != 15 or any(
            joined.get(k,1) for k in ('failures','io_rc','trace_lost','rejected','dropped','final_send_rc')):
        errors.append('worker/cleanup counters failed')
    stopped = stages.get('stopped', {})
    if stopped.get('rc') != [0,0] or not close(stopped.get('values',[]), [ZERO,ZERO]):
        errors.append('final zero readback failed')
    if not end.get('complete') or end.get('events') != 45 or end.get('readings') != len(reads) or end.get('interrupted',True) or end.get('overflowed',True):
        errors.append('incomplete collection')
    same = all(steady.get((0,t)) is not None and steady.get((1,t)) is not None and
               steady.get((2,t)) is not None and close(steady[0,t],steady[1,t]) and
               close(steady[1,t],steady[2,t]) for t in range(3))
    return dict(diagnostic_complete=not errors, platform=platform, observations=observations,
                steady_public_201_206_and_worker_match=same,
                strict_5ms_fidelity_verified=False, physical_fidelity_verified=False,
                loader_return_verified=False, errors=errors,
                boundary='Long-hold system readback/transport diagnosis; no substitute for native 5ms acceptance or physical measurements')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nro', type=Path)
    parser.add_argument('--address')
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.nro and not args.address:
        parser.error('--address is required with --nro')
    args.log.parent.mkdir(parents=True,exist_ok=True)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    collection = None
    if args.nro:
        nxlink = shutil.which('nxlink') or '/opt/devkitpro/tools/bin/nxlink'
        try:
            result = subprocess.run([nxlink,'-a',args.address,'-r','1','-s',str(args.nro.resolve())],
                                    stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=45)
            args.log.write_bytes(result.stdout)
            collection = dict(returncode=result.returncode,timed_out=False)
        except subprocess.TimeoutExpired as error:
            args.log.write_bytes(error.stdout or b'')
            collection = dict(returncode=None,timed_out=True)
    try:
        records = [json.loads(m[1]) for line in args.log.read_text(errors='replace').splitlines()
                   if (m:=re.search(r'HCAL (\{.*\})',line))]
        report = analyze(records)
    except (KeyError,ValueError,TypeError,IndexError) as error:
        report = dict(diagnostic_complete=False,errors=[str(error)],strict_5ms_fidelity_verified=False,
                      physical_fidelity_verified=False,loader_return_verified=False)
    if collection is not None:
        report['collection'] = collection
        report['nro_sha256'] = hashlib.sha256(args.nro.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if not report['diagnostic_complete'] or collection and (collection['timed_out'] or collection['returncode'] != 0):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
