/* Compat bcm_io_map.h avoiding legacy <mach/hardware.h> */
#ifndef __BCM_IO_MAP_H
#define __BCM_IO_MAP_H

#define BCM_IO_ADDR(x) (x)
#define BCM_IO_NOMAP(index, phy_base, offset) ((phy_base) + (offset))
#define BCM_IO_MAP(index, phy_base, offset)   ((phy_base) + (offset))

#endif
