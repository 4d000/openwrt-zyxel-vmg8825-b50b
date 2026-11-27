#!/usr/bin/env python3
"""
Zyxel VMG8825-B50B Local Hardware / MRD Metadata Inspector (Read-Only)

Inspects non-secret hardware metadata from your OWN `mtd0_cferom.bin` backup
(Broadcom NVRAM Board ID, Zyxel MRD Vendor/Product/Model ID `4543` vs `454A`,
and MAC count) so you can verify your exact router variant (`--brand wind3`
vs `--brand generic`) before building a `.ras.bin` image.

Intentionally does NOT read, print, or derive passwords or private keys.

SPDX-License-Identifier: GPL-2.0-only
"""

import struct
import sys


def read_cstr(buf: bytes, offset: int, max_len: int) -> str:
    raw = buf[offset : offset + max_len]
    raw = raw.split(b"\x00", 1)[0].split(b"\xff", 1)[0]
    return raw.decode("ascii", "replace").strip()


def inspect_cferom(path: str):
    data = open(path, "rb").read()
    print(f"File: {path} ({len(data)} bytes)")

    # Broadcom NVRAM block sits at 0x10580 in BCM63138 NAND cferom (128 KiB)
    nvram_off = 0x10580
    if len(data) < nvram_off + 0x200:
        sys.exit("ERROR: File is smaller than a 128 KiB BCM63138 mtd0_cferom.bin dump.")

    nvram_ver = struct.unpack_from("<I", data, nvram_off)[0]
    board_id = read_cstr(data, nvram_off + 0x104, 16)
    voice_board_id = read_cstr(data, nvram_off + 0x114, 16)
    mac_count = struct.unpack_from("<I", data, nvram_off + 0x124)[0]

    print("Broadcom CFE NVRAM:")
    print(f"  NVRAM version  : {nvram_ver}")
    print(f"  Board ID       : {board_id or '<unknown>'}")
    if voice_board_id:
        print(f"  Voice Board ID : {voice_board_id}")
    if 0 < mac_count <= 256:
        print(f"  MAC Count      : {mac_count}")

    # Zyxel MRD block starts at VendorName ("Zyxel ...", 32B) + ProductName (32B) + ModelID (4 nibble bytes)
    mrd_off = -1
    for vendor_marker in (b"Zyxel", b"ZyXEL"):
        pos = data.find(vendor_marker, 0x10000)
        if pos != -1:
            mrd_off = pos
            break

    if mrd_off != -1 and mrd_off + 0x44 <= len(data):
        mrd_vendor = read_cstr(data, mrd_off, 32)
        mrd_product = read_cstr(data, mrd_off + 0x20, 32)
        raw_mid = data[mrd_off + 0x40 : mrd_off + 0x44]
        model_id = "".join(f"{b:X}" for b in raw_mid)
        print("Zyxel MRD Hardware Identity:")
        print(f"  MRD offset     : 0x{mrd_off:x}")
        print(f"  Vendor         : {mrd_vendor}")
        print(f"  Product        : {mrd_product}")
        print(f"  Model ID       : {model_id}")
        if model_id == "4543":
            print("  Detected Brand : WindTre Home&Life HUB (ABLZ / 4543 -> use --brand wind3)")
        elif model_id == "454A":
            print("  Detected Brand : Generic Unbranded Zyxel (ABNY / 454A -> use --brand generic)")
        else:
            print(f"  Detected Brand : Custom ISP variant (Model ID {model_id} -> pass --model-id {model_id})")
    else:
        print("  Zyxel MRD block not located; check Firmware Version in Zyxel Web GUI.")

    print("\nNote: MAC addresses, serial numbers, and credentials are intentionally omitted.")


def main():
    if len(sys.argv) != 2 or sys.argv[1] in ("-h", "--help"):
        print("Usage: python3 scripts/vmg8825_mrd_info.py <mtd0_cferom.bin>")
        sys.exit(0 if len(sys.argv) == 2 else 1)
    inspect_cferom(sys.argv[1])


if __name__ == "__main__":
    main()
