# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

DTS_DIR := $(DTS_DIR)/broadcom

define Build/bcm63138-bootfs
	rm -fr $@-bootfs
	mkdir -p $@-bootfs
	touch $@-bootfs/1-openwrt
	if [ -f $(CURDIR)/cferam-$(DEVICE_NAME).bin ]; then \
		cp $(CURDIR)/cferam-$(DEVICE_NAME).bin $@-bootfs/cferam.000; \
	fi
	$(STAGING_DIR_HOST)/bin/lzma e -lc1 -lp2 -pb2 -d22 $(IMAGE_KERNEL) $@-bootfs/vmlinux.lz.raw
	python3 -c 'import struct, sys; d=open(sys.argv[1],"rb").read(); open(sys.argv[2],"wb").write(struct.pack("<III8s",0xc0008000,0xc0008000,len(d),b"BRCM\x00\x00\x00\x00")+d)' \
		$@-bootfs/vmlinux.lz.raw $@-bootfs/vmlinux.lz
	rm -f $@-bootfs/vmlinux.lz.raw
	$(STAGING_DIR_HOST)/bin/mkfs.jffs2 --pad=0x800000 --little-endian --squash-uids \
		-v -e 128KiB -o $@-bootfs.jffs2 -d $@-bootfs -m none -n
	rm -fr $@-bootfs
endef

define Build/bcm63138img
	$(STAGING_DIR_HOST)/bin/bcm4908img create $@.new \
		-c 0x63138 \
		-f $@-bootfs.jffs2 \
		-a 0x20000 \
		-f $@
	rm -f $@-bootfs.jffs2
	mv $@.new $@
endef

define Build/zyxel-ras
	cat $@-bootfs.jffs2 $@ > $@.merged
	rm -f $@-bootfs.jffs2
	python3 $(TOPDIR)/scripts/addvtoken.py \
		--endian little \
		--chip 63138 \
		--flashtype NAND128 \
		--btrm 0 \
		--product "VMG8825-B50B" \
		--fw-version "V5.17(ABNY.7)C0" \
		--model-id 454A \
		--kernelfile $@.merged \
		$@.merged $@.ras
	rm -f $@.merged
	mv $@.ras $@
endef

define Device/Default
  PROFILES := Generic $$(DEVICE_NAME)
  KERNEL := kernel-bin | append-dtb
  KERNEL_DEPENDS = $$(wildcard $(DTS_DIR)/$$(DEVICE_DTS).dts)
  KERNEL_INITRAMFS_SUFFIX := .bin
  KERNEL_INITRAMFS := kernel-bin | append-dtb
  FILESYSTEMS := squashfs
  KERNEL_NAME := zImage
  DEVICE_IMG_NAME = $$(DEVICE_IMG_PREFIX)-$$(1).$$(2)
  BLOCKSIZE := 128k
  PAGESIZE := 2048
  IMAGES := bin
  IMAGE/bin := append-ubi | bcm63138-bootfs | bcm63138img
endef

define Device/zyxel_vmg8825-b50b
  DEVICE_VENDOR := Zyxel
  DEVICE_MODEL := VMG8825-B50B
  DEVICE_DTS := bcm63138-zyxel-vmg8825-b50b
  SUPPORTED_DEVICES := zyxel,vmg8825-b50b
  DEVICE_PACKAGES := kmod-brcmfmac brcmfmac-firmware-4366c0-pcie brcmfmac-firmware-43602a1-pcie pciutils iw wpad-basic-openssl wireless-regdb ubi-utils
ifneq ($(wildcard $(CURDIR)/cferam-zyxel_vmg8825-b50b.bin),)
  IMAGES := bin ras.bin
  IMAGE/ras.bin := append-ubi | bcm63138-bootfs | zyxel-ras
endif
endef
TARGET_DEVICES += zyxel_vmg8825-b50b

$(eval $(call BuildImage))
