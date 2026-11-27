# Optimization, Tuning & Use Cases

With **512 MiB DDR3 RAM**, **142 MiB writable UBIFS NAND**, a **dual-core 1.0 GHz Cortex-A9 CPU**, a **4-core Runner hardware NAT engine**, **5× Gigabit DSA Ethernet ports**, **AC2200 dual-band Wi-Fi**, and **USB 3.0**, the Zyxel VMG8825-B50B running OpenWrt goes far beyond a basic ISP router.

---

## 1. Wi-Fi Tuning & Optional 2020 OEM Wi-Fi Firmware Boost

### Recommended Default Wi-Fi Settings
- **Country Code:** Always set your real regulatory country code (e.g. `IT`, `DE`, `US`) in **Network → Wireless → Advanced Settings**.
- **5 GHz (`radio0` / BCM43664 4×4 AC):**
  - Width: `80 MHz`, Channel: `36` (non-DFS) or `100` (DFS, higher TX power in EU/ETSI).
  - Security: **WPA2-PSK (CCMP)** with optional **802.11w Management Frame Protection = Optional** (`ieee80211w='1'`).
  - *Note on WPA3:* Neither BCM43664 nor BCM43602 FullMAC firmware supports WPA3-SAE in AP mode (even Zyxel stock `V5.17` lacks AP SAE). Do not select WPA3-SAE in LuCI.
- **2.4 GHz (`radio1` / BCM43602 3×3 N):**
  - Width: `20 MHz` (best compatibility in crowded neighborhoods) or `40 MHz` (up to ~195 Mbps).
  - Channel: `1`, `6`, or `11`.

### Optional: Extract Your Own March 2020 `V5.13` Wi-Fi Blobs (+8 dB / +124 Mbps 5 GHz)
Public builds ship with official upstream `linux-firmware` (`brcmfmac-firmware-4366c0-pcie` + `brcmfmac-firmware-43602a1-pcie`), which work out of the box and are 100% legally redistributable.

If you want higher 5 GHz signal strength (**+8 dB** TX/RX) and faster single-stream 5 GHz throughput (**~551–600 Mbps** vs ~427 Mbps), you can legally extract the March 2020 CLM-enabled Broadcom radio blobs (`10.10.122.26` / `7.76.6`) from your **own** Zyxel `V5.13` firmware onto OpenWrt's writable `/overlay`:

#### Option 1 — Directly on the Router (if your untouched `firmware_backup` bank has Zyxel `V5.13`)
```bash
ssh root@192.168.1.1
vmg8825-extract-wifi-fw
reboot
```
To check active firmware versions or revert back to default `linux-firmware` anytime:
```bash
vmg8825-extract-wifi-fw --status
vmg8825-extract-wifi-fw --restore && reboot
```

#### Option 2 — From a Local Official Zyxel `V5.13` OEM `.bin` on Your PC
If your router's backup bank has `V5.17` (whose blobs use `-noclminc` and are automatically skipped), extract the CLM-enabled blobs from any official Zyxel `V5.13` OEM `.bin` on your PC:
```bash
python3 scripts/vmg8825_factory_builder.py \
  --oem /path/to/V513ABNY2C0.bin \
  --extract-wifi-fw ./wifi_fw

scp ./wifi_fw/brcmfmac*.bin root@192.168.1.1:/lib/firmware/brcm/
ssh root@192.168.1.1 reboot
```

---

## 2. Installing Packages (`apk`) & Kernel Modules (`kmod-*`)

OpenWrt snapshots on this port use the **`apk`** package manager:
- **All 6 official upstream OpenWrt `arm_cortex-a9` feeds** (`base`, `luci`, `packages`, `routing`, `telephony`, `video`) work natively out of the box — thousands of user-space packages (`adguardhome`, `samba4-server`, `htop`, `iperf3`, `tailscale`, `ddns-scripts`, `luci-app-*`, etc.) install immediately with:
  ```bash
  apk update
  apk add htop iperf3
  ```
- **Kernel modules (`kmod-*`)** must match the exact custom `bcm63xx_arm` kernel vermagic. To install additional `kmod-*` packages beyond those pre-installed in the firmware image, add this repository's GitHub Pages `kmod-*` feed (published automatically by GitHub Actions on each release):
  ```bash
  # Download the release signing public key (replace owner/repo if forked):
  uclient-fetch -O /etc/apk/keys/vmg8825-kmod.pem \
    https://4d000.github.io/openwrt-zyxel-vmg8825-b50b/public-key.pem

  # Point the target feed to GitHub Pages:
  echo "https://4d000.github.io/openwrt-zyxel-vmg8825-b50b/targets/bcm63xx_arm/generic/packages/packages.adb" \
    > /etc/apk/repositories.d/customfeeds.list

  apk update
  ```

---

## 3. High-Value Use Cases Beyond a Basic Router

### A. High-Speed WireGuard / OpenVPN DCO VPN Gateway
Both **WireGuard** (`kmod-wireguard`, `wireguard-tools`, `luci-proto-wireguard`) and **OpenVPN 2.7 with Kernel DCO** (`openvpn-openssl`, `kmod-ovpn-backports`, `luci-proto-openvpn`) are **pre-installed** in the default image!
- **Measured WireGuard throughput:** **260.8 Mbps** real TCP (encrypts an entire 200+ Mbps home connection without breaking a sweat).
- **Measured OpenVPN 2.7 DCO throughput:** **208.8 Mbps** real TCP (`CHACHA20-POLY1305` / `AES-256-GCM` offloaded in kernel space).
- Configure directly via LuCI: **Network → Interfaces → Add new interface → WireGuard VPN** (includes QR code generation via `luci-lib-uqr`).

### B. Network-Wide AdGuard Home / Encrypted DNS Appliance
Unlike typical 16 MiB flash routers, the VMG8825-B50B has **512 MiB RAM** and **142 MiB free NAND `/overlay`** — easily enough to run **AdGuard Home** directly on NAND (or on a USB 3.0 flash drive to minimize NAND wear from query logs):
```bash
apk update
apk add adguardhome
```
*Tip:* Set AdGuard Home's query log storage to in-memory (or a mounted USB stick) and disable high-frequency stats disk writes so SLC NAND lasts decades.

### C. 5-Port Managed VLAN Gigabit Switch + AC2200 Dumb AP
Because all 5 Ethernet ports (`lan1`, `lan2`, `lan3`, `lan4`, `wan`) are exposed via Linux **DSA** (`bcm_sf2`), you can turn the VMG8825-B50B into a **5-port 802.1Q VLAN-aware managed switch + high-power AC2200 Wi-Fi Access Point**:
1. Add `wan` into `br-lan` (giving you 5 unified Gigabit switch ports).
2. Enable **Bridge VLAN Filtering** under **Network → Interfaces → Devices → `br-lan` → Configure → Bridge VLAN filtering** to tag/untag separate Guest, IoT, Management, and Main VLANs per port and per SSID.
3. Disable DHCP/DNS on `lan`, assign a static IP on your main subnet, and enjoy wire-speed L2 switching across all 5 ports + 4×4 5 GHz Wi-Fi.

### D. Lightweight USB 3.0 NAS / Media Server
The rear **USB 3.0** port supports full xHCI + UAS (`kmod-usb-storage-uas` pre-installed):
```bash
apk update
apk add kmod-fs-ext4 block-mount samba4-server luci-app-samba4
```
Format a USB 3.0 SSD/HDD as `ext4` for best speed and reliability.

### E. Built-in Performance Tuning (Already Pre-Configured)
You do **not** need to manually tweak IRQ/RPS or firewall offloading — `etc/uci-defaults/99_vmg8825_perf`, `etc/init.d/vmg8825-irq`, `etc/hotplug.d/net/99-vmg8825-rps`, and `etc/sysctl.d/99-vmg8825.conf` automatically configure:
- **4-Core Broadcom Runner Hardware NAT** (`flow_offloading=1`, `flow_offloading_hw=1`) with `FLOW_OFFLOAD_XMIT_DIRECT` bridge bypass (`940+ Mbps` WAN↔LAN at `~6%` CPU).
- **2-Stage Dual-Core Pipeline:** `eth0` IRQ pinned to `CPU1`, `eth0` RPS on `CPU0`, Wi-Fi RPS spread across `CPU0+CPU1`, and PL310 L2 cache + `DHDHDR` TX cache-sync fast-path for `brcmfmac`.
