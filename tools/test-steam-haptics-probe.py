#!/usr/bin/env python3
"""Cross-build and exercise the Windows source with a Wine-only mock Steam DLL.

Tests request/cleanup contracts and private-layout guards, NOT Valve IPC or
delivery. The fixture uses dedicated PE sections at the pinned interface RVAs.
Only the test executable accepts the mock DLL hash; never distribute it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--wine-prefix', type=Path, required=True)
    args = parser.parse_args()
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    mock = directory / 'mock-steam'
    mock.mkdir(exist_ok=True)
    dll = mock / 'steamclient64.dll'
    compiler = 'x86_64-w64-mingw32-gcc'
    common = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
    subprocess.run(common + ['-DNSL_MOCK_PRIVATE_LAYOUT', '-shared',
        str(ROOT / 'app/tests/steam_haptics_host_mock.c'),
        '-Wl,--image-base,0x138000000', '-Wl,--section-start,.nslpub=0x13933f000',
        '-Wl,--section-start,.nslctl=0x139304000', '-Wl,--section-start,.nslqry=0x13861a000',
        '-o', str(dll)], check=True)
    digest = hashlib.sha256(dll.read_bytes()).hexdigest()
    source = str(ROOT / 'tools/steam-haptics-probe-win.c')
    production, fixture = directory / 'steam-haptics-probe.exe', directory / 'mock-hash-probe.exe'
    for exe, defines in ((production, []), (fixture, [f'-DNSL_PROBE_HOST_SHA256="{digest}"'])):
        subprocess.run(common + defines + ['-municode', source, '-o', str(exe),
                                           '-ladvapi32', '-lbcrypt'], check=True)
    environment = {k: v for k, v in os.environ.items() if not k.startswith('NSL_MOCK_')}
    environment.update(WINEPREFIX=str(args.wine_prefix.resolve()), WINEDEBUG='-all',
                       WINEDLLOVERRIDES='winemenubuilder.exe=d')
    # Wine Z: maps the Unix filesystem; fixtures remain in the ignored build tree.
    windows_mock = 'Z:' + str(mock).replace('/', '\\')
    cases = [
        ('public_probe', production, ['--probe'], {'NSL_MOCK_EXPECT_PROBE': '1'}, 0,
         'requests_complete_delivery_unverified'),
        ('context_probe', fixture, ['--probe'],
         {'NSL_MOCK_EMPTY_PUBLIC': '1', 'NSL_MOCK_EXPECT_PROBE': '1'}, 0,
         'requests_complete_delivery_unverified'),
        ('context_list', fixture, ['--list'], {'NSL_MOCK_EMPTY_PUBLIC': '1'}, 0, 'list_complete'),
        ('unknown_hash', production, ['--probe'], {'NSL_MOCK_EMPTY_PUBLIC': '1'}, 1,
         'context_query_unknown_host_binary'),
        ('wrong_layout', fixture, ['--probe'],
         {'NSL_MOCK_EMPTY_PUBLIC': '1', 'NSL_MOCK_BAD_LAYOUT': '1'}, 1,
         'context_query_unexpected_input_layout'),
        ('wrong_method', fixture, ['--probe'],
         {'NSL_MOCK_EMPTY_PUBLIC': '1', 'NSL_MOCK_BAD_METHOD': '1'}, 1,
         'context_query_unexpected_controller_layout'),
        ('invalid_count', fixture, ['--probe'],
         {'NSL_MOCK_EMPTY_PUBLIC': '1', 'NSL_MOCK_BAD_COUNT': '1'}, 1, 'context_query_invalid_count'),
        ('ambiguous_target', fixture, ['--probe'],
         {'NSL_MOCK_EMPTY_PUBLIC': '1', 'NSL_MOCK_MULTIPLE': '1'}, 1,
         'probe_requires_exactly_one_switch_pro'),
        ('non_switch', production, ['--probe'], {'NSL_MOCK_NO_SWITCH': '1'}, 1,
         'probe_requires_exactly_one_switch_pro'),
        ('invalid_appid', production, ['--appid', '-1'], {}, 1, 'probe_failed'),
    ]
    report = []
    for name, exe, options, extra, expected_exit, expected_stage in cases:
        result = subprocess.run(['wine', str(exe), '--steam-root', windows_mock] + options,
                                env=dict(environment, **extra), capture_output=True,
                                text=True, timeout=45)
        (directory / (name + '.jsonl')).write_text(result.stdout)
        (directory / (name + '.stderr')).write_text(result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        stages = [row.get('stage') for row in rows]
        if result.returncode != expected_exit or expected_stage not in stages:
            raise AssertionError((name, result.returncode, result.stdout, result.stderr))
        expected_pulses = 3 if extra.get('NSL_MOCK_EXPECT_PROBE') else 0
        if stages.count('native_left_request') != expected_pulses or stages.count('native_right_request') != expected_pulses:
            raise AssertionError((name, stages))
        report.append(dict(name=name, exit_code=result.returncode, passed=True))
    summary = dict(passed=True, mock_sha256=digest, cases=report,
                   actual_steam_verified=False, delivery_verified=False)
    (directory / 'report.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
