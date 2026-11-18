#!/usr/bin/env python3
"""Ensure source completion, background native packets and bad IPC cannot pass."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

tool = Path(__file__).resolve().parents[2] / 'tools/analyze-steam-haptics-probe.py'
spec = importlib.util.spec_from_file_location('correlation', tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture():
    requests, capture = [], []
    serial = 0
    for route in ('public', 'host_rpc'):
        for side in (0, 1):
            for ordinal in range(3):
                now = 1000 + serial * 1000
                requests += [dict(stage='snapshot_end', label='before_pulse', route=route,
                                  after_serial=serial, elapsed_us=now),
                             dict(stage='pulse_request', route=route, side=side, ordinal=ordinal,
                                  elapsed_us=now + 1, flags=0, duration_us=1000)]
                serial += 1
                seq = serial
                native = bytes.fromhex('1000000005c000014040' if side == 0 else '100000014040000005c0')
                capture.append(dict(serial=serial, kind=1, slot=0, seq=seq, rc=0,
                                    received_ns=now, wire=(b'\x0d\x0a\0\0\0' + native + bytes(54)).hex()))
                for sample in range(3):
                    serial += 1
                    values = [[0, 160, 0, 320], [0, 160, 0, 320]]
                    if sample == 1:
                        values[side][0] = 0.0441941731
                    capture.append(dict(serial=serial, kind=2, slot=0, seq=seq, rc=0, sample=sample,
                                        received_ns=now, due_ns=now, sent_ns=now + 1,
                                        completed_ns=now + 2, values=values))
                requests.append(dict(stage='snapshot_end', label='after_pulse', route=route,
                                     after_serial=serial, elapsed_us=now + 200))
    requests.append(dict(stage='cleanup_complete'))
    return requests, capture, dict(exit_code=0)


class Correlation(unittest.TestCase):
    def test_complete_two_routes(self):
        result = module.analyze(*fixture())
        self.assertTrue(all(r['software_correlation_verified'] for r in result['routes'].values()))
        self.assertFalse(result['physical_fidelity_verified'])
        self.assertFalse(result['game_support_verified'])

    def test_inline_udp_capture(self):
        requests, capture, manifest = fixture()
        requests.insert(0, dict(stage='device_reply', error=0,
                                reply='\n'.join(json.dumps(r) for r in capture)))
        result = module.analyze(requests, (), manifest)
        self.assertTrue(result['routes']['host_rpc']['software_correlation_verified'])

    def test_background_packets_without_brackets(self):
        requests, capture, manifest = fixture()
        requests = [r for r in requests if r['stage'] != 'snapshot_end']
        result = module.analyze(requests, capture, manifest)
        self.assertEqual(result['nonzero_native_packets'], 12)
        self.assertFalse(any(r['software_correlation_verified'] for r in result['routes'].values()))

    def test_bypass_result_stays_labelled(self):
        requests, capture, manifest = fixture()
        for row in requests:
            if row.get('route') == 'host_rpc':
                row['route'] = 'host_rpc_flags1'
                if row['stage'] == 'pulse_request':
                    row['flags'] = 1
        result = module.analyze(requests, capture, manifest)
        self.assertTrue(result['routes']['host_rpc_flags1']['diagnostic_gate_bypass'])
        self.assertTrue(result['routes']['host_rpc_flags1']['software_correlation_verified'])
        self.assertNotIn('host_rpc', result['routes'])

    def test_negative_controls(self):
        mutations = {
            'source_flags_changed': lambda q, c, m: q[1].__setitem__('flags', 1),
            'source_duration_changed': lambda q, c, m: q[1].__setitem__('duration_us', 1),
            'lost_command': lambda q, c, m: c.pop(0),
            'lost_sample': lambda q, c, m: c.pop(2),
            'wrong_target': lambda q, c, m: c[2]['values'].reverse(),
            'wrong_value': lambda q, c, m: c[2]['values'][0].__setitem__(0, .1),
            'nan_value': lambda q, c, m: c[2]['values'][0].__setitem__(0, float('nan')),
            'wrong_order': lambda q, c, m: c[2].__setitem__('sample', 2),
            'ipc_failed': lambda q, c, m: c[2].__setitem__('rc', 1),
            'bad_time': lambda q, c, m: c[2].__setitem__('completed_ns', 1),
            'source_failed': lambda q, c, m: m.__setitem__('exit_code', 1),
            'udp_timeout': lambda q, c, m: q.insert(0, dict(stage='device_reply', error=10060, reply='')),
            'conflicting_trace': lambda q, c, m: c.append(dict(c[2], rc=1)),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                requests, capture, manifest = copy.deepcopy(fixture())
                mutate(requests, capture, manifest)
                result = module.analyze(requests, capture, manifest)
                self.assertFalse(result['routes']['public']['software_correlation_verified'])


if __name__ == '__main__':
    unittest.main()
