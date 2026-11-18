#!/usr/bin/env python3
"""Separate host requests, received native effects and bracketed IPC correlation.

No physical-fidelity, game-compatibility or loader verdict is inferred. Host and
Switch clock values are never subtracted. Without per-request UDP brackets,
nonzero effects cannot be attributed to the test helper.
"""
import argparse
import json
import math
from pathlib import Path


NEUTRAL = bytes.fromhex('00014040')
PULSE = bytes.fromhex('000005c0')


def read_rows(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8-sig').splitlines()
            if line.startswith('{')]


def native_payload(row):
    wire = bytes.fromhex(row.get('wire', ''))
    return wire[5:15] if row.get('kind') == 1 and row.get('rc') == 0 and len(wire) == 69 and wire[:5] == bytes.fromhex('0d0a000000') and wire[5:7] == b'\x10\0' else None


def nonzero(row):
    data = native_payload(row)
    return data is not None and data[2:] != NEUTRAL * 2


def analyze(requests, capture=(), manifest=None):
    manifest = manifest or {}
    records, conflicts, stats, reply_errors = {}, [], [], []
    capture_stats = [r for r in capture if r.get('stage') == 'haptics']

    def collect(row):
        if 'serial' in row:
            row = {k: v for k, v in row.items() if k not in
                   ('stage', 'collector_wall_ns', 'collector_monotonic_ns')}
            serial = row['serial']
            if serial in records and records[serial] != row:
                conflicts.append(serial)
            records[serial] = row
        elif row.get('stage') == 'haptics':
            stats.append(row)

    for row in capture:
        collect(row)
    stats.clear()
    for row in requests:
        if row.get('stage') == 'device_reply':
            if row.get('error'):
                reply_errors.append(row)
            for line in row.get('reply', '').splitlines():
                try:
                    collect(json.loads(line))
                except (json.JSONDecodeError, TypeError):
                    reply_errors.append(row)
    ordered = [records[s] for s in sorted(records)]
    pulses = [row for row in requests if row.get('stage') == 'pulse_request']
    legacy = [row for row in requests if row.get('stage') in ('native_left_request', 'native_right_request')]
    intervals, pending, before, target_slot = [], None, None, None
    for row in requests:
        if row.get('stage') == 'snapshot_end' and row.get('label') == 'before_pulse':
            before = row
        elif row.get('stage') == 'pulse_request':
            if pending:
                intervals.append(dict(route=pending['route'], matched=False, reason='missing_after_snapshot'))
            pending = row
        elif row.get('stage') == 'snapshot_end' and row.get('label') == 'after_pulse' and pending:
            route = pending['route']
            lo = before['after_serial'] if before else -1
            hi = row['after_serial']
            window = [records[s] for s in sorted(records) if lo < s <= hi]
            active = [r for r in window if nonzero(r)]
            errors = []
            if len(active) == 1:
                if target_slot is None:
                    target_slot = active[0]['slot']
                elif active[0]['slot'] != target_slot:
                    errors.append('output_slot_changed')
            # Original Steam producer updates only the target's cached word.
            # An API stop request does not prove that Steam emitted a stop.
            prior = [r for r in ordered if r['serial'] <= lo and native_payload(r) is not None
                     and (target_slot is None or r['slot'] == target_slot)]
            words, expected, baseline_serial = None, None, None
            if not prior:
                errors.append('missing_source_baseline')
            else:
                baseline = prior[-1]
                baseline_serial = baseline['serial']
                payload = native_payload(baseline)
                words = [payload[2:6], payload[6:10]]
                if any(s not in records for s in range(baseline_serial, lo + 1)):
                    errors.append('missing_baseline_trace_records')
                if any(w not in (NEUTRAL, PULSE) for w in words):
                    errors.append('unsupported_baseline_for_fixed_pulse_fixture')
                words[pending['side']] = PULSE
                expected = b'\x10\0' + b''.join(words)
            expected_flags = 1 if route == 'host_rpc_flags1' else 0
            if pending.get('duration_us') != 1000 or pending.get('flags') != expected_flags:
                errors.append('unexpected_source_parameters')
            if not before or before['route'] != route or row['route'] != route or not (
                    before['elapsed_us'] <= pending['elapsed_us'] <= row['elapsed_us']):
                errors.append('invalid_request_bracket')
            if hi < lo or len(window) != hi - lo:
                errors.append('missing_trace_records')
            if len(active) != 1 or native_payload(active[0]) != expected:
                errors.append('nonzero_packet_count_or_target_mismatch')
            if any(r.get('rc') != 0 for r in window):
                errors.append('device_error')
            holds, lateness = [], []
            if len(active) == 1 and words is not None:
                samples = [r for r in window if r.get('kind') == 2 and
                           r.get('seq') == active[0]['seq'] and r.get('slot') == active[0]['slot']]
                if [r['sample'] for r in samples] != [0, 1, 2]:
                    errors.append('missing_or_reordered_samples')
                else:
                    holds = [b['sent_ns'] - a['completed_ns'] for a, b in zip(samples, samples[1:])]
                    lateness = [s['sent_ns'] - s['due_ns'] for s in samples]
                    if any(hold < 5_000_000 for hold in holds):
                        errors.append('sample_hold_shorter_than_5ms')
                    for index, sample in enumerate(samples):
                        values = [[0, 160, 0, 320], [0, 160, 0, 320]]
                        if index == 1:
                            for side, word in enumerate(words):
                                if word == PULSE:
                                    values[side][0] = 0.0441941731
                        actual = sample.get('values', [])
                        if len(actual) != 2 or any(len(v) != 4 for v in actual) or any(
                                not math.isfinite(a) or abs(a - b) > 5e-8
                                for va, vb in zip(actual, values) for a, b in zip(va, vb)):
                            errors.append('sample_values_mismatch')
                        if not (0 < sample.get('due_ns', 0) <= sample.get('sent_ns', 0) <= sample.get('completed_ns', 0)):
                            errors.append('invalid_submission_times')
            intervals.append(dict(route=route, side=pending['side'], ordinal=pending['ordinal'],
                                  before_serial=lo, after_serial=hi, baseline_serial=baseline_serial,
                                  expected_payload=expected.hex() if expected else None,
                                  minimum_hold_ns=min(holds) if holds else None,
                                  maximum_lateness_ns=max(lateness) if lateness else None,
                                  matched=not errors, errors=errors))
            pending, before = None, None
    if pending:
        intervals.append(dict(route=pending['route'], matched=False, reason='missing_after_snapshot'))
    successful_source = manifest.get('exit_code') == 0 and any(
        r.get('stage') == 'cleanup_complete' for r in requests)
    routes = {}
    for route in sorted({p['route'] for p in pulses}):
        phase = [p for p in pulses if p['route'] == route]
        matches = [r for r in intervals if r['route'] == route]
        complete = sorted((p['side'], p['ordinal']) for p in phase) == [(s, n) for s in (0, 1) for n in range(3)]
        verified = (successful_source and complete and len(matches) == 6 and all(r['matched'] for r in matches)
                    and not conflicts and not reply_errors)
        routes[route] = dict(requests=len(phase), matched=sum(r['matched'] for r in matches),
                             diagnostic_gate_bypass=any(p.get('flags', 0) for p in phase),
                             software_correlation_verified=bool(verified))
    active = [r for r in ordered if nonzero(r)]
    timestamps = [r['received_ns'] for r in active]
    request_times = [r['elapsed_us'] for r in (pulses or legacy)]
    return dict(host_exit_code=manifest.get('exit_code'), source_completed=successful_source,
                context_counts=[r for r in requests if r.get('stage') == 'context_controller_count'],
                capture_summary=next((r for r in reversed(list(capture)) if r.get('stage') == 'capture_end'), None),
                pulse_requests=len(pulses or legacy), trace_records=len(records),
                accepted_native_packets=sum(native_payload(r) is not None for r in ordered),
                nonzero_native_packets=len(active),
                submitted_samples=sum(r.get('kind') == 2 and r.get('rc') == 0 for r in ordered),
                nonzero_packet_span_ms=(max(timestamps) - min(timestamps)) / 1e6 if timestamps else None,
                source_request_span_ms=(max(request_times) - min(request_times)) / 1000 if request_times else None,
                routes=routes, intervals=intervals, conflicting_serials=conflicts,
                correlated_submission_samples=3 * sum(i['matched'] for i in intervals),
                device_reply_errors=len(reply_errors), last_inline_counters=stats[-1] if stats else None,
                last_capture_counters=capture_stats[-1] if capture_stats else None,
                host_gate_observations=[r for r in requests if r.get('stage') == 'host_gate_state'],
                game_support_verified=False, system_parameter_readback_verified=False,
                physical_fidelity_verified=False, loader_return_verified=False,
                limits='Packet presence alone does not attribute effects to this helper. RPC correlation does not verify the public game API route. IPC success is not physical-fidelity proof.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--requests', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--capture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = analyze(read_rows(args.requests), read_rows(args.capture) if args.capture else (),
                     json.loads(args.manifest.read_text(encoding='utf-8-sig')))
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
