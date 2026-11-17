#!/usr/bin/env python3
"""Compare production Switch haptics IPC serialization to installed libnx.

Cross-compile the real io_send() source without NSL_RUMBLE_TEST, link unchanged
hid.o from libnx, and execute both ARM64 paths. Only TLS, memcpy/memset,
AppletResourceUserId and the final svcSendSyncRequest are terminals. No Switch
connection, IPC acceptance, service lifetime, timing or motor proof is implied.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
PREFIX = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-"
ARCHIVE = Path("/opt/devkitpro/libnx/lib/libnx.a")
ARUID = 0x1111222233334444
HANDLE = 0x1234
TLS = 0x10010000
STOP = 0x101ff000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def audit():
    with tempfile.TemporaryDirectory(prefix="nsl-haptics-ipc-") as temp:
        directory = Path(temp)
        subprocess.run([PREFIX + "ar", "x", str(ARCHIVE), "hid.o"], cwd=directory, check=True)
        # Include the production translation unit; section GC discards SDL,
        # worker and applet lifecycle code that is not part of this audit.
        source = '#include "' + str(ROOT / "app/platforms/switch/rumble.c") + '"\n' + r'''
Result audit_private(const HidVibrationDeviceHandle *handles, const sl_hd_value *values,
                      unsigned count, unsigned style) {
    native_io io = {.service = {.session = 0x1234, .own_handle = 1,
                                .pointer_buffer_size = 0x100}, .open = true};
    binding b = {.count = count, .style = style};
    for (unsigned i = 0; i < count; ++i) b.handles[i] = handles[i];
    resource_id = 0x1111222233334444ull;
    return io_send(&io, &b, values);
}
'''
        (directory / "audit.c").write_text(source)
        subprocess.run([PREFIX + "gcc", "-O2", "-DNDEBUG", "-D__SWITCH__", "-DNSL_DIAGNOSTICS=0",
                        "-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft",
                        "-ffunction-sections", "-fdata-sections", "-I" + str(ROOT / "app/src"),
                        "-I/opt/devkitpro/libnx/include", "-I/opt/devkitpro/portlibs/switch/include/SDL2",
                        "-c", "audit.c", "-o", "audit.o"], cwd=directory, check=True)
        terminals = ("svcSendSyncRequest", "appletGetAppletResourceUserId", "memcpy", "memset")
        (directory / "terminal.s").write_text("\n".join(
            f'.section .text.{name},"ax"\n.global {name}\n{name}: ret' for name in terminals) + "\n")
        subprocess.run([PREFIX + "as", "terminal.s", "-o", "terminal.o"], cwd=directory, check=True)
        subprocess.run([PREFIX + "ld", "--gc-sections", "-Ttext=0x100000", "-e", "audit_private",
                        "--undefined=hidSendVibrationValues", "audit.o", "hid.o", "terminal.o",
                        "-o", "audit.elf"], cwd=directory, check=True)
        binary = (directory / "audit.elf").read_bytes()
        elf = ELFFile(io.BytesIO(binary))
        symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()
                   if s["st_shndx"] != "SHN_UNDEF"}
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0x100000, 0x400000)
        u.mem_map(0x10000000, 0x200000)
        for segment in elf.iter_segments():
            if segment["p_type"] == "PT_LOAD":
                u.mem_write(segment["p_vaddr"], segment.data())
        captured = []

        def reg(n):
            return u.reg_read(UC_ARM64_REG_X0 + n)

        def ret(value):
            u.reg_write(UC_ARM64_REG_X0, value)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

        def hook(u, pc, size, data):
            if pc == symbols["svcSendSyncRequest"]:
                raw = bytes(u.mem_read(TLS, 0x100))
                first, second = struct.unpack_from("<II", raw)
                count = (first >> 16) & 15
                if (first & 0xffff) != 4 or second >> 31 or first >> 20:
                    raise AssertionError("Unexpected HIPC type, special header or buffer mode")
                payloads = []
                for index in range(count):
                    a, low = struct.unpack_from("<II", raw, 8 + index * 8)
                    address = low | ((a >> 12 & 15) << 32) | ((a >> 6 & 63) << 36)
                    payloads.append(bytes(u.mem_read(address, a >> 16)).hex())
                start = (8 + count * 8 + 15) & ~15
                magic, version, command, token, aruid = struct.unpack_from("<IIIIQ", raw, start)
                captured.append(dict(handle=reg(0), header=[first, second], magic=magic,
                                     version=version, command=command, token=token,
                                     aruid=aruid, payloads=payloads))
                u.emu_stop()
            elif pc == symbols["appletGetAppletResourceUserId"]:
                ret(ARUID)
            elif pc == symbols.get("memcpy", -1):
                destination, source, length = reg(0), reg(1), reg(2)
                u.mem_write(destination, bytes(u.mem_read(source, length)))
                ret(destination)
            elif pc == symbols.get("memset", -1):
                destination, value, length = reg(0), reg(1), reg(2)
                u.mem_write(destination, bytes([value & 255]) * length)
                ret(destination)
            else:
                instruction = struct.unpack("<I", u.mem_read(pc, 4))[0]
                if instruction & ~31 == 0xD53BD060:  # mrs xN, tpidrro_el0
                    u.reg_write(UC_ARM64_REG_X0 + (instruction & 31), TLS)
                    u.reg_write(UC_ARM64_REG_PC, pc + 4)

        u.hook_add(UC_HOOK_CODE, hook)
        handles, values, expected_values = 0x10030000, 0x10030100, 0x10030200
        handle_data = struct.pack("<II", 0x01020304, 0x05060708)
        u.mem_write(handles, handle_data)
        # Service session representation produced by smGetService, without
        # actually opening a service or claiming activation/permission.
        u.mem_write(symbols["g_hidSrv"], struct.pack("<IIIHH", HANDLE, 1, 0, 0x100, 0))

        def call(name, *args):
            u.mem_write(TLS, bytes(0x100))
            u.reg_write(UC_ARM64_REG_SP, 0x101f0000)
            u.reg_write(UC_ARM64_REG_LR, STOP)
            for i in range(8):
                u.reg_write(UC_ARM64_REG_X0 + i, args[i] if i < len(args) else 0)
            before = len(captured)
            u.emu_start(symbols[name], STOP, count=100000)
            if len(captured) != before + 1:
                raise AssertionError("Native call did not reach exactly one IPC terminal")
            return captured[-1]

        rows = []
        for name, count, style in (("pro", 2, 1), ("handheld", 2, 2), ("joydual", 2, 4),
                                    ("joyleft", 1, 8), ("joyright", 1, 16), ("zero", 2, 1)):
            motors = [(0.125, 80, 0.25, 640), (0.5, 160, 0.75, 320)]
            if name == "zero":
                motors = [(0, 160, 0, 320)] * 2
            raw_values = b"".join(struct.pack("<ffff", *v) for v in motors)
            wanted = raw_values[16:32] if name == "joyright" else raw_values[:16*count]
            u.mem_write(values, raw_values)
            u.mem_write(expected_values, wanted)
            actual = call("audit_private", handles, values, count, style)
            original = call("hidSendVibrationValues", handles, expected_values, count)
            if actual != original:
                raise AssertionError((name, actual, original))
            if actual["command"] != 206 or actual["aruid"] != ARUID or actual["handle"] != HANDLE:
                raise AssertionError("Wrong HID command, process resource or session")
            if actual["magic"] != 0x49434653 or actual["payloads"] != [handle_data[:count*4].hex(), wanted.hex()]:
                raise AssertionError("HID descriptor/payload mismatch")
            rows.append(dict(case=name, count=count, style=style, packet=actual))
        return dict(passed=True, cases=rows, libnx_archive_sha256=sha(ARCHIVE.read_bytes()),
                    libnx_hid_object_sha256=sha((directory / "hid.o").read_bytes()),
                    production_source_sha256=sha((ROOT / "app/platforms/switch/rumble.c").read_bytes()),
                    linked_elf_sha256=sha(binary),
                    boundary="Production ARM64 io_send and original libnx IPC 206 serialization; "
                    "SVC/TLS/memory/resource terminals. No real service, response, timing, permission or motor proof.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = audit()
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
