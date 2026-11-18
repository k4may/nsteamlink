#!/usr/bin/env python3
"""Execute hash-pinned Steam hosts' pulse enqueue, worker and output gates.

Original CSteamController forwarding, CHIDIOThread enqueue/constructor,
CPulseHapticWorkItem, Generic GetInfo/pulse and protocol forwarding execute.
SteamInput006 legacy pulse adapter/core execute with synthetic handle lookup.
Original active-config application also checks capability bit 0x2000 and the
haptics preference. Config-to-slot copy instructions execute in isolation;
November cache-to-config copy instructions also execute in isolation.
RemoteHID caps parsing and real configuration initialization are not executed.
November RPC factory/destructor, pulse marshaler and server thunk also execute;
their engine context, buffer primitives, synchronous IPC/reply are substitutes.
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
          enhanced=True, writes_enabled=True, api_entry=False, valid_handle=True,
          stop_entry=False):
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
    if stop_entry:
        if not getattr(pe, 'haptics_november', False):
            raise ValueError('Stop caller is only identified in the November binary')
        # Original caller emits duration=0, flags=2 for both sides. Its separate
        # optional settings-refresh predicate is a substitute returning false.
        m.stubs[m.read(address(pe, 0x1392d6370) + 0x58)] = lambda: m.ret(0)
        m.allowed += [(0x13862a810, 0x13862a830), (0x13862a938, 0x13862a999)]
    # Windows x64 args: slot, side, duration, off, repeat, flags.
    m.write(0x4f0008 + 0x28, 0, 2)
    m.write(0x4f0008 + 0x30, 1, 2)
    m.write(0x4f0008 + 0x38, flags, 4)
    if stop_entry:
        m.run(0x13862a810, parent, 0)
    elif api_entry:
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
                stop_entry=stop_entry,
                queued=len(queued), commands=commands)


def preference_evidence(pe):
    """Original preference constructor and SetControllerHapticsSetting setter.

    Current binary only. String allocation, preference lookup/persistence and
    allocator cleanup are substitutes; no live user's setting or UI is read.
    """
    if not getattr(pe, 'haptics_november', False):
        return None
    m = rumble.gyro.Machine(pe)
    preferences = m.alloc()
    m.stubs[0x138d243f0] = lambda: m.ret()  # string initializer only
    m.allowed = [(0x13860e910, 0x13860e9ff)]
    m.run(0x13860e910, preferences)
    default = m.read(preferences + 0x8c, 1)
    if default != 1:
        raise AssertionError('Unexpected constructor haptics default')
    obj, vtable, service_holder, service, service_vtable = [m.alloc() for _ in range(5)]
    m.write(obj, vtable)
    m.write(vtable + 0x590, 0x480000)
    m.write(0x1391b74d8, service_holder)
    m.write(service_holder, service)
    m.write(service, service_vtable)
    m.write(service_vtable + 0x38, 0x480010)
    saved = []

    def persist():
        saved.append(m.read(m.reg('r9') + 0xd4, 1))
        m.ret(1)

    m.stubs.update({0x13861b910: lambda: m.ret(1),  # preference lookup succeeds
                    0x480000: persist, 0x480010: lambda: m.ret()})
    m.allowed += [(0x138632090, 0x13863214e)]
    for enabled in (0, 1):
        m.run(0x138632090, obj, 0, enabled)
        if m.reg('rax') != 1 or saved[-1] != enabled:
            raise AssertionError('Haptics setter did not persist supplied boolean')
    return dict(constructor_haptics_default=bool(default), setter_saved=saved,
                constructor='0x13860e910', setter='0x138632090',
                constructor_field='0x8c', aggregate_preference_field='0xd4',
                boundary=preference_evidence.__doc__.strip())


def simple_haptic_evidence(pe):
    """Current Generic abstraction's simple-event worker; not other backends."""
    if not getattr(pe, 'haptics_november', False):
        return None
    m = rumble.gyro.Machine(pe)
    work, obj = m.alloc(), m.alloc()
    m.write(obj, 0x139318fd0)
    m.write(obj + 8, 38, 4)
    m.write(work + 0x78, 3, 4)  # both locations
    m.write(work + 0x7c, 100, 1)
    m.write(work + 0x7e, 200, 1)
    m.allowed = [(0x13862dd60, 0x13862ddc6), (0x1380cf3d0, 0x1380cf3d3)]
    m.run(0x13862dd60, work, obj)
    if 0x1380cf3d0 not in m.visited:
        raise AssertionError('Generic simple-event target was not executed')
    return dict(worker='0x13862dd60', abstraction_slot='0x160',
                resolved_function='0x1380cf3d0', function='mov al, 1; ret',
                no_hid_output=True, boundary=simple_haptic_evidence.__doc__.strip())


def context_enumeration_evidence(pe):
    """Original app association filter with synthetic cache, no live enumeration."""
    if not getattr(pe, 'haptics_november', False):
        return None
    m = rumble.gyro.Machine(pe)
    obj, out = m.alloc(0x11c000), m.alloc()
    handle = 0x112233445566
    m.write(obj + 0x1116d5, 1, 1)
    m.write(obj + 0x1116d6, handle)
    m.write(obj + 0x1114d5, 769, 4)
    m.write(obj + 0x1114d9, 413080, 4)
    for pc, stub in ((0x13861a9c5, 0x480000), (0x13861acf4, 0x480010)):
        rumble.indirect(m, pe, pc, lambda: m.ret(), stub)
    m.allowed = [(0x13861a9a0, 0x13861ad0b)]
    rows = []
    for appid, expected in ((1049320, 0), (0, 1), (769, 1), (413080, 1)):
        m.run(0x13861a9a0, obj, appid, out)
        count = m.reg('rax')
        if count != expected or (count and m.read(out) != handle):
            raise AssertionError('Unexpected original app association filtering')
        rows.append(dict(appid=appid, count=count))
    return dict(cases=rows, original_function='0x13861a9a0',
                boundary=context_enumeration_evidence.__doc__.strip())


def rpc_bridge_evidence(pe):
    """Original November factory/destructor, pulse marshaler and server thunk.

    Buffer primitives, engine/IPC, memory allocation and reply are substitutes.
    This proves ABI/serialization only, not a running Steam RPC transaction.
    """
    if not getattr(pe, 'haptics_november', False):
        return None
    m = rumble.gyro.Machine(pe)
    engine = m.alloc()
    freed = []
    m.stubs.update({0x1389a7fe0: lambda: m.ret(engine),
                    0x1388c2de0: lambda: m.ret(1),
                    0x1388cea80: lambda: m.ret(m.alloc(m.reg('rcx'))),
                    0x1388cead0: lambda: (freed.append((m.reg('rcx'), m.reg('rdx'))), m.ret())})
    m.allowed = [(0x13876d560, 0x13876d64a), (0x1387483d0, 0x1387483fb)]
    m.run(0x13876d560, 0, 11)
    obj = m.reg('rax')
    assert m.read(obj) == 0x139369980
    assert (m.read(obj + 8, 4), m.read(obj + 12, 4), m.read(obj + 24)) == (0, 11, engine + 8)
    wire = bytearray()
    acknowledged = []
    reply = m.alloc()

    def init_buffer():
        m.write(m.reg('rcx') + 12, 0x80000000, 4)  # supplied non-owning buffer
        m.ret()

    def append_byte():
        wire.append(m.reg('rdx') & 255)
        m.ret()

    def append_bytes():
        wire.extend(m.u.mem_read(m.reg('rdx'), m.reg('r8')))
        m.ret()

    def response_type():
        m.write(m.reg('rdx'), 11, 1)
        m.ret(1)

    m.stubs.update({0x138d1aa40: init_buffer, 0x138d1d2a0: append_byte,
                    0x138d1c6e0: append_bytes, 0x1388c31c0: lambda: m.ret(reply),
                    0x138d1b5c0: response_type,
                    0x1388c26e0: lambda: (acknowledged.append(m.reg('rdx')), m.ret())})
    m.allowed += [(0x1388b6b60, 0x1388b6d40)]
    # Actual seven-argument x64 call; non-default fields expose swapped arguments.
    m.write(0x4f0008 + 0x28, 17, 2)
    m.write(0x4f0008 + 0x30, 3, 2)
    m.write(0x4f0008 + 0x38, 0, 4)
    m.run(0x1388b6b60, obj, 4, 1, 1000)
    expected = (bytes([1, 0x29]) + rumble.struct.pack('<IIIIHHHI',
                0, 0x4f277b59, 4, 1, 1000, 17, 3, 0) +
                rumble.struct.pack('<I', 0x545f70e4))
    assert bytes(wire) == expected, wire.hex()
    assert acknowledged == [reply]
    pulse_wire = wire.hex()
    parent, table = m.alloc(), m.alloc()
    m.write(parent, table)
    m.write(table + 0x50, 0x480070)
    forwarded = []

    def forwarded_pulse():
        sp = m.reg('rsp')
        forwarded.append([m.reg('rcx'), m.reg('rdx'), m.reg('r8'), m.reg('r9'),
                          m.read(sp + 0x28, 2), m.read(sp + 0x30, 2), m.read(sp + 0x38, 4)])
        m.ret()

    m.stubs[0x480070] = forwarded_pulse
    m.allowed += [(0x1389c2e90, 0x1389c2eaf)]
    m.write(0x4f0008 + 0x28, 17, 2)
    m.write(0x4f0008 + 0x30, 3, 2)
    m.write(0x4f0008 + 0x38, 0, 4)
    m.run(0x1389c2e90, parent - 16, 4, 1, 1000)
    assert forwarded == [[parent, 4, 1, 1000, 17, 3, 0]]
    details, output = m.alloc(0x1800), m.alloc()
    index = 4
    supplied = bytearray(88)
    rumble.struct.pack_into('<Q', supplied, 8, 0x836bff)
    rumble.struct.pack_into('<Q', supplied, 0x20, 0x0057e2009f4e844f)
    rumble.struct.pack_into('<I', supplied, 0x34, 38)
    supplied[0x40] = 1
    wire.clear()
    reads = []

    def read_reply():
        size = m.reg('r8')
        reads.append(size)
        m.u.mem_write(m.reg('rdx'), b'\x01' if size == 1 else bytes(supplied))
        m.ret(1)

    m.stubs[0x138745c30] = read_reply
    m.allowed += [(0x13877e0b0, 0x13877e26f)]
    m.run(0x13877e0b0, obj, index, output)
    assert m.reg('rax') & 255 == 1 and reads == [1, 88]
    assert bytes(m.u.mem_read(output, 88)) == bytes(supplied)
    assert bytes(wire) == bytes([1, 0x29]) + rumble.struct.pack('<IIII', 0, 0xc3fe8503, index, 0xc93f17b1)
    assert acknowledged == [reply, reply]
    details_wire = wire.hex()
    m.u.mem_write(details + 0xfe8 + index * 88, bytes(supplied))
    m.allowed += [(0x138619090, 0x13861912b)]
    m.run(0x138619090, details, index, output)
    assert m.reg('rax') & 255 == 1
    assert bytes(m.u.mem_read(output, 88)) == bytes(supplied)
    m.run(0x1387483d0, obj, 1)
    assert freed == [(obj, 32)]
    return dict(passed=True, factory_pipe=0, factory_user=11,
                vtable_rva='1369980', pulse_rpc_wire=pulse_wire, details_rpc_wire=details_wire,
                server_arguments=forwarded[0][1:], details_bytes=88,
                owned_map_freed=True, actual_steam_ipc_verified=False)


def host_state_layout_evidence(pe):
    """Original scalar-address/copy fragments; not remote process enumeration.

    String allocation, object locks and controller validity predicate are stubs.
    Heap contents are synthetic; no live user's preferences or gates are read.
    """
    if not getattr(pe, 'haptics_november', False):
        return None
    m = rumble.gyro.Machine(pe)
    m.write(0x139854717, 1, 1)
    m.allowed = [(0x1389a8009, 0x1389a802c)]
    m.run(0x1389a8009, stops=(0x1389a802c,))
    engine = m.reg('rbx')
    assert engine == 0x139854550
    m.reg('rdi', engine)
    m.allowed += [(0x1389a6734, 0x1389a673e)]
    m.run(0x1389a6734, stops=(0x1389a673e,))
    assert m.read(engine) == 0x13938f580
    node, parent = m.alloc(), m.alloc(0x11c000)
    m.reg('rdx', node)
    m.reg('r14', parent)
    m.allowed += [(0x1389a7deb, 0x1389a7def)]
    m.run(0x1389a7deb, stops=(0x1389a7def,))
    assert m.read(node + 0x78) == parent
    obj = m.alloc()
    m.write(obj + 0x98, 0x0057e2009f4e844f)
    m.write(obj + 0xb8, 1, 1)
    for pc, stub in ((0x13868e867, 0x480080), (0x13868e884, 0x480090)):
        rumble.indirect(m, pe, pc, lambda: m.ret(), stub)
    m.allowed += [(0x13868e850, 0x13868e898)]
    m.run(0x13868e850, obj)
    assert m.reg('rax') == 0x0057e2009f4e844f
    table, output = m.alloc(), m.alloc()
    m.write(parent, table)
    m.write(table + 0x58, 0x4800a0)
    m.stubs[0x4800a0] = lambda: m.ret(1)
    m.stubs[0x138d243f0] = lambda: m.ret()
    m.write(parent + 0x1f0, 1, 1)
    for pc, stub in ((0x13861bbfd, 0x4800b0), (0x13861bc26, 0x4800c0)):
        rumble.indirect(m, pe, pc, lambda: m.ret(), stub)
    m.allowed += [(0x13861bbb0, 0x13861bc46), (0x1386024d0, 0x138602660)]
    for enabled in (0, 1):
        m.write(parent + 0x230 + 0x8c, enabled, 1)
        m.run(0x13861bbb0, parent, 0, output)
        assert m.reg('rax') & 255 == 1
        assert m.read(output + 0x8c, 1) == enabled
    return dict(passed=True, engine_rva=hex(engine - pe.OPTIONAL_HEADER.ImageBase),
                engine_vtable_rva='138f580', controller_record_offset='0x78',
                unique_handle_offset='0x98', cached_haptics_offset='0x230 + slot*0xe0 + 0x8c',
                unlocked_observation_only=True, actual_host_state_verified=False)


def native_sequence(pe, events, cached_ordinary=0, initial_words=None):
    """Original worker/producer over persistent state and a synthetic ms clock.

    No enqueue/gate/IPC initialization runs here. Original import names are
    Plat_RelativeTicks / Plat_TickDiffMilliSec; their values are substitutes.
    Cached ordinary intensity is a supplied condition, not a live observation.
    """
    if not getattr(pe, 'haptics_november', False):
        return None
    m = rumble.gyro.Machine(pe)
    obj, handler, hvtable, device, dvtable = [m.alloc() for _ in range(5)]
    m.write(obj, 0x139318fd0)
    m.write(obj + 8, 38, 4)
    m.write(obj + 0xac, 38, 4)
    m.write(obj + 0xb8, 1, 1)
    m.write(obj + 0x588, 1, 1)
    m.write(0x139845410, 1, 4)
    m.write(obj + 0x10, handler)
    m.write(handler, hvtable)
    m.write(handler + 0x10, device)
    m.write(hvtable + 0x20, 0x138697ef0)
    m.write(device, dvtable)
    m.write(dvtable + 0x18, 0x480000)
    words = initial_words or bytes.fromhex('0001404000014040')
    m.u.mem_write(obj + 0x54a, words)
    m.write(obj + 0x552, cached_ordinary, 2)
    now, commands = [100000], []

    def record():
        assert m.reg('r8') == 69
        commands.append(bytes(m.u.mem_read(m.reg('rdx'), 69)).hex())
        m.ret(69)

    m.stubs[0x480000] = record
    for pc, callback, stub in (
        (0x13868e51c, lambda: m.ret(), 0x480020),
        (0x13868e570, lambda: m.ret(), 0x480030),
        (0x138695422, lambda: m.ret(now[0]), 0x480040),
        (0x138695435, lambda: m.ret(m.reg('rdx') - m.reg('rcx')), 0x480050)):
        rumble.indirect(m, pe, pc, callback, stub)
    m.allowed = [(0x13862df10, 0x13862dfe1), (0x13868e500, 0x13868e589),
                 (0x13868b960, 0x13868baeb), (0x1386953e0, 0x13869559a),
                 (0x13868cdf0, 0x13868ce5b), (0x138697ef0, 0x138697efc)]
    results = []
    for event in events:
        now[0] = 100000 + event['time_ms']
        work = m.alloc()
        m.write(work + 0x78, event['side'], 1)
        m.write(work + 0x7a, event['duration'], 2)
        m.write(work + 0x7e, 1, 2)
        m.write(work + 0x80, event.get('flags', 0), 2)
        before = len(commands)
        m.run(0x13862df10, work, obj)
        results.append(dict(**event, commands=commands[before:]))
    return dict(cached_ordinary=cached_ordinary, events=results,
                final_words=bytes(m.u.mem_read(obj + 0x54a, 8)).hex(),
                actual_runtime_cache_verified=False)


def sequence_evidence(pe):
    if not getattr(pe, 'haptics_november', False):
        return None
    def event(t, side, duration):
        return dict(time_ms=t, side=side, duration=duration)
    preserve = native_sequence(pe, [event(0,0,1000), event(100,1,1000),
                                    event(200,0,0), event(200,1,0)])
    bodies = [e['commands'][0][10:30] for e in preserve['events']]
    assert bodies == ['1000000005c000014040', '1000000005c0000005c0',
                      '100000014040000005c0', '10000001404000014040']
    short = [event(0,0,0), event(0,1,0), event(150,0,1000),
             event(650,0,0), event(650,1,0), event(800,0,1000)]
    normal = native_sequence(pe, short)
    conflict = native_sequence(pe, short, cached_ordinary=16384)
    assert [len(e['commands']) for e in normal['events']] == [1,1,1,1,1,1]
    assert [len(e['commands']) for e in conflict['events']] == [1,0,0,1,0,0]
    long = native_sequence(pe, [event(0,0,0), event(0,1,0), event(800,0,1000),
                               event(1150,0,0), event(1150,1,0), event(1950,1,1000)],
                           cached_ordinary=16384)
    assert [len(e['commands']) for e in long['events']] == [1,0,1,0,0,1]
    assert long['events'][-1]['commands'][0][10:30] == '1000000005c0000005c0'
    return dict(passed=True, preserves_other_word=preserve,
                short_gap_zero_ordinary=normal, short_gap_nonzero_ordinary=conflict,
                long_gap_nonzero_ordinary=long,
                boundary='Synthetic clocks/cache reproduce conditions; they do not establish the prior live failure cause.')


def request_packet_oracle(pe, path):
    """Compare each real bracket's wire with original code using prior wire state.

    Target/duration/flags are actual logged requests. Other motor words come
    only from the last packet BEFORE the request. Eligibility, clocks and ordinary
    cache are synthetic allow conditions; this is a parameter oracle, not a
    reconstruction of runtime gates or a physical-fidelity certificate.
    """
    rows = [json.loads(s) for s in path.read_text(encoding='utf-8-sig').splitlines() if s.startswith('{')]
    trace, intervals, before, request = {}, [], None, None
    for row in rows:
        if row.get('stage') == 'device_reply':
            for line in row.get('reply', '').splitlines():
                record = json.loads(line)
                if 'serial' in record: trace[record['serial']] = record
    def payload(r):
        b = bytes.fromhex(r.get('wire',''))
        return b if r.get('kind') == 1 and r.get('rc') == 0 and len(b) == 69 and b[:7] == bytes.fromhex('0d0a0000001000') else None
    for row in rows:
        if row.get('stage') == 'snapshot_end' and row.get('label') == 'before_pulse':
            before = row['after_serial']
        elif row.get('stage') == 'pulse_request': request = row
        elif row.get('stage') == 'snapshot_end' and row.get('label') == 'after_pulse' and request:
            prior = [payload(trace[s]) for s in sorted(trace) if s <= before and payload(trace[s])]
            emitted = [payload(trace[s]).hex() for s in sorted(trace) if before < s <= row['after_serial'] and payload(trace[s])]
            if not prior: raise ValueError('Missing pre-request native baseline')
            expected = native_sequence(pe, [dict(time_ms=0, side=request['side'],
                duration=request['duration_us'], flags=request['flags'])], initial_words=prior[-1][7:15])
            predicted = expected['events'][0]['commands']
            intervals.append(dict(route=request['route'], side=request['side'], ordinal=request['ordinal'],
                                  prior_words=prior[-1][7:15].hex(), expected=predicted,
                                  actual=emitted, matched=predicted == emitted))
            request = None
    if not intervals: raise ValueError('No bracketed requests in archive')
    return dict(all_matched=all(i['matched'] for i in intervals), requests=len(intervals),
                intervals=intervals, boundary=request_packet_oracle.__doc__.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--requests', type=Path, help='Optional bracketed real request JSONL parameter oracle')
    args = parser.parse_args()
    raw = args.host.read_bytes()
    host_sha = rumble.hashlib.sha256(raw).hexdigest()
    if host_sha not in (rumble.HOST_SHA, NOVEMBER_SHA):
        raise ValueError(f'Unrecognized binary: {args.host}')
    pe = rumble.pefile.PE(data=raw)
    pe.haptics_november = host_sha == NOVEMBER_SHA
    if args.requests and not pe.haptics_november:
        parser.error('The stateful real-request oracle is pinned to the November host')
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
    if pe.haptics_november:
        neutral = (bytes.fromhex('0d0a00000010000001404000014040') + bytes(54)).hex()
        for enabled in (False, True):
            case = route(pe, pulse_enabled=enabled, stop_entry=True)
            if case['queued'] != 2 or case['commands'] != [neutral, neutral]:
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
                  preference_evidence=preference_evidence(pe),
                  simple_haptic_evidence=simple_haptic_evidence(pe),
                  context_enumeration_evidence=context_enumeration_evidence(pe),
                  rpc_bridge_evidence=rpc_bridge_evidence(pe),
                  host_state_layout_evidence=host_state_layout_evidence(pe),
                  sequence_evidence=sequence_evidence(pe),
                  request_packet_oracle=request_packet_oracle(pe, args.requests) if args.requests else None,
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
