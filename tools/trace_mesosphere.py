#!/usr/bin/env python3
"""Trace the Mesosphère kernel and loader entries in QEMU or HorizonVM."""

import argparse
import hashlib
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import time

IMAGE_BASE = 0x40200000  # Leave room for QEMU's DTB at 0x40000000.
MAX_IMAGE_SIZE = 64 << 20


def image_layout(image):
    if len(image) < 12 or image[4:8] != b"MSS1":
        raise ValueError("expected a packaged Mesosphère image with MSS1 metadata")
    metadata, = struct.unpack_from("<I", image, 8)
    if metadata < 12 or metadata + 0x48 > len(image):
        raise ValueError("kernel metadata is outside the image")
    ini_relative, loader_relative = struct.unpack_from("<QQ", image, metadata)
    ini_offset = metadata + ini_relative
    loader_offset = metadata + 8 + loader_relative
    if not metadata + 0x48 <= ini_offset < loader_offset <= len(image) - 16:
        raise ValueError("invalid INI1 or loader offset")
    if (ini_offset | loader_offset) & 0xfff or image[loader_offset + 4:loader_offset + 8] != b"MLD0":
        raise ValueError("invalid loader magic or alignment")
    branch, = struct.unpack_from("<I", image, loader_offset)
    if branch >> 26 != 0b000101:
        raise ValueError("expected a direct branch at loader entry")
    displacement = (branch & 0x3ffffff) << 2
    if displacement & (1 << 27):
        displacement -= 1 << 28
    target = loader_offset + displacement
    if not loader_offset + 16 <= target <= len(image) - 4:
        raise ValueError("loader entry branch target is outside executable image")
    return IMAGE_BASE + metadata + 0x14, IMAGE_BASE + ini_offset, IMAGE_BASE + loader_offset, IMAGE_BASE + target


class GdbRemote:
    def __init__(self, process, timeout):
        self.process = process
        self.deadline = time.monotonic() + timeout

    def read_byte(self):
        remaining = self.deadline - time.monotonic()
        fd = self.process.stdout.fileno()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise TimeoutError("QEMU trace exceeded its deadline")
        data = os.read(fd, 1)
        if not data:
            raise RuntimeError("QEMU closed the debugger connection")
        return data

    def send(self, command):
        packet = command.encode("ascii")
        self.process.stdin.write(b"$" + packet + b"#%02x" % (sum(packet) & 255))
        self.process.stdin.flush()
        if self.read_byte() != b"+":
            raise RuntimeError("QEMU did not acknowledge the debugger command")
        if self.read_byte() != b"$":
            raise RuntimeError("invalid debugger response")
        payload = bytearray()
        while (byte := self.read_byte()) != b"#":
            payload += byte
        checksum = int(self.read_byte() + self.read_byte(), 16)
        if sum(payload) & 255 != checksum:
            raise RuntimeError("invalid debugger response checksum")
        self.process.stdin.write(b"+")
        self.process.stdin.flush()
        return payload.decode("ascii")

    def registers(self, label):
        regs = struct.unpack_from("<33QI", bytes.fromhex(self.send("g")))
        print(f"{label}: PC={regs[32]:#x} PSTATE={regs[33]:#x} "
              f"X0={regs[0]:#x} X1={regs[1]:#x} X2={regs[2]:#x} "
              f"X30={regs[30]:#x} SP={regs[31]:#x}")
        return regs

    def expect(self, command, response):
        actual = self.send(command)
        if actual != response:
            raise RuntimeError(f"expected {response!r} from {command}, got {actual!r}")

    def run(self, command):
        response = self.send(command)
        if not response.startswith(("S05", "T05")):
            raise RuntimeError(f"unexpected QEMU stop: {response}")


def trace(image_path, timeout, native):
    with image_path.open("rb") as file:
        image = file.read(MAX_IMAGE_SIZE + 1)
    if len(image) > MAX_IMAGE_SIZE:
        raise ValueError("image does not fit in reference VM RAM")
    layout, ini, loader, target = image_layout(image)
    print(f"Image SHA-256: {hashlib.sha256(image).hexdigest()}", flush=True)
    if native is not None:
        command = [str(native.resolve()), "--trace-loader",
                   *(hex(value) for value in (len(image), layout, ini, loader, target))]
        subprocess.run(command, input=image, timeout=timeout, check=True)
        return

    with tempfile.TemporaryDirectory(prefix="hvm-qemu-") as directory:
        path = Path(directory) / "mesosphere.bin"
        path.write_bytes(image)
        trace_qemu(path, image, timeout, layout, ini, loader, target)


def trace_qemu(image_path, image, timeout, layout, ini, loader, target):
    loader_path = str(image_path).replace(",", ",,")
    command = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2",
        "-accel", "tcg", "-cpu", "cortex-a57", "-smp", "1", "-m", "256M",
        "-display", "none", "-serial", "none", "-monitor", "none",
        "-S", "-chardev", "stdio,id=debugger,signal=off", "-gdb", "chardev:debugger",
        "-device", f"loader,file={loader_path},addr={IMAGE_BASE:#x},cpu-num=0,force-raw=on",
    ]
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0) as process:
        try:
            debugger = GdbRemote(process, timeout)
            entry = debugger.registers("Kernel entry")
            if entry[32] != IMAGE_BASE:
                raise RuntimeError("unexpected kernel entry PC")
            debugger.expect(f"Z1,{loader:x},4", "OK")
            debugger.run("c")
            regs = debugger.registers("Loader entry")
            if regs[32] != loader or regs[:3] != (IMAGE_BASE, layout, ini) or regs[33] & 0xf != 5:
                raise RuntimeError("loader entry registers do not match the EL1h boot contract")
            x30 = regs[30]
            caller = x30 - IMAGE_BASE
            if caller < 4 or caller >= loader - IMAGE_BASE or caller & 3:
                raise RuntimeError("loader link register is outside kernel code")
            if image[caller - 4:caller] != struct.pack("<I", 0xD63F0060):
                raise RuntimeError("loader link register does not follow BLR X3")
            debugger.expect(f"z1,{loader:x},4", "OK")
            debugger.run("s")
            if debugger.registers("After first loader instruction")[32] != target:
                raise RuntimeError("loader did not reach the decoded branch target")
        finally:
            if process.poll() is None:
                process.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--native", type=Path, metavar="HORIZONVM",
                        help="use the signed HorizonVM executable instead of QEMU")
    args = parser.parse_args()
    if not 0 < args.timeout < float("inf"):
        parser.error("--timeout must be positive and finite")
    try:
        trace(args.image, args.timeout, args.native)
    except (OSError, ValueError, RuntimeError, struct.error, subprocess.SubprocessError) as error:
        sys.exit(f"Trace failed: {error}")


if __name__ == "__main__":
    main()
