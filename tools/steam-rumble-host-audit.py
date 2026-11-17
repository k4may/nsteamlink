#!/usr/bin/env python3
"""Execute pinned Valve rumble producers and effect receivers offline.

Requires pefile, capstone and unicorn, plus steam-gyro-host-audit.py beside this
file. Executes original instructions, with synthetic controller objects, clocks,
eligibility inputs, locks and a recording HID/SDL terminal. The original output
eligibility predicate executes; its initialization is not emulated. Does not execute
Steam Input API dispatch, network delivery, Nintendo encoding or physical motors.
The Linux receiver is from Valve's 1.4.5.326 standalone Steam Link archive.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

import capstone
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE

spec = importlib.util.spec_from_file_location(
    "gyro_audit", Path(__file__).with_name("steam-gyro-host-audit.py"))
gyro = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gyro)

HOST_SHA = gyro.SHA256
LINUX_SHA = "92373306ed2c91818bb5dc89d2cd7ba18cc8244f9d436de671c35d776d0425ac"
LINUX_BASE = 0x40000000


def checked(path, expected):
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError(f"Unrecognized binary: {path}")
    return raw


def indirect(m, pe, instruction, callback, stub):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    ins = next(md.disasm(pe.get_data(instruction - pe.OPTIONAL_HEADER.ImageBase, 15), instruction))
    assert ins.mnemonic == "call" and ins.operands[0].type == capstone.CS_OP_MEM
    mem = ins.operands[0].mem
    assert mem.base == capstone.x86.X86_REG_RIP
    m.write(ins.address + ins.size + mem.disp, stub)
    m.stubs[stub] = callback


def producer(pe, kind, low=0x1234, high=0xabcd, side=0, duration=1000,
             elapsed=1000, off=0, repeats=0, enhanced=True, writes_enabled=True):
    m = gyro.Machine(pe)
    obj, handler, hvtable, device, dvtable = [m.alloc() for _ in range(5)]
    m.write(obj + 8, 38, 4)  # actual Nintendo Switch Pro controller type
    m.write(obj + 0x4f8, 0x057e, 2)
    m.write(obj + 0x4fa, 0x2009, 2)
    m.write(obj + 0x588, int(enhanced), 1)
    m.write(0x13980d070, int(writes_enabled), 4)
    m.write(obj + 0x10, handler)
    m.write(handler, hvtable)
    m.write(handler + 0x10, device)
    m.write(hvtable + 0x20, 0x13867f7f0)  # original protocol-handler forwarding
    m.write(device, dvtable)
    m.write(dvtable + 0x18, 0x480000)
    # Constructor's neutral Nintendo motor state, not an invented rumble sample.
    m.write(obj + 0x54a, 0x40400100, 4)
    m.write(obj + 0x54e, 0x40400100, 4)
    commands = []

    def record():
        n = m.reg("r8")
        assert n in (21, 69)
        commands.append(bytes(m.u.mem_read(m.reg("rdx"), n)))
        m.ret(n)

    m.stubs[0x480000] = record
    clock = lambda: m.ret(100000)
    elapsed_clock = lambda: m.ret(elapsed)
    if kind == "rumble":
        start, end = 0x13867bda0, 0x13867be8d
        indirect(m, pe, 0x13867bdbd, clock, 0x480010)
        indirect(m, pe, 0x13867bde2, elapsed_clock, 0x480020)

        def seconds():
            m.reg("xmm0", int.from_bytes(struct.pack("<d", 10.0), "little"))
            m.ret()

        indirect(m, pe, 0x13867bdf2, seconds, 0x480030)
        m.write(0x4f0008 + 0x28, high, 2)  # fifth Windows x64 argument
        args = (obj, 0, 0, low)
    else:
        start, end = 0x13867cce0, 0x13867ce98
        indirect(m, pe, 0x13867cd22, clock, 0x480010)
        indirect(m, pe, 0x13867cd35, elapsed_clock, 0x480020)
        # Both elapsed calls use the same original clock import slot.
        m.write(0x4f0008 + 0x28, repeats, 2)
        m.write(0x4f0008 + 0x30, 0, 1)
        args = (obj, side, duration, off)
    m.allowed = [(start, end), (0x13867f7f0, 0x13867f7fb),
                 (0x1386746f0, 0x13867475a), (0x138673260, 0x1386733ea)]
    m.run(start, *args)
    return commands


def receive_windows(pe, command):
    m = gyro.Machine(pe)
    obj, data = m.alloc(), m.alloc()
    m.u.mem_write(data, command)
    m.write(obj + 8, 0x123456)
    calls = []

    def effect():
        calls.append({"kind": "effect", "data": bytes(m.u.mem_read(
            m.reg("rdx"), m.reg("r8"))).hex()})
        m.ret(1)

    def rumble():
        calls.append({"kind": "rumble", "low": m.reg("rdx"),
                      "high": m.reg("r8"), "duration_ms": m.reg("r9")})
        m.ret(1)

    for pc, cb, stub in [(0x1386d3dd8, lambda: m.ret(), 0x480000),
                         (0x1386d4017, lambda: m.ret(), 0x480010),
                         (0x1386d3dff, effect, 0x480020),
                         (0x1386d3e52, rumble, 0x480030)]:
        indirect(m, pe, pc, cb, stub)
    m.allowed = [(0x1386d3dc0, 0x1386d402f)]
    m.run(0x1386d3dc0, obj, data, len(command))
    return {"returned_bytes": m.reg("rax"), "calls": calls}


class LinuxMachine(gyro.Machine):
    def __init__(self, raw):
        self.u = Uc(UC_ARCH_X86, UC_MODE_64)
        assert raw[:6] == b"\x7fELF\x02\x01"
        phoff = struct.unpack_from("<Q", raw, 32)[0]
        size, count = struct.unpack_from("<HH", raw, 54)
        headers = [struct.unpack_from("<IIQQQQQQ", raw, phoff + i * size)
                   for i in range(count)]
        loads = [h for h in headers if h[0] == 1]
        top = max(h[3] + h[6] for h in loads)
        self.u.mem_map(LINUX_BASE, (top + 4095) & ~4095)
        for h in loads:
            self.u.mem_write(LINUX_BASE + h[3], raw[h[2]:h[2] + h[5]])
        self.u.mem_map(0x100000, 0x400000)
        self.cursor = 0x100000
        self.stubs, self.stops, self.allowed = {}, set(), []
        self.visited = set()
        self.u.hook_add(UC_HOOK_CODE, self.hook)


def receive_linux(raw, command):
    m = LinuxMachine(raw)
    obj, data = m.alloc(), m.alloc()
    m.u.mem_write(data, command)
    m.write(obj + 8, 0x123456)
    calls = []

    def effect():
        calls.append({"kind": "effect", "data": bytes(m.u.mem_read(
            m.reg("rsi"), m.reg("rdx"))).hex()})
        m.ret(1)

    def rumble():
        calls.append({"kind": "rumble", "low": m.reg("rsi"),
                      "high": m.reg("rdx"), "duration_ms": m.reg("rcx")})
        m.ret(1)

    for slot, cb, stub in [(0x1441a70, lambda: m.ret(), 0x480000),
                           (0x1441a28, lambda: m.ret(), 0x480010),
                           (0x1441a48, effect, 0x480020),
                           (0x1441a58, rumble, 0x480030)]:
        m.write(LINUX_BASE + slot, stub)
        m.stubs[stub] = cb
    m.allowed = [(LINUX_BASE + 0x2dbc30, LINUX_BASE + 0x2dbe98)]
    m.reg("rdi", obj)
    m.reg("rsi", data)
    m.reg("rdx", len(command))
    m.run(LINUX_BASE + 0x2dbc30)
    return {"returned_bytes": m.reg("rax"), "calls": calls}


def audit(pe, linux):
    imports = {i.address: i.name for d in pe.DIRECTORY_ENTRY_IMPORT for i in d.imports}
    assert imports[0x13918e380] == b"Plat_RelativeTicks"
    assert imports[0x13918e1c0] == b"Plat_TickDiffMilliSec"
    assert imports[0x13918e4b8] == b"Plat_FloatTime"
    for enhanced, enabled in ((False, True), (True, False), (False, False)):
        for kind in ("rumble", "pulse"):
            assert producer(pe, kind, enhanced=enhanced, writes_enabled=enabled) == []
    ordinary = producer(pe, "rumble")[0]
    assert ordinary[:9] == b"\x01" + struct.pack("<HHI", 0x1234, 0xabcd, 5000)
    assert producer(pe, "rumble", elapsed=49) == []
    assert len(producer(pe, "rumble", elapsed=50)) == 1
    assert len(producer(pe, "rumble", low=0, high=0, elapsed=0)) == 1
    pulse = producer(pe, "pulse", side=0, duration=1)[0]
    # Test whether fine pulse parameters actually survive this Switch branch.
    for duration, off, repeats in [(1000, 500, 10), (65535, 65535, 65535)]:
        assert producer(pe, "pulse", side=0, duration=duration,
                        off=off, repeats=repeats)[0] == pulse
    assert pulse[0] == 13 and len(pulse) == 69
    assert struct.unpack_from("<I", pulse, 1)[0] == 10
    assert pulse[5] == 0x10
    right = producer(pe, "pulse", side=1, duration=1)[0]
    stop = producer(pe, "pulse", side=0, duration=0)[0]
    assert right != pulse and stop != pulse
    assert producer(pe, "pulse", elapsed=49) == []
    assert len(producer(pe, "pulse", elapsed=50)) == 1
    outputs = []
    # A receiver capability check, NOT evidence that Steam generates this payload.
    synthetic = b"\x0d" + struct.pack("<I", 64) + bytes(range(64))
    for name, command in [("ordinary", ordinary), ("pad0_pulse", pulse),
                          ("pad1_pulse", right), ("pulse_off", stop),
                          ("synthetic_64_byte_receiver_only", synthetic)]:
        win, lin = receive_windows(pe, command), receive_linux(linux, command)
        assert win == lin, (name, win, lin)
        assert win["returned_bytes"] == len(command)
        if command[0] == 13:
            n = struct.unpack_from("<I", command, 1)[0]
            assert win["calls"] == [{"kind": "effect", "data": command[5:5+n].hex()}]
        else:
            assert win["calls"] == [{"kind": "rumble", "low": 0x1234,
                                     "high": 0xabcd, "duration_ms": 5000}]
        outputs.append({"name": name, "command": command.hex(), "receiver": win})
    return {"host_sha256": HOST_SHA, "linux_receiver_sha256": LINUX_SHA,
            "passed": True, "cases": outputs,
            "observations": {"original_output_eligibility_executes": True,
                             "type38_requires_enabled_writes_and_enhanced_state": True,
                             "nonzero_rumble_gate_ms": 50,
                             "pulse_gate_ms": 50,
                             "switch_pulse_duration_off_repeat_collapsed": True},
            "boundary": "Synthetic clocks, controller state and eligibility inputs, locks and recording "
                        "HID/SDL terminals. Original output eligibility, producer, protocol forwarder and two "
                        "receivers execute. No API dispatch, network, Switch API, sample "
                        "timing or physical actuator proof; no claim of arbitrary HD waveform "
                        "generation by this host branch."}


def main():
    if not __debug__:
        raise SystemExit("Assertions must be enabled")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--linux-client", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = audit(pefile.PE(data=checked(args.host, HOST_SHA)),
                   checked(args.linux_client, LINUX_SHA))
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end="")


if __name__ == "__main__":
    main()
