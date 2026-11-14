#!/usr/bin/env python3
"""Put a standalone HorizonVM EL0 probe KIP in an empty Mesosphère image."""

import argparse
from pathlib import Path
import struct

PAGE_SIZE = 0x1000


def probe_kip():
    # mov x0, #0x4856; svc #7 (ExitProcess); b .
    text = struct.pack("<III", 0xD2890AC0, 0xD40000E1, 0x14000000)
    header = bytearray(0x100)
    struct.pack_into("<4s12sQIBBBB", header, 0, b"KIP1", b"HvmEl0Probe",
                     0x010000000000FF01, 0, 32, 0, 0, 0x18)
    # Uncompressed text at offset zero, CPU 0 affinity, 16 KiB stack.
    struct.pack_into("<4I", header, 0x20, 0, len(text), len(text), 1)
    struct.pack_into("<4I", header, 0x30, PAGE_SIZE, 0, 0, 4 * PAGE_SIZE)
    struct.pack_into("<4I", header, 0x40, PAGE_SIZE, 0, 0, 0)
    struct.pack_into("<II", header, 0x50, PAGE_SIZE, 0)
    # Grant only SVC #7 and reserve a small handle table.
    capabilities = [0xF | (1 << (5 + 7)), 0x7FFF | (16 << 16)]
    struct.pack_into("<32I", header, 0x80, *capabilities, *([0xFFFFFFFF] * 30))
    return header + text


def insert_probe(image):
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
    kip = probe_kip()
    ini1 = struct.pack("<4sIII", b"INI1", 16 + len(kip), 1, 0) + kip
    if len(ini1) > loader - ini or any(image[ini:loader]):
        raise ValueError("probe requires an unused INI1 area")
    image[ini:ini + len(ini1)] = ini1
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.image.resolve() == args.output.resolve() or (
            args.output.exists() and args.image.samefile(args.output)
        ):
            raise ValueError("choose a separate output file")
        args.output.write_bytes(insert_probe(bytearray(args.image.read_bytes())))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"Cannot build EL0 probe: {error}\n")


if __name__ == "__main__":
    main()
