/* Pre-included compatibility definitions for Broadcom RDP/RDD sources in Linux 6.18 */
#ifndef _RDP_KBUILD_COMPAT_H
#define _RDP_KBUILD_COMPAT_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/export.h>

typedef void *bdmf_sysb;

#define KMALLOC(sz, align) kmalloc((sz), GFP_ATOMIC)
#define KFREE(p) kfree(p)

#endif
