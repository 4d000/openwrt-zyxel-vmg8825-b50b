# Contributing, Local Building & Porting Sibling BCM63138 Routers

Contributions, bug reports, hardware testing, and ports for sibling **Broadcom BCM63138** devices are very welcome!

---

## 1. Repository Layout

```text
.
├── .github/workflows/build-release.yml   # Automated CI build + GitHub Release + Pages kmod feed
├── build.sh                              # Reproducible one-command local OpenWrt build script
├── docs/                                 # User, tuning, and developer documentation
├── patches/upstream/
│   └── 001-bcm63xx-arm-openwrt-integration.patch  # 3 tiny upstream OpenWrt diffs
├── openwrt-overlay/                      # Clean GPLv2 bcm63xx_arm target, patches, DTS, drivers
│   ├── package/kernel/mac80211/patches/brcm/      # brcmfmac DHDHDR, LED, TX pktid, VHT fixes
│   ├── package/network/utils/iwinfo/patches/      # BCM43664 PCI ID metadata
│   ├── scripts/                                   # addvtoken.py & vmg8825_factory_builder.py
│   └── target/linux/bcm63xx_arm/                  # Kernel 6.18 patches, DTS, RDP Runner driver, base-files
└── scripts/                              # Standalone end-user helpers
```

Upstream OpenWrt is pinned to tested commit **`3d5a743f1f0cb77aed8ed63b2f9aa75700b4322c`**.

---

## 2. Building Locally (Linux / WSL2)

### Prerequisites (Ubuntu / Debian)
```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential clang flex bison g++ gawk gcc-multilib g++-multilib \
  gettext git libncurses-dev libssl-dev python3-setuptools rsync swig \
  unzip zlib1g-dev file wget zstd
```

### Standard Legal Build (`squashfs.bin` + `initramfs.bin`)
```bash
git clone https://github.com/4d000/openwrt-zyxel-vmg8825-b50b.git
cd openwrt-zyxel-vmg8825-b50b
./build.sh
```

### Useful Build Flags
- `./build.sh --jobs 8` — Control parallel compilation threads.
- `./build.sh --all-kmods` — Also compile all `kmod-*` packages (`CONFIG_ALL_KMODS=y`).
- `./build.sh --cferam /path/to/your/cferam.bin` — Optionally supply your **own** local `cferam` during a private local build to also emit a `.ras.bin` directly in `openwrt-src/bin/targets/bcm63xx_arm/generic/` (never commit `cferam` to Git!).

---

## 3. Open Technical Roadmap (Where Help Is Needed Most!)

### 1. SFP Cage SerDes Bring-Up (High Impact)
- **Current status:** `kmod-sfp`, `i2c-gpio` bit-banged SFP I2C bus, TX-disable / LOS / MOD-DEF0 GPIOs, and SFP module EEPROM/DOM detection are already wired in the Device Tree (`bcm63138-zyxel-vmg8825-b50b.dts`).
- **What's missing:** Bringing up the BCM63138 StarFighter2 / Runner SerDes lane (SGMII / 1000BASE-X / 2500BASE-X) so an SFP optical transceiver or GPON stick passes Ethernet traffic cleanly via `phylink`.
- **Who can help:** Anyone with an SFP transceiver (copper 1000BASE-T or fiber 1000BASE-X / GPON stick), serial UART access, and Broadcom `bcm_sf2` / SerDes familiarity.

### 2. Cleaning Up & Upstreaming `bcm63xx_arm` to `openwrt/openwrt`
- Converting target kernel patches (`700`–`710`) into upstreamable Linux kernel + OpenWrt submissions.
- Evaluating splitting the `bcm63138_rdp` Runner driver or contributing cleaner `nf_flow_table` offload hooks.
- Rebasing onto upcoming OpenWrt stable release branches (`25.x` / `26.x`).

### 3. Porting Sibling Broadcom BCM63138 Routers
Because this repository already solves the hardest SoC-level bring-up hurdles on Linux 6.18 (**PL310 L2 cache**, **PCIe PMB power domains + imprecise abort handler**, **USB 3.0 PHY**, **StarFighter2 GPHY/DSA**, **4-core Runner HW NAT offload**, **dual-bank `cferam` MTD parser**, and **brcmfmac DHDHDR**), porting sibling BCM63138 boards mainly requires a new `.dts`, `01_leds`/`02_network` entries, and image packaging rules!

Great candidate sibling devices sharing **BCM63138**:
- **Zyxel VMG8828-B50B** / **VMG8924-B10D**
- **Sercomm WD300** (TIM Hub / Vodafone)
- **Technicolor DGA4130 / DGA4132** (TIM Hub — very popular in Italy!)
- **Netgear D7000 (v1)**
- **ASUS DSL-AC87VG**

### 4. Automated Testing & Documentation
- Testing on additional ISP-branded VMG8825-B50B variants (documenting their `Model ID` and `FWID` strings in `docs/INSTALLATION.md`).
- Translations (e.g., Italian installation guide for WindTre Home&Life HUB users).

---

## 4. Pull Request & Legal Policy (Strictly Enforced)

To keep this repository 100% legal and safe for everyone:
1. **NEVER commit proprietary binaries** (`cferam*`, Broadcom OEM `rtecdc.bin` / `brcmfmac*.bin`, stock Zyxel `.bin` images, or closed-source xDSL/VoIP blobs).
2. **NEVER commit private router data** (`mtd0_cferom.bin`, real MAC addresses, serial numbers, ISP PPPoE credentials, or Wi-Fi passwords).
3. **NEVER include password cracking / unauthorized bypass tools.** Document only official Web GUI workflows, standard `zycli` commands for users who already own root/supervisor access, and CFE UART recovery.
4. Preserve all SPDX license headers (`GPL-2.0-only` / `DUAL/GPL`).
5. Test changes on real hardware (or verify clean compilation with `./build.sh`) and include a brief summary + `dmesg` / benchmark output in your PR description.
