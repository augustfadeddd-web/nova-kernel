#!/usr/bin/env python3
"""Verify that a 64-bit x86 ELF kernel contains a valid Multiboot2 header.

This is a portable stand-in for ``grub-file --is-x86-multiboot2`` so the build
also works on macOS, where GRUB's host tools are not packaged.  It also accepts
a legacy Multiboot1 header in a 32-bit ELF for comparison builds.
"""

import struct
import sys
from pathlib import Path

MULTIBOOT_MAGIC = 0x1BADB002
MULTIBOOT2_MAGIC = 0xE85250D6
HEADER_SCAN_BYTES = 32768


def has_multiboot_header(data: bytes) -> bool:
    limit = min(len(data), HEADER_SCAN_BYTES) - 12
    for offset in range(0, limit + 1, 8):
        magic, flags, checksum = struct.unpack_from("<III", data, offset)
        if magic == MULTIBOOT_MAGIC and (magic + flags + checksum) & 0xFFFFFFFF == 0:
            return True
    return False


def has_multiboot2_header(data: bytes) -> bool:
    limit = min(len(data), HEADER_SCAN_BYTES) - 16
    for offset in range(0, limit + 1, 8):
        magic, architecture, header_length, checksum = struct.unpack_from("<IIII", data, offset)
        if (
            magic == MULTIBOOT2_MAGIC
            and architecture == 0
            and 16 <= header_length <= 32768
            and offset + header_length <= len(data)
            and (magic + architecture + header_length + checksum) & 0xFFFFFFFF == 0
        ):
            return True
    return False


def main(argv: list[str]) -> int:
    paths = [argument for argument in argv[1:] if not argument.startswith("-")]
    if len(paths) != 1:
        print(
            "usage: check_multiboot.py [--is-x86-multiboot2] KERNEL",
            file=sys.stderr,
        )
        return 2

    kernel = Path(paths[0])
    try:
        data = kernel.read_bytes()
    except OSError as error:
        print(f"{kernel}: {error}", file=sys.stderr)
        return 1

    if data[:4] != b"\x7fELF":
        print(f"{kernel}: not an ELF file", file=sys.stderr)
        return 1
    if data[5] != 1:
        print(f"{kernel}: expected a little-endian ELF kernel", file=sys.stderr)
        return 1
    elf_class = data[4]
    if elf_class == 2:  # ELF64: require the Multiboot2 header.
        if not has_multiboot2_header(data):
            print(
                f"{kernel}: no valid Multiboot2 header in the first "
                f"{HEADER_SCAN_BYTES} bytes",
                file=sys.stderr,
            )
            return 1
    elif elf_class == 1:  # ELF32: legacy Multiboot1 header (reference builds).
        if not has_multiboot_header(data):
            print(
                f"{kernel}: no valid Multiboot1 header in the first "
                f"8192 bytes",
                file=sys.stderr,
            )
            return 1
    else:
        print(f"{kernel}: unknown ELF class {elf_class}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
