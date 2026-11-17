#!/usr/bin/env python3
"""Read the diagnostic app's haptics counters and bounded trace via UDP.

Does not start a stream, generate vibration, or access hbmenu netloader.
The capture is software evidence, not a physical-fidelity verdict.
"""
import argparse
import json
from pathlib import Path
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", default="10.10.10.17")
    parser.add_argument("--port", type=int, default=28772)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("seconds must be positive")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    after, gaps, captured, timeouts = 0, 0, 0, 0
    deadline, next_stats = time.monotonic() + args.seconds, 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock, args.output.open("w") as out:
        sock.connect((args.address, args.port))
        sock.settimeout(.5)

        def save(row):
            out.write(json.dumps(row) + "\n")
            out.flush()

        save(dict(stage="capture_begin", address=args.address, port=args.port,
                  physical_fidelity_verified=False))
        while time.monotonic() < deadline:
            command = "haptics" if time.monotonic() >= next_stats else f"haptic-trace {after}"
            sock.send(command.encode())
            try:
                data = sock.recv(8192).decode()
            except (TimeoutError, ConnectionRefusedError):
                timeouts += 1
                continue
            try:
                rows = [json.loads(line) for line in data.splitlines()]
            except json.JSONDecodeError:
                raise SystemExit(f"Unexpected diagnostic response: {data[:120]}")
            full_page = False
            for row in rows:
                if row.get("stage") == "haptics":
                    save(row)
                    next_stats = time.monotonic() + 1
                elif "serial" in row:
                    serial = row["serial"]
                    if serial <= after:
                        continue
                    if serial != after + 1:
                        gaps += serial - after - 1
                        save(dict(stage="capture_gap", after=after, next=serial))
                    after = serial
                    captured += 1
                    save(dict(stage="trace", **row))
            full_page = len(rows) == 6
            if not full_page:
                time.sleep(.02)
        summary = dict(stage="capture_end", records=captured, missing_serials=gaps,
                       timeouts=timeouts, physical_fidelity_verified=False,
                       loader_return_verified=False)
        save(summary)
        print(json.dumps(summary, indent=2))
    if not captured:
        raise SystemExit("No output trace captured; this does not prove that the game sent vibration")


if __name__ == "__main__":
    main()
