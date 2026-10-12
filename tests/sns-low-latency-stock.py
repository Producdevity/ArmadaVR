#!/usr/bin/env python3
"""Compare the native encoder with the exact b1977 ARM64 stub in Unicorn 2.1.4.

The stock ELF runs only in emulated memory. Remote-handle acquisition and invoke
are mocked; no Android libraries, device nodes or DSP transport are opened.
"""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import unicorn
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (
    UC_ARM64_REG_X0,
    UC_ARM64_REG_X1,
    UC_ARM64_REG_X2,
    UC_ARM64_REG_X3,
    UC_ARM64_REG_X4,
    UC_ARM64_REG_X5,
    UC_ARM64_REG_SP,
    UC_ARM64_REG_LR,
    UC_ARM64_REG_PC,
    UC_ARM64_REG_TPIDR_EL0,
)

STUB_SHA256 = "893aefcbcb74320fcce5660e2d4c2d88cf403839225638a6f1681636312f700f"

base = 0x100000
stack = 0x600000
tls = 0x400000
mock = 0x500000
data = 0x700000
end = 0x800000
regs = [
    UC_ARM64_REG_X0,
    UC_ARM64_REG_X1,
    UC_ARM64_REG_X2,
    UC_ARM64_REG_X3,
    UC_ARM64_REG_X4,
    UC_ARM64_REG_X5,
]


def execute(
    binary,
    address,
    args,
    retval=0,
    padding=0xCC,
    suid=(0x1122334455667788, 0x99AABBCCDDEEFF00),
):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(base, 0x10000)
    phoff = struct.unpack_from("<Q", binary, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", binary, 54)
    for index in range(phnum):
        kind, flags, offset, virtual, physical, filesize, memsize, align = (
            struct.unpack_from("<IIQQQQQQ", binary, phoff + index * phentsize)
        )
        if kind == 1:
            assert virtual + memsize < 0x10000 and offset + filesize <= len(binary)
            u.mem_write(base + virtual, binary[offset : offset + filesize])
    u.mem_map(stack, 0x10000)
    u.mem_write(stack, bytes([padding]) * 0x10000)
    for page in (tls, mock, data, end):
        u.mem_map(page, 0x1000)
    u.mem_write(base + 0x3430, struct.pack("<Q", base + 0x4008))
    u.mem_write(base + 0x4008, struct.pack("<Q", mock))
    u.mem_write(base + 0x3480, struct.pack("<Q", base + 0x1048))
    u.mem_write(base + 0x3478, struct.pack("<Q", mock + 4))
    u.mem_write(data, struct.pack("<QQ", *suid))
    u.mem_write(data + 0x100, struct.pack("<I", 0xDEADBEEF))
    u.mem_write(tls + 0x28, struct.pack("<Q", 0x55AAFF88BB00DD11))
    u.reg_write(UC_ARM64_REG_TPIDR_EL0, tls)
    u.reg_write(UC_ARM64_REG_SP, stack + 0xFF00)
    u.reg_write(UC_ARM64_REG_LR, end)
    for reg, value in zip(regs, args):
        u.reg_write(reg, value & ((1 << 64) - 1))
    calls = []

    def hook(u, pc, size, context):
        if pc == base + 0x1048:
            u.reg_write(UC_ARM64_REG_X0, 0x5A)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
        elif pc == mock:
            handle = u.reg_read(UC_ARM64_REG_X0)
            scalar = u.reg_read(UC_ARM64_REG_X1)
            pointer = u.reg_read(UC_ARM64_REG_X2)
            ninput = (scalar >> 16) & 255
            noutput = (scalar >> 8) & 255
            assert ninput + noutput <= 2
            buffers = []
            for i in range(ninput + noutput):
                address, length = struct.unpack("<QQ", u.mem_read(pointer + i * 16, 16))
                assert length <= 48
                buffers.append(
                    {"size": length, "hex": bytes(u.mem_read(address, length)).hex()}
                )
                if i >= ninput:
                    u.mem_write(address, struct.pack("<I", 7))
            calls.append(
                {
                    "handle": handle,
                    "scalar": scalar,
                    "inputs": buffers[:ninput],
                    "output_sizes": [b["size"] for b in buffers[ninput:]],
                }
            )
            u.reg_write(UC_ARM64_REG_X0, retval & 0xFFFFFFFF)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
        elif pc == mock + 4:
            raise AssertionError("stack protector failure")

    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(base + address, end, timeout=1000000, count=1000)
    assert u.reg_read(UC_ARM64_REG_PC) == end and len(calls) == 1
    return {
        "call": calls[0],
        "return": ctypes.c_int32(u.reg_read(UC_ARM64_REG_X0)).value,
        "init_output": struct.unpack("<I", u.mem_read(data + 0x100, 4))[0],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stub", required=True, type=Path)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    binary = args.stub.read_bytes()
    assert hashlib.sha256(binary).hexdigest() == STUB_SHA256
    assert unicorn.__version__ == "2.1.4"
    report = {
        "status": "started",
        "stub_sha256": STUB_SHA256,
        "unicorn_version": unicorn.__version__,
        "cases": [],
        "test_binary_sha256": hashlib.sha256(args.test_binary.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    try:
        for n in range(32):
            suid = (0x1122334455667788 ^ n, 0x99AABBCCDDEEFF00 ^ (n << 32))
            offset = -(2**63) + n if n & 1 else 2**63 - 1 - n
            calls = [
                ("init", 0x11C4, [n + 1, 104 * (n + 1), 0, data + 0x100]),
                ("config", 0x1270, [n, offset, data, 125 + n, 2**32 - 1 - n, 161 + n]),
                ("stop", 0x1320, [n]),
                ("deinit", 0x1394, [n]),
            ]
            for code in (0, 57, -5):
                native = subprocess.run(
                    [str(args.test_binary.resolve()), str(n), str(code)],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                assert native.returncode == 0, native.stderr
                lines = native.stdout.splitlines()
                assert len(lines) == 4
                for index, (name, address, arguments) in enumerate(calls):
                    fields = lines[index].split()
                    assert len(fields) == 5 and fields[0] == "WIRE"
                    scalar, size, output_size = (
                        int(fields[1], 16),
                        int(fields[2]),
                        int(fields[3]),
                    )
                    payload = bytes.fromhex(fields[4])
                    assert len(payload) == size
                    for padding in (0xCC, 0xA5):
                        stock = execute(binary, address, arguments, code, padding, suid)
                        report["cases"].append(
                            {
                                "case": n,
                                "operation": name,
                                "return": code,
                                "padding": padding,
                                "native": lines[index],
                                "stock": stock,
                            }
                        )
                        assert stock["return"] == code
                        assert stock["call"]["handle"] == 0x5A
                        assert stock["call"]["scalar"] == scalar
                        assert stock["call"]["output_sizes"] == (
                            [output_size] if output_size else []
                        )
                        assert stock["call"]["inputs"][0]["size"] == size
                        actual = bytearray.fromhex(stock["call"]["inputs"][0]["hex"])
                        if name == "config":
                            for position in (*range(4, 8), *range(44, 48)):
                                assert (
                                    actual[position] == padding
                                    and payload[position] == 0
                                )
                                actual[position] = 0
                        assert actual == payload
                        if name == "init":
                            assert stock["init_output"] == (
                                7 if code == 0 else 0xDEADBEEF
                            )
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=repr(error))
        raise
    finally:
        (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "comparisons": len(report["cases"])}))


if __name__ == "__main__":
    main()
