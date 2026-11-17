#!/usr/bin/env python3
"""Execute Valve Steam Link's pinned SDL ordinary Switch encoder offline.

Original high/low amplitude lookups execute exhaustively for all uint16 inputs.
Original complete encoding runs on boundary and deterministic random pairs.
Only SDL clock and final WritePacket are terminals; no API dispatch, duration,
30ms pacing, HID transport or motors execute. Requires the supplied SDL binary.
"""
import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn import x86_const as x86

SDL_SHA = "dda2c0b8617e63f6824aad07b7917c0cef58dceb8c0785faa783cbc99c1bbdb4"
BASE = 0x40000000
STOP = 0x4ff000


def audit(path, oracle, decoder=None, reference=None):
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SDL_SHA:
        raise ValueError("Unrecognized Valve SDL binary")
    u = Uc(UC_ARCH_X86, UC_MODE_64)
    u.mem_map(BASE, (len(raw) + 0x200000 + 4095) & ~4095)
    for segment in ELFFile(io.BytesIO(raw)).iter_segments():
        if segment["p_type"] == "PT_LOAD":
            u.mem_write(BASE + segment["p_vaddr"], segment.data())
    u.mem_map(0x100000, 0x400000)
    u.reg_write(x86.UC_X86_REG_FS_BASE, 0x120000)
    u.mem_write(0x120028, struct.pack("<Q", 0x12345678))
    context, device = 0x130000, 0x131000
    packets = []

    def reg(name, value=None):
        number = getattr(x86, "UC_X86_REG_" + name.upper())
        if value is not None:
            u.reg_write(number, value)
        return u.reg_read(number)

    def ret(value):
        reg("rax", value)
        stack = reg("rsp")
        reg("rip", struct.unpack("<Q", u.mem_read(stack, 8))[0])
        reg("rsp", stack + 8)

    def clock(u, pc, size, data):
        ret(100000)

    def write_packet(u, pc, size, data):
        packets.append(bytes(u.mem_read(reg("rsi"), reg("rdx"))))
        ret(1)

    u.hook_add(UC_HOOK_CODE, clock, begin=BASE+0x117fc0, end=BASE+0x117fc0)
    u.hook_add(UC_HOOK_CODE, write_packet, begin=BASE+0x22e4c0, end=BASE+0x22e4c0)

    def call(address, *args, budget=100000):
        reg("rsp", 0x4f0008)
        u.mem_write(0x4f0008, struct.pack("<Q", STOP))
        for name, value in zip(("rdi", "rsi", "rdx"), args):
            reg(name, value)
        u.emu_start(address, STOP, count=budget)
        if reg("rip") != STOP:
            raise AssertionError("Original SDL execution exhausted its instruction budget")

    # Execute an assembly loop around the original two lookup functions. No
    # per-instruction Python hook replaces or computes the lookup algorithm.
    with tempfile.TemporaryDirectory(prefix="nsl-sdl-rumble-") as temp:
        directory = Path(temp)
        (directory / "batch.s").write_text(f'''.intel_syntax noprefix
.text
.global batch
batch:
push rbx
push r12
push r13
push r14
push r15
mov rbx, 0x200000
xor r12d, r12d
mov r13, {BASE+0x22e260}
mov r14, {BASE+0x22e2f0}
again:
mov edi, r12d
call r13
mov [rbx+r12*4], ax
mov edi, r12d
call r14
mov [rbx+r12*4+2], ax
inc r12d
cmp r12d, 65536
jb again
pop r15
pop r14
pop r13
pop r12
pop rbx
ret
''')
        subprocess.run(["as", "--64", "batch.s", "-o", "batch.o"], cwd=directory, check=True)
        subprocess.run(["objcopy", "-O", "binary", "-j", ".text", "batch.o", "batch.bin"],
                       cwd=directory, check=True)
        u.mem_write(0x4d0000, (directory / "batch.bin").read_bytes())
        call(0x4d0000, budget=500000000)
    coefficients = bytes(u.mem_read(0x200000, 65536*4))
    entries = list(struct.iter_unpack("<HH", coefficients))

    def expected_word(low, high):
        if not low and not high:
            return 0x40400100
        high_code, low_code = entries[high][0], entries[low][1]
        return 0x74 | high_code << 8 | (0x3d | ((low_code >> 8) & 0x80)) << 16 | (low_code & 255) << 24

    pairs = [(0, 0), (1, 0), (0, 1), (65535, 0), (0, 65535), (65535, 65535), (65535, 32768), (0x1234, 0xabcd)]
    thresholds = [i for i in range(1, 65536) if entries[i] != entries[i-1]]
    for t in thresholds:
        for value in (t-1, t, min(65535, t+1)):
            pairs.extend([(value, 0), (0, value)])
    rng = random.Random(0x53444c)
    pairs.extend((rng.randrange(65536), rng.randrange(65536)) for _ in range(512))
    u.mem_write(device + 0x20, struct.pack("<HH", 0x057e, 0x2009))
    examples = []
    for low, high in pairs:
        u.mem_write(context, bytes(0x200))
        u.mem_write(context, struct.pack("<Q", device))
        before = len(packets)
        call(BASE+0x230380, context, low, high)
        if len(packets) != before + 1:
            raise AssertionError("Original Switch encoder omitted its output")
        packet = packets[-1]
        word = struct.pack("<I", expected_word(low, high))
        if packet != bytes([16, 0]) + word * 2:
            raise AssertionError((low, high, packet.hex(), word.hex()))
        if len(examples) < 8:
            examples.append(dict(low=low, high=high, report=packet.hex()))
    # Cover every low input and every high input in independent lanes, plus
    # special zero branches and arbitrary pair combinations from above.
    corpus = [(i, 65535-i) for i in range(65536)] + pairs
    expected = b"".join(struct.pack("<I", expected_word(*pair)) for pair in corpus)
    if oracle:
        inputs = b"".join(struct.pack("<HH", *pair) for pair in corpus)
        actual = subprocess.run([str(oracle.resolve())], input=inputs, capture_output=True, check=True).stdout
        if actual != expected:
            mismatch = next((i for i in range(min(len(actual), len(expected))) if actual[i] != expected[i]), 0)
            raise AssertionError(f"production ordinary mapping mismatch at pair {mismatch//4}")
    decoder_samples, max_error = 0, 0.0
    if decoder and reference:
        # Every ordinary word seeds a relative native sequence. This catches
        # approximate amplitude/frequency history even when the ordinary
        # output itself looks plausible.
        relative = 0xc0000000 | 23<<25 | 25<<20 | 17<<15 | 31<<10 | 24<<5 | 24
        stream = b"".join(expected[i:i+4] + struct.pack("<I", relative) for i in range(0, len(expected), 4))
        a = subprocess.run([str(reference.resolve())], input=stream, capture_output=True, check=True).stdout.splitlines()
        b = subprocess.run([str(decoder.resolve())], input=stream, capture_output=True, check=True).stdout.splitlines()
        if len(a) != len(b) or len(a) != len(corpus)*2:
            raise AssertionError("Missing mixed ordinary/native decoder records")
        for index, (left, right) in enumerate(zip(a, b)):
            x, y = list(map(float, left.split())), list(map(float, right.split()))
            if len(x) != len(y) or x[0] != y[0]:
                raise AssertionError(f"Mixed decoder count mismatch at word {index}")
            decoder_samples += int(x[0])
            for want, got in zip(x[1:], y[1:]):
                if not math.isfinite(got) or not math.isclose(want, got, rel_tol=1e-6, abs_tol=1e-8):
                    raise AssertionError(f"Mixed decoder parameter mismatch at word {index}")
                max_error = max(max_error, abs(want-got))
    return dict(passed=True, sdl_sha256=SDL_SHA, amplitude_inputs=65536,
                original_full_encoder_pairs=len(pairs), production_pairs=len(corpus) if oracle else 0,
                mixed_decoder_samples=decoder_samples, max_absolute_float_error=max_error,
                coefficients_sha256=hashlib.sha256(coefficients).hexdigest(), examples=examples,
                boundary="Fixed Valve SDL original lookup/encoding instructions, synthetic Pro context, "
                "clock and WritePacket terminals. No duration/pacing, transport or motor proof.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdl", type=Path, required=True)
    parser.add_argument("--oracle", type=Path)
    parser.add_argument("--decoder", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if bool(args.decoder) != bool(args.reference):
        parser.error("--decoder and --reference must be used together")
    report = audit(args.sdl, args.oracle, args.decoder, args.reference)
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
