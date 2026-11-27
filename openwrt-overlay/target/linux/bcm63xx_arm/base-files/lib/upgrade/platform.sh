# SPDX-License-Identifier: GPL-2.0-only

RAMFS_COPY_BIN="bcm4908img dd expr grep head hexdump mtd mv sed sync tr ubidetach wc"

PART_NAME=firmware
BCM63138_FW_FORMAT=

get_hex_u32_le() {
	dd if="$1" skip="$2" bs=1 count=4 2>/dev/null | hexdump -v -e '1/4 "%08x"'
}

get_content() {
	dd if="$1" skip="$2" bs=1 count="$3" 2>/dev/null
}

# Prepare a bcm4908img-compatible file (strips the 1004-byte Zyxel vZyToken
# extension after the 20-byte Broadcom WFI trailer when flashing a .ras.bin or
# stock Zyxel VMG8825-B50B OEM firmware image).
platform_prepare_bcm4908img() {
	local src="$1"
	local dst="$2"
	local size wfi_ver chip_id product

	size=$(wc -c < "$src")
	if [ "$size" -gt 1024 ]; then
		wfi_ver=$(get_hex_u32_le "$src" $((size - 1020)))
		chip_id=$(get_hex_u32_le "$src" $((size - 1016)))
		product=$(get_content "$src" $((size - 1004)) 12)
		if [ "$wfi_ver" = "00005732" ] && [ "$chip_id" = "00063138" ]; then
			case "$product" in
				VMG8825-B50B*)
					BCM63138_FW_FORMAT="zyxel-ras"
					head -c $((size - 1004)) "$src" > "$dst"
					return 0
					;;
			esac
		fi
	fi

	if [ "$size" -gt 20 ]; then
		wfi_ver=$(get_hex_u32_le "$src" $((size - 16)))
		chip_id=$(get_hex_u32_le "$src" $((size - 12)))
		if [ "$wfi_ver" = "00005732" ] && [ "$chip_id" = "00063138" ]; then
			BCM63138_FW_FORMAT="bcm4908img"
			cp "$src" "$dst"
			return 0
		fi
	fi

	return 1
}

# Scan an MTD partition's JFFS2 bootfs (if present) for cferam.XXX index.
platform_read_cferam_idx() {
	local part="$1"
	local mtd dir idx

	mtd=$(find_mtd_part "$part")
	[ -z "$mtd" ] && return 1

	dir="/tmp/sysupgrade-cferam-$part"
	rm -rf "$dir"
	mkdir -p "$dir"
	if mount -t jffs2 -o ro "mtd:$part" "$dir" 2>/dev/null || mount -t jffs2 -o ro "$mtd" "$dir" 2>/dev/null; then
		idx=$(ls "$dir"/cferam.??? 2>/dev/null | sed -n 's/.*cferam\.\([0-9][0-9][0-9]\)/\1/p' | sort -n | tail -n 1)
		umount "$dir" 2>/dev/null
	fi
	rm -rf "$dir"

	[ -n "$idx" ] && echo "$idx" && return 0
	return 1
}

# Rescue the router's own cferam.XXX binary from existing NAND JFFS2 bootfs
# (used when upgrading from a public cferam-free OpenWrt squashfs.bin).
platform_save_existing_cferam() {
	local dst="$1"
	local part dir src

	rm -f "$dst"
	for part in bootfs firmware_backup firmware; do
		[ -n "$(find_mtd_index "$part")" ] || continue
		dir="/tmp/sysupgrade-rescue-$part"
		rm -rf "$dir"
		mkdir -p "$dir"
		if mount -t jffs2 -o ro "mtd:$part" "$dir" 2>/dev/null; then
			src=$(ls "$dir"/cferam.??? 2>/dev/null | sort | tail -n 1)
			if [ -n "$src" ] && [ -s "$src" ]; then
				cp "$src" "$dst"
			fi
			umount "$dir" 2>/dev/null
		fi
		rm -rf "$dir"
		[ -s "$dst" ] && return 0
	done

	return 1
}

# Compute next cferam.XXX strictly higher than BOTH firmware banks so CFE
# (cferom) is guaranteed to boot the newly flashed partition.
platform_calc_new_cferam() {
	local max_idx=-1 idx part parts

	# When bootfs is already split out (8 MiB), scan bootfs + firmware_backup
	# instead of scanning the entire 188 MiB firmware (which includes UBI).
	if [ -n "$(find_mtd_index bootfs)" ]; then
		parts="bootfs firmware_backup"
	else
		parts="firmware firmware_backup"
	fi

	for part in $parts; do
		idx=$(platform_read_cferam_idx "$part")
		if [ -n "$idx" ]; then
			idx=$(expr "$idx" + 0)
			[ "$idx" -gt "$max_idx" ] && max_idx="$idx"
		fi
	done

	[ "$max_idx" -lt 0 ] && max_idx=0
	printf "cferam.%03d" $(((max_idx + 1) % 1000))
}

platform_check_image() {
	[ "$#" -gt 1 ] && return 1

	local tmp="/tmp/sysupgrade-check.img"
	local magic bootfs_ls
	rm -f "$tmp"

	if ! platform_prepare_bcm4908img "$1" "$tmp"; then
		echo "Invalid image: expected BCM63138 bcm4908img or Zyxel VMG8825-B50B RAS/OEM firmware." >&2
		rm -f "$tmp"
		return 1
	fi

	if bcm4908img info -i "$tmp" >/dev/null 2>&1; then
		bootfs_ls=$(bcm4908img bootfs -i "$tmp" ls 2>/dev/null)
		if ! echo "$bootfs_ls" | grep -q "1-openwrt"; then
			notify_firmware_no_backup
		fi
		# Public cferam-free squashfs.bin requires an existing split bootfs
		# partition (OpenWrt already installed on NAND) so we can safely preserve
		# the device's own cferam.XXX inside the 8 MiB bootfs partition.
		if ! echo "$bootfs_ls" | grep -q "^cferam\.000$"; then
			if [ -z "$(find_mtd_index bootfs)" ] || [ -z "$(find_mtd_index ubi)" ]; then
				echo "Image does not include cferam.000. For first-time flash from initramfs, build a factory .ras.bin with scripts/vmg8825_factory_builder.py using your own OEM firmware." >&2
				rm -f "$tmp"
				return 1
			fi
		fi
		rm -f "$tmp"
		return 0
	fi

	# Stock Zyxel VMG8825-B50B OEM firmware is a single unified JFFS2 image
	# (no separate UBI/SquashFS rootfs offset for bcm4908img). Accept it if
	# vZyToken matched BCM63138/VMG8825-B50B and payload starts with JFFS2 magic.
	magic=$(dd if="$tmp" bs=1 count=2 2>/dev/null | hexdump -v -e '1/2 "%04x"')
	if [ "$BCM63138_FW_FORMAT" = "zyxel-ras" ] && [ "$magic" = "1985" ]; then
		notify_firmware_no_backup
		rm -f "$tmp"
		return 0
	fi

	echo "Failed to validate BCM63138 image CRC32/structure." >&2
	rm -f "$tmp"
	return 1
}

platform_do_upgrade_ubi() {
	local img="$1"
	local dir="/tmp/sysupgrade-bcm63138"
	local saved_cfe="/tmp/sysupgrade-saved-cferam.bin"
	local mtd_bootfs_size img_bootfs_size cferam has_cferam=0

	mtd_bootfs_size=$(grep '"bootfs"' /proc/mtd | sed 's/mtd[0-9]*:[ \t]*\([^ \t]*\).*/\1/')
	[ -z "$mtd_bootfs_size" ] && return 1
	mtd_bootfs_size=$((0x$mtd_bootfs_size))

	img_bootfs_size=$(bcm4908img extract -i "$img" -t bootfs | wc -c)
	if [ "$img_bootfs_size" -gt "$mtd_bootfs_size" ]; then
		echo "New bootfs ($img_bootfs_size B) exceeds bootfs partition ($mtd_bootfs_size B)." >&2
		return 1
	fi

	cferam=$(platform_calc_new_cferam)
	if bcm4908img bootfs -i "$img" ls 2>/dev/null | grep -q "^cferam\.000$"; then
		has_cferam=1
		bcm4908img bootfs -i "$img" mv cferam.000 "$cferam" || {
			echo "Failed to rename cferam.000 to $cferam" >&2
			return 1
		}
	else
		# Public cferam-free image: rescue device's own cferam.XXX BEFORE erasing bootfs
		platform_save_existing_cferam "$saved_cfe" || {
			echo "Failed to rescue existing cferam.XXX from NAND; aborting before erase!" >&2
			return 1
		}
	fi

	rm -rf "$dir"
	mkdir -p "$dir"
	bcm4908img extract -i "$img" -t rootfs > "$dir/root" || {
		echo "Failed to extract UBI rootfs" >&2
		rm -rf "$dir" "$saved_cfe"
		return 1
	}

	mtd erase bootfs || {
		echo "Failed to erase bootfs" >&2
		rm -rf "$dir" "$saved_cfe"
		return 1
	}
	bcm4908img extract -i "$img" -t bootfs | mtd write - bootfs || {
		echo "Failed to write bootfs" >&2
		rm -rf "$dir" "$saved_cfe"
		return 1
	}

	if [ "$has_cferam" = "0" ]; then
		# Copy rescued cferam.XXX into fresh 8 MiB bootfs with compr=none
		# (Broadcom cferom requires uncompressed JFFS2 data nodes for cferam).
		local mnt="/tmp/sysupgrade-bootfs-rw"
		rm -rf "$mnt"
		mkdir -p "$mnt"
		if ! mount -t jffs2 -o compr=none "mtd:bootfs" "$mnt"; then
			echo "CRITICAL: Failed to mount bootfs RW to restore $cferam" >&2
			rm -rf "$mnt" "$dir" "$saved_cfe"
			return 1
		fi
		cp "$saved_cfe" "$mnt/$cferam" && sync
		if [ ! -s "$mnt/$cferam" ]; then
			echo "CRITICAL: Failed to restore $cferam into bootfs" >&2
			umount "$mnt" 2>/dev/null
			rm -rf "$mnt" "$dir" "$saved_cfe"
			return 1
		fi
		umount "$mnt" 2>/dev/null
		rm -rf "$mnt" "$saved_cfe"
	fi

	nand_do_upgrade "$dir/root"
}

platform_do_upgrade() {
	local img="/tmp/sysupgrade-work.img"
	local cferam

	rm -f "$img"
	platform_prepare_bcm4908img "$1" "$img" || {
		echo "Failed to prepare firmware image" >&2
		exit 1
	}

	# OpenWrt -> OpenWrt on NAND: use NAND-aware UBI upgrade (preserves config,
	# UBI wear-leveling counters, and existing cferam.XXX for cferam-free builds).
	if bcm4908img bootfs -i "$img" ls 2>/dev/null | grep -q "1-openwrt" &&
	   [ -n "$(find_mtd_index bootfs)" ] && [ -n "$(find_mtd_index ubi)" ]; then
		platform_do_upgrade_ubi "$img"
	fi

	# Return-to-Stock Zyxel OEM firmware (unified JFFS2) or first OpenWrt install
	# from initramfs (bootfs+UBI): detach any active UBI volume before erasing.
	cferam=$(platform_calc_new_cferam)
	local is_bcm4908=0
	if bcm4908img info -i "$img" >/dev/null 2>&1; then
		is_bcm4908=1
		if ! bcm4908img bootfs -i "$img" ls 2>/dev/null | grep -q "^cferam\.000$"; then
			echo "ERROR: First-time flash from initramfs requires a factory .ras.bin containing cferam.000." >&2
			rm -f "$img"
			exit 1
		fi
		bcm4908img bootfs -i "$img" mv cferam.000 "$cferam" || {
			echo "Failed to rename cferam.000 to $cferam" >&2
			rm -f "$img"
			exit 1
		}
	fi

	local ubi_idx=$(find_mtd_index ubi)
	[ -n "$ubi_idx" ] && ubidetach -p "/dev/mtd$ubi_idx" 2>/dev/null || true
	local fw_idx=$(find_mtd_index "$PART_NAME")
	[ -n "$fw_idx" ] && ubidetach -p "/dev/mtd$fw_idx" 2>/dev/null || true

	mtd erase "$PART_NAME" || {
		echo "Failed to erase $PART_NAME" >&2
		rm -f "$img"
		exit 1
	}

	if [ "$is_bcm4908" = "1" ]; then
		bcm4908img extract -i "$img" -t firmware | mtd write - "$PART_NAME" || {
			echo "Failed to write $PART_NAME" >&2
			rm -f "$img"
			exit 1
		}
	else
		# Unified JFFS2 Zyxel OEM image: strip 20-byte WFI trailer, write raw
		# JFFS2 to firmware, then rename cferam.000 -> cferam.XXX via JFFS2 mount.
		local img_size=$(wc -c < "$img")
		head -c $((img_size - 20)) "$img" | mtd write - "$PART_NAME" || {
			echo "Failed to write OEM JFFS2 to $PART_NAME" >&2
			rm -f "$img"
			exit 1
		}
		local mnt="/tmp/sysupgrade-oem-jffs2"
		mkdir -p "$mnt"
		if mount -t jffs2 "mtd:$PART_NAME" "$mnt" 2>/dev/null; then
			local cur_cfe=$(ls "$mnt"/cferam.??? 2>/dev/null | head -n 1)
			[ -n "$cur_cfe" ] && [ "$cur_cfe" != "$mnt/$cferam" ] && mv "$cur_cfe" "$mnt/$cferam"
			sync
			umount "$mnt" 2>/dev/null
		fi
		rm -rf "$mnt"

		# Clear stale zcfg_config.json* from Zyxel 'data' partition so cross-branch
		# return-to-stock (e.g. ABLZ <-> ABNY) regenerates a clean default config
		# instead of hanging zcmd on a mismatched schema.
		local dmnt="/tmp/sysupgrade-oem-data"
		mkdir -p "$dmnt"
		if mount -t jffs2 "mtd:data" "$dmnt" 2>/dev/null; then
			rm -f "$dmnt"/zcfg_config.json "$dmnt"/zcfg_config.json.bak
			sync
			umount "$dmnt" 2>/dev/null
		fi
		rm -rf "$dmnt"
	fi
	rm -f "$img"
}
