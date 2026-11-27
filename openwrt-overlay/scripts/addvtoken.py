#!/usr/bin/env python3
"""
Pure-Python implementation of Broadcom/Zyxel `addvtoken` utility for BCM63138
(Zyxel VMG8825-B50B).

Generates the 1024-byte Zyxel `vZyToken` trailer (or 20-byte standard Broadcom
vtoken trailer) required by the stock Zyxel Web GUI / `zyfwinfo` / CFE firmware
upgrader.
"""
import argparse
import os
import struct
import zlib

FLASHTYPE_MAP = {
    "NOR": 1,
    "NAND16": 2,
    "NAND128": 3,
    "NAND256": 4,
    "NAND512": 5,
    "NAND1024": 6,
    "NAND2048": 7,
}


def bcm_crc32(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    """Broadcom raw IEEE 802.3 CRC32 (initial 0xffffffff, no final inversion)."""
    # Python's zlib.crc32(data, init) pre-inverts and post-inverts `init`.
    return (zlib.crc32(data, crc ^ 0xFFFFFFFF) ^ 0xFFFFFFFF) & 0xFFFFFFFF


def zyxel_checksum(data: bytes) -> int:
    """Zyxel 16-bit folded byte-sum checksum (`zyxelChecksum` in addvtoken)."""
    total = sum(data) & 0xFFFFFFFF
    return ((total >> 16) + total) & 0xFFFF


def build_zyxel_vtoken(
    image_data: bytes,
    chip: str = "63138",
    flashtype: str = "NAND128",
    endian: str = "little",
    btrm: int = 1,
    pmc: int = 0,
    ddrtype: str = "",
    product_name: str = "VMG8825-B50B",
    fw_version: str = "V5.17(ABNY.7)C0",
    fw_version_ex: str = "",
    model_id: str = "454A",
    kernel_data: bytes = b"",
    rootfs_data: bytes = b"",
    zyxel_ext: bool = True,
) -> bytes:
    fmt = "<" if endian == "little" else ">"
    img_crc = bcm_crc32(image_data)
    version = 0x00005732 if endian == "little" else 0x32570000
    chip_val = int(chip, 16)
    if flashtype not in FLASHTYPE_MAP:
        raise ValueError(f"Unsupported flashtype: {flashtype}")
    flash_val = FLASHTYPE_MAP[flashtype]

    flags = 0
    if pmc:
        flags |= 1
    if btrm:
        flags |= 2
    if ddrtype == "DDR3":
        flags |= 4
    elif ddrtype == "DDR4":
        flags |= 8

    if not zyxel_ext:
        return struct.pack(
            f"{fmt}I", img_crc
        ) + struct.pack(
            f"{fmt}4I",
            0x00005732,
            chip_val,
            flash_val,
            flags,
        )

    buf = bytearray(0x400)
    struct.pack_into(f"{fmt}I", buf, 0x00, img_crc)
    struct.pack_into("<I", buf, 0x04, version)
    struct.pack_into(f"{fmt}I", buf, 0x08, chip_val)
    struct.pack_into(f"{fmt}I", buf, 0x0C, flash_val)
    struct.pack_into(f"{fmt}I", buf, 0x10, flags)

    prod_bytes = product_name.encode("ascii")[:0x20]
    buf[0x14 : 0x14 + len(prod_bytes)] = prod_bytes

    fw_bytes = fw_version.encode("ascii")[:0x1E]
    buf[0x34 : 0x34 + len(fw_bytes)] = fw_bytes

    if not fw_version_ex:
        fw_version_ex = fw_version
    fw_ex_bytes = fw_version_ex.encode("ascii")[:0x1E]
    buf[0x82 : 0x82 + len(fw_ex_bytes)] = fw_ex_bytes

    if len(model_id) != 4:
        raise ValueError("model_id must be 4 hex characters (e.g. '454A')")
    for i, ch in enumerate(model_id):
        buf[0x52 + i] = int(ch, 16)

    k_sum = zyxel_checksum(kernel_data) if kernel_data else 0
    r_sum = zyxel_checksum(rootfs_data) if rootfs_data else 0
    struct.pack_into(f"{fmt}I", buf, 0x7A, k_sum)
    struct.pack_into(f"{fmt}I", buf, 0x7E, r_sum)

    hdr_sum = zyxel_checksum(buf[:0x3FE])
    struct.pack_into(f"{fmt}H", buf, 0x3FE, hdr_sum)
    return bytes(buf)


def main():
    parser = argparse.ArgumentParser(description="Generate Zyxel/Broadcom vtoken firmware trailer")
    parser.add_argument("input", help="Input firmware payload (e.g. merged bootfs+marker+ubi)")
    parser.add_argument("output", help="Output tagged firmware image (e.g. ras.bin)")
    parser.add_argument("--endian", default="little", choices=["little", "big"])
    parser.add_argument("--chip", default="63138")
    parser.add_argument("--flashtype", default="NAND128", choices=list(FLASHTYPE_MAP.keys()))
    parser.add_argument("--btrm", type=int, default=1)
    parser.add_argument("--pmc", type=int, default=0)
    parser.add_argument("--ddrtype", default="")
    parser.add_argument("--kernelfile", default="", help="Kernel/merged file for Zyxel checksum")
    parser.add_argument("--rootfsfile", default="", help="Rootfs/fake_rootfs file for Zyxel checksum")
    parser.add_argument("--product", default=os.environ.get("PRODUCTNAME", "VMG8825-B50B"))
    parser.add_argument("--fw-version", default=os.environ.get("FIRMWARE_VERSION", "V5.17(ABNY.7)C0"))
    parser.add_argument("--fw-version-ex", default=os.environ.get("FIRMWARE_VERSION_EX", ""))
    parser.add_argument("--model-id", default=os.environ.get("ZYXEL_MODEL_ID", "454A"))
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        image_data = f.read()

    zyxel_ext = bool(args.kernelfile or args.rootfsfile)
    kernel_data = b""
    rootfs_data = b""
    if args.kernelfile:
        with open(args.kernelfile, "rb") as f:
            kernel_data = f.read()
    if args.rootfsfile:
        with open(args.rootfsfile, "rb") as f:
            rootfs_data = f.read()

    trailer = build_zyxel_vtoken(
        image_data=image_data,
        chip=args.chip,
        flashtype=args.flashtype,
        endian=args.endian,
        btrm=args.btrm,
        pmc=args.pmc,
        ddrtype=args.ddrtype,
        product_name=args.product,
        fw_version=args.fw_version,
        fw_version_ex=args.fw_version_ex,
        model_id=args.model_id,
        kernel_data=kernel_data,
        rootfs_data=rootfs_data,
        zyxel_ext=zyxel_ext,
    )

    with open(args.output, "wb") as f:
        f.write(image_data)
        f.write(trailer)

    img_crc = struct.unpack("<I" if args.endian == "little" else ">I", trailer[:4])[0]
    print(f"addvtoken.py: {args.endian} endian Output file size = {len(image_data) + len(trailer)} with image crc = 0x{img_crc:08x}")


if __name__ == "__main__":
    main()
