#!/usr/bin/env python3
"""
Zyxel VMG8825-B50B OpenWrt Factory Image Builder & Legal OEM Helper
(Zero-Dependency Pure Python 3 — Windows / macOS / Linux)

Allows any user to legally assemble a Web-GUI-flashable OpenWrt `.ras.bin`
image for the Zyxel VMG8825-B50B (both WindTre `ABLZ` branded and generic
`ABNY` unbranded units) WITHOUT redistributing proprietary Zyxel/Broadcom
`cferam` or Wi-Fi firmware binaries.

Supported modes:
  1. Factory `.ras.bin` assembly from an official Zyxel OEM `.bin`:
       python3 vmg8825_factory_builder.py \\
         --oem V513ABNY2C0.bin \\
         --openwrt openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin \\
         --brand wind3 -o openwrt-vmg8825-factory.ras.bin

  2. Factory `.ras.bin` assembly from a `cferam` binary dumped via SSH:
       python3 vmg8825_factory_builder.py \\
         --cferam cferam.001 \\
         --openwrt openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin \\
         --brand wind3 -o openwrt-vmg8825-factory.ras.bin

  3. Optional legal extraction of CLM-enabled V5.13 Wi-Fi firmware blobs
     from the user's own official Zyxel V5.13 OEM `.bin`:
       python3 vmg8825_factory_builder.py \\
         --oem V513ABNY2C0.bin --extract-wifi-fw ./wifi_fw

  4. Firmware inspection (header, CRC32, Model ID, bootfs contents):
       python3 vmg8825_factory_builder.py --inspect firmware.bin

SPDX-License-Identifier: GPL-2.0-only
"""

import argparse
import os
import re
import struct
import sys
import zlib

# JFFS2 constants (little-endian)
JFFS2_MAGIC_BITMASK = 0x1985
JFFS2_NODETYPE_DIRENT = 0xE001
JFFS2_NODETYPE_INODE = 0xE002
JFFS2_NODETYPE_CLEANMARKER = 0x2003
JFFS2_COMPR_NONE = 0x00
JFFS2_COMPR_ZERO = 0x01
JFFS2_COMPR_RTIME = 0x02
JFFS2_COMPR_ZLIB = 0x06
DT_DIR = 4
DT_REG = 8

ERASE_SIZE = 0x20000      # 128 KiB NAND eraseblock
BOOTFS_SIZE = 0x800000    # 8 MiB JFFS2 bootfs
WFI_VERSION_LE = 0x00005732
BCM63138_CHIP = 0x00063138
FLASHTYPE_NAND128 = 3


def bcm_crc32(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    """Broadcom raw IEEE 802.3 CRC32 (init 0xffffffff, no final inversion)."""
    return (zlib.crc32(data, crc ^ 0xFFFFFFFF) ^ 0xFFFFFFFF) & 0xFFFFFFFF


def jffs2_crc32(data: bytes) -> int:
    """Linux JFFS2 CRC32 (matches kernel `crc32(0, buf, len)`)."""
    return zlib.crc32(data, 0xFFFFFFFF) ^ 0xFFFFFFFF


def zyxel_checksum(data: bytes) -> int:
    """Zyxel 16-bit folded byte-sum checksum (`zyxelChecksum` in addvtoken)."""
    total = sum(data) & 0xFFFFFFFF
    return ((total >> 16) + total) & 0xFFFF


def parse_vzytoken(data: bytes) -> dict:
    """Parse 1024-byte Zyxel vZyToken trailer (or 20-byte Broadcom WFI trailer)."""
    info = {"format": "unknown", "trailer_len": 0}
    n = len(data)

    # Check 1024-byte Zyxel vZyToken first
    if n > 1024:
        t = data[-1024:]
        img_crc, ver, chip, flashtype, flags = struct.unpack_from("<5I", t, 0)
        if ver == WFI_VERSION_LE and chip == BCM63138_CHIP:
            prod = t[0x14:0x34].split(b"\x00", 1)[0].decode("ascii", "replace")
            fw_ver = t[0x34:0x52].split(b"\x00", 1)[0].decode("ascii", "replace")
            fw_ver_ex = t[0x82:0xA0].split(b"\x00", 1)[0].decode("ascii", "replace")
            model_id = "".join(f"{b:X}" for b in t[0x52:0x56])
            calc_crc = bcm_crc32(data[:-1024])
            hdr_sum = struct.unpack_from("<H", t, 0x3FE)[0]
            calc_hdr_sum = zyxel_checksum(t[:0x3FE])
            return {
                "format": "zyxel-ras",
                "trailer_len": 1024,
                "chip": f"{chip:x}",
                "flashtype": flashtype,
                "flags": flags,
                "product": prod,
                "fw_version": fw_ver,
                "fw_version_ex": fw_ver_ex,
                "model_id": model_id,
                "crc_ok": img_crc == calc_crc,
                "hdr_sum_ok": hdr_sum == calc_hdr_sum,
                "img_crc": img_crc,
            }

    # Check 20-byte Broadcom WFI trailer (`bcm4908img`)
    if n > 20:
        t = data[-20:]
        img_crc, ver, chip, flashtype, flags = struct.unpack_from("<5I", t, 0)
        if ver == WFI_VERSION_LE and chip == BCM63138_CHIP:
            calc_crc = bcm_crc32(data[:-20])
            return {
                "format": "bcm4908img",
                "trailer_len": 20,
                "chip": f"{chip:x}",
                "flashtype": flashtype,
                "flags": flags,
                "crc_ok": img_crc == calc_crc,
                "img_crc": img_crc,
            }

    return info


def build_vzytoken(
    image_data: bytes,
    product: str,
    fw_version: str,
    model_id: str,
    flashtype: int = FLASHTYPE_NAND128,
    flags: int = 0,
    kernel_data: bytes = b"",
    rootfs_data: bytes = b"",
) -> bytes:
    """Build 1024-byte Zyxel vZyToken trailer for BCM63138 VMG8825-B50B."""
    img_crc = bcm_crc32(image_data)
    buf = bytearray(0x400)
    struct.pack_into("<5I", buf, 0x00, img_crc, WFI_VERSION_LE, BCM63138_CHIP, flashtype, flags)

    p_b = product.encode("ascii")[:0x20]
    buf[0x14 : 0x14 + len(p_b)] = p_b

    fv_b = fw_version.encode("ascii")[:0x1E]
    buf[0x34 : 0x34 + len(fv_b)] = fv_b
    buf[0x82 : 0x82 + len(fv_b)] = fv_b

    if len(model_id) != 4:
        raise ValueError(f"model_id must be 4 hex chars (e.g. 4543 or 454A), got: {model_id!r}")
    for i, ch in enumerate(model_id):
        buf[0x52 + i] = int(ch, 16)

    k_sum = zyxel_checksum(kernel_data if kernel_data else image_data)
    r_sum = zyxel_checksum(rootfs_data) if rootfs_data else 0
    struct.pack_into("<II", buf, 0x7A, k_sum, r_sum)
    struct.pack_into("<H", buf, 0x3FE, zyxel_checksum(buf[:0x3FE]))
    return bytes(buf)


def jffs2_rtime_decompress(src: bytes, dst_len: int) -> bytes:
    """Decompress JFFS2 RTIME compressed data node."""
    out = bytearray()
    positions = [0] * 256
    pos = 0
    while len(out) < dst_len and pos + 1 < len(src):
        val = src[pos]
        pos += 1
        out.append(val)
        repeat = src[pos]
        pos += 1
        backoffs = positions[val]
        positions[val] = len(out)
        if repeat:
            if backoffs + repeat >= len(out):
                while repeat > 0 and len(out) < dst_len:
                    out.append(out[backoffs])
                    backoffs += 1
                    repeat -= 1
            else:
                out.extend(out[backoffs : backoffs + repeat])
    return bytes(out[:dst_len])


def extract_jffs2_files(buf: bytes, max_scan: int = BOOTFS_SIZE) -> dict:
    """Extract all top-level regular files from a little-endian JFFS2 bootfs image."""
    limit = min(len(buf), max_scan)
    dirents = {}   # name -> (version, ino)
    inodes = {}    # ino -> list of (version, offset, data_bytes, isize)
    pos = 0

    while pos + 12 <= limit:
        magic, nodetype, totlen = struct.unpack_from("<HHI", buf, pos)
        if magic != JFFS2_MAGIC_BITMASK:
            if magic == 0xFFFF:
                next_eb = (pos + ERASE_SIZE) & ~(ERASE_SIZE - 1)
                pos = next_eb if next_eb > pos else pos + 4
            else:
                pos += 4
            continue

        if totlen < 12 or pos + totlen > limit:
            pos += 4
            continue

        if nodetype == JFFS2_NODETYPE_DIRENT and totlen >= 40:
            pino, version, ino, _mctime, nsize, ftype = struct.unpack_from("<IIIIBB", buf, pos + 12)
            name = buf[pos + 40 : pos + 40 + nsize].decode("ascii", "replace")
            if pino == 1 and ino != 0:
                prev = dirents.get(name)
                if prev is None or version >= prev[0]:
                    dirents[name] = (version, ino)
        elif nodetype == JFFS2_NODETYPE_INODE and totlen >= 68:
            (
                ino, version, _mode, _uid, _gid, isize,
                _atime, _mtime, _ctime, offset, csize, dsize, compr
            ) = struct.unpack_from("<IIHHIIIIIIIIB", buf, pos + 12)
            raw = buf[pos + 68 : pos + 68 + csize]
            if compr == JFFS2_COMPR_NONE:
                chunk = raw[:dsize]
            elif compr == JFFS2_COMPR_ZERO:
                chunk = b"\x00" * dsize
            elif compr == JFFS2_COMPR_ZLIB:
                chunk = zlib.decompress(raw)
            elif compr == JFFS2_COMPR_RTIME:
                chunk = jffs2_rtime_decompress(raw, dsize)
            else:
                raise ValueError(f"Unsupported JFFS2 compression type {compr} in inode {ino}")
            inodes.setdefault(ino, []).append((version, offset, chunk, isize))

        pos = (pos + totlen + 3) & ~3

    files = {}
    for name, (_, ino) in dirents.items():
        frags = inodes.get(ino, [])
        if not frags:
            files[name] = b""
            continue
        frags.sort(key=lambda x: x[0])
        final_size = frags[-1][3]
        out = bytearray(final_size)
        for _, off, chunk, _ in frags:
            end = min(off + len(chunk), final_size)
            if off < final_size:
                out[off:end] = chunk[: end - off]
        files[name] = bytes(out)
    return files


def extract_jffs2_paths(buf: bytes, target_paths: list) -> dict:
    """
    Scan a full unified JFFS2 OEM image and extract specific nested file paths
    (e.g. 'etc/wlan/dhd/4366c0/rtecdc.bin'). Two-pass scan keeps RAM low.
    """
    limit = len(buf)
    dirents = {}  # (pino, name) -> (version, ino)

    # Pass 1: collect directory entries only
    pos = 0
    while pos + 12 <= limit:
        magic, nodetype, totlen = struct.unpack_from("<HHI", buf, pos)
        if magic != JFFS2_MAGIC_BITMASK:
            if magic == 0xFFFF:
                next_eb = (pos + ERASE_SIZE) & ~(ERASE_SIZE - 1)
                pos = next_eb if next_eb > pos else pos + 4
            else:
                pos += 4
            continue
        if totlen < 12 or pos + totlen > limit:
            pos += 4
            continue
        if nodetype == JFFS2_NODETYPE_DIRENT and totlen >= 40:
            pino, version, ino, _mctime, nsize, _ftype = struct.unpack_from("<IIIIBB", buf, pos + 12)
            name = buf[pos + 40 : pos + 40 + nsize].decode("ascii", "replace")
            key = (pino, name)
            prev = dirents.get(key)
            if prev is None or version >= prev[0]:
                dirents[key] = (version, ino)
        pos = (pos + totlen + 3) & ~3

    # Resolve target paths to inode numbers
    target_inos = {}  # ino -> target_path
    for path in target_paths:
        cur_ino = 1
        found = True
        for part in path.strip("/").split("/"):
            entry = dirents.get((cur_ino, part))
            if entry is None or entry[1] == 0:
                found = False
                break
            cur_ino = entry[1]
        if found:
            target_inos[cur_ino] = path

    if not target_inos:
        return {}

    # Pass 2: collect data inodes for matched inode numbers only
    inodes = {}
    pos = 0
    while pos + 12 <= limit:
        magic, nodetype, totlen = struct.unpack_from("<HHI", buf, pos)
        if magic != JFFS2_MAGIC_BITMASK:
            if magic == 0xFFFF:
                next_eb = (pos + ERASE_SIZE) & ~(ERASE_SIZE - 1)
                pos = next_eb if next_eb > pos else pos + 4
            else:
                pos += 4
            continue
        if totlen < 12 or pos + totlen > limit:
            pos += 4
            continue
        if nodetype == JFFS2_NODETYPE_INODE and totlen >= 68:
            ino = struct.unpack_from("<I", buf, pos + 12)[0]
            if ino in target_inos:
                (
                    ino, version, _mode, _uid, _gid, isize,
                    _atime, _mtime, _ctime, offset, csize, dsize, compr
                ) = struct.unpack_from("<IIHHIIIIIIIIB", buf, pos + 12)
                raw = buf[pos + 68 : pos + 68 + csize]
                if compr == JFFS2_COMPR_NONE:
                    chunk = raw[:dsize]
                elif compr == JFFS2_COMPR_ZERO:
                    chunk = b"\x00" * dsize
                elif compr == JFFS2_COMPR_ZLIB:
                    chunk = zlib.decompress(raw)
                elif compr == JFFS2_COMPR_RTIME:
                    chunk = jffs2_rtime_decompress(raw, dsize)
                else:
                    raise ValueError(f"Unsupported JFFS2 compression type {compr} in inode {ino}")
                inodes.setdefault(ino, []).append((version, offset, chunk, isize))
        pos = (pos + totlen + 3) & ~3

    results = {}
    for ino, path in target_inos.items():
        frags = inodes.get(ino, [])
        if not frags:
            continue
        frags.sort(key=lambda x: x[0])
        final_size = frags[-1][3]
        out = bytearray(final_size)
        for _, off, chunk, _ in frags:
            end = min(off + len(chunk), final_size)
            if off < final_size:
                out[off:end] = chunk[: end - off]
        results[path] = bytes(out)
    return results


def make_jffs2_bootfs(file_list: list, pad_size: int = BOOTFS_SIZE) -> bytes:
    """
    Build a clean, uncompressed little-endian JFFS2 bootfs image (matching
    `mkfs.jffs2 --pad=0x800000 --little-endian --squash-uids -e 128KiB -m none -n`).
    """
    out = bytearray()
    eb_used = 0
    ino = 2

    def align4():
        nonlocal eb_used
        rem = len(out) & 3
        if rem:
            pad = 4 - rem
            out.extend(b"\xff" * pad)
            eb_used += pad

    def ensure_space(needed: int):
        nonlocal eb_used
        if eb_used + needed > ERASE_SIZE:
            pad = ERASE_SIZE - eb_used
            out.extend(b"\xff" * pad)
            eb_used = 0

    for name, data in file_list:
        name_b = name.encode("ascii")
        # 1. Dirent node (parent ino = 1)
        totlen = 40 + len(name_b)
        ensure_space((totlen + 3) & ~3)
        hdr = struct.pack("<HHI", JFFS2_MAGIC_BITMASK, JFFS2_NODETYPE_DIRENT, totlen)
        hdr_crc = jffs2_crc32(hdr)
        body = struct.pack(
            "<IIIIBBH",
            1,          # pino (root)
            1,          # version
            ino,        # ino
            0,          # mctime
            len(name_b),
            DT_REG,
            0,          # unused
        )
        node_crc = jffs2_crc32(hdr + struct.pack("<I", hdr_crc) + body)
        name_crc = jffs2_crc32(name_b)
        dirent = hdr + struct.pack("<I", hdr_crc) + body + struct.pack("<II", node_crc, name_crc) + name_b
        out.extend(dirent)
        eb_used += len(dirent)
        align4()

        # 2. Data inode(s) in 4096-byte page chunks (or 1 empty inode for 0-byte marker)
        ver = 1
        off = 0
        isize = len(data)
        while True:
            chunk = data[off : off + 4096]
            dsize = len(chunk)
            csize = dsize
            totlen = 68 + csize
            ensure_space((totlen + 3) & ~3)

            hdr = struct.pack("<HHI", JFFS2_MAGIC_BITMASK, JFFS2_NODETYPE_INODE, totlen)
            hdr_crc = jffs2_crc32(hdr)
            meta = struct.pack(
                "<IIHHIIIIIIIIBBH",
                ino,
                ver,
                0o100644,   # regular file rw-r--r--
                0,          # uid
                0,          # gid
                isize,
                0, 0, 0,    # atime, mtime, ctime
                off,
                csize,
                dsize,
                JFFS2_COMPR_NONE,
                0,          # usercompr
                0,          # flags
            )
            data_crc = jffs2_crc32(chunk) if csize else 0
            prefix = hdr + struct.pack("<I", hdr_crc) + meta
            node_crc = jffs2_crc32(prefix)
            node = prefix + struct.pack("<II", data_crc, node_crc) + chunk
            out.extend(node)
            eb_used += len(node)
            align4()

            ver += 1
            off += dsize
            if off >= isize:
                break

        ino += 1

    if len(out) > pad_size:
        raise ValueError(f"JFFS2 bootfs ({len(out)} bytes) exceeds {pad_size} bytes")
    out.extend(b"\xff" * (pad_size - len(out)))
    return bytes(out)


def find_ubi_offset(data: bytes) -> int:
    """Locate the first 128 KiB-aligned UBI# eraseblock after bootfs."""
    for off in range(ERASE_SIZE, len(data) - 4, ERASE_SIZE):
        if data[off : off + 4] == b"UBI#":
            return off
    raise ValueError("Could not find UBI# rootfs partition in OpenWrt image")


def cmd_inspect(path: str):
    data = open(path, "rb").read()
    info = parse_vzytoken(data)
    print(f"File: {path} ({len(data)} bytes)")
    for k, v in info.items():
        print(f"  {k}: {v}")
    files = extract_jffs2_files(data)
    print("  bootfs files:")
    for name, content in sorted(files.items()):
        print(f"    - {name} ({len(content)} bytes)")


def cmd_extract_wifi_fw(oem_path: str, out_dir: str):
    print(f"Scanning {oem_path} for Broadcom Wi-Fi firmware blobs (this takes a few seconds)...")
    oem_data = open(oem_path, "rb").read()
    targets = {
        "etc/wlan/dhd/4366c0/rtecdc.bin": "brcmfmac4366c-pcie.bin",
        "etc/wlan/dhd/43602a3/rtecdc.bin": "brcmfmac43602-pcie.bin",
    }
    found = extract_jffs2_paths(oem_data, list(targets.keys()))
    if not found:
        sys.exit("ERROR: No Broadcom dhd Wi-Fi firmware blobs found in the supplied OEM image.")

    os.makedirs(out_dir, exist_ok=True)
    extracted = 0
    for src_path, dst_name in targets.items():
        blob = found.get(src_path)
        if not blob:
            print(f"  [WARN] {src_path} not present in OEM image")
            continue
        if b"noclminc" in blob:
            print(
                f"  [SKIP] {dst_name}: built with -noclminc (Zyxel V5.17+ blob without embedded CLM).\n"
                "         Standalone brcmfmac requires an embedded-CLM blob (from Zyxel V5.13) or\n"
                "         the default linux-firmware package already included in OpenWrt."
            )
            continue
        dst_path = os.path.join(out_dir, dst_name)
        with open(dst_path, "wb") as f:
            f.write(blob)
        ver_match = re.search(rb"(?:10\.10\.\d+\.\d+|7\.\d+\.\d+)", blob)
        ver_str = ver_match.group(0).decode("ascii") if ver_match else "unknown"
        print(f"  [OK]   Wrote {dst_path} ({len(blob)} bytes, fw={ver_str}, embedded CLM OK)")
        extracted += 1

    if extracted == 0:
        sys.exit("ERROR: No CLM-enabled Wi-Fi blobs were extracted (use a Zyxel V5.13 OEM .bin).")
    print(f"\nDone! Copy these files to /lib/firmware/brcm/ on your router and reboot:")
    print(f"  scp {out_dir}/brcmfmac*.bin root@192.168.1.1:/lib/firmware/brcm/")


def main():
    p = argparse.ArgumentParser(
        description="Build a legal, Web-GUI-flashable OpenWrt .ras.bin for Zyxel VMG8825-B50B"
    )
    p.add_argument("--inspect", metavar="IMAGE", help="Inspect any Zyxel/OpenWrt firmware image and exit")
    p.add_argument("--oem", help="Path to your official Zyxel VMG8825-B50B OEM firmware (.bin)")
    p.add_argument("--cferam", help="Alternative to --oem: path to cferam.XXX extracted directly from your router via SSH")
    p.add_argument("--openwrt", help="Path to public OpenWrt squashfs.bin (or .ras.bin)")
    p.add_argument("--extract-wifi-fw", metavar="OUTDIR",
                   help="Extract optional CLM-enabled V5.13 Wi-Fi blobs from --oem into OUTDIR and exit")
    p.add_argument("-o", "--output", default="openwrt-vmg8825-b50b-factory.ras.bin", help="Output .ras.bin path")
    p.add_argument("--brand", choices=["auto", "wind3", "generic"], default="auto",
                   help="Target router brand header: auto (copy from --oem), wind3 (ABLZ / 4543), or generic (ABNY / 454A)")
    p.add_argument("--fw-version", help="Override Zyxel fw-version string (default: derived from --brand or --oem)")
    p.add_argument("--model-id", help="Override 4-hex-char Zyxel model ID (default: derived from --brand or --oem)")
    args = p.parse_args()

    if args.inspect:
        cmd_inspect(args.inspect)
        return

    if args.extract_wifi_fw:
        if not args.oem:
            p.error("--extract-wifi-fw requires --oem <V513_OEM.bin>")
        cmd_extract_wifi_fw(args.oem, args.extract_wifi_fw)
        return

    if not args.openwrt or (not args.oem and not args.cferam):
        p.error("--openwrt and either --oem or --cferam are required (or use --inspect / --extract-wifi-fw)")

    product = "VMG8825-B50B"
    flashtype = FLASHTYPE_NAND128

    if args.oem:
        oem_data = open(args.oem, "rb").read()
        oem_info = parse_vzytoken(oem_data)
        if oem_info["format"] != "zyxel-ras":
            sys.exit(f"ERROR: {args.oem} does not have a valid BCM63138 Zyxel vZyToken trailer.")
        product = oem_info["product"] or product
        flashtype = oem_info["flashtype"]
        if args.brand == "wind3":
            fw_version, model_id = "V5.17(ABLZ.9)C0", "4543"
        elif args.brand == "generic":
            fw_version, model_id = "V5.17(ABNY.9)C0", "454A"
        else:
            fw_version, model_id = oem_info["fw_version"], oem_info["model_id"]
        print(f"[1/4] OEM header: product={product} fw_version={fw_version} model_id={model_id} crc_ok={oem_info['crc_ok']}")

        oem_files = extract_jffs2_files(oem_data)
        cferam_names = sorted(k for k in oem_files if re.fullmatch(r"cferam\.\d{3}", k))
        if not cferam_names:
            sys.exit(f"ERROR: No cferam.XXX found in {args.oem} bootfs (found: {list(oem_files.keys())})")
        cferam_name = cferam_names[-1]
        cferam_bin = oem_files[cferam_name]
        print(f"[2/4] Extracted {cferam_name} ({len(cferam_bin)} bytes) from OEM firmware")
    else:
        if args.brand == "wind3":
            fw_version, model_id = "V5.17(ABLZ.9)C0", "4543"
        elif args.brand == "generic":
            fw_version, model_id = "V5.17(ABNY.9)C0", "454A"
        else:
            if not (args.fw_version and args.model_id):
                p.error("When using --cferam without --oem, specify --brand wind3 or --brand generic (or --fw-version and --model-id)")
            fw_version, model_id = args.fw_version, args.model_id
        cferam_bin = open(args.cferam, "rb").read()
        if len(cferam_bin) < 65536:
            sys.exit(f"ERROR: {args.cferam} looks too small ({len(cferam_bin)} bytes) to be a valid BCM63138 cferam binary.")
        print(f"[1/4] Target header: product={product} fw_version={fw_version} model_id={model_id}")
        print(f"[2/4] Loaded {args.cferam} ({len(cferam_bin)} bytes)")

    if args.fw_version:
        fw_version = args.fw_version
    if args.model_id:
        model_id = args.model_id

    # Extract OpenWrt vmlinux.lz + UBI rootfs
    owrt_data = open(args.openwrt, "rb").read()
    owrt_info = parse_vzytoken(owrt_data)
    owrt_payload = owrt_data[: len(owrt_data) - owrt_info["trailer_len"]] if owrt_info["trailer_len"] else owrt_data
    owrt_files = extract_jffs2_files(owrt_payload)
    if "vmlinux.lz" not in owrt_files:
        sys.exit(f"ERROR: vmlinux.lz not found in {args.openwrt} bootfs (found: {list(owrt_files.keys())})")
    vmlinux_lz = owrt_files["vmlinux.lz"]
    ubi_off = find_ubi_offset(owrt_payload)
    ubi_data = owrt_payload[ubi_off:]
    print(f"[3/4] Extracted OpenWrt vmlinux.lz ({len(vmlinux_lz)} bytes) + UBI rootfs ({len(ubi_data)} bytes)")

    # Assemble fresh 8 MiB JFFS2 bootfs + UBI + Zyxel vZyToken
    bootfs = make_jffs2_bootfs([
        ("1-openwrt", b""),
        ("cferam.000", cferam_bin),
        ("vmlinux.lz", vmlinux_lz),
    ])
    merged = bootfs + ubi_data
    trailer = build_vzytoken(
        merged,
        product=product,
        fw_version=fw_version,
        model_id=model_id,
        flashtype=flashtype,
        flags=0,
        kernel_data=bootfs,
        rootfs_data=ubi_data,
    )
    with open(args.output, "wb") as f:
        f.write(merged)
        f.write(trailer)

    print(f"[4/4] Wrote {args.output} ({len(merged) + len(trailer)} bytes) — ready for Zyxel Web GUI upload!")


if __name__ == "__main__":
    main()
