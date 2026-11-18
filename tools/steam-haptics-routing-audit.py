#!/usr/bin/env python3
"""Execute hash-pinned Steam hosts' pulse enqueue, worker and output gates.

Original CSteamController forwarding, CHIDIOThread enqueue/constructor,
CPulseHapticWorkItem, Generic GetInfo/pulse and protocol forwarding execute.
SteamInput006 legacy pulse adapter/core execute with synthetic handle lookup.
Original active-config application also checks capability bit 0x2000 and the
haptics preference. Config-to-slot copy instructions execute in isolation;
November cache-to-config copy instructions also execute in isolation.
RemoteHID caps parsing and real configuration initialization are not executed.
Synthetic controller/slot state, memory allocation, locks, clock imports and
the work queue/HID terminal are substitutes. No actual game/API invocation,
real configuration initialization, live Steam process or network is proved.
"""
import argparse
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('rumble_audit', Path(__file__).with_name('steam-rumble-host-audit.py'))
rumble = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rumble)


# Separate, hash-pinned layouts. The November binary changes CControllerConfig
# fields as well as code addresses; applying a uniform relocation is incorrect.
NOVEMBER_SHA = '669fa08e259277fbf5a1dfe15ce88afe734e89151194304b44d92f418dcc985b'
NOVEMBER_ADDRESSES = {
    0x138600690: 0x138616ff0,
    0x13860edae: 0x13862520e, 0x13860edbd: 0x13862521d,
    0x138621660: 0x138637ac0, 0x1386216f4: 0x138637b54,
    0x138721fd0: 0x138738510, 0x138721fde: 0x13873851e,
    0x1381a85e0: 0x1381af5c0,
    0x1385e7520: 0x1385fde60, 0x1385e759e: 0x1385fdede,
    0x1385f1710: 0x138608090, 0x1385f1850: 0x1386081d0,
    0x138617a90: 0x13862df10, 0x138617b61: 0x13862dfe1,
    0x1386214c0: 0x138637920, 0x1386214de: 0x13863793e,
    0x13862158c: 0x1386379ec,
    0x138621620: 0x138637a80, 0x13862165b: 0x138637abb,
    0x1386571d0: 0x13866f0a0, 0x1386572ef: 0x13866f1bf,
    0x138665a20: 0x13867e0b0, 0x138665c3d: 0x13867e2cd,
    0x1386731d0: 0x13868b8d0, 0x138673260: 0x13868b960,
    0x1386733eb: 0x13868baeb, 0x138673470: 0x13868bb70,
    0x1386746f0: 0x13868cdf0, 0x13867475b: 0x13868ce5b,
    0x138675e00: 0x13868e500, 0x138675e1c: 0x13868e51c,
    0x138675e70: 0x13868e570, 0x138675e89: 0x13868e589,
    0x13867cce0: 0x1386953e0, 0x13867cd22: 0x138695422,
    0x13867cd35: 0x138695435, 0x13867ce99: 0x138695599,
    0x13867f7f0: 0x138697ef0, 0x13867f7fc: 0x138697efc,
    0x1388aa6c0: 0x1388cea80,
    0x1392d6218: 0x139304c68, 0x1392d6370: 0x139304dc0,
    0x1392ea3d0: 0x139318fd0,
    0x13980d070: 0x139845410,
}
NOVEMBER_FIELDS = {0x230: 0x240, 0x220: 0x230, 0x640: 0x658,
                   0x649: 0x661, 0x648: 0x660}


def address(pe, october_address):
    if getattr(pe, 'haptics_november', False):
        return NOVEMBER_ADDRESSES[october_address]
    return october_address


def field(pe, october_offset):
    if getattr(pe, 'haptics_november', False):
        return NOVEMBER_FIELDS[october_offset]
    return october_offset


def configuration_gate(pe, caps, haptics_enabled):
    """Original active-config application; opaque preferences/identity supplied.

    This does not prove RemoteHID caps become this config's override, or that a
    real game's settings enable haptics. Those are separate initialization steps.
    """
    m = rumble.gyro.Machine(pe)
    config, info, preferences = [m.alloc() for _ in range(3)]
    m.write(config + field(pe, 0x230), caps)
    m.write(config + field(pe, 0x220), 38, 4)
    m.write(config + 0x20, 38, 4)
    m.write(config + field(pe, 0x640), 1, 4)  # rumble preference on
    m.write(info + 0x34, 38, 4)
    m.write(preferences + 0xd4, int(haptics_enabled), 1)
    if getattr(pe, 'haptics_november', False):
        # Actual cache-info -> config copy fragment. The cache-info identity
        # and caps are synthetic, NOT evidence of RemoteHID's real parsing.
        cached, source, slot = [m.alloc() for _ in range(3)]
        m.write(cached + 8, source)
        m.write(source + 0x30, 38, 4)
        m.write(source + 0x38, caps)
        m.write(slot, config)
        m.write(config + field(pe, 0x230), 0)
        m.reg('r12', cached)
        m.reg('r15', slot)
        m.allowed = [(0x13860485d, 0x138604897)]
        m.run(0x13860485d, stops=(0x138604897,))
        if m.read(config + field(pe, 0x230)) != caps:
            raise AssertionError('Cache-to-config caps copy differs')
    m.stubs[address(pe, 0x1385f1850)] = lambda: m.ret()
    m.allowed = [(address(pe, 0x138665a20),address(pe, 0x138665c3d)), (address(pe, 0x1386571d0),address(pe, 0x1386572ef)),
                 (address(pe, 0x1386731d0),address(pe, 0x138673470))]
    m.run(address(pe, 0x138665a20),config,info,preferences)
    slot = m.alloc()
    m.reg('r12', slot)
    m.reg('rdi', 0)
    start, end = address(pe, 0x13860edae), address(pe, 0x13860edbd)
    m.allowed = [(start, end)]
    m.run(start, config, stops=(end,))
    pulse_enabled = bool(m.read(config+field(pe, 0x649),1))
    if bool(m.read(slot+0x1d1,1)) != pulse_enabled:
        raise AssertionError('Config-to-slot pulse enable copy differs')
    return dict(config_caps_override=hex(caps), haptics_preference=haptics_enabled,
                pulse_enabled=pulse_enabled, config_to_slot_fragment=True,
                cache_to_config_fragment=getattr(pe, 'haptics_november', False),
                rumble_enabled=bool(m.read(config+field(pe, 0x648),1)))


def route(pe, pulse_enabled=True, flags=0, side=0, duration=1000,
          enhanced=True, writes_enabled=True, api_entry=False, valid_handle=True):
    m = rumble.gyro.Machine(pe)
    obj, handler, hvtable, device, dvtable = [m.alloc() for _ in range(5)]
    parent = m.alloc(0x110000)
    m.write(parent, address(pe, 0x1392d6370))
    io = m.alloc()
    m.write(parent + 0x10922d, 1, 1)
    m.write(parent + 0x180, io)
    m.write(io, address(pe, 0x1392d6218))  # original CHIDIOThread vtable
    m.write(io + 0x1d1, int(pulse_enabled), 1)
    m.write(obj, address(pe, 0x1392ea3d0))  # original Generic abstraction vtable
    m.write(obj + 8, 38, 4)
    m.write(obj + 0xac, 38, 4)  # copied GetInfo controller-type field
    m.write(obj + 0xb8, 1, 1)   # copied GetInfo valid byte
    m.write(obj + 0x4f8, 0x057e, 2)
    m.write(obj + 0x4fa, 0x2009, 2)
    m.write(obj + 0x588, int(enhanced), 1)
    m.write(address(pe, 0x13980d070), int(writes_enabled), 4)
    m.write(obj + 0x10, handler)
    m.write(handler, hvtable)
    m.write(handler + 0x10, device)
    m.write(hvtable + 0x20, address(pe, 0x13867f7f0))
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
                   address(pe, 0x138600690): lambda: m.ret(0 if valid_handle else -1),
                   address(pe, 0x1388aa6c0): lambda: m.ret(m.alloc(m.reg('rcx'))),
                   address(pe, 0x1381a85e0): lambda: m.ret(1),
                   address(pe, 0x1385f1710): enqueue})
    for pc, callback, stub in (
        (address(pe, 0x1386214de), lambda: m.ret(1), 0x480010),
        (address(pe, 0x138675e1c), lambda: m.ret(), 0x480020),
        (address(pe, 0x138675e70), lambda: m.ret(), 0x480030),
        (address(pe, 0x13867cd22), lambda: m.ret(100000), 0x480040),
        (address(pe, 0x13867cd35), lambda: m.ret(1000), 0x480050)):
        rumble.indirect(m, pe, pc, callback, stub)
    m.allowed = [(address(pe, 0x138621620), address(pe, 0x13862165b)), (address(pe, 0x1386214c0), address(pe, 0x13862158c)),
                 (address(pe, 0x1385e7520), address(pe, 0x1385e759e)), (address(pe, 0x138617a90), address(pe, 0x138617b61)),
                 (address(pe, 0x138675e00), address(pe, 0x138675e89)), (address(pe, 0x138673260), address(pe, 0x1386733eb)),
                 (address(pe, 0x13867cce0), address(pe, 0x13867ce99)), (address(pe, 0x1386746f0), address(pe, 0x13867475b)),
                 (address(pe, 0x13867f7f0), address(pe, 0x13867f7fc))]
    if api_entry:
        m.allowed += [(address(pe, 0x138721fd0), address(pe, 0x138721fde)),
                      (address(pe, 0x138621660), address(pe, 0x1386216f4))]
    # Windows x64 args: slot, side, duration, off, repeat, flags.
    m.write(0x4f0008 + 0x28, 0, 2)
    m.write(0x4f0008 + 0x30, 1, 2)
    m.write(0x4f0008 + 0x38, flags, 4)
    if api_entry:
        adapter = m.alloc()
        m.write(adapter + 8, parent)
        m.run(address(pe, 0x138721fd0), adapter, 0xfeed, side, duration)
    else:
        m.run(address(pe, 0x138621620), parent, 0, side, duration)
    for work in queued:
        m.run(address(pe, 0x138617a90), work, obj)
    return dict(pulse_enabled=pulse_enabled, flags=flags, side=side, duration=duration,
                enhanced=enhanced, writes_enabled=writes_enabled,
                api_entry=api_entry, synthetic_valid_handle=valid_handle,
                queued=len(queued), commands=commands)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    raw = args.host.read_bytes()
    host_sha = rumble.hashlib.sha256(raw).hexdigest()
    if host_sha not in (rumble.HOST_SHA, NOVEMBER_SHA):
        raise ValueError(f'Unrecognized binary: {args.host}')
    pe = rumble.pefile.PE(data=raw)
    pe.haptics_november = host_sha == NOVEMBER_SHA
    cases = []

    def expected_pulse(side):
        # Independent 69B envelope for the fixed Nintendo pulse, including target.
        native = ('1000000005c000014040' if side == 0 else '100000014040000005c0')
        return [(bytes.fromhex('0d0a000000' + native) + bytes(54)).hex()]

    for enabled, flags in ((True,0), (False,0), (False,1), (False,2)):
        for side in (0,1):
            case = route(pe, pulse_enabled=enabled, flags=flags, side=side)
            should_queue = enabled or bool(flags & 3)
            if case['queued'] != int(should_queue):
                raise AssertionError(case)
            expected = expected_pulse(side) if should_queue else []
            if case['commands'] != expected:
                raise AssertionError(case)
            cases.append(case)
    for enhanced, writes in ((False,True),(True,False),(False,False)):
        case = route(pe,enhanced=enhanced,writes_enabled=writes)
        if case['queued'] != 1 or case['commands']:
            raise AssertionError(case)
        cases.append(case)
    for enabled in (False, True):
        for side in (0, 1):
            case = route(pe, pulse_enabled=enabled, side=side, api_entry=True)
            if case['queued'] != int(enabled) or case['commands'] != (expected_pulse(side) if enabled else []):
                raise AssertionError(case)
            cases.append(case)
    case = route(pe, api_entry=True, valid_handle=False)
    if case['queued'] or case['commands']:
        raise AssertionError(case)
    cases.append(case)
    for duration in (1, 65535):
        for side in (0, 1):
            case = route(pe, duration=duration, side=side, api_entry=True)
            if case['queued'] != 1 or case['commands'] != expected_pulse(side):
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
    report = dict(passed=True,host_sha256=host_sha,cases=cases,configuration_gates=configs,
                  original_addresses={
                      'active_config': hex(address(pe, 0x138665a20)),
                      'config_to_slot_fragment': hex(address(pe, 0x13860edae)),
                      'steaminput006_pulse_adapter': hex(address(pe, 0x138721fd0)),
                      'steaminput_pulse_core': hex(address(pe, 0x138621660)),
                      'pulse_enqueue': hex(address(pe, 0x1386214c0)),
                      'pulse_work_item': hex(address(pe, 0x138617a90)),
                      'nintendo_pulse_producer': hex(address(pe, 0x13867cce0))},
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
