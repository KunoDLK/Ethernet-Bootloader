#!/usr/bin/env python3
import argparse
import hashlib
import struct
import subprocess

APP_IMAGE_MAGIC = 0x41505031
APP_IMAGE_ABI_VERSION = 1
APP_IMAGE_DIGEST_SHA1 = 1
APP_IMAGE_FLAG_VALID = 1 << 0
APP_EXEC_BASE = 0x10000000
APP_EXEC_SIZE = 64 * 1024
HEADER_FORMAT = "<IHHIIIIIIIIIB3s20s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


def read_symbols(nm, elf):
    output = subprocess.check_output([nm, "-g", elf], text=True)
    symbols = {}
    for line in output.splitlines():
      parts = line.split()
      if len(parts) == 3:
        symbols[parts[2]] = int(parts[0], 16)
    return symbols


def main():
    parser = argparse.ArgumentParser(description="Package a CCMRAM application image for the resident bootloader.")
    parser.add_argument("--elf", required=True)
    parser.add_argument("--bin", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    parser.add_argument("--version", type=lambda value: int(value, 0), default=1)
    args = parser.parse_args()

    symbols = read_symbols(args.nm, args.elf)
    app_start = symbols["app_start"]
    app_stop = symbols.get("app_stop", 0)
    bss_start = symbols.get("__app_bss_start__", APP_EXEC_BASE)
    bss_end = symbols.get("__app_bss_end__", bss_start)

    with open(args.bin, "rb") as input_file:
        payload = input_file.read()

    digest = hashlib.sha1(payload).digest()
    header = struct.pack(
        HEADER_FORMAT,
        APP_IMAGE_MAGIC,
        HEADER_SIZE,
        APP_IMAGE_ABI_VERSION,
        len(payload),
        APP_EXEC_BASE,
        APP_EXEC_SIZE,
        app_start - APP_EXEC_BASE,
        (app_stop - APP_EXEC_BASE) if app_stop else 0,
        bss_start - APP_EXEC_BASE,
        bss_end - bss_start,
        args.version,
        APP_IMAGE_FLAG_VALID,
        APP_IMAGE_DIGEST_SHA1,
        b"\x00\x00\x00",
        digest,
    )

    with open(args.out, "wb") as output_file:
        output_file.write(header)
        output_file.write(payload)


if __name__ == "__main__":
    main()
