#!/usr/bin/env python3
"""Put the HorizonVM EL0 or IPC probe into an empty Mesosphère image."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

PAGE_SIZE = 0x1000


def make_kip(text, name, program_id, priority, syscalls):
    if not 0 < len(text) <= PAGE_SIZE:
        raise ValueError("probe text must fit in one page")
    header = bytearray(0x100)
    struct.pack_into("<4s12sQIBBBB", header, 0, b"KIP1", name, program_id, 0, priority, 0, 0, 0x18)
    # Uncompressed text at offset zero, CPU 0 affinity, 16 KiB stack.
    struct.pack_into("<4I", header, 0x20, 0, len(text), len(text), 1)
    struct.pack_into("<4I", header, 0x30, PAGE_SIZE, 0, 0, 4 * PAGE_SIZE)
    struct.pack_into("<4I", header, 0x40, PAGE_SIZE, 0, 0, 0)
    struct.pack_into("<II", header, 0x50, PAGE_SIZE, 0)
    syscall_words = {}
    for number in syscalls:
        group, index = divmod(number, 24)
        syscall_words[group] = syscall_words.get(group, 0) | (1 << (5 + index))
    capabilities = [mask | (group << 29) | 0xF for group, mask in sorted(syscall_words.items())]
    capabilities.append(0x7FFF | (16 << 16))
    capabilities += [0xFFFFFFFF] * (32 - len(capabilities))
    struct.pack_into("<32I", header, 0x80, *capabilities)
    return header + text


def el0_kips():
    # mov x0, #0x4856; svc #7 (ExitProcess); b .
    text = struct.pack("<III", 0xD2890AC0, 0xD40000E1, 0x14000000)
    return [make_kip(text, b"HvmEl0Probe", 0x010000000000FF01, 32, (7,))]


def ipc_kips(prefix):
    tools_directory = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="hvm-ipc-") as directory:
        obj = Path(directory) / "probe.o"
        binary = Path(directory) / "probe.bin"

        def assemble(name):
            source = tools_directory / f"ipc_{name}.s"
            subprocess.run([prefix + "as", "-o", str(obj), str(source)], check=True)
            subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".text", str(obj), str(binary)], check=True)
            return binary.read_bytes()

        return [
            make_kip(assemble("server"), b"HvmIpcServer", 0x010000000000FF02, 30, (7, 0x41, 0x43, 0x71)),
            make_kip(assemble("client"), b"HvmIpcClient", 0x010000000000FF03, 32, (7, 0x1F, 0x21)),
        ]


def insert_probe(image, kips):
    if len(image) < 12 or image[4:8] != b"MSS1":
        raise ValueError("expected a packaged Mesosphère image")
    metadata, = struct.unpack_from("<I", image, 8)
    if metadata < 12 or metadata + 0x48 > len(image):
        raise ValueError("invalid kernel metadata offset")
    ini_relative, loader_relative = struct.unpack_from("<QQ", image, metadata)
    ini = metadata + ini_relative
    loader = metadata + 8 + loader_relative
    if (
        not metadata + 0x48 <= ini < loader <= len(image) - 8
        or (ini | loader) & (PAGE_SIZE - 1)
        or image[loader + 4:loader + 8] != b"MLD0"
    ):
        raise ValueError("invalid INI1 or loader offset")
    payload = b"".join(kips)
    ini1 = struct.pack("<4sIII", b"INI1", 16 + len(payload), len(kips), 0) + payload
    if len(ini1) > loader - ini or any(image[ini:loader]):
        raise ValueError("probe requires an unused INI1 area")
    image[ini:ini + len(ini1)] = ini1
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--ipc", action="store_true", help="build the two-process IPC probe")
    parser.add_argument("--tool-prefix", default="aarch64-none-elf-",
                        help="GNU assembler and objcopy prefix for --ipc")
    args = parser.parse_args()
    try:
        if args.image.resolve() == args.output.resolve() or (
            args.output.exists() and args.image.samefile(args.output)
        ):
            raise ValueError("choose a separate output file")
        kips = ipc_kips(args.tool_prefix) if args.ipc else el0_kips()
        args.output.write_bytes(insert_probe(bytearray(args.image.read_bytes()), kips))
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Cannot build EL0 probe: {error}\n")


if __name__ == "__main__":
    main()
