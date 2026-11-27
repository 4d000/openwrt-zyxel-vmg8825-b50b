// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/errno.h>

uint8_t *soc_base_address;

int BcmMemReserveGetByName(char *name, void **virt, uint32_t *phys, uint32_t *size)
{
	return -ENODEV;
}
