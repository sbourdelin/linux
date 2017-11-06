/*
 * Realtek SMI interface driver defines
 *
 * Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 * Copyright (C) 2009-2010 Gabor Juhos <juhosg@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef _REALTEK_SMI_H
#define _REALTEK_SMI_H

#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <net/dsa.h>

struct realtek_smi_ops;
struct dentry;
struct inode;
struct file;

struct rtl8366_mib_counter {
	unsigned	base;
	unsigned	offset;
	unsigned	length;
	const char	*name;
};

struct rtl8366_vlan_mc {
	u16	vid;
	u16	untag;
	u16	member;
	u8	fid;
	u8	priority;
};

struct rtl8366_vlan_4k {
	u16	vid;
	u16	untag;
	u16	member;
	u8	fid;
};

struct realtek_smi {
	struct device		*dev;
	struct gpio_desc	*reset;
	struct gpio_desc	*mdc;
	struct gpio_desc	*mdio;
	struct regmap		*map;
	unsigned int		clk_delay;
	u8			cmd_read;
	u8			cmd_write;
	spinlock_t		lock;
	struct dsa_switch	*ds;
	struct irq_domain	*irqdomain;

	unsigned int		cpu_port;
	unsigned int		num_ports;
	unsigned int		num_vlan_mc;
	unsigned int		num_mib_counters;
	struct rtl8366_mib_counter *mib_counters;

	const struct realtek_smi_ops *ops;

	int			vlan_enabled;
	int			vlan4k_enabled;

	char			buf[4096];
#ifdef CONFIG_RTL8366_SMI_DEBUG_FS
	struct dentry           *debugfs_root;
	u8			dbg_vlan_4k_page;
#endif
};

/**
 * struct realtek_smi_ops - vtable for the per-SMI-chiptype operations
 * @detect: detects the chiptype
 */
struct realtek_smi_ops {
	int	(*detect)(struct realtek_smi *smi);
	int	(*reset_chip)(struct realtek_smi *smi);
	int	(*setup)(struct realtek_smi *smi);
	void	(*cleanup)(struct realtek_smi *smi);
	int	(*get_mib_counter)(struct realtek_smi *smi,
				   int port,
				   struct rtl8366_mib_counter *mib,
				   u64 *mibvalue);
	int	(*get_vlan_mc)(struct realtek_smi *smi, u32 index,
			       struct rtl8366_vlan_mc *vlanmc);
	int	(*set_vlan_mc)(struct realtek_smi *smi, u32 index,
			       const struct rtl8366_vlan_mc *vlanmc);
	int	(*get_vlan_4k)(struct realtek_smi *smi, u32 vid,
			       struct rtl8366_vlan_4k *vlan4k);
	int	(*set_vlan_4k)(struct realtek_smi *smi,
			       const struct rtl8366_vlan_4k *vlan4k);
	int	(*get_mc_index)(struct realtek_smi *smi, int port, int *val);
	int	(*set_mc_index)(struct realtek_smi *smi, int port, int index);
	bool	(*is_vlan_valid)(struct realtek_smi *smi, unsigned vlan);
	int	(*enable_vlan)(struct realtek_smi *smi, bool enable);
	int	(*enable_vlan4k)(struct realtek_smi *smi, bool enable);
	int	(*enable_port)(struct realtek_smi *smi, int port, bool enable);
};

struct realtek_smi_variant {
	const struct dsa_switch_ops *ds_ops;
	const struct realtek_smi_ops *ops;
	unsigned int clk_delay;
	u8 cmd_read;
	u8 cmd_write;
};

/* SMI core calls */
int realtek_smi_write_reg_noack(struct realtek_smi *smi, u32 addr,
				u32 data);

/* RTL8366 library helpers */
int rtl8366_mc_is_used(struct realtek_smi *smi, int mc_index, int *used);
int rtl8366_set_vlan(struct realtek_smi *smi, int vid, u32 member,
		     u32 untag, u32 fid);
int rtl8366_get_pvid(struct realtek_smi *smi, int port, int *val);
int rtl8366_set_pvid(struct realtek_smi *smi, unsigned port,
		     unsigned vid);
int rtl8366_enable_vlan4k(struct realtek_smi *smi, bool enable);
int rtl8366_enable_vlan(struct realtek_smi *smi, bool enable);
int rtl8366_reset_vlan(struct realtek_smi *smi);
int rtl8366_init_vlan(struct realtek_smi *smi);
int rtl8366_vlan_filtering(struct dsa_switch *ds, int port, bool vlan_filtering);
int rtl8366_vlan_prepare(struct dsa_switch *ds, int port,
			 const struct switchdev_obj_port_vlan *vlan,
			 struct switchdev_trans *trans);
void rtl8366_vlan_add(struct dsa_switch *ds, int port,
		      const struct switchdev_obj_port_vlan *vlan,
		      struct switchdev_trans *trans);
int rtl8366_vlan_del(struct dsa_switch *ds, int port,
		     const struct switchdev_obj_port_vlan *vlan);
void rtl8366_get_strings(struct dsa_switch *ds, int port, uint8_t *data);
int rtl8366_get_sset_count(struct dsa_switch *ds);
void rtl8366_get_ethtool_stats(struct dsa_switch *ds, int port, uint64_t *data);

extern const struct realtek_smi_variant rtl8366rb_variant;

#endif /*  _REALTEK_SMI_H */
