# Complete Installation, Backup & Recovery Guide

This guide covers **every type of Zyxel VMG8825-B50B user** — whether you only have the standard ISP Web GUI, already have root/supervisor SSH access, or have a 3.3V USB-UART serial cable.

> **Disclaimer — Proceed at Your Own Risk:** Flashing custom firmware is done entirely at your own risk and may void your warranty. While the VMG8825-B50B's dual-bank NAND layout provides a strong safety net, **the authors and contributors are not responsible** for bricked devices, lost factory partitions (`mtd0`), ISP service issues, or any other damage. Read the [Backup Procedure](#3-best-practice-backup-procedure-do-not-skip) carefully and never power off the router while flashing.

---

## 1. Choose Your Installation Path

| Your Current Situation | Recommended Path | Serial Cable Needed? |
| :--- | :--- | :---: |
| **Standard stock router** (WindTre Home&Life HUB `ABLZ` or Generic Zyxel `ABNY`, Web GUI only, **NO SSH/root**) | **[Path A: Zero-Serial Web GUI Install](#path-a-zero-serial-web-gui-install-recommended)** | No |
| **Stock router with root / supervisor SSH access already enabled** | **[Path B: Root / Supervisor SSH Prep + Dual-Bank Install](#path-b-users-who-already-have-root--supervisor-ssh-on-stock-oem)** | No |
| **Bricked / Bootloop / Developer testing `initramfs`** | **[Path C: Serial UART / CFE TFTP Recovery](#path-c-serial-uart--cfe-tftp-install-or-recovery)** | Yes (3.3V) |
| **Already running OpenWrt on this router** | **[Upgrading OpenWrt](#6-upgrading-openwrt-cferam-preserved-automatically)** | No |
| **Want to revert to stock Zyxel OEM firmware** | **[Return to Stock Zyxel OEM (2-Click)](#7-returning-to-stock-zyxel-oem-firmware-2-click-zero-serial)** | No |

> **Dual-Bank Safety Net:** The VMG8825-B50B has **two 188 MiB firmware banks** (`firmware` and `firmware_backup`). When you install OpenWrt from stock Zyxel firmware, Zyxel flashes OpenWrt into the **inactive** bank and leaves your original stock Zyxel firmware completely intact in the other bank!

---

## 2. How to Legally Obtain Required Proprietary Files

To keep this project **100% legal**, we never distribute Broadcom's proprietary `cferam` bootloader or Zyxel OEM Wi-Fi blobs. Depending on your situation, here is how you legally obtain what you need:

### What do you actually need?
1. **`cferam.XXX` (~529 KB) — Required ONCE for first-time installation:**
   - Needed to assemble your first-time `.ras.bin` installer (after OpenWrt is on NAND, `sysupgrade` preserves your router's own `cferam.XXX` automatically forever).
   - **If you have root/supervisor SSH on stock OEM (Path B):** You do **NOT** even need to download an OEM `.bin` — our backup script (`scripts/vmg8825_ssh_backup_and_install.sh`) extracts `cferam` directly from your own router!
   - **If you only have Web GUI access (Path A):** Download **any** official Zyxel `VMG8825-B50B` OEM `.bin` firmware (either `ABLZ` or `ABNY`, any version `V5.13` or `V5.17`) and pass it to `--oem` in `vmg8825_factory_builder.py`.
2. **Optional March 2020 `V5.13` Wi-Fi firmware blobs — NOT required:**
   - OpenWrt ships with official `linux-firmware` Wi-Fi blobs and works out of the box.
   - If you want the optional **+8 dB / +124 Mbps 5 GHz boost**, you can legally extract the March 2020 CLM-enabled blobs directly from your router's untouched `firmware_backup` bank (if your unit came with `V5.13`) OR from an official Zyxel `V5.13` OEM `.bin` (see [docs/OPTIMIZATION_AND_USE_CASES.md](OPTIMIZATION_AND_USE_CASES.md)).
   - *Important:* Zyxel `V5.17` Wi-Fi blobs were compiled with `-noclminc` (no embedded CLM) and do **not** work with standalone `brcmfmac`. Our extractor scripts detect and skip them automatically.

### Where to legally find official Zyxel `VMG8825-B50B` OEM firmware `.bin` files:
1. **Official Zyxel Support / Download Library:** Search for `VMG8825-B50B` on [zyxel.com/support](https://www.zyxel.com) (generic unbranded `ABNY` releases such as `V5.13(ABNY.2)C0` or `V5.17(ABNY.7)C0`).
2. **Your ISP Support Portal:** WindTre Italy distributes `ABLZ` releases for the Home&Life HUB (`VMG8825-B50B`).
3. **Public Hardware Communities Archiving Official Zyxel Releases:** The Italian **HWUpgrade** official thread for Zyxel VMG8825-B50B / WindTre Home&Life HUB (`forum.hwupgrade.it`, thread `t=2871045`) maintains verified links and checksums to official Zyxel `ABNY` and WindTre `ABLZ` `.bin` releases.
4. **Always verify any downloaded `.bin` before using it:**
   ```bash
   python3 scripts/vmg8825_factory_builder.py --inspect /path/to/V513ABNY2C0.bin
   ```
   Confirm the output reports:
   - `format: zyxel-ras`
   - `chip: 63138`
   - `product: VMG8825-B50B`
   - `crc_ok: True` and `hdr_sum_ok: True`
   - A `cferam.XXX` entry under `bootfs files:`

---

## 3. Best-Practice Backup Procedure (Do Not Skip!)

Your router's NAND contains unique per-device factory data in **`mtd0` (`cferom`)** (base MAC address, serial number, hardware Model ID, default Wi-Fi/admin credentials) and Zyxel runtime data in `romfile`, `data`, `rom-d`, and `wwan`.

### Best procedure by user type:

| User Type | When to Back Up | Command |
| :--- | :--- | :--- |
| **Have root/supervisor SSH on stock OEM (Path B)** | **BEFORE** flashing OpenWrt | `./scripts/vmg8825_ssh_backup_and_install.sh backup root@192.168.1.1 ./vmg8825_backup --full` |
| **Standard Web GUI only (Path A, no SSH)** | 1) Export Zyxel config backup via Web GUI **before** flashing.<br>2) Run full NAND backup via OpenWrt SSH **immediately after** first OpenWrt boot! | `./scripts/vmg8825_ssh_backup_and_install.sh backup root@192.168.1.1 ./vmg8825_backup --full` |

> **Why post-boot backup works safely for Path A users:** Installing OpenWrt via the Zyxel Web GUI **never touches** `mtd0` (`cferom`), `romfile`, `data`, `rom-d`, `wwan`, OR your original OEM bank (`firmware_backup`). So backing up immediately after booting OpenWrt captures 100% of your factory partitions AND your untouched original stock OEM firmware!

> **Privacy Warning:** Store your `vmg8825_backup/` directory somewhere safe and **never upload `mtd0_cferom.bin` or `mtd5_data.bin` publicly** — they contain your router's unique MAC addresses, serial number, and credentials.

---

## Path A: Zero-Serial Web GUI Install (Recommended)

This works on completely stock routers with **no SSH, no supervisor password, and no serial cable**.

### Step A1 — Identify Your Router's Brand (`ABLZ` vs `ABNY`)
Log into your stock Zyxel Web GUI (usually `http://192.168.1.1`) and look at the **Firmware Version** on the home/status page:
- Contains **`ABLZ`** (e.g. `V5.13(ABLZ.1)b8`, `V5.17(ABLZ.x)`) → **WindTre Home&Life HUB** → use **`--brand wind3`** (`Model ID = 4543`)
- Contains **`ABNY`** (e.g. `V5.13(ABNY.2)C0`, `V5.17(ABNY.7)C0`) → **Generic Unbranded Zyxel** → use **`--brand generic`** (`Model ID = 454A`)

> **Important:** The running stock Zyxel Web GUI verifies that the uploaded image's Firmware ID (`ABLZ` vs `ABNY`) and hardware Model ID (`4543` vs `454A`) match the router. Picking the wrong `--brand` will safely reject the upload with *"Invalid image"* / *"Il file selezionato è un'immagine non valida"* (it will **not** brick the router).

### Step A2 — Download OpenWrt & Assemble Your `.ras.bin`
Download `openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin` and `vmg8825_factory_builder.py` from **[Releases](../../releases)** (verify `sha256sums`!).

Run the pure-Python builder on Windows, macOS, or Linux (Python 3.8+, zero extra packages needed):

**For WindTre Home&Life HUB (`ABLZ`):**
```bash
python3 vmg8825_factory_builder.py \
  --oem /path/to/official_zyxel_vmg8825.bin \
  --openwrt openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin \
  --brand wind3 \
  -o openwrt-vmg8825-b50b-wind3.ras.bin
```

**For Generic Unbranded Zyxel (`ABNY`):**
```bash
python3 vmg8825_factory_builder.py \
  --oem /path/to/official_zyxel_vmg8825.bin \
  --openwrt openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin \
  --brand generic \
  -o openwrt-vmg8825-b50b-generic.ras.bin
```

*(You can use ANY official VMG8825-B50B OEM `.bin` as `--oem` — even an `ABNY` file to build a `--brand wind3` image, because the builder extracts `cferam` and stamps the exact `--brand` header!)*

### Step A3 — Flash via Zyxel Web GUI
1. Connect your PC directly to **LAN1–LAN4** via Ethernet cable (avoid flashing over Wi-Fi).
2. Optional: Export your current Zyxel settings (*Maintenance → Backup/Restore*).
3. Open **Maintenance → Firmware Upgrade** in the Zyxel Web GUI.
4. Select `openwrt-vmg8825-b50b-*.ras.bin` and click **Upload**.
5. **Do not power off the router.** The upload, NAND write, and reboot take **2 to 3 minutes**.
6. Wait until your PC receives a DHCP lease in `192.168.1.x` and open **`https://192.168.1.1`** (accept the self-signed LuCI HTTPS certificate).
7. Log in as `root` (blank password initially), immediately set a strong root password, and enable Wi-Fi under **Network → Wireless** (Wi-Fi is disabled by default on fresh OpenWrt installs).
8. **Run your full NAND backup right away** from your PC:
   ```bash
   ./scripts/vmg8825_ssh_backup_and_install.sh backup root@192.168.1.1 ./vmg8825_backup --full
   ```

---

## Path B: Users Who Already Have Root / Supervisor SSH on Stock OEM

If you **already** have root or supervisor SSH access on your stock Zyxel firmware, you get three advantages:
1. Full pre-flash NAND backup **before** modifying anything.
2. Direct extraction of your router's own `cferam.XXX` — **no external OEM `.bin` download required**.
3. Optional `zycli` check bypass if you have a non-standard ISP branding.

### Step B1 — Full Pre-Flash NAND Backup & `cferam` Extraction
```bash
./scripts/vmg8825_ssh_backup_and_install.sh backup root@192.168.1.1 ./vmg8825_backup --full
```
*(Replace `root@192.168.1.1` with `supervisor@192.168.1.1` or your stock SSH user if applicable.)*

This creates `./vmg8825_backup/` containing all factory partitions (`mtd0_cferom.bin`, `data`, `romfile`, `rom-d`, `wwan`, full compressed firmware banks, `SHA256SUMS`) and `cferam_router.bin`.

### Step B2 — Assemble Your `.ras.bin` Using Your Own Extracted `cferam`
```bash
python3 scripts/vmg8825_factory_builder.py \
  --cferam ./vmg8825_backup/cferam_router.bin \
  --openwrt openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin \
  --brand wind3 \
  -o openwrt-vmg8825-b50b-factory.ras.bin
```
*(Use `--brand generic` for unbranded `ABNY` units.)*

### Step B3 — (Optional) Relax Cross-Brand Web GUI Checks
If your ISP uses a custom Firmware ID or Model ID other than `ABLZ`/`4543` or `ABNY`/`454A`:
```bash
./scripts/vmg8825_ssh_backup_and_install.sh unlock-gui root@192.168.1.1
```
*(Runs `zycli fwidcheck off && zycli modelcheck off`.)*

### Step B4 — Upload via Zyxel Web GUI
Upload `openwrt-vmg8825-b50b-factory.ras.bin` in the Zyxel Web GUI (**Maintenance → Firmware Upgrade**). Because Zyxel writes it to the inactive NAND bank, your original stock firmware remains untouched in the second bank!

---

## Path C: Serial UART / CFE TFTP Install or Recovery

Use this method for development, testing `initramfs.bin` in RAM without touching NAND, or recovering from a failed flash.

### UART Hardware Connection
- **Voltage:** **3.3V TTL ONLY** (never connect 5V or RS-232 — it will damage the SoC!).
- **Settings:** `115200` baud, `8N1`, no hardware/software flow control.
- Connect **GND**, **TX**, and **RX** to the 4-pin UART header on the mainboard (leave **VCC** disconnected!).

### Option C1 — Boot OpenWrt `initramfs.bin` in RAM (Non-Destructive)
1. Set your PC's Ethernet interface to static IP **`192.168.1.100/24`** and serve `openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-initramfs.bin` via a TFTP server.
2. Power on the router and press any key rapidly in the serial console to stop at the **`CFE>`** prompt.
3. Boot `initramfs` in RAM:
   ```text
   CFE> b 192.168.1.100:openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-initramfs.bin
   ```
4. Once OpenWrt boots in RAM, back up NAND if you haven't already, then copy your assembled `.ras.bin` (which includes `cferam.000`) to `/tmp` and flash permanently:
   ```bash
   scp openwrt-vmg8825-b50b-factory.ras.bin root@192.168.1.1:/tmp/fw.ras.bin
   ssh root@192.168.1.1 'sysupgrade -n /tmp/fw.ras.bin'
   ```

### Option C2 — CFE Web Recovery (If Router Won't Boot)
1. Set your PC's Ethernet interface to **`192.168.1.2/24`**.
2. Power off the router, hold the recessed **Reset** button, power on, and hold Reset for ~10–15 seconds until CFE enters recovery mode.
3. Open `http://192.168.1.1` in a browser and upload an official Zyxel OEM `.bin` (or your assembled `.ras.bin`).
4. Alternatively, from the `CFE>` serial prompt, you can switch back to the untouched second NAND bank using CFE's image selection command (`a` / `c`).

---

## 6. Upgrading OpenWrt (`cferam` Preserved Automatically!)

Once OpenWrt is installed on NAND, you **never** need `vmg8825_factory_builder.py` again for OpenWrt upgrades!

`platform.sh` automatically rescues your router's own `cferam.XXX` from `mtd:bootfs`, flashes the new public GPLv2 `squashfs.bin`, and writes `cferam.(max+1)` back uncompressed (`-o compr=none`) while preserving `/etc/config` and SSH keys.

### Method 1 — LuCI Web GUI (1-Click)
1. Download `openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin` from **[Releases](../../releases)**.
2. Go to **System → Backup / Flash Firmware → Flash new firmware image**.
3. Upload the `.bin`, keep *"Keep settings and retain the current configuration"* checked, and click **Continue**.

### Method 2 — SSH One-Liner (Auto-Checks SHA-256)
```bash
# Check latest release without flashing:
vmg8825-upgrade --check

# Download, verify SHA-256, and upgrade (preserves config):
vmg8825-upgrade
```

---

## 7. Returning to Stock Zyxel OEM Firmware (2-Click Zero-Serial)

You can revert to **any** official Zyxel `VMG8825-B50B` OEM firmware (`ABLZ` or `ABNY`, `V5.13` or `V5.17`) directly from OpenWrt LuCI — no serial cable needed!

1. Open LuCI: **System → Backup / Flash Firmware → Flash new firmware image**.
2. Select your official Zyxel OEM `.bin` file (e.g. `V513ABNY2C0.bin` or `V513ABLZ1b8.bin`).
3. Click **Upload** (OpenWrt automatically disables *"Keep settings"* for OEM images), then click **Continue**.
4. Wait ~2–3 minutes for reboot.

**What `platform.sh` automatically does under the hood for a clean stock boot:**
- Validates the 1024-byte Zyxel `vZyToken` trailer (`BCM63138` / `VMG8825-B50B`) and JFFS2 magic (`0x1985`).
- Detaches UBI, writes the raw unified JFFS2 OEM image to `mtd:firmware`, and bumps `cferam.XXX` strictly higher than both banks so CFE boots the stock firmware.
- Clears stale `zcfg_config.json*` from `mtd:data` so cross-branch rollbacks (e.g. WindTre `ABLZ` → Generic `ABNY`) regenerate a clean stock configuration instead of hanging `zcmd`.

---

## 8. Troubleshooting

| Symptom | Cause & Fix |
| :--- | :--- |
| Zyxel Web GUI says **"Invalid image"** / **"Il file selezionato è un'immagine non valida"** | Mismatched `--brand`! Check whether stock firmware shows `ABLZ` (`--brand wind3`, Model ID `4543`) or `ABNY` (`--brand generic`, Model ID `454A`), rebuild with `vmg8825_factory_builder.py`, and re-upload. |
| Browser warns about certificate on `https://192.168.1.1` after flashing OpenWrt | Normal! OpenWrt generates a unique self-signed HTTPS certificate per router on first boot. Click *Advanced → Proceed*. |
| Wi-Fi SSIDs don't appear after first OpenWrt boot | Normal! OpenWrt disables Wi-Fi by default for security. Log into LuCI → **Network → Wireless**, set your country code, configure WPA2-PSK (CCMP), and click **Enable**. |
| `sysupgrade` from `initramfs` refuses public `squashfs.bin` | Public `squashfs.bin` intentionally omits proprietary `cferam.000`. On first-time flash from `initramfs`, pass a `.ras.bin` built with `vmg8825_factory_builder.py`. |
| Can't reach `192.168.1.1` after return-to-stock | Release/renew your PC's DHCP lease (`ipconfig /renew` or reconnect Ethernet) and allow up to 3 minutes for Zyxel's first-boot `zcfg_config.json` regeneration. |
