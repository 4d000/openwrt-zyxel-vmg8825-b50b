#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Reproducible local OpenWrt build script for Zyxel VMG8825-B50B (BCM63138).
# Clones upstream OpenWrt at the pinned tested commit, applies the tiny
# upstream integration patch and target overlay, and builds clean GPLv2 images.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENWRT_REPO="${OPENWRT_REPO:-https://github.com/openwrt/openwrt.git}"
OPENWRT_COMMIT="3d5a743f1f0cb77aed8ed63b2f9aa75700b4322c"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/openwrt-src}"
JOBS="${JOBS:-$(nproc)}"
ALL_KMODS=0
CFERAM_PATH=""

usage() {
	cat <<'EOF'
Usage: ./build.sh [options]

Options:
  --dir PATH        OpenWrt source directory (default: ./openwrt-src)
  --jobs N          Parallel make jobs (default: nproc)
  --all-kmods       Build all kernel modules (CONFIG_ALL_KMODS=y) for self-hosted apk feed
  --cferam PATH     Optional local path to your OWN cferam binary (also generates .ras.bin locally)
  -h, --help        Show this help

Examples:
  ./build.sh
  ./build.sh --jobs 8 --all-kmods
  ./build.sh --cferam /path/to/your/cferam.001
EOF
	exit 0
}

while [ $# -gt 0 ]; do
	case "$1" in
		--dir) BUILD_DIR="$2"; shift 2 ;;
		--jobs) JOBS="$2"; shift 2 ;;
		--all-kmods) ALL_KMODS=1; shift ;;
		--cferam) CFERAM_PATH="$2"; shift 2 ;;
		-h|--help) usage ;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

echo "[1/6] Preparing upstream OpenWrt source in $BUILD_DIR (commit $OPENWRT_COMMIT)..."
if [ ! -d "$BUILD_DIR/.git" ]; then
	git init "$BUILD_DIR"
	git -C "$BUILD_DIR" remote add origin "$OPENWRT_REPO"
fi
if ! git -C "$BUILD_DIR" cat-file -e "${OPENWRT_COMMIT}^{commit}" 2>/dev/null; then
	git -C "$BUILD_DIR" fetch --depth=1 origin "$OPENWRT_COMMIT"
fi
git -C "$BUILD_DIR" checkout -f "$OPENWRT_COMMIT"
git -C "$BUILD_DIR" clean -fd

echo "[2/6] Applying upstream integration patches..."
for p in "$SCRIPT_DIR"/patches/upstream/*.patch; do
	[ -f "$p" ] || continue
	echo "  Applying $(basename "$p")"
	git -C "$BUILD_DIR" apply "$p"
done

echo "[3/6] Copying bcm63xx_arm / Zyxel VMG8825-B50B overlay..."
cp -a "$SCRIPT_DIR/openwrt-overlay/." "$BUILD_DIR/"

if [ -n "$CFERAM_PATH" ]; then
	echo "  Including local user-supplied cferam -> cferam-zyxel_vmg8825-b50b.bin (will also build .ras.bin)"
	cp "$CFERAM_PATH" "$BUILD_DIR/target/linux/bcm63xx_arm/image/cferam-zyxel_vmg8825-b50b.bin"
fi

echo "[4/6] Updating and installing OpenWrt feeds..."
cd "$BUILD_DIR"
./scripts/feeds update -a
./scripts/feeds install -a

echo "[5/6] Configuring target (zyxel_vmg8825-b50b)..."
cp target/linux/bcm63xx_arm/vmg8825-b50b.diffconfig .config
if [ "$ALL_KMODS" = "1" ]; then
	echo "CONFIG_ALL_KMODS=y" >> .config
fi
make defconfig

echo "[6/6] Downloading sources and compiling with -j${JOBS}..."
EXTRA_MAKE_ARGS=""
if [ "$ALL_KMODS" = "1" ]; then
	EXTRA_MAKE_ARGS="IGNORE_ERRORS=m"
fi
make download -j"$JOBS" $EXTRA_MAKE_ARGS
make -j"$JOBS" V=s $EXTRA_MAKE_ARGS

echo ""
echo "Build complete! Output images:"
ls -lh bin/targets/bcm63xx_arm/generic/openwrt-*
