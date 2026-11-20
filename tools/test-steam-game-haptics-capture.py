#!/usr/bin/env python3
"""Exercise the real PowerShell collector/export with synthetic host/device data.

This verifies collection, backlog handling and export, not a game or real Steam.
Requires Linux PowerShell; Windows host binary is checked separately under Wine.
"""
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pwsh', required=True)
    parser.add_argument('--transport', choices=['udp', 'http'], default='udp')
    args = parser.parse_args()
    requests = []
    stop = threading.Event()
    def respond(command):
        requests.append(command)
        if command == 'haptics':
            return dict(stage='haptics', ordinary_commands=20, native_commands=2)
        assert command.startswith('haptic-trace '), command
        after = int(command.split()[1])
        if after == 0:
            return dict(serial=8, kind=1, source=0, rc=0, wire='010001000000000000')
        if after == 8:
            return dict(serial=10, kind=1, source=1, rc=0,
                        wire='0d0a0000001000000005c000014040'+'00'*54)
        return {}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path == '/health':
                data = dict(stage='diagnostic_health', diagnostics=True, version='fixture')
            elif self.path == '/haptics':
                data = respond('haptics')
            else:
                assert self.path.startswith('/haptic-trace?after=')
                data = respond('haptic-trace '+self.path.split('=', 1)[1])
            body = json.dumps(data).encode()
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    http = HTTPServer(('127.0.0.1', 28773), Handler) if args.transport == 'http' else None
    http_thread = threading.Thread(target=http.serve_forever) if http else None
    if http_thread:
        http_thread.start()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(('127.0.0.1', 28772))
        udp.settimeout(.1)

        def serve():
            while not stop.is_set():
                try:
                    data, peer = udp.recvfrom(1024)
                except TimeoutError:
                    continue
                command = data.decode()
                reply = respond(command)
                udp.sendto(json.dumps(reply).encode(), peer)

        thread = threading.Thread(target=serve)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='nsl-game-capture-') as tmp:
                folder = Path(tmp)
                collector = folder / 'run-game-haptics-capture.ps1'
                collector.write_text((ROOT/'tools/run-game-haptics-capture.ps1').read_text())
                # Start-Process, JSONL redirects and waits are real; only the host
                # producer and explorer/registry boundary are substituted.
                fake = folder / 'steam-game-haptics-watch.exe'
                state = dict(stage='host_state', appid=3489700, baseline=True,
                             native_producer_changed=False, ordinary_cache_changed=False)
                fake.write_text('#!/bin/sh\nprintf \'%s\\n\' \''+json.dumps(state)+"'\nsleep 1\n")
                fake.chmod(0o755)
                harness = folder / 'harness.ps1'
                harness.write_text('''param($folder)
$env:LOCALAPPDATA = Join-Path $folder 'localdata'
function Start-Process {
    param($FilePath,$ArgumentList,$WorkingDirectory,[switch]$PassThru,$RedirectStandardOutput,$RedirectStandardError)
    if ($FilePath -eq 'explorer.exe') { return }
    Microsoft.PowerShell.Management\\Start-Process @PSBoundParameters
}
function Get-ItemProperty { param($Path,$ErrorAction); return $null }
& (Join-Path $folder 'run-game-haptics-capture.ps1') -Seconds 2 -Address 127.0.0.1
''')
                result = subprocess.run([args.pwsh, '-NoProfile', '-File', str(harness), str(folder)],
                                        capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, (result.stdout, result.stderr)
                archives = list(folder.glob('game-results-*.zip'))
                assert len(archives) == 1, (result.stdout, result.stderr)
                with zipfile.ZipFile(archives[0]) as archive:
                    assert archive.testzip() is None
                    exported = json.loads(archive.read('export.json').decode('utf-8-sig'))
                    assert not exported['errors'], exported
                    assert '3489700' in archive.read('采集说明.txt').decode('utf-8-sig')
                    rows = [json.loads(line) for line in archive.read('switch.jsonl').decode().splitlines()]
                    assert not [r for r in rows if r['stage'] == 'collector_error'], rows
                    assert any(r['stage'] == 'trace_baseline' and r['after_serial'] == 8 for r in rows), rows
                    assert any(r['stage'] == 'capture_gap' and r['next'] == 10 for r in rows), rows
                    replies = [r for r in rows if r['stage'] == 'device_reply']
                    assert replies and all(r['transport'] == args.transport for r in replies)
                    assert any(r['baseline_backlog'] for r in replies)
                    assert any(not r['baseline_backlog'] for r in replies)
                assert 'haptics' in requests and 'haptic-trace 0' in requests
        finally:
            stop.set()
            thread.join()
            if http:
                http.shutdown()
                http_thread.join()
                http.server_close()
    print(json.dumps(dict(passed=True, actual_powershell_collection_and_export=True, transport=args.transport,
                          synthetic_host_and_switch=True, actual_game_verified=False)))


if __name__ == '__main__':
    main()
