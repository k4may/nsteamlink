#!/usr/bin/env python3
"""Cross-build and execute the actual Win32 UI with test-only vendor DLLs.

Checks slider/edit changes, both public routes, pulse timing, hotplug/focus/
close stops, real UDP collection and retained settings. Not actual Steam proof.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine-prefix", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    mock = out / "mock-steam"
    mock.mkdir(exist_ok=True)
    gcc = "x86_64-w64-mingw32-gcc"
    flags = [gcc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
    subprocess.run(flags + ["-shared", str(ROOT / "app/tests/haptics_panel_mock.c"),
                           "-o", str(mock / "steamclient64.dll")], check=True)
    shutil.copy2(mock / "steamclient64.dll", mock / "nsl-xinput-mock.dll")
    for name, definitions in (("steam-haptics-panel.exe", []),
                              ("panel-ui-test.exe", ["-DNSL_PANEL_TEST=1"])):
        subprocess.run(flags + definitions + ["-municode", "-mwindows",
            str(ROOT / "tools/steam-haptics-panel-win.c"), "-o", str(out / name),
            "-lcomctl32", "-lshell32", "-ladvapi32", "-lws2_32", "-lgdi32"], check=True)
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "app/tests/test_haptics_panel_state.c"), "-o", str(out / "state-test")], check=True)
    subprocess.run([str(out / "state-test")], check=True)
    environment = dict(os.environ, WINEPREFIX=str(args.wine_prefix.resolve()),
                       WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d")
    # A real Windows/Winsock request/response boundary. It supplies only records
    # from the documented read-only diagnostics; no outgoing vibration shortcut.
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("127.0.0.1", 28772))
    server.settimeout(.1)
    stopped = threading.Event()

    def serve():
        current_profile = dict(low_hz=160, high_hz=320, peak=.5571934, routing=0)
        while not stopped.is_set():
            try:
                data, peer = server.recvfrom(256)
            except TimeoutError:
                continue
            if data == b"rumble-config" or data.startswith(b"rumble-config "):
                fields = data.decode().split()
                if len(fields) == 5:
                    current_profile = dict(low_hz=float(fields[1]),high_hz=float(fields[2]),peak=float(fields[3]),routing=int(fields[4]))
                reply=json.dumps(dict(stage="rumble_config",ok=True,revision=1,**current_profile),separators=(',',':')).encode()
            elif data == b"haptics":
                reply = b'{"stage":"haptics","commands":1}'
            elif data == b"haptic-trace 0":
                reply = (b'{"serial":1,"kind":1,"wire":"010040008000000000","rc":0}\n'
                         b'{"serial":2,"kind":2,"values":[[0.1393,160,0,320],[0,160,0.2786,320]],"rc":0}\n')
            elif data.startswith(b"haptic-trace "):
                reply = b'{}'
            else:
                raise AssertionError(data)
            server.sendto(reply, peer)

    thread = threading.Thread(target=serve)
    thread.start()
    report = []
    try:
        for case in ("close", "focus", "disconnect", "invalid", "steam", "init_retry", "user_retry", "empty_public", "pulse", "capture", "frequency"):
            case_env=dict(environment, NSL_PANEL_CASE=case)
            if case=="init_retry":case_env["NSL_PANEL_INIT_FAIL_ONCE"]="1"
            if case=="user_retry":case_env["NSL_PANEL_USER_FAIL_ONCE"]="1"
            run = subprocess.run(["wine", str(out / "panel-ui-test.exe"), "--steam-root",
                "Z:" + str(mock).replace("/", "\\")],
                env=case_env, capture_output=True, timeout=45)
            (out / f"{case}.stderr").write_bytes(run.stderr)
            if run.returncode:
                raise AssertionError((case, run.returncode, run.stderr.decode(errors="replace")))
            folder = args.wine_prefix / "drive_c/users" / os.environ.get("USER", "root") / "AppData/Local/NSteamLink/HapticsPanel"
            logs = sorted(folder.glob("session-*.jsonl"), key=lambda p: p.stat().st_mtime_ns)
            assert logs, (case, folder)
            rows = [json.loads(line) for line in logs[-1].read_text().splitlines()]
            shutil.copy2(logs[-1], out / f"{case}.jsonl")
            assert any(r["stage"] == "ui_test_passed" for r in rows), (case, rows)
            assert rows[-1]["stage"] == "end" and rows[-1]["stopped"] and rows[-1]["capture_joined"]
            requests = [r for r in rows if r["stage"] == "request"]
            assert requests and requests[-1]["low"] == requests[-1]["high"] == 0
            if case == "steam":
                assert any(r["route"] == "steaminput" and r["low"] == 789 and r["high"] == 23456 for r in requests)
            if case == "capture":
                assert any(r["stage"] == "device_reply" and r["record"].get("serial") == 2 for r in rows)
            settings_bytes = (folder / "settings.ini").read_bytes()
            settings = settings_bytes.decode("utf-16" if settings_bytes.startswith(b'\xff\xfe') else "utf-8")
            assert "Low=" in settings and "High=" in settings and "LimitMs=" in settings
            report.append(dict(case=case, passed=True, requests=len(requests)))
    finally:
        stopped.set()
        thread.join()
        server.close()
    result = dict(passed=True, cases=report, actual_steam_verified=False,
                  remote_delivery_verified=False, physical_fidelity_verified=False)
    (out / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
