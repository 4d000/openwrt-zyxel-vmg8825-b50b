#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Zyxel VMG8825-B50B SSH Backup & Preparation Helper
#
# For users who ALREADY have SSH access to their router (either root/supervisor
# shell on stock Zyxel OEM firmware, or root SSH on OpenWrt).
#
# Modes:
#   backup [user@host] [outdir] [--full]
#     Backs up all critical factory MTD partitions (cferom / MRD / MACs,
#     romfile, data, rom-d, wwan, misc*) over SSH, saves /proc/mtd, generates
#     SHA256SUMS, and attempts to copy the device's own cferam.XXX binary so
#     you can assemble a factory .ras.bin with vmg8825_factory_builder.py even
#     without downloading an external OEM .bin file.
#     Add --full to also gzip-dump the large firmware / firmware_backup banks
#     (~188 MiB each, takes ~2-3 minutes).
#
#   unlock-gui [user@host]
#     For stock Zyxel OEM firmware with supervisor/root shell: disables
#     cross-brand FWID/Model checks (`zycli fwidcheck off`, `zycli modelcheck off`)
#     so the Zyxel Web GUI accepts either ABLZ or ABNY headers.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  vmg8825_ssh_backup_and_install.sh backup [user@host] [outdir] [--full]
  vmg8825_ssh_backup_and_install.sh unlock-gui [user@host]

Examples:
  # Backup critical factory partitions from stock Zyxel OEM (supervisor/root SSH):
  ./scripts/vmg8825_ssh_backup_and_install.sh backup root@192.168.1.1 ./vmg8825_backup

  # Full NAND backup (including 188 MiB firmware banks, gzip-compressed):
  ./scripts/vmg8825_ssh_backup_and_install.sh backup root@192.168.1.1 ./vmg8825_backup --full

  # Backup from an already-running OpenWrt router:
  ./scripts/vmg8825_ssh_backup_and_install.sh backup root@192.168.1.1 ./vmg8825_backup

  # Optional (stock Zyxel OEM with root/supervisor): relax Web GUI model/FWID checks:
  ./scripts/vmg8825_ssh_backup_and_install.sh unlock-gui root@192.168.1.1
EOF
	exit 1
}

[ $# -lt 1 ] && usage
CMD="$1"
shift

# Stock Zyxel OEM Dropbear uses older ssh-rsa / kex algorithms on some builds;
# these SSH options stay compatible with both OpenWrt and stock Zyxel OEM.
SSH_OPTS=(
	-o StrictHostKeyChecking=accept-new
	-o HostKeyAlgorithms=+ssh-rsa
	-o PubkeyAcceptedAlgorithms=+ssh-rsa
)

case "$CMD" in
backup)
	TARGET="${1:-root@192.168.1.1}"
	OUTDIR="${2:-./vmg8825_nand_backup_$(date +%Y%m%d_%H%M%S)}"
	FULL=0
	for arg in "$@"; do
		[ "$arg" = "--full" ] && FULL=1
	done

	mkdir -p "$OUTDIR"
	echo "[1/4] Reading /proc/mtd from $TARGET ..."
	ssh "${SSH_OPTS[@]}" "$TARGET" 'cat /proc/mtd' > "$OUTDIR/proc_mtd.txt"
	cat "$OUTDIR/proc_mtd.txt"

	echo "[2/4] Dumping factory MTD partitions into $OUTDIR ..."
	while IFS= read -r line; do
		case "$line" in
			mtd[0-9]*:*) ;;
			*) continue ;;
		esac
		dev=$(echo "$line" | cut -d: -f1)
		size_hex=$(echo "$line" | awk '{print $2}')
		name=$(echo "$line" | sed -n 's/.*"\(.*\)"/\1/p' | tr ' /' '__')
		size_dec=$((0x$size_hex))

		# Skip huge partitions (>16 MiB) unless --full is requested
		if [ "$size_dec" -gt 16777216 ] && [ "$FULL" = "0" ]; then
			echo "  [SKIP] $dev ($name, $((size_dec / 1048576)) MiB) — pass --full to include"
			continue
		fi

		if [ "$size_dec" -gt 16777216 ]; then
			out="$OUTDIR/${dev}_${name}.bin.gz"
			echo "  [DUMP] $dev ($name, $((size_dec / 1048576)) MiB) -> $out (gzip)..."
			ssh "${SSH_OPTS[@]}" "$TARGET" "cat /dev/${dev}ro 2>/dev/null || cat /dev/${dev}" | gzip -1 > "$out"
		else
			out="$OUTDIR/${dev}_${name}.bin"
			echo "  [DUMP] $dev ($name, $((size_dec / 1024)) KiB) -> $out ..."
			ssh "${SSH_OPTS[@]}" "$TARGET" "cat /dev/${dev}ro 2>/dev/null || cat /dev/${dev}" > "$out"
			if [ ! -s "$out" ]; then
				echo "  [WARN] $out is empty — check permissions on $TARGET"
			fi
		fi
	done < "$OUTDIR/proc_mtd.txt"

	echo "[3/4] Attempting to extract device's own cferam.XXX (for local factory image assembly)..."
	if ssh "${SSH_OPTS[@]}" "$TARGET" '
		for p in / /mnt /tmp; do
			f=$(ls "$p"/cferam.[0-9][0-9][0-9] 2>/dev/null | sort | tail -n 1)
			if [ -n "$f" ] && [ -s "$f" ]; then
				cat "$f"
				exit 0
			fi
		done
		for part in bootfs firmware firmware_backup; do
			grep -q "\"$part\"" /proc/mtd || continue
			mkdir -p /tmp/.cfe_mnt
			if mount -t jffs2 -o ro "mtd:$part" /tmp/.cfe_mnt 2>/dev/null; then
				f=$(ls /tmp/.cfe_mnt/cferam.[0-9][0-9][0-9] 2>/dev/null | sort | tail -n 1)
				if [ -n "$f" ] && [ -s "$f" ]; then
					cat "$f"
					umount /tmp/.cfe_mnt 2>/dev/null
					rmdir /tmp/.cfe_mnt 2>/dev/null
					exit 0
				fi
				umount /tmp/.cfe_mnt 2>/dev/null
			fi
			rmdir /tmp/.cfe_mnt 2>/dev/null
		done
		exit 1
	' > "$OUTDIR/cferam_router.bin"; then
		if [ -s "$OUTDIR/cferam_router.bin" ]; then
			echo "  [OK]   Extracted cferam ($(wc -c < "$OUTDIR/cferam_router.bin") bytes) -> $OUTDIR/cferam_router.bin"
		else
			rm -f "$OUTDIR/cferam_router.bin"
		fi
	else
		rm -f "$OUTDIR/cferam_router.bin"
		echo "  [INFO] Could not directly read cferam.XXX over SSH (you can still use --oem <official_zyxel.bin>)."
	fi

	echo "[4/4] Generating SHA256SUMS ..."
	(cd "$OUTDIR" && sha256sum * > SHA256SUMS)
	echo ""
	echo "Backup complete in: $OUTDIR"
	echo "Keep this backup folder PRIVATE — mtd0_cferom.bin contains your router's"
	echo "unique MAC addresses, serial number, and default Wi-Fi/admin credentials."
	if [ -s "$OUTDIR/cferam_router.bin" ]; then
		echo ""
		echo "Next step — assemble your Web-GUI-flashable OpenWrt .ras.bin:"
		echo "  python3 scripts/vmg8825_factory_builder.py \\"
		echo "    --cferam $OUTDIR/cferam_router.bin \\"
		echo "    --openwrt openwrt-bcm63xx_arm-generic-zyxel_vmg8825-b50b-squashfs.bin \\"
		echo "    --brand wind3   # or --brand generic for unbranded ABNY units"
	fi
	;;

unlock-gui)
	TARGET="${1:-root@192.168.1.1}"
	echo "Running zycli fwidcheck/modelcheck off on $TARGET ..."
	ssh "${SSH_OPTS[@]}" "$TARGET" 'zycli fwidcheck off && zycli modelcheck off && echo "OK: Web GUI FWID/Model checks relaxed."'
	;;

*)
	usage
	;;
esac
