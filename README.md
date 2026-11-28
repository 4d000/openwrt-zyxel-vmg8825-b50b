# OpenWrt for Zyxel VMG8825-B50B (`bcm63xx_arm` / BCM63138)

[![License: GPL-2.0-only](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](LICENSE)
[![Linux Kernel](https://img.shields.io/badge/Linux-6.18-brightgreen.svg)](https://kernel.org)
[![Hardware NAT](https://img.shields.io/badge/Hardware%20NAT-4--Core%20Runner%20940%20Mbps-orange.svg)](#performance-openwrt-vs-stock-zyxel-oem)

Mainline **OpenWrt (Linux 6.18)** port for the **Zyxel VMG8825-B50B** (also widely sold in Italy as the **WindTre Home&Life HUB**), powered by the Broadcom **BCM63138** dual-core ARM Cortex-A9 SoC.

> **100% Legal & Open Source:** This repository contains **zero proprietary Zyxel or Broadcom closed-source binaries** (no `cferam`, no OEM Wi-Fi blobs, no private calibration dumps). Public releases use official `linux-firmware` for Wi-Fi and include an open-source zero-dependency Python tool (`scripts/vmg8825_factory_builder.py`) so you assemble your first-time Web GUI installer locally using your **own** router's `cferam` or official Zyxel firmware. Every future OpenWrt upgrade preserves your router's `cferam` automatically.

> **Disclaimer — Use at Your Own Risk:** This project is provided **"AS IS"**, without warranty of any kind (express or implied). Flashing third-party firmware may void your hardware warranty and carries an inherent risk of rendering your device inoperable ("bricking") if instructions are not followed, power is lost during flashing, or hardware variants differ. **You are solely responsible** for backing up your device and for any damage, data loss, or service interruption resulting from using these files or guides. This is an independent community project and is not affiliated with or endorsed by Zyxel, Broadcom, WindTre, or the OpenWrt project.

---

## Quick Navigation

- **[Installation, Backup & Return-to-Stock Guide](docs/INSTALLATION.md)** — Complete step-by-step instructions for **every user type** (standard Web GUI without SSH, users who already have root/supervisor SSH, serial UART/CFE recovery, how to legally obtain OEM files, full NAND backup, and 2-click return to stock Zyxel firmware).
- **[Packages (`apk` Feeds) & Firmware Updates Guide](docs/PACKAGES_AND_UPDATES.md)** — How to add the GitHub Pages `kmod-*` kernel module feed, install packages via `apk`, update firmware with `vmg8825-upgrade`, and configure GitHub Actions without wasting quota.
- **[Optimization & Use Cases Guide](docs/OPTIMIZATION_AND_USE_CASES.md)** — Optional 2020 OEM Wi-Fi firmware boost, WireGuard / OpenVPN DCO VPN gateway, AdGuard Home, 5-port 802.1Q managed VLAN switch + multi-SSID AC2200 Access Point, and USB 3.0 NAS.
- **[Hardware Reference & Tinkering Guide](docs/TINKERING_AND_HARDWARE.md)** — Exact 512 MiB NAND MTD layout, CFE serial console cheatsheet, PCIe/DSA/SFP/GPIO map, and live diagnostics (`vmg8825-diag`, `vmg8825_mrd_info.py`).
- **[Contributing & Porting Guide](docs/CONTRIBUTING_AND_PORTING.md)** — Local builds (`./build.sh`), open roadmap (SFP SerDes, upstreaming to `openwrt/openwrt`), and porting sibling `BCM63138` routers.

---

## Hardware Specification

| Component | Details |
| :--- | :--- |
| **SoC** | Broadcom BCM63138 (Dual-Core ARM Cortex-A9 @ 1.0 GHz, PL310 L2 cache enabled) |
| **Network Accelerator** | Broadcom RDP / 4-Core Runner (`bcm63138_rdp` + Linux `nf_flow_table` Hardware Offload) |
| **RAM** | 512 MiB DDR3 |
| **Flash** | 512 MiB SLC NAND (Dual-bank layout: ~188 MiB per bank, **142 MiB writable UBIFS `/overlay`**) |
| **Ethernet** | 5× Gigabit Ethernet (`lan1`–`lan4` + `wan`) via BCM63138 StarFighter2 DSA switch |
| **5 GHz Wi-Fi** | Broadcom BCM43664 4×4:4 802.11ac Wave 2 (PCIe, `brcmfmac` + `DHDHDR` fast-path) |
| **2.4 GHz Wi-Fi** | Broadcom BCM43602 3×3:3 802.11n (PCIe, `brcmfmac`) |
| **USB** | 1× USB 3.0 (xHCI + UAS mass storage support) |
| **Bootloader** | Broadcom CFE (`cferom` + dual-bank `cferam.XXX`, 100% preserved) |

---

## Feature Matrix: What Works & What Doesn't

| Feature | Status | Notes |
| :--- | :---: | :--- |
| **Native Persistent NAND Boot** | Working | Dual-bank `ofpart_bcm4908` (`bootfs` + UBI `rootfs` + 142 MiB UBIFS `/overlay`) |
| **Zero-Serial Web GUI Install** | Working | Tested live on WindTre `ABLZ` (`4543`) and Generic `ABNY` (`454A`) |
| **Live OpenWrt Upgrades (`sysupgrade`)** | Working | Preserves `/etc/config`, SSH keys, and `cferam.XXX` automatically |
| **2-Click Return to Stock Zyxel OEM** | Working | Flash any official Zyxel `.bin` directly from LuCI (`platform.sh` handles JFFS2 + `/data` reset) |
| **Gigabit LAN / WAN Routing + NAT** | Working | **940+ Mbps** line-rate via 4-Core Runner HW NAT (`~5–7%` CPU load!) |
| **5× Gigabit Managed Switch (DSA)** | Working | Full per-port VLAN filtering (`lan1`–`lan4` + `wan`) |
| **5 GHz Wi-Fi (BCM43664 4×4 AC)** | Working | Works out of the box (`linux-firmware`); up to **~600 Mbps** with optional local V5.13 blob extract |
| **2.4 GHz Wi-Fi (BCM43602 3×3 N)** | Working | **151–195 Mbps** TCP (outperforms stock Zyxel OEM) |
| **WireGuard VPN** | Working | **260.8 Mbps** real TCP throughput (4× faster than stock OpenVPN) |
| **OpenVPN 2.7 DCO (Kernel Offload)** | Working | **208.8 Mbps** real TCP throughput |
| **USB 3.0 (Storage / UAS / Extroot)** | Working | Full xHCI USB 3.0 + `kmod-usb-storage-uas` |
| **LEDs & Buttons** | Working | Per-port LAN/WAN/Internet/Wi-Fi LEDs + Reset button |
| **LuCI Web Interface (HTTPS)** | Working | Pre-installed on `https://192.168.1.1` with OpenSSL TLS |
| **WPA3-SAE (AP Mode)** | Not supported | Hardware/firmware limitation of BCM43664/BCM43602 FullMAC (even stock V5.17 lacks AP SAE). Use **WPA2-PSK (CCMP) + 802.11w PMF**. |
| **xDSL / VDSL2 Modem** | Not supported | Proprietary Broadcom closed-source xDSL PHY/firmware (unsupported across all mainline OpenWrt Broadcom targets). |
| **VoIP / FXS Phone Ports** | Not supported | Proprietary Broadcom closed-source SLIC/DSP stack. |
| **SFP Cage** | WIP / Not working yet | I2C EEPROM/DOM probing works (`kmod-sfp`), BCM63138 SerDes bring-up is open for contributions. |

---

## Who Is This Project For?

### Ideal for you if:
- You have **FTTH / FTTC (with external ONT or bridge modem)** connected via the Gigabit Ethernet **WAN** port and want a modern, secure **Linux 6.18** router with **940 Mbps hardware-offloaded NAT**.
- You want to repurpose an inexpensive/spare **Zyxel VMG8825-B50B** or **WindTre Home&Life HUB** into:
  - A high-speed **WireGuard / OpenVPN DCO VPN router** (200–260 Mbps)
  - A **5-port 802.1Q managed VLAN Gigabit switch + multi-SSID AC2200 Access Point** (supports per-port VLAN tagging, isolated Guest/IoT SSIDs, 802.11r fast roaming — either as a standalone L3 router or in AP/switch mode behind another main router)
  - A **network-wide AdGuard Home / DNS-over-HTTPS** appliance (512 MiB RAM + 142 MiB NAND storage)
  - A lightweight **USB 3.0 home NAS / SMB / DLNA** box
- You want to replace the end-of-life Zyxel stock firmware (stuck on **Linux 3.4 from 2012** with unpatched CVEs and ISP TR-069 remote management).

### Stay on stock Zyxel firmware if:
- You rely on the built-in **VDSL2/ADSL RJ11 modem** directly into a phone wall socket (without an external modem/ONT).
- You rely on the built-in **FXS analog telephone ports** (VoIP).
- You plug a **GPON SFP stick** directly into the SFP cage (until SerDes support is implemented).

---

## Performance: OpenWrt vs. Stock Zyxel OEM

All benchmarks were measured on real VMG8825-B50B hardware (Linux 6.18.60, 4-core Runner HW NAT enabled, 2×2 802.11ac 80 MHz client at 866.7 Mbps PHY):

| Test | Stock Zyxel OEM (`V5.13`) | OpenWrt (Default `linux-firmware`) | OpenWrt (+ Local `V5.13` Wi-Fi Blobs) |
| :--- | :---: | :---: | :---: |
| **Linux Kernel** | 3.4.11 (2012, EOL) | **6.18.60** | **6.18.60** |
| **WAN ↔ LAN NAT (1× TCP)** | 938.7 Mbps | **941.3 Mbps** (`~6%` CPU) | **941.3 Mbps** (`~6%` CPU) |
| **WAN ↔ LAN NAT (4× TCP)** | 944.9 Mbps | **942.0 Mbps** (`~7%` CPU) | **942.0 Mbps** (`~7%` CPU) |
| **5 GHz Wi-Fi DL (1× / 4× TCP)** | 638.7 / 655.0 Mbps | ~427 / ~515 Mbps | **551.3 / 578.5 Mbps** |
| **5 GHz Wi-Fi UL (1× / 4× / 8× TCP)** | 604.8 / 501.4 / 493.9 Mbps | ~480 / ~505 / ~520 Mbps | **584.1 / 561.9 / 599.9 Mbps** |
| **2.4 GHz Wi-Fi (1× / 4× TCP)** | 100.6 / 87.7 Mbps | **151.7 / 195.2 Mbps** | **151.7 / 195.2 Mbps** |
| **WireGuard VPN (TCP)** | Not available | **260.8 Mbps** | **260.8 Mbps** |
| **OpenVPN 2.7 DCO (TCP)** | Not available | **208.8 Mbps** | **208.8 Mbps** |
| **Writable NAND Storage** | Locked | **142.2 MiB UBIFS `/overlay`** | **142.2 MiB UBIFS `/overlay`** |

---

## Quick Start (Zero-Serial Web GUI Install)

1. Download `openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin` and `vmg8825_factory_builder.py` from **[Releases](../../releases)** (or build locally with `./build.sh`).
2. Obtain an official Zyxel `VMG8825-B50B` OEM `.bin` firmware file (see [docs/INSTALLATION.md#how-to-legally-obtain-required-proprietary-files](docs/INSTALLATION.md#how-to-legally-obtain-required-proprietary-files)).
3. Assemble your Web-GUI-flashable `.ras.bin` locally (Python 3, zero external dependencies, works on Windows/macOS/Linux):
   ```bash
   # For WindTre Home&Life HUB (stock firmware shows ABLZ):
   python3 scripts/vmg8825_factory_builder.py \
     --oem /path/to/official_zyxel_vmg8825.bin \
     --openwrt openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin \
     --brand wind3 \
     -o openwrt-vmg8825-factory.ras.bin

   # For Generic Unbranded Zyxel VMG8825-B50B (stock firmware shows ABNY):
   python3 scripts/vmg8825_factory_builder.py \
     --oem /path/to/official_zyxel_vmg8825.bin \
     --openwrt openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin \
     --brand generic \
     -o openwrt-vmg8825-factory.ras.bin
   ```
4. Upload `openwrt-vmg8825-factory.ras.bin` in your Zyxel Web GUI (**Maintenance → Firmware Upgrade**).
5. Wait ~2–3 minutes until the router reboots into OpenWrt at **`https://192.168.1.1`** (user `root`, no password initially — set one immediately!), then **back up your NAND partitions** as described in [docs/INSTALLATION.md](docs/INSTALLATION.md).

---

## Legal Compliance & Licensing

- **Code License:** All original OpenWrt target files, patches, DTS, scripts, and tooling in this repository are licensed under **GPL-2.0-only** (see [LICENSE](LICENSE)).
- **Broadcom RDP / Runner Driver (`bcm63138_rdp/`):** Derived from Broadcom's official GPLv2-licensed BCM63138 SDK (published in Zyxel's GPL source release); every file carries Broadcom's `DUAL/GPL` header.
- **Wi-Fi Firmware:** Public builds pull official upstream `linux-firmware` (`brcmfmac-firmware-4366c0-pcie` and `brcmfmac-firmware-43602a1-pcie`), redistributed under `LICENCE.broadcom_bcm43xx`.
- **Zero Proprietary Blobs:** Broadcom's `cferam` bootloader and Zyxel's OEM Wi-Fi blobs are **never** stored or distributed in this repository or its releases. Users extract them strictly locally from their own hardware or official Zyxel firmware.

---

## How You Can Help

- **Test & Report Issues:** Open an Issue with your hardware variant (`ABLZ` / `ABNY`), kernel log (`dmesg`), and reproduction steps.
- **SFP SerDes Bring-Up:** Help bring up the BCM63138 SFP SerDes link (see [docs/CONTRIBUTING_AND_PORTING.md](docs/CONTRIBUTING_AND_PORTING.md)).
- **Upstream OpenWrt Support:** Help clean up and submit `bcm63xx_arm` patches toward mainline `openwrt/openwrt`.
- **Port Sibling BCM63138 Routers:** Reuse this `bcm63xx_arm` target + 4-core Runner HW NAT driver for other BCM63138 routers (Zyxel VMG8828, Sercomm WD300, Technicolor DGA4130/DGA4132, Netgear D7000).
