# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=bcm63xx_arm
BOARDNAME:=Broadcom BCM63xx ARM (BCM63138)
FEATURES:=squashfs nand usb pci pcie gpio ramdisk
CPU_TYPE:=cortex-a9
SUBTARGETS:=generic

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for Broadcom BCA ARM Cortex-A9 SoCs (e.g. BCM63138).
endef

include $(INCLUDE_DIR)/target.mk

KERNELNAME:=zImage dtbs

DEFAULT_PACKAGES += \
	bcm4908img fdt-utils \
	kmod-gpio-button-hotplug \
	kmod-leds-gpio \
	kmod-usb-ohci kmod-usb2 kmod-usb3

$(eval $(call BuildTarget))
