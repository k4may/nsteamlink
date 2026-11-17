#!/usr/bin/env python3
"""Execute the pinned Steam host's pulse enqueue, worker and output gates.

Original CSteamController forwarding, CHIDIOThread enqueue/constructor,
CPulseHapticWorkItem, Generic GetInfo/pulse and protocol forwarding execute.
Original active-config application also checks capability bit 0x2000 and the
haptics preference. Config-to-slot transfer is supplied as synthetic state;
RemoteHID caps-to-config initialization is not executed.
Synthetic controller/slot state, memory allocation, locks, clock imports and
the work queue/HID terminal are substitutes. No game/API adapter dispatch,
real configuration initialization, live Steam process or network is proved.
"""
import argparse
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('rumble_audit', Path(__file__).with_name('steam-rumble-host-audit.py'))
rumble = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rumble)


def configuration_gate(pe, caps, haptics_enabled):
    """Original active-config application; opaque preferences/identity supplied.

    This does not prove RemoteHID caps become this config's override, or that a
    real game's settings enable haptics. Those are separate initialization steps.
    """
    m = rumble.gyro.Machine(pe)
    config, info, preferences = [m.alloc() for _ in range(3)]
    m.write(config + 0x230, caps)
    m.write(config + 0x220, 38, 4)
    m.write(config + 0x20, 38, 4)
    m.write(config + 0x640, 1, 4)  # rumble preference on
    m.write(info + 0x34, 38, 4)
    m.write(preferences + 0xd4, int(haptics_enabled), 1)
    m.stubs[0x1385f1850] = lambda: m.ret()
    m.allowed = [(0x138665a20,0x138665c3d), (0x1386571d0,0x1386572ef),
                 (0x1386731d0,0x138673470)]
    m.run(0x138665a20,config,info,preferences)
    return dict(config_caps_override=hex(caps), haptics_preference=haptics_enabled,
                pulse_enabled=bool(m.read(config+0x649,1)),
                rumble_enabled=bool(m.read(config+0x648,1)))


def route(pe, pulse_enabled=True, flags=0, side=0, duration=1000,
          enhanced=True, writes_enabled=True):
    m = rumble.gyro.Machine(pe)
    obj, handler, hvtable, device, dvtable = [m.alloc() for _ in range(5)]
    parent = m.alloc(0x110000)
    io = m.alloc()
    m.write(parent + 0x10922d, 1, 1)
    m.write(parent + 0x180, io)
    m.write(io, 0x1392d6218)  # original CHIDIOThread vtable
    m.write(io + 0x1d1, int(pulse_enabled), 1)
    m.write(obj, 0x1392ea3d0)  # original Generic abstraction vtable
    m.write(obj + 8, 38, 4)
    m.write(obj + 0xac, 38, 4)  # copied GetInfo controller-type field
    m.write(obj + 0xb8, 1, 1)   # copied GetInfo valid byte
    m.write(obj + 0x4f8, 0x057e, 2)
    m.write(obj + 0x4fa, 0x2009, 2)
    m.write(obj + 0x588, int(enhanced), 1)
    m.write(0x13980d070, int(writes_enabled), 4)
    m.write(obj + 0x10, handler)
    m.write(handler, hvtable)
    m.write(handler + 0x10, device)
    m.write(hvtable + 0x20, 0x13867f7f0)
    m.write(device, dvtable)
    m.write(dvtable + 0x18, 0x480000)
    m.write(obj + 0x54a, 0x40400100, 4)
    m.write(obj + 0x54e, 0x40400100, 4)
    queued, commands = [], []

    def write():
        length = m.reg('r8')
        if length != 69:
            raise AssertionError('Unexpected native envelope length')
        commands.append(bytes(m.u.mem_read(m.reg('rdx'), length)).hex())
        m.ret(length)

    def enqueue():
        queued.append(m.reg('r8'))
        m.ret()

    m.stubs.update({0x480000: write,
                   0x1388aa6c0: lambda: m.ret(m.alloc(m.reg('rcx'))),
                   0x1381a85e0: lambda: m.ret(1),
                   0x1385f1710: enqueue})
    for pc, callback, stub in (
        (0x1386214de, lambda: m.ret(1), 0x480010),
        (0x138675e1c, lambda: m.ret(), 0x480020),
        (0x138675e70, lambda: m.ret(), 0x480030),
        (0x13867cd22, lambda: m.ret(100000), 0x480040),
        (0x13867cd35, lambda: m.ret(1000), 0x480050)):
        rumble.indirect(m, pe, pc, callback, stub)
    m.allowed = [(0x138621620, 0x13862165b), (0x1386214c0, 0x13862158c),
                 (0x1385e7520, 0x1385e759e), (0x138617a90, 0x138617b61),
                 (0x138675e00, 0x138675e89), (0x138673260, 0x1386733eb),
                 (0x13867cce0, 0x13867ce99), (0x1386746f0, 0x13867475b),
                 (0x13867f7f0, 0x13867f7fc)]
    # Windows x64 args: slot, side, duration, off, repeat, flags.
    m.write(0x4f0008 + 0x28, 0, 2)
    m.write(0x4f0008 + 0x30, 1, 2)
    m.write(0x4f0008 + 0x38, flags, 4)
    m.run(0x138621620, parent, 0, side, duration)
    for work in queued:
        m.run(0x138617a90, work, obj)
    return dict(pulse_enabled=pulse_enabled, flags=flags, side=side, duration=duration,
                enhanced=enhanced, writes_enabled=writes_enabled,
                queued=len(queued), commands=commands)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    pe = rumble.pefile.PE(data=rumble.checked(args.host, rumble.HOST_SHA))
    cases = []
    for enabled, flags in ((True,0), (False,0), (False,1), (False,2)):
        for side in (0,1):
            case = route(pe, pulse_enabled=enabled, flags=flags, side=side)
            should_queue = enabled or bool(flags & 3)
            if case['queued'] != int(should_queue):
                raise AssertionError(case)
            expected = [b.hex() for b in rumble.producer(pe,'pulse',side=1-side)] if should_queue else []
            if case['commands'] != expected:
                raise AssertionError(case)
            cases.append(case)
    for enhanced, writes in ((False,True),(True,False),(False,False)):
        case = route(pe,enhanced=enhanced,writes_enabled=writes)
        if case['queued'] != 1 or case['commands']:
            raise AssertionError(case)
        cases.append(case)
    configs = []
    for name,caps,pref,expected in (
            ('current_client_caps',0x834bff,True,False),
            ('pulse_bit_enabled',0x836bff,True,True),
            ('haptics_preference_disabled',0x836bff,False,False),
            ('host_default_switch_caps',0,True,False)):
        config = configuration_gate(pe,caps,pref)
        if config['pulse_enabled'] != expected or not config['rumble_enabled']:
            raise AssertionError(config)
        config['name'] = name
        config['downstream_route'] = route(pe,pulse_enabled=config['pulse_enabled'])
        if bool(config['downstream_route']['commands']) != expected:
            raise AssertionError(config)
        configs.append(config)
    report = dict(passed=True,host_sha256=rumble.HOST_SHA,cases=cases,configuration_gates=configs,
                  observations=dict(separate_upstream_pulse_gate=True,
                                    pulse_gate_offset='CHIDIOThread + slot*0x2a0 + 0x1d1',
                                    config_requires_capability_bit='0x2000',
                                    config_requires_haptics_preference=True,
                                    flags_low_two_bits_bypass_gate=True,
                                    worker_inverts_switch_target=True),
                  boundary=__doc__.strip())
    if args.output:
        args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
