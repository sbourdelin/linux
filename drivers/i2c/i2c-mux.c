/*
 * Multiplexed I2C bus driver.
 *
 * Copyright (c) 2008-2009 Rodolfo Giometti <giometti@linux.it>
 * Copyright (c) 2008-2009 Eurotech S.p.A. <info@eurotech.it>
 * Copyright (c) 2009-2010 NSN GmbH & Co KG <michael.lawnick.ext@nsn.com>
 *
 * Simplifies access to complex multiplexed I2C bus topologies, by presenting
 * each multiplexed bus segment as an additional I2C adapter.
 * Supports multi-level mux'ing (mux behind a mux).
 *
 * Based on:
 *	i2c-virt.c from Kumar Gala <galak@kernel.crashing.org>
 *	i2c-virtual.c from Ken Harrenstien, Copyright (c) 2004 Google, Inc.
 *	i2c-virtual.c from Brian Kuschak <bkuschak@yahoo.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/acpi.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>

/**
 * Hold the alias that as been assigned to a client.
 */
struct i2c_mux_cli2alias_pair {
	struct list_head node;
	struct i2c_client *client;
	u16 alias;
};

/* multiplexer per channel data */
struct i2c_mux_priv {
	struct i2c_adapter adap;
	struct i2c_algorithm algo;
	struct i2c_mux_core *muxc;
	u32 chan_id;

	/* ATR */
	struct list_head alias_list;
	struct mutex atr_lock; /* Locks orig_addrs during ATR operation */
	u16 *orig_addrs;
	unsigned orig_addrs_size;
};

static struct i2c_mux_cli2alias_pair *
i2c_mux_find_mapping_by_client(struct list_head *list,
			       struct i2c_client *client)
{
	struct i2c_mux_cli2alias_pair *c2a;

	list_for_each_entry(c2a, list, node) {
		if (c2a->client == client)
			return c2a;
	}

	return NULL;
}

static struct i2c_mux_cli2alias_pair *
i2c_mux_find_mapping_by_addr(struct list_head *list,
			     u16 phys_addr)
{
	struct i2c_mux_cli2alias_pair *c2a;

	list_for_each_entry(c2a, list, node) {
		if (c2a->client->addr == phys_addr)
			return c2a;
	}

	return NULL;
}

/**
 * Replace all message addresses with their aliases, saving the
 * original addresses.
 *
 * This function is internal for use in i2c_mux_master_xfer() and
 * similar. It must be followed by i2c_mux_unmap_msgs() to restore the
 * original addresses.
 */
static int i2c_mux_map_msgs(struct i2c_mux_priv *priv,
			    struct i2c_msg msgs[], int num)

{
	struct i2c_mux_core *muxc = priv->muxc;
	static struct i2c_mux_cli2alias_pair *c2a;
	int i;

	if (list_empty(&priv->alias_list))
		return 0;

	/* Ensure we have enough room to save the original addresses */
	if (unlikely(priv->orig_addrs_size < num)) {
		void *new_buf = kmalloc(num * sizeof(priv->orig_addrs[0]),
					GFP_KERNEL);
		if (new_buf == NULL) {
			dev_err(muxc->dev,
				"Cannot allocate %d orig_addrs array", num);
			return -ENOMEM;
		}

		kfree(priv->orig_addrs);
		priv->orig_addrs = new_buf;
		priv->orig_addrs_size = num;
	}

	for (i = 0; i < num; i++) {
		priv->orig_addrs[i] = msgs[i].addr;

		c2a = i2c_mux_find_mapping_by_addr(&priv->alias_list,
						   msgs[i].addr);
		if (c2a) {
			msgs[i].addr = c2a->alias;
		} else {
			dev_warn(muxc->dev, "client 0x%02x not mapped!\n",
				 msgs[i].addr);
		}
	}

	return 0;
}

/**
 * Restore all message addres aliases with the original addresses.
 *
 * This function is internal for use in i2c_mux_master_xfer() and
 * similar.
 *
 * @see i2c_mux_map_msgs()
 */
static void i2c_mux_unmap_msgs(struct i2c_mux_priv *priv,
			       struct i2c_msg msgs[], int num)
{
	int i;

	if (list_empty(&priv->alias_list))
		return;

	for (i = 0; i < num; i++)
		msgs[i].addr = priv->orig_addrs[i];
}

static int __i2c_mux_master_xfer(struct i2c_adapter *adap,
				 struct i2c_msg msgs[], int num)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	struct i2c_adapter *parent = muxc->parent;
	int ret;

	/* Switch to the right mux port and perform the transfer. */

	ret = muxc->select(muxc, priv->chan_id);
	if (ret >= 0)
		ret = __i2c_transfer(parent, msgs, num);
	if (muxc->deselect)
		muxc->deselect(muxc, priv->chan_id);

	return ret;
}

static int i2c_mux_master_xfer(struct i2c_adapter *adap,
			       struct i2c_msg msgs[], int num)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	struct i2c_adapter *parent = muxc->parent;
	int ret;

	/* Switch to the right mux port */
	ret = muxc->select(muxc, priv->chan_id);
	if (ret < 0)
		goto out;

	/* Translate addresses if ATR enabled */
	if (muxc->atr) {
		mutex_lock(&priv->atr_lock);
		ret = i2c_mux_map_msgs(priv, msgs, num);
		if (ret < 0) {
			mutex_unlock(&priv->atr_lock);
			goto out;
		}
	}

	/* Perform the transfer */
	ret = i2c_transfer(parent, msgs, num);

	/* Restore addresses if ATR enabled */
	if (muxc->atr) {
		i2c_mux_unmap_msgs(priv, msgs, num);
		mutex_unlock(&priv->atr_lock);
	}

out:
	if (muxc->deselect)
		muxc->deselect(muxc, priv->chan_id);

	return ret;
}

static int __i2c_mux_smbus_xfer(struct i2c_adapter *adap,
				u16 addr, unsigned short flags,
				char read_write, u8 command,
				int size, union i2c_smbus_data *data)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	struct i2c_adapter *parent = muxc->parent;
	int ret;

	/* Select the right mux port and perform the transfer. */

	ret = muxc->select(muxc, priv->chan_id);
	if (ret >= 0)
		ret = __i2c_smbus_xfer(parent, addr, flags,
				       read_write, command, size, data);
	if (muxc->deselect)
		muxc->deselect(muxc, priv->chan_id);

	return ret;
}

static int i2c_mux_smbus_xfer(struct i2c_adapter *adap,
			      u16 addr, unsigned short flags,
			      char read_write, u8 command,
			      int size, union i2c_smbus_data *data)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	struct i2c_adapter *parent = muxc->parent;
	int ret;

	/* Select the right mux port and perform the transfer. */

	ret = muxc->select(muxc, priv->chan_id);
	if (ret >= 0)
		ret = i2c_smbus_xfer(parent, addr, flags,
				     read_write, command, size, data);
	if (muxc->deselect)
		muxc->deselect(muxc, priv->chan_id);

	return ret;
}

/* Return the parent's functionality */
static u32 i2c_mux_functionality(struct i2c_adapter *adap)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	return parent->algo->functionality(parent);
}

/* Return all parent classes, merged */
static unsigned int i2c_mux_parent_classes(struct i2c_adapter *parent)
{
	unsigned int class = 0;

	do {
		class |= parent->class;
		parent = i2c_parent_is_i2c_adapter(parent);
	} while (parent);

	return class;
}

static void i2c_mux_lock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	rt_mutex_lock_nested(&parent->mux_lock, i2c_adapter_depth(adapter));
	if (!(flags & I2C_LOCK_ROOT_ADAPTER))
		return;
	i2c_lock_bus(parent, flags);
}

static int i2c_mux_trylock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	if (!rt_mutex_trylock(&parent->mux_lock))
		return 0;	/* mux_lock not locked, failure */
	if (!(flags & I2C_LOCK_ROOT_ADAPTER))
		return 1;	/* we only want mux_lock, success */
	if (i2c_trylock_bus(parent, flags))
		return 1;	/* parent locked too, success */
	rt_mutex_unlock(&parent->mux_lock);
	return 0;		/* parent not locked, failure */
}

static void i2c_mux_unlock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	if (flags & I2C_LOCK_ROOT_ADAPTER)
		i2c_unlock_bus(parent, flags);
	rt_mutex_unlock(&parent->mux_lock);
}

static void i2c_parent_lock_bus(struct i2c_adapter *adapter,
				unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	rt_mutex_lock_nested(&parent->mux_lock, i2c_adapter_depth(adapter));
	i2c_lock_bus(parent, flags);
}

static int i2c_parent_trylock_bus(struct i2c_adapter *adapter,
				  unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	if (!rt_mutex_trylock(&parent->mux_lock))
		return 0;	/* mux_lock not locked, failure */
	if (i2c_trylock_bus(parent, flags))
		return 1;	/* parent locked too, success */
	rt_mutex_unlock(&parent->mux_lock);
	return 0;		/* parent not locked, failure */
}

static void i2c_parent_unlock_bus(struct i2c_adapter *adapter,
				  unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	i2c_unlock_bus(parent, flags);
	rt_mutex_unlock(&parent->mux_lock);
}

struct i2c_adapter *i2c_root_adapter(struct device *dev)
{
	struct device *i2c;
	struct i2c_adapter *i2c_root;

	/*
	 * Walk up the device tree to find an i2c adapter, indicating
	 * that this is an i2c client device. Check all ancestors to
	 * handle mfd devices etc.
	 */
	for (i2c = dev; i2c; i2c = i2c->parent) {
		if (i2c->type == &i2c_adapter_type)
			break;
	}
	if (!i2c)
		return NULL;

	/* Continue up the tree to find the root i2c adapter */
	i2c_root = to_i2c_adapter(i2c);
	while (i2c_parent_is_i2c_adapter(i2c_root))
		i2c_root = i2c_parent_is_i2c_adapter(i2c_root);

	return i2c_root;
}
EXPORT_SYMBOL_GPL(i2c_root_adapter);

static int i2c_mux_attach_client(struct i2c_adapter *adapter,
				 const struct i2c_board_info *info,
				 struct i2c_client *client)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	const struct i2c_mux_attach_operations *ops = muxc->attach_ops;
	struct i2c_mux_cli2alias_pair *c2a;
	u16 alias_id = 0;
	int err = 0;

	if (ops && ops->i2c_mux_attach_client)
		err = ops->i2c_mux_attach_client(muxc, priv->chan_id,
						 info, client, &alias_id);
	if (err)
		goto err_attach;

	if (alias_id != 0) {
		c2a = kzalloc(sizeof(struct i2c_mux_cli2alias_pair), GFP_KERNEL);
		if (!c2a) {
			err = -ENOMEM;
			goto err_alloc;
		}

		c2a->client = client;
		c2a->alias = alias_id;
		list_add(&c2a->node, &priv->alias_list);
	}

	return 0;

err_alloc:
	if (ops && ops->i2c_mux_detach_client)
		ops->i2c_mux_detach_client(muxc, priv->chan_id, client);
err_attach:
	return err;
}

static void i2c_mux_detach_client(struct i2c_adapter *adapter,
				  struct i2c_client *client)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	const struct i2c_mux_attach_operations *ops = muxc->attach_ops;
	struct i2c_mux_cli2alias_pair *c2a;

	if (ops && ops->i2c_mux_detach_client)
		ops->i2c_mux_detach_client(muxc, priv->chan_id, client);

	c2a = i2c_mux_find_mapping_by_client(&priv->alias_list, client);
	if (c2a != NULL) {
		list_del(&c2a->node);
		kfree(c2a);
	}
}

static const struct i2c_attach_operations i2c_mux_attach_operations = {
	.attach_client = i2c_mux_attach_client,
	.detach_client = i2c_mux_detach_client,
};

struct i2c_mux_core *i2c_mux_alloc(struct i2c_adapter *parent,
				   struct device *dev, int max_adapters,
				   int sizeof_priv, u32 flags,
				   int (*select)(struct i2c_mux_core *, u32),
				   int (*deselect)(struct i2c_mux_core *, u32),
				   const struct i2c_mux_attach_operations *attach_ops)
{
	struct i2c_mux_core *muxc;

	muxc = devm_kzalloc(dev, sizeof(*muxc)
			    + max_adapters * sizeof(muxc->adapter[0])
			    + sizeof_priv, GFP_KERNEL);
	if (!muxc)
		return NULL;
	if (sizeof_priv)
		muxc->priv = &muxc->adapter[max_adapters];

	muxc->parent = parent;
	muxc->dev = dev;
	if (flags & I2C_MUX_LOCKED)
		muxc->mux_locked = true;
	if (flags & I2C_MUX_ARBITRATOR)
		muxc->arbitrator = true;
	if (flags & I2C_MUX_GATE)
		muxc->gate = true;
	if (flags & I2C_MUX_ATR)
		muxc->atr = true;
	muxc->select = select;
	muxc->deselect = deselect;
	muxc->attach_ops = attach_ops;
	muxc->max_adapters = max_adapters;

	return muxc;
}
EXPORT_SYMBOL_GPL(i2c_mux_alloc);

static const struct i2c_lock_operations i2c_mux_lock_ops = {
	.lock_bus =    i2c_mux_lock_bus,
	.trylock_bus = i2c_mux_trylock_bus,
	.unlock_bus =  i2c_mux_unlock_bus,
};

static const struct i2c_lock_operations i2c_parent_lock_ops = {
	.lock_bus =    i2c_parent_lock_bus,
	.trylock_bus = i2c_parent_trylock_bus,
	.unlock_bus =  i2c_parent_unlock_bus,
};

int i2c_mux_add_adapter(struct i2c_mux_core *muxc,
			u32 force_nr, u32 chan_id,
			unsigned int class)
{
	struct i2c_adapter *parent = muxc->parent;
	struct i2c_mux_priv *priv;
	char symlink_name[20];
	int ret;

	if (muxc->num_adapters >= muxc->max_adapters) {
		dev_err(muxc->dev, "No room for more i2c-mux adapters\n");
		return -EINVAL;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Set up private adapter data */
	priv->muxc = muxc;
	priv->chan_id = chan_id;
	INIT_LIST_HEAD(&priv->alias_list);
	mutex_init(&priv->atr_lock);

	/* Need to do algo dynamically because we don't know ahead
	 * of time what sort of physical adapter we'll be dealing with.
	 */
	if (parent->algo->master_xfer) {
		if (muxc->mux_locked)
			priv->algo.master_xfer = i2c_mux_master_xfer;
		else
			priv->algo.master_xfer = __i2c_mux_master_xfer;
	}
	if (parent->algo->smbus_xfer) {
		if (muxc->mux_locked)
			priv->algo.smbus_xfer = i2c_mux_smbus_xfer;
		else
			priv->algo.smbus_xfer = __i2c_mux_smbus_xfer;
	}
	priv->algo.functionality = i2c_mux_functionality;

	/* Now fill out new adapter structure */
	snprintf(priv->adap.name, sizeof(priv->adap.name),
		 "i2c-%d-mux (chan_id %d)", i2c_adapter_id(parent), chan_id);
	priv->adap.owner = THIS_MODULE;
	priv->adap.algo = &priv->algo;
	priv->adap.algo_data = priv;
	priv->adap.dev.parent = &parent->dev;
	priv->adap.retries = parent->retries;
	priv->adap.timeout = parent->timeout;
	priv->adap.quirks = parent->quirks;

	if (muxc->attach_ops) {
		priv->adap.attach_ops = &i2c_mux_attach_operations;
	}

	if (muxc->mux_locked)
		priv->adap.lock_ops = &i2c_mux_lock_ops;
	else
		priv->adap.lock_ops = &i2c_parent_lock_ops;

	/* Sanity check on class */
	if (i2c_mux_parent_classes(parent) & class)
		dev_err(&parent->dev,
			"Segment %d behind mux can't share classes with ancestors\n",
			chan_id);
	else
		priv->adap.class = class;

	/*
	 * Try to populate the mux adapter's of_node, expands to
	 * nothing if !CONFIG_OF.
	 */
	if (muxc->dev->of_node) {
		struct device_node *dev_node = muxc->dev->of_node;
		struct device_node *mux_node, *child = NULL;
		u32 reg;

		if (muxc->arbitrator)
			mux_node = of_get_child_by_name(dev_node, "i2c-arb");
		else if (muxc->gate)
			mux_node = of_get_child_by_name(dev_node, "i2c-gate");
		else
			mux_node = of_get_child_by_name(dev_node, "i2c-mux");

		if (mux_node) {
			/* A "reg" property indicates an old-style DT entry */
			if (!of_property_read_u32(mux_node, "reg", &reg)) {
				of_node_put(mux_node);
				mux_node = NULL;
			}
		}

		if (!mux_node)
			mux_node = of_node_get(dev_node);
		else if (muxc->arbitrator || muxc->gate)
			child = of_node_get(mux_node);

		if (!child) {
			for_each_child_of_node(mux_node, child) {
				ret = of_property_read_u32(child, "reg", &reg);
				if (ret)
					continue;
				if (chan_id == reg)
					break;
			}
		}

		priv->adap.dev.of_node = child;
		of_node_put(mux_node);
	}

	/*
	 * Associate the mux channel with an ACPI node.
	 */
	if (has_acpi_companion(muxc->dev))
		acpi_preset_companion(&priv->adap.dev,
				      ACPI_COMPANION(muxc->dev),
				      chan_id);

	if (force_nr) {
		priv->adap.nr = force_nr;
		ret = i2c_add_numbered_adapter(&priv->adap);
		if (ret < 0) {
			dev_err(&parent->dev,
				"failed to add mux-adapter %u as bus %u (error=%d)\n",
				chan_id, force_nr, ret);
			goto err_free_priv;
		}
	} else {
		ret = i2c_add_adapter(&priv->adap);
		if (ret < 0) {
			dev_err(&parent->dev,
				"failed to add mux-adapter %u (error=%d)\n",
				chan_id, ret);
			goto err_free_priv;
		}
	}

	WARN(sysfs_create_link(&priv->adap.dev.kobj, &muxc->dev->kobj,
			       "mux_device"),
	     "can't create symlink to mux device\n");

	snprintf(symlink_name, sizeof(symlink_name), "channel-%u", chan_id);
	WARN(sysfs_create_link(&muxc->dev->kobj, &priv->adap.dev.kobj,
			       symlink_name),
	     "can't create symlink to channel %u\n", chan_id);
	dev_info(&parent->dev, "Added multiplexed i2c bus %d\n",
		 i2c_adapter_id(&priv->adap));

	muxc->adapter[muxc->num_adapters++] = &priv->adap;
	return 0;

err_free_priv:
	mutex_destroy(&priv->atr_lock);
	kfree(priv);
	return ret;
}
EXPORT_SYMBOL_GPL(i2c_mux_add_adapter);

void i2c_mux_del_adapters(struct i2c_mux_core *muxc)
{
	char symlink_name[20];

	while (muxc->num_adapters) {
		struct i2c_adapter *adap = muxc->adapter[--muxc->num_adapters];
		struct i2c_mux_priv *priv = adap->algo_data;
		struct device_node *np = adap->dev.of_node;

		muxc->adapter[muxc->num_adapters] = NULL;

		snprintf(symlink_name, sizeof(symlink_name),
			 "channel-%u", priv->chan_id);
		sysfs_remove_link(&muxc->dev->kobj, symlink_name);

		sysfs_remove_link(&priv->adap.dev.kobj, "mux_device");
		i2c_del_adapter(adap);
		of_node_put(np);
		mutex_destroy(&priv->atr_lock);
		kfree(priv);
	}
}
EXPORT_SYMBOL_GPL(i2c_mux_del_adapters);

MODULE_AUTHOR("Rodolfo Giometti <giometti@linux.it>");
MODULE_DESCRIPTION("I2C driver for multiplexed I2C busses");
MODULE_LICENSE("GPL v2");
