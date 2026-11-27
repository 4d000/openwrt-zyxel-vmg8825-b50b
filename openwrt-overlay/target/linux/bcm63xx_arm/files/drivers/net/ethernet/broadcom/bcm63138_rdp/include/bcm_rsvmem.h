/* Compat bcm_rsvmem.h for OpenWrt DT reserved-memory Runner pool */
#ifndef __BCM_RSVMEM_H
#define __BCM_RSVMEM_H

#include <linux/types.h>

#define BcmMemReserveVirtToPhys(vbase, pbase, addr) \
	((uint32_t)(pbase) + (uint32_t)((uintptr_t)(addr) - (uintptr_t)(vbase)))

#define BcmMemReservePhysToVirt(vbase, pbase, phys) \
	((void *)((uintptr_t)(vbase) + ((uint32_t)(phys) - (uint32_t)(pbase))))

int BcmMemReserveGetByName(char *name, void **virt, uint32_t *phys, uint32_t *size);

#endif
