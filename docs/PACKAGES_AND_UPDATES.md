# Packages (`apk` Feeds) & Firmware Updates Guide

This guide explains how package installation (`apk`), the custom GitHub Pages kernel module feed (`kmod-*`), and firmware updates work on this OpenWrt port.

---

## 1. How OpenWrt Packages Work on `bcm63xx_arm`

Modern OpenWrt snapshots use **`apk`** (Alpine Package Keeper) instead of legacy `opkg`. On the Zyxel VMG8825-B50B, packages come from **two sources**:

| Package Type | Examples | Source | Works Out of the Box? |
| :--- | :--- | :--- | :---: |
| **User-space packages (`arm_cortex-a9`)** | `adguardhome`, `samba4-server`, `htop`, `iperf3`, `tailscale`, `ddns-scripts`, `luci-app-*`, `openssh-sftp-server` | Official upstream OpenWrt servers (`downloads.openwrt.org/snapshots/packages/arm_cortex-a9/*`) | **Yes!** All 6 official feeds (`base`, `luci`, `packages`, `routing`, `telephony`, `video`) verify and install natively. |
| **Kernel modules (`kmod-*`)** | `kmod-fs-ext4`, `kmod-fs-exfat`, `kmod-usb-net-rndis`, `kmod-nft-tproxy`, `kmod-sched-cake`, `kmod-vxlan` | **This repository's GitHub Pages feed** (`https://4d000.github.io/openwrt-zyxel-vmg8825-b50b/...`) | Requires 2-command one-time setup below. |

### Why do `kmod-*` packages need a separate feed?
In Linux and OpenWrt, every kernel module (`kmod-*`) is strictly locked to the exact kernel version and configuration hash (**vermagic**). Because `bcm63xx_arm` is a custom target not yet in upstream `downloads.openwrt.org`, `kmod-*` packages compiled for your exact kernel are published automatically by GitHub Actions to **GitHub Pages** alongside every release.

*(Note: Core kernel modules — `kmod-brcmfmac`, `kmod-wireguard`, `kmod-ovpn-backports`, `kmod-usb-storage`, `kmod-usb-storage-uas`, `kmod-sfp`, `kmod-tun` — are already pre-installed inside the firmware image.)*

---

## 2. How to Add the GitHub `kmod-*` Package Feed

### Method A — Via SSH (Fastest, 3 Commands)
Connect to your router (`ssh root@192.168.1.1`) and run:

```sh
# 1. Download and trust the repository's apk public signing key:
uclient-fetch -O /etc/apk/keys/vmg8825-kmod.pem \
  https://4d000.github.io/openwrt-zyxel-vmg8825-b50b/public-key.pem

# 2. Add the GitHub Pages kmod-* feed to apk's custom feeds list:
cat <<'EOF' > /etc/apk/repositories.d/customfeeds.list
https://4d000.github.io/openwrt-zyxel-vmg8825-b50b/targets/bcm63xx_arm/generic/packages/packages.adb
EOF

# 3. Refresh package indexes:
apk update
```

*(If you forked this repository, replace `4d000` with your own GitHub username in both URLs.)*

### Verify It Works
```sh
# Search for kernel modules from the custom feed:
apk search kmod-fs-ext4

# Install any user-space package + kernel module seamlessly:
apk add kmod-fs-ext4 block-mount htop
```

### Method B — Via LuCI Web Interface
1. First import the signing key once via SSH (`uclient-fetch -O /etc/apk/keys/vmg8825-kmod.pem https://4d000.github.io/openwrt-zyxel-vmg8825-b50b/public-key.pem`).
2. Open **System → Software** (or **Package Manager**) → **Configure apk** → **customfeeds.list**.
3. Paste:
   ```text
   https://4d000.github.io/openwrt-zyxel-vmg8825-b50b/targets/bcm63xx_arm/generic/packages/packages.adb
   ```
4. Click **Save**, then click **Update lists**.

### Preserving Your Feed Across Firmware Upgrades
OpenWrt's `sysupgrade` automatically backs up `/etc/apk/repositories.d/` and `/etc/apk/keys/` if listed in `/etc/sysupgrade.conf`. To make sure your custom feed and key survive every firmware upgrade automatically, add them once:
```sh
grep -qxF "/etc/apk/keys/vmg8825-kmod.pem" /etc/sysupgrade.conf || echo "/etc/apk/keys/vmg8825-kmod.pem" >> /etc/sysupgrade.conf
grep -qxF "/etc/apk/repositories.d/customfeeds.list" /etc/sysupgrade.conf || echo "/etc/apk/repositories.d/customfeeds.list" >> /etc/sysupgrade.conf
```

---

## 3. Keeping One Persistent Signing Key Across Releases (For Repo Maintainers)

By default, OpenWrt generates a fresh ephemeral ECDSA signing key on every GitHub Actions build. If you maintain this repository (or a fork), you can generate **one** persistent ECDSA signing key and store it as a GitHub Actions Secret so users only ever trust `public-key.pem` **once**:

1. Generate an ECDSA P-256 keypair locally (on your PC, never commit the private key!):
   ```bash
   openssl ecparam -name prime256v1 -genkey -noout -out private-key.pem
   openssl ec -in private-key.pem -pubout -out public-key.pem
   ```
2. In your GitHub repo go to **Settings → Secrets and variables → Actions → New repository secret**:
   - Name: **`APK_SIGN_KEY`**
   - Value: Paste the full contents of `private-key.pem` (including `-----BEGIN EC PRIVATE KEY-----` and `-----END EC PRIVATE KEY-----`).
3. Delete `private-key.pem` from any temporary directory (keep a secure offline backup if desired).

When `APK_SIGN_KEY` is configured, [`.github/workflows/build-release.yml`](../.github/workflows/build-release.yml) automatically injects it into OpenWrt's build tree before signing `packages.adb`, and publishes the matching `public-key.pem` to GitHub Pages.

---

## 4. Upgrading OpenWrt Firmware

Once OpenWrt is installed on NAND, upgrading to a newer release takes **one command** (or 2 clicks in LuCI) and **never requires `vmg8825_factory_builder.py` again**:

### Option 1 — Automatic CLI Updater (`vmg8825-upgrade`)
Pre-installed on the router at `/usr/bin/vmg8825-upgrade`:
```sh
# Check if a newer GitHub Release exists (read-only):
vmg8825-upgrade --check

# Download latest squashfs.bin, verify SHA-256, and flash (keeps /etc/config + SSH keys):
vmg8825-upgrade

# Clean upgrade (resets settings to defaults):
vmg8825-upgrade --no-save
```
*(Fork maintainers: run `VMG8825_REPO=youruser/yourrepo vmg8825-upgrade` to pull from your fork.)*

### Option 2 — LuCI Web Interface
1. Download `openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin` from **[Releases](../../releases)** and verify it against `sha256sums`.
2. In LuCI, go to **System → Backup / Flash Firmware → Flash new firmware image**.
3. Upload the `.bin`, leave *"Keep settings"* checked, and click **Continue**.

### What happens safely behind the scenes during upgrade?
1. `platform.sh` rescues your router's **own** `cferam.XXX` from `mtd:bootfs` before erasing anything (if missing, it aborts before touching flash).
2. It flashes the new kernel (`vmlinux.lz`) into `bootfs`, mounts `bootfs` with **`-o compr=none`** (required by Broadcom `cferom`), and restores your `cferam.(max+1)`.
3. `nand_do_upgrade` writes the new UBI SquashFS `rootfs` while preserving UBI wear-leveling counters and your `/etc/config` backup archive.
4. Note that extra packages installed via `apk` after flashing are stored in `/overlay` and should be reinstalled after a firmware upgrade (you can save your installed package list first via `apk info > /etc/config/installed-packages.txt`).

---

## 5. GitHub Actions Usage & Billing (Zero Free-Tier Burn!)

- **Public Repositories:** Standard GitHub-hosted Linux runners (`ubuntu-24.04`) are **100% free with unlimited minutes** on public GitHub repositories — they do **not** consume your monthly Actions quota.
- **Private Repositories / Smart Resource Control:** Even when private (or to keep public builds fast and polite), [`.github/workflows/build-release.yml`](../.github/workflows/build-release.yml) is configured to prevent wasted runs:
  1. **Manual (`workflow_dispatch`) & explicit version tags (`v*`) ONLY** — normal `git push` commits and pull requests do **not** trigger OpenWrt compilation.
  2. **Concurrency guard (`cancel-in-progress: true`)** — prevents duplicate builds from running simultaneously.
  3. **Aggressive build caching** (`openwrt-src/dl`, `openwrt-src/staging_dir/host*`, and `~/.ccache`) — cuts rebuild time from ~60+ min down to ~15–20 min.
  4. **Selectable `build_all_kmods`:** Defaults to `false` on manual test runs (fast image-only build), and automatically enables `CONFIG_ALL_KMODS=y` on tagged `v*` releases (or when checked manually) so the full GitHub Pages `kmod-*` feed is built only when publishing a release.
  5. **Local alternative:** You can always build locally on your own machine (`./build.sh --all-kmods`) and upload artifacts with `gh release create`, using **0** GitHub Actions minutes.
