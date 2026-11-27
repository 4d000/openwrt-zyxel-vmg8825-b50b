# Hardware Reference & Tinkering Guide

Everything you need for safe hardware tinkering, diagnostics, serial console recovery, and kernel/driver hacking on the **Zyxel VMG8825-B50B (Broadcom BCM63138)**.

---

## 1. 512 MiB SLC NAND Flash Layout (`/proc/mtd`)

All offsets match [`bcm63138-zyxel-vmg8825-b50b.dts`](../openwrt-overlay/target/linux/bcm63xx_arm/files/arch/arm/boot/dts/broadcom/bcm63138-zyxel-vmg8825-b50b.dts) (eraseblock size: `128 KiB` / `0x20000`, page size: `2048 B`, 4-bit ECC per 512 B):

| MTD | Offset | Size | Label | Purpose & Safety |
| :--- | :---: | :---: | :--- | :--- |
| `mtd0` | `0x00000000` | `128 KiB` | `cferom` | **READ-ONLY! NEVER ERASE.** Primary Broadcom `cferom` bootloader + CFE NVRAM + Zyxel MRD (base MAC at `0x106a0`, Model ID, hardware config). |
| `mtd1` | `0x00020000` | `187.875 MiB` | `firmware` | Active firmware bank (dynamically swapped by `ofpart_bcm4908` based on highest `cferam.XXX`). Split dynamically into `bootfs` (`8 MiB` JFFS2) + `ubi` (`179.875 MiB` UBI: `rootfs` + `142.2 MiB` UBIFS `rootfs_data`). |
| `mtd2` | `0x0bc00000` | `187.875 MiB` | `firmware_backup` | Inactive second firmware bank (holds your untouched stock Zyxel OEM firmware after first Web GUI install!). |
| `mtd3` | `0x177e0000` | `128.125 MiB` | `misc1` | Zyxel auxiliary storage partition (untouched by OpenWrt). |
| `mtd4` | `0x1f800000` | `1 MiB` | `romfile` | Zyxel factory default config template (untouched by OpenWrt). |
| `mtd5` | `0x1f900000` | `4 MiB` | `data` | Zyxel runtime JFFS2 partition (`zcfg_config.json`, logs, TR-069). Automatically cleaned on OpenWrt → Stock OEM rollback. |
| `mtd6` | `0x1fd00000` | `1 MiB` | `rom-d` | Zyxel factory `rom-d` defaults (untouched by OpenWrt). |
| `mtd7` | `0x1fe00000` | `1 MiB` | `wwan` | Zyxel WWAN/LTE dongle config (untouched by OpenWrt). |

### How Broadcom Dual-Bank Boot (`cferam.XXX`) Works
At power-on, `cferom` (`mtd0`) scans the JFFS2 root directory of **both** `0x00020000` (bank 1) and `0x0bc00000` (bank 2) for `cferam.XXX` (`000`–`999`). It loads `cferam` and `vmlinux.lz` from whichever bank has the **higher** 3-digit suffix (modulo 1000).
- Our kernel patch `710-mtd-parsers-ofpart-bcm4908-cferam-active-bank.patch` reads the same `cferam.XXX` numbers at boot and automatically maps the active bank to `mtd:firmware` (`bootfs` + `ubi`) and the inactive bank to `mtd:firmware_backup`.
- On every `sysupgrade` or OEM rollback, `platform.sh` calculates `max(bank1, bank2) + 1`, guaranteeing CFE boots the newly flashed image cleanly.

---

## 2. Included Tinkering & Diagnostic Tools

### A. On-Router Diagnostics (`vmg8825-diag`)
Pre-installed at `/usr/bin/vmg8825-diag` (also in [`scripts/vmg8825-diag`](../scripts/vmg8825-diag)). Prints a complete, privacy-safe hardware & performance snapshot suitable for pasting directly into GitHub Issues:
```sh
vmg8825-diag
```
Checks:
- Kernel version, OpenWrt release, uptime, free RAM
- Active NAND bank & `cferam.XXX` index, UBIFS `/overlay` free space & wear counters
- 4-Core Broadcom Runner `[HW_OFFLOAD]` active conntrack flow count
- Ethernet DSA switch port link states (`lan1`–`lan4`, `wan`)
- IRQ affinity (`eth0`, PCIe Wi-Fi) & RPS masks
- Both Wi-Fi radios (`BCM43664` 5 GHz + `BCM43602` 2.4 GHz): active firmware versions and sources (`linux-firmware` vs local OEM extract)

### B. Local Hardware / MRD Backup Inspector (`vmg8825_mrd_info.py`)
Run on your PC against your **own** `mtd0_cferom.bin` backup or any Zyxel `.bin` firmware to inspect hardware board identity (Model ID `4543` vs `454A`, Board ID, MAC count, flash layout) without exposing secrets:
```bash
python3 scripts/vmg8825_mrd_info.py ./vmg8825_backup/mtd0_cferom.bin
```

### C. Firmware Builder & Inspector (`vmg8825_factory_builder.py`)
Inspect any Zyxel `.bin` or OpenWrt image (`vZyToken`, CRC32, Model ID, JFFS2 bootfs contents):
```bash
python3 scripts/vmg8825_factory_builder.py --inspect firmware.bin
```

---

## 3. Hardware Subsystem Map

### PCIe Devices
| PCIe Bus | Chip | Band / Specs | Driver | NVRAM File |
| :--- | :--- | :--- | :--- | :--- |
| `0000:01:00.0` | Broadcom **BCM43602** (`14e4:aa52`) | 2.4 GHz 3×3:3 802.11n | `brcmfmac` | `brcmfmac43602-pcie.zyxel,vmg8825-b50b.txt` |
| `0001:01:00.0` | Broadcom **BCM43664** (`14e4:4365`) | 5 GHz 4×4:4 802.11ac Wave 2 | `brcmfmac` (`DHDHDR`) | `brcmfmac4366c-pcie.zyxel,vmg8825-b50b.txt` |

### Ethernet Switch (`bcm_sf2` DSA)
| Switch Port | Linux Interface | PHY | Notes |
| :---: | :--- | :--- | :--- |
| `port@0` | `lan1` | Internal GPHY (`phy8`) | 10/100/1000 Mbps |
| `port@1` | `lan2` | Internal GPHY (`phy9`) | 10/100/1000 Mbps |
| `port@2` | `lan3` | Internal GPHY (`phy10`) | 10/100/1000 Mbps |
| `port@3` | `lan4` | Internal GPHY (`phy11`) | 10/100/1000 Mbps |
| `port@4` | `wan` | Internal GPHY (`phy12`) | 10/100/1000 Mbps |
| `port@7` | `sfp` (disabled) | SerDes (WIP) | I2C EEPROM/DOM wired (`i2c-gpio`) |
| `port@8` | `eth0` (`conduit`) | 1000 Mbps fixed-link | Connected to 4-core Runner RDP |

### Key GPIOs (Reference for Hardware Hackers)
- **Buttons (`gpio-keys-polled`):** Reset (`gpio1 0`), WPS (`gpio1 1`), Wi-Fi (`gpio1 3`) — all active-low.
- **Wi-Fi Reset / Disable Hogs:** 5 GHz reset (`gpio2 27`), 2.4 GHz reset (`gpio2 28`), 5 GHz disable (`gpio3 19`), 2.4 GHz disable (`gpio3 24`).
- **SFP Cage (`i2c-sfp` & `sff,sfp`):** SDA (`gpio3 21`), SCL (`gpio3 23`), MOD-DEF0 (`gpio2 14`), LOS (`gpio0 28`), TX-Fault (`gpio2 12`), TX-Disable (`gpio2 13`), SFP LED (`gpio0 20`).

---

## 4. Serial UART & CFE Bootloader Cheatsheet

- **Interface:** 3.3V TTL UART (**never 5V!**), `115200 8N1`, no flow control.
- Press any key immediately at power-on to break into the `CFE>` prompt.

Useful CFE commands:
- `p` — Print bootline and board parameters (shows active boot image, IP addresses, Board ID).
- `b 192.168.1.100:openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-initramfs.bin` — Boot OpenWrt `initramfs.bin` directly in RAM over TFTP (non-destructive!).
- `a` — Select which firmware partition (`1` = latest, `2` = previous/backup bank) CFE boots next (great way to jump back to your untouched stock bank!).
- `f 192.168.1.100:firmware.bin` — Flash a `.ras.bin` / OEM `.bin` over TFTP.
- `reset` — Reboot the board.

---

## 5. Verifying Hardware Offload & Performance Live

```sh
# 1. Confirm 4-Core Broadcom Runner HW NAT is offloading active WAN<->LAN flows:
grep -c HW_OFFLOAD /proc/net/nf_conntrack
grep HW_OFFLOAD /proc/net/nf_conntrack | head -n 5

# 2. Check per-CPU IRQ distribution (eth0 on CPU1, PCIe radios balanced):
cat /proc/interrupts | grep -E "eth0|brcm|pcie|xhci"

# 3. Inspect 5 GHz client PHY rate (e.g. 866.7 MBit/s VHT-MCS 9 80MHz):
iw dev phy0-ap0 station dump | grep -E "Station|signal|bitrate"

# 4. Inspect NAND UBI volume health and free LEBs:
ubinfo -a

# 5. Inspect connected SFP transceiver EEPROM (if an SFP module is inserted):
dmesg | grep -i sfp
```

---

## 6. Note on Stock Zyxel Supervisor Access

This repository intentionally does **not** include supervisor password generators, keygens, config decryptors, or credential-bypass tools for stock ISP firmware, to remain 100% compliant with legal and security standards.

You do **not** need stock supervisor access to install OpenWrt:
- **Path A ([docs/INSTALLATION.md](INSTALLATION.md#path-a-zero-serial-web-gui-install-recommended))** flashes OpenWrt directly through the standard user Web GUI without any supervisor or SSH access.
- Once OpenWrt boots, you immediately have **full native Linux `root` SSH access** with no ISP locks or TR-069 restrictions.
