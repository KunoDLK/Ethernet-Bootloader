#!/usr/bin/env python3
import argparse
import hashlib
import struct

BLST_MAGIC = 0x54534C42  # "BLST"
HEADER_STRUCT = struct.Struct("<II20sI")
MAX_IMAGE_BYTES = (256 * 1024) - HEADER_STRUCT.size


def main() -> None:
    parser = argparse.ArgumentParser(description="Pack bootloader payload for sectors 9-10.")
    parser.add_argument("input", help="raw bootloader .bin to program into resident sectors 0-5")
    parser.add_argument("-o", "--output", required=True, help="output boot payload blob")
    args = parser.parse_args()

    with open(args.input, "rb") as input_file:
        image = input_file.read()

    if not image:
        raise SystemExit("input image is empty")
    if len(image) > MAX_IMAGE_BYTES:
        raise SystemExit(f"input image is {len(image)} bytes; max is {MAX_IMAGE_BYTES}")

    sha1 = hashlib.sha1(image).digest()
    header = HEADER_STRUCT.pack(BLST_MAGIC, len(image), sha1, 0)

    with open(args.output, "wb") as output_file:
        output_file.write(header)
        output_file.write(image)

    print(f"wrote {args.output} ({len(header) + len(image)} bytes)")


if __name__ == "__main__":
    main()
