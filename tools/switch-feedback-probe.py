#!/usr/bin/env python3
"""Collect the self-contained feedback matrix through nxlink, or inspect a log.

The Switch only needs hbmenu netloader. No SD fixtures, port probes, host helper,
or game are required. The matrix checker verifies commands, outputs, explicit
stop and worker cleanup independently of nxlink's exit status. Neither normal
exit nor this tool's report certifies the Switch display or loader state.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import ipaddress
import json
from pathlib import Path
import shutil
import subprocess


CHECKER = Path(__file__).resolve().with_name("check-feedback-matrix.py")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_checker():
    spec = importlib.util.spec_from_file_location("feedback_matrix_checker", CHECKER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ipv4(value):
    try:
        return str(ipaddress.IPv4Address(value))
    except ipaddress.AddressValueError as error:
        raise argparse.ArgumentTypeError("--ip must be an explicit IPv4 address") from error


def bounded_timeout(value):
    try:
        seconds = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("timeout must be a number") from error
    if not 0 < seconds <= 30:
        raise argparse.ArgumentTypeError("timeout must be greater than zero and at most 30 seconds")
    return seconds


def bytes_output(value):
    if value is None:
        return b""
    return value.encode() if isinstance(value, str) else bytes(value)


def invalidate(report, message):
    for flag in ("software_path_verified", "ipc_submission_verified", "ipc_readback_verified",
                 "stopped_ipc_readback_verified"):
        report[flag] = False
    report.setdefault("errors", []).append(message)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ip", type=ipv4, required=True,
                        help="user-specified Switch address; no discovery or port probing")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--nro", type=Path, help="deploy the built nsl-feedback-probe.nro")
    source.add_argument("--log", type=Path, help="offline only: inspect this existing matrix log")
    parser.add_argument("--output", type=Path, default=Path("build/feedback-device.json"),
                        help="report JSON; sibling .log and .metadata.json preserve collection evidence")
    parser.add_argument("--timeout", type=bounded_timeout, default=30.0,
                        help="bounded nxlink upload/log capture duration, at most 30 seconds")
    args = parser.parse_args(argv)
    output = args.output.resolve()
    log_path = output.with_suffix(".log")
    metadata_path = output.with_suffix(".metadata.json")
    if output == log_path or output == metadata_path:
        parser.error("--output must name a JSON report, separate from its log and metadata sidecars")
    if args.log and args.log.resolve() in (output, metadata_path):
        parser.error("--output must not overwrite the input log")
    if args.nro and args.nro.resolve() in (output, log_path, metadata_path):
        parser.error("report artifacts must not overwrite the input NRO")

    started = datetime.now(timezone.utc).isoformat()
    metadata = dict(mode="deploy" if args.nro else "offline", ip=args.ip,
                    started_utc=started, command=None, timeout_seconds=args.timeout,
                    returncode=None, timed_out=False, collection_error=None,
                    nro_path=str(args.nro.resolve()) if args.nro else None,
                    nro_sha256=None, input_log=str(args.log.resolve()) if args.log else None,
                    checker_path=str(CHECKER), checker_sha256=None,
                    captured_log=str(log_path), report_path=str(output),
                    physical_fidelity_verified=False, loader_return_verified=False)
    data = b""
    checker = None
    try:
        metadata["checker_sha256"] = sha256(CHECKER.read_bytes())
        checker = load_checker()
        if args.nro:
            # Hash/read the concrete artifact before making the only network
            # action: nxlink itself. Never consume a netloader receive attempt
            # with a separate empty TCP connection.
            metadata["nro_sha256"] = sha256(args.nro.read_bytes())
            nxlink = shutil.which("nxlink") or "/opt/devkitpro/tools/bin/nxlink"
            command = [nxlink, "-a", args.ip, "-r", "1", "-s", str(args.nro.resolve())]
            metadata["command"] = command
            try:
                run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     timeout=args.timeout, check=False)
                data = bytes_output(run.stdout)
                metadata["returncode"] = run.returncode
            except subprocess.TimeoutExpired as error:
                # subprocess.run kills and waits for nxlink before raising.
                # Save even incomplete records; a timeout cannot certify a run.
                data = bytes_output(error.stdout)
                metadata["timed_out"] = True
        else:
            data = args.log.read_bytes()
    except (OSError, ValueError, ImportError) as error:
        metadata["collection_error"] = str(error)

    try:
        if checker is None:
            raise ValueError("matrix checker could not be loaded")
        report = checker.analyze(checker.parse_records(data.decode(errors="replace")))
    except (ValueError, TypeError, KeyError, OSError) as error:
        report = dict(software_path_verified=False, ipc_submission_verified=False,
                      ipc_readback_verified=False, stopped_ipc_readback_verified=False,
                      errors=[str(error)])

    collection_ok = metadata["collection_error"] is None and not metadata["timed_out"] and \
        (not args.nro or metadata["returncode"] == 0)
    if not collection_ok:
        invalidate(report, "nxlink/file collection failed or timed out; complete logs alone do not certify collection")
    if args.nro and report.get("platform") != "switch":
        invalidate(report, "deployed NRO must identify its matrix platform as switch")
    cleanup_complete = report.get("software_path_verified") is True and \
        report.get("zero_output_cleanup_verified") is True and \
        report.get("explicit_stop_verified") is True and \
        report.get("stopped_parameter_readback_matches") is True
    if not cleanup_complete:
        invalidate(report, "explicit-stop, native cleanup, joined-worker and stopped parameter-readback evidence is required")
    # These properties cannot be observed by a loader uploader or software
    # matrix, including when every readback happens to match.
    report["physical_fidelity_verified"] = False
    report["loader_return_verified"] = False
    metadata["completed_utc"] = datetime.now(timezone.utc).isoformat()
    metadata["captured_log_sha256"] = sha256(data)
    metadata["captured_log_bytes"] = len(data)
    report["collection_complete"] = collection_ok and report.get("software_path_verified") is True
    report["cleanup_evidence_complete"] = cleanup_complete
    report["collection"] = metadata
    report["metadata_path"] = str(metadata_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes(data)
    metadata_path.write_text(json.dumps(metadata, indent=2, allow_nan=False) + "\n")
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps(report, indent=2, allow_nan=False))
    return 0 if report["collection_complete"] and cleanup_complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
