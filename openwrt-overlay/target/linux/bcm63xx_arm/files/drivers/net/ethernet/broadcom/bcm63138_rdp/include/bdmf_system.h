/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BDMF_SYSTEM_SHIM_H_
#define _BDMF_SYSTEM_SHIM_H_

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include "bdmf_data_types.h"
#include "bdmf_errno.h"

#define bdmf_fastlock spinlock_t
#define DEFINE_BDMF_FASTLOCK(lock) DEFINE_SPINLOCK(lock)
#define bdmf_fastlock_init(_lock) spin_lock_init(_lock)
#define bdmf_fastlock_lock(_lock) spin_lock(_lock)
#define bdmf_fastlock_unlock(_lock) spin_unlock(_lock)
#define bdmf_fastlock_lock_irq(_lock, _flag) spin_lock_irqsave(_lock, _flag)
#define bdmf_fastlock_unlock_irq(_lock, _flag) spin_unlock_irqrestore(_lock, _flag)

static inline void *bdmf_alloc(size_t size)
{
	if (in_interrupt() || irqs_disabled() || in_atomic())
		return kzalloc(size, GFP_ATOMIC);
	return kvzalloc(size, GFP_KERNEL);
}

static inline void bdmf_free(void *p)
{
	kvfree(p);
}

static inline void *bdmf_sysb_data(const bdmf_sysb sysb)
{
	const struct sk_buff *skb = (const struct sk_buff *)sysb;

	return skb ? skb->data : NULL;
}

static inline void *bdmf_sysb_gso_data(const bdmf_sysb sysb, uint32_t *is_gso_pkt)
{
	if (is_gso_pkt)
		*is_gso_pkt = 0;
	return bdmf_sysb_data(sysb);
}

static inline uint32_t bdmf_sysb_length(const bdmf_sysb sysb)
{
	const struct sk_buff *skb = (const struct sk_buff *)sysb;

	return skb ? skb->len : 0;
}

static inline void bdmf_sysb_free(bdmf_sysb sysb)
{
	if (sysb)
		dev_kfree_skb_any((struct sk_buff *)sysb);
}

enum {
	bdmf_trace_level_none = 0,
	bdmf_trace_level_error = 1,
	bdmf_trace_level_info = 2,
	bdmf_trace_level_debug = 3,
};

#define bdmf_global_trace_level 0
#define bdmf_trace(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)
#define printf printk

#endif /* _BDMF_SYSTEM_SHIM_H_ */
