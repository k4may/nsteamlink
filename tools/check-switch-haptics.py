#!/usr/bin/env python3
"""Compare the production decoder to a pinned independent executable.

MissionControl's GPL-2.0-only source is fetched into an ignored build directory
and compiled as a separate reference process, never linked into NSteamLink.
Inputs include Valve's actual pulse, format boundaries, every absolute AM/FM
pair and deterministic stateful random streams. This tests values, not motors.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import urllib.request

REVISION = "71a837fcff422a652bd1326a214f925792ae8449"
SOURCES = {
    "switch_hd_rumble_decoder.cpp": "a61c8415a74d795959c091dd996cb1c285d2b0b0",
    "switch_hd_rumble_decoder.hpp": "3e1bd863b0a30dc61f68b32e682cb4e8d62b48dd",
    "switch_hd_rumble_types.hpp": "84ba772c2c5da5b453975eb4d872d4aa651b95c0",
}
SHIM = """#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
using u8 = uint8_t;
using u32 = uint32_t;
#define PACKED __attribute__((packed))
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define AMS_UNREACHABLE_DEFAULT_CASE() default: __builtin_trap()
"""
DRIVER = """#include "switch_hd_rumble_decoder.hpp"
#include <cstdio>
#include <cstring>
using namespace ams::controller;
int main() {
    SwitchHdRumbleDecoder decoder;
    unsigned char bytes[4];
    while (std::fread(bytes, 1, 4, stdin) == 4) {
        SwitchEncodedVibrationSamples input;
        std::memcpy(&input, bytes, 4);
        SwitchVibrationSamples output{};
        decoder.DecodeSamples(&input, &output);
        std::printf("%u", unsigned(output.count));
        for (unsigned i = 0; i < output.count; ++i) {
            const auto& s = output.samples[i];
            std::printf(" %.9g %.9g %.9g %.9g", s.low_band_amp,
                s.low_band_freq, s.high_band_amp, s.high_band_freq);
        }
        std::putchar('\\n');
    }
    return std::ferror(stdin) ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--cache", type=Path, default=Path("build/haptic-reference"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    for name, digest in SOURCES.items():
        path = args.cache / name
        if not path.exists():
            url = f"https://raw.githubusercontent.com/ndeadly/MissionControl/{REVISION}/mc_mitm/source/controllers/{name}"
            with urllib.request.urlopen(url, timeout=30) as response:
                path.write_bytes(response.read())
        data = path.read_bytes()
        actual = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if actual != digest:
            raise ValueError(f"Reference source hash mismatch: {name}")
    (args.cache / "stratosphere.hpp").write_text(SHIM)
    (args.cache / "driver.cpp").write_text(DRIVER)
    reference = args.cache / "reference"
    subprocess.run(["g++", "-std=c++23", "-O2", "-I" + str(args.cache),
                    str(args.cache / "driver.cpp"),
                    str(args.cache / "switch_hd_rumble_decoder.cpp"),
                    "-o", str(reference)], check=True)
    words = [0x40400100, 0xc0050000, 0x40400100, 0, 0xffffffff,
             0x40000000, 0x80000000, 0xc0000000]
    for am in range(128):
        for fm in range(128):
            words.append(0x40000000 | am << 23 | fm << 16 | am << 9 | fm << 2)
    for cmd in range(32):
        words.extend([0x40400100, 0xc0000000 | cmd << 25 | cmd << 20 |
                      cmd << 15 | cmd << 10 | cmd << 5 | cmd] * 4)
    rng = random.Random(0x10d)
    words.extend(rng.getrandbits(32) for _ in range(65536))
    payload = struct.pack("<" + "I" * len(words), *words)
    expected = subprocess.run([str(reference.resolve())], input=payload,
                              capture_output=True, check=True).stdout.splitlines()
    actual = subprocess.run([str(args.oracle.resolve())], input=payload,
                            capture_output=True, check=True).stdout.splitlines()
    if len(expected) != len(words) or len(actual) != len(words):
        raise AssertionError("Missing decoder records")
    samples = 0
    max_error = 0.0
    for index, (left, right) in enumerate(zip(expected, actual)):
        a, b = list(map(float, left.split())), list(map(float, right.split()))
        if len(a) != len(b) or a[0] != b[0]:
            raise AssertionError((index, hex(words[index]), a, b))
        samples += int(a[0])
        for x, y in zip(a[1:], b[1:]):
            if not math.isfinite(y) or not math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-8):
                raise AssertionError((index, hex(words[index]), a, b))
            max_error = max(max_error, abs(x - y))
    report = {"passed": True, "reference_revision": REVISION,
              "words": len(words), "samples": samples,
              "input_sha256": hashlib.sha256(payload).hexdigest(),
              "max_absolute_float_error": max_error,
              "boundary": "Independent decoder comparison; no sample-timing, IPC or motor proof"}
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
