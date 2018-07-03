// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */

#include <linux/slab.h>

#include "gadget.h"
#include "gadget-debugfs.h"

static const struct debugfs_reg32 usbssp_cap_regs[] = {
	dump_register(CAPLENGTH),
	dump_register(HCSPARAMS1),
	dump_register(HCSPARAMS2),
	dump_register(HCSPARAMS3),
	dump_register(HCCPARAMS1),
	dump_register(DOORBELLOFF),
	dump_register(RUNTIMEOFF),
	dump_register(HCCPARAMS2),
};

static const struct debugfs_reg32 usbssp_op_regs[] = {
	dump_register(USBCMD),
	dump_register(USBSTS),
	dump_register(PAGESIZE),
	dump_register(DNCTRL),
	dump_register(CRCR),
	dump_register(DCBAAP_LOW),
	dump_register(DCBAAP_HIGH),
	dump_register(CONFIG),
};

static const struct debugfs_reg32 usbssp_runtime_regs[] = {
	dump_register(MFINDEX),
	dump_register(IR0_IMAN),
	dump_register(IR0_IMOD),
	dump_register(IR0_ERSTSZ),
	dump_register(IR0_ERSTBA_LOW),
	dump_register(IR0_ERSTBA_HIGH),
	dump_register(IR0_ERDP_LOW),
	dump_register(IR0_ERDP_HIGH),
};

static const struct debugfs_reg32 usbssp_extcap_legsup[] = {
	dump_register(EXTCAP_USBLEGSUP),
	dump_register(EXTCAP_USBLEGCTLSTS),
};

static const struct debugfs_reg32 usbssp_extcap_protocol[] = {
	dump_register(EXTCAP_REVISION),
	dump_register(EXTCAP_NAME),
	dump_register(EXTCAP_PORTINFO),
	dump_register(EXTCAP_PORTTYPE),
	dump_register(EXTCAP_MANTISSA1),
	dump_register(EXTCAP_MANTISSA2),
	dump_register(EXTCAP_MANTISSA3),
	dump_register(EXTCAP_MANTISSA4),
	dump_register(EXTCAP_MANTISSA5),
	dump_register(EXTCAP_MANTISSA6),
};

static const struct debugfs_reg32 usbssp_extcap_dbc[] = {
	dump_register(EXTCAP_DBC_CAPABILITY),
	dump_register(EXTCAP_DBC_DOORBELL),
	dump_register(EXTCAP_DBC_ERSTSIZE),
	dump_register(EXTCAP_DBC_ERST_LOW),
	dump_register(EXTCAP_DBC_ERST_HIGH),
	dump_register(EXTCAP_DBC_ERDP_LOW),
	dump_register(EXTCAP_DBC_ERDP_HIGH),
	dump_register(EXTCAP_DBC_CONTROL),
	dump_register(EXTCAP_DBC_STATUS),
	dump_register(EXTCAP_DBC_PORTSC),
	dump_register(EXTCAP_DBC_CONT_LOW),
	dump_register(EXTCAP_DBC_CONT_HIGH),
	dump_register(EXTCAP_DBC_DEVINFO1),
	dump_register(EXTCAP_DBC_DEVINFO2),
};

static struct dentry *usbssp_debugfs_root;

static struct usbssp_regset *usbssp_debugfs_alloc_regset(
				struct usbssp_udc *usbssp_data)
{
	struct usbssp_regset *regset;

	regset = kzalloc(sizeof(*regset), GFP_KERNEL);
	if (!regset)
		return NULL;

	/*
	 * The allocation and free of regset are executed in order.
	 * We needn't a lock here.
	 */
	INIT_LIST_HEAD(&regset->list);
	list_add_tail(&regset->list, &usbssp_data->regset_list);

	return regset;
}

static void usbssp_debugfs_free_regset(struct usbssp_regset *regset)
{
	if (!regset)
		return;

	list_del(&regset->list);
	kfree(regset);
}

static void usbssp_debugfs_regset(struct usbssp_udc *usbssp_data, u32 base,
				  const struct debugfs_reg32 *regs,
				  size_t nregs, struct dentry *parent,
				  const char *fmt, ...)
{
	struct usbssp_regset *rgs;
	va_list args;
	struct debugfs_regset32	*regset;

	rgs = usbssp_debugfs_alloc_regset(usbssp_data);
	if (!rgs)
		return;

	va_start(args, fmt);
	vsnprintf(rgs->name, sizeof(rgs->name), fmt, args);
	va_end(args);

	regset = &rgs->regset;
	regset->regs = regs;
	regset->nregs = nregs;
	regset->base = usbssp_data->regs + base;

	debugfs_create_regset32((const char *)rgs->name, 0444, parent, regset);
}

static void usbssp_debugfs_extcap_regset(struct usbssp_udc *usbssp_data,
					 int cap_id,
					 const struct debugfs_reg32 *regs,
					 size_t n, const char *cap_name)
{
	u32 offset;
	int index = 0;
	size_t psic, nregs = n;
	void __iomem *base = &usbssp_data->cap_regs->hc_capbase;

	offset = usbssp_find_next_ext_cap(base, 0, cap_id);
	while (offset) {
		if (cap_id == USBSSP_EXT_CAPS_PROTOCOL) {
			psic = USBSSP_EXT_PORT_PSIC(readl(base + offset + 8));
			nregs = min(4 + psic, n);
		}

		usbssp_debugfs_regset(usbssp_data, offset, regs, nregs,
				usbssp_data->debugfs_root, "%s:%02d",
				cap_name, index);
		offset = usbssp_find_next_ext_cap(base, offset, cap_id);
		index++;
	}
}

static int usbssp_ring_enqueue_show(struct seq_file *s, void *unused)
{
	dma_addr_t dma;
	struct usbssp_ring *ring = *(struct usbssp_ring **)s->private;

	dma = usbssp_trb_virt_to_dma(ring->enq_seg, ring->enqueue);
	seq_printf(s, "%pad\n", &dma);

	return 0;
}

static int usbssp_ring_dequeue_show(struct seq_file *s, void *unused)
{
	dma_addr_t dma;
	struct usbssp_ring *ring = *(struct usbssp_ring **)s->private;

	dma = usbssp_trb_virt_to_dma(ring->deq_seg, ring->dequeue);
	seq_printf(s, "%pad\n", &dma);

	return 0;
}

static int usbssp_ring_cycle_show(struct seq_file *s, void *unused)
{
	struct usbssp_ring *ring = *(struct usbssp_ring **)s->private;

	seq_printf(s, "%d\n", ring->cycle_state);

	return 0;
}

static void usbssp_ring_dump_segment(struct seq_file *s,
				     struct usbssp_segment *seg)
{
	int i;
	dma_addr_t dma;
	union usbssp_trb *trb;

	for (i = 0; i < TRBS_PER_SEGMENT; i++) {
		trb = &seg->trbs[i];
		dma = seg->dma + i * sizeof(*trb);
		seq_printf(s, "%pad: %s\n", &dma,
				usbssp_decode_trb(trb->generic.field[0],
					trb->generic.field[1],
					trb->generic.field[2],
					trb->generic.field[3]));
	}
}

static int usbssp_ring_trb_show(struct seq_file *s, void *unused)
{
	int i;
	struct usbssp_ring *ring = *(struct usbssp_ring **)s->private;
	struct usbssp_segment *seg = ring->first_seg;

	for (i = 0; i < ring->num_segs; i++) {
		usbssp_ring_dump_segment(s, seg);
		seg = seg->next;
	}

	return 0;
}

static struct usbssp_file_map ring_files[] = {
	{"enqueue",	usbssp_ring_enqueue_show, },
	{"dequeue",	usbssp_ring_dequeue_show, },
	{"cycle",	usbssp_ring_cycle_show, },
	{"trbs",	usbssp_ring_trb_show, },
};

static int usbssp_ring_open(struct inode *inode, struct file *file)
{
	int i;
	struct usbssp_file_map *f_map;
	const char *file_name = file_dentry(file)->d_iname;

	for (i = 0; i < ARRAY_SIZE(ring_files); i++) {
		f_map = &ring_files[i];

		if (strcmp(f_map->name, file_name) == 0)
			break;
	}

	return single_open(file, f_map->show, inode->i_private);
	return 0;
}

static const struct file_operations usbssp_ring_fops = {
	.open = usbssp_ring_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int usbssp_slot_context_show(struct seq_file *s, void *unused)
{
	struct usbssp_udc *usbssp_data;
	struct usbssp_slot_ctx *slot_ctx;
	struct usbssp_slot_priv *priv = s->private;
	struct usbssp_device *dev = priv->dev;

	usbssp_data = gadget_to_usbssp(dev->gadget);
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev->out_ctx);
	seq_printf(s, "%pad: %s\n", &dev->out_ctx->dma,
		   usbssp_decode_slot_context(slot_ctx->dev_info,
				slot_ctx->dev_info2, slot_ctx->int_target,
				slot_ctx->dev_state));

	return 0;
}

static int usbssp_endpoint_context_show(struct seq_file *s, void *unused)
{
	int dci;
	dma_addr_t dma;
	struct usbssp_udc *usbssp_data;
	struct usbssp_ep_ctx *ep_ctx;
	struct usbssp_slot_priv	*priv = s->private;
	struct usbssp_device *dev = priv->dev;

	usbssp_data = gadget_to_usbssp(dev->gadget);

	for (dci = 1; dci < 32; dci++) {
		ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev->out_ctx, dci);
		dma = dev->out_ctx->dma +
		      dci * CTX_SIZE(usbssp_data->hcc_params);
		seq_printf(s, "%pad: %s\n", &dma,
			   usbssp_decode_ep_context(ep_ctx->ep_info,
					ep_ctx->ep_info2, ep_ctx->deq,
					ep_ctx->tx_info));
	}

	return 0;
}

static int usbssp_device_name_show(struct seq_file *s, void *unused)
{
	struct usbssp_slot_priv *priv = s->private;
	struct usbssp_device *dev = priv->dev;

	seq_printf(s, "%s\n", dev_name(&dev->gadget->dev));

	return 0;
}

static struct usbssp_file_map context_files[] = {
	{"name",	 usbssp_device_name_show, },
	{"slot-context", usbssp_slot_context_show, },
	{"ep-context",	 usbssp_endpoint_context_show, },
};

static int usbssp_context_open(struct inode *inode, struct file *file)
{
	int i;
	struct usbssp_file_map *f_map;
	const char *file_name = file_dentry(file)->d_iname;

	for (i = 0; i < ARRAY_SIZE(context_files); i++) {
		f_map = &context_files[i];

		if (strcmp(f_map->name, file_name) == 0)
			break;
	}

	return single_open(file, f_map->show, inode->i_private);
}

static const struct file_operations usbssp_context_fops = {
	.open		= usbssp_context_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void usbssp_debugfs_create_files(struct usbssp_udc *usbssp_data,
					struct usbssp_file_map *files,
					size_t nentries, void *data,
					struct dentry *parent,
					const struct file_operations *fops)
{
	int i;

	for (i = 0; i < nentries; i++)
		debugfs_create_file(files[i].name, 0444, parent, data, fops);
}

static struct dentry *usbssp_debugfs_create_ring_dir(struct usbssp_udc
						     *usbssp_data,
						     struct usbssp_ring **ring,
						     const char *name,
						     struct dentry *parent)
{
	struct dentry *dir;

	dir = debugfs_create_dir(name, parent);
	usbssp_debugfs_create_files(usbssp_data, ring_files,
			ARRAY_SIZE(ring_files), ring, dir, &usbssp_ring_fops);

	return dir;
}

static void usbssp_debugfs_create_context_files(struct usbssp_udc *usbssp_data,
						struct dentry *parent,
						int slot_id)
{
	struct usbssp_device	*dev = &usbssp_data->devs;

	usbssp_debugfs_create_files(usbssp_data, context_files,
			ARRAY_SIZE(context_files), dev->debugfs_private,
			parent, &usbssp_context_fops);
}

void usbssp_debugfs_create_endpoint(struct usbssp_udc *usbssp_data,
				    struct usbssp_device *dev,
				    int ep_index)
{
	struct usbssp_ep_priv *epriv;
	struct usbssp_slot_priv *spriv = dev->debugfs_private;

	if (spriv->eps[ep_index])
		return;

	epriv = kzalloc(sizeof(*epriv), GFP_KERNEL);
	if (!epriv)
		return;

	snprintf(epriv->name, sizeof(epriv->name), "ep%02d", ep_index);
	epriv->root = usbssp_debugfs_create_ring_dir(usbssp_data,
				&dev->eps[ep_index].ring, epriv->name,
				spriv->root);
	spriv->eps[ep_index] = epriv;
}

void usbssp_debugfs_remove_endpoint(struct usbssp_udc *usbssp_data,
				    struct usbssp_device *dev,
				    int ep_index)
{
	struct usbssp_ep_priv *epriv;
	struct usbssp_slot_priv *spriv = dev->debugfs_private;

	if (!spriv || !spriv->eps[ep_index])
		return;

	epriv = spriv->eps[ep_index];
	debugfs_remove_recursive(epriv->root);
	spriv->eps[ep_index] = NULL;
	kfree(epriv);
}

void usbssp_debugfs_create_slot(struct usbssp_udc *usbssp_data, int slot_id)
{
	struct usbssp_slot_priv *priv;
	struct usbssp_device *dev = &usbssp_data->devs;

	priv = kzalloc(sizeof(*priv), GFP_ATOMIC);
	if (!priv)
		return;

	snprintf(priv->name, sizeof(priv->name), "%02d", slot_id);
	priv->root = debugfs_create_dir(priv->name, usbssp_data->debugfs_slots);
	priv->dev = dev;
	dev->debugfs_private = priv;

	usbssp_debugfs_create_ring_dir(usbssp_data, &dev->eps[0].ring,
			"ep00", priv->root);

	usbssp_debugfs_create_context_files(usbssp_data, priv->root, slot_id);
}

void usbssp_debugfs_remove_slot(struct usbssp_udc *usbssp_data, int slot_id)
{
	int i;
	struct usbssp_slot_priv *priv;
	struct usbssp_device *dev = &usbssp_data->devs;

	if (!dev || !dev->debugfs_private)
		return;

	priv = dev->debugfs_private;

	debugfs_remove_recursive(priv->root);

	for (i = 0; i < 31; i++)
		kfree(priv->eps[i]);

	kfree(priv);
	dev->debugfs_private = NULL;
}

void usbssp_debugfs_init(struct usbssp_udc *usbssp_data)
{
	struct device *dev =  usbssp_data->dev;

	usbssp_data->debugfs_root = debugfs_create_dir(dev_name(dev),
					usbssp_debugfs_root);

	INIT_LIST_HEAD(&usbssp_data->regset_list);

	usbssp_debugfs_regset(usbssp_data,
			0, usbssp_cap_regs, ARRAY_SIZE(usbssp_cap_regs),
			usbssp_data->debugfs_root, "reg-cap");

	usbssp_debugfs_regset(usbssp_data,
			HC_LENGTH(readl(&usbssp_data->cap_regs->hc_capbase)),
			usbssp_op_regs, ARRAY_SIZE(usbssp_op_regs),
			usbssp_data->debugfs_root, "reg-op");

	usbssp_debugfs_regset(usbssp_data,
			readl(&usbssp_data->cap_regs->run_regs_off) & RTSOFF_MASK,
			usbssp_runtime_regs, ARRAY_SIZE(usbssp_runtime_regs),
			usbssp_data->debugfs_root, "reg-runtime");

	usbssp_debugfs_extcap_regset(usbssp_data, USBSSP_EXT_CAPS_PROTOCOL,
			usbssp_extcap_protocol,
			ARRAY_SIZE(usbssp_extcap_protocol),
			"reg-ext-protocol");

	usbssp_debugfs_create_ring_dir(usbssp_data, &usbssp_data->cmd_ring,
			"command-ring",
			usbssp_data->debugfs_root);

	usbssp_debugfs_create_ring_dir(usbssp_data, &usbssp_data->event_ring,
			"event-ring",
			usbssp_data->debugfs_root);

	usbssp_data->debugfs_slots = debugfs_create_dir("devices",
			usbssp_data->debugfs_root);
}

void usbssp_debugfs_exit(struct usbssp_udc *usbssp_data)
{
	struct usbssp_regset *rgs, *tmp;

	debugfs_remove_recursive(usbssp_data->debugfs_root);
	usbssp_data->debugfs_root = NULL;
	usbssp_data->debugfs_slots = NULL;

	list_for_each_entry_safe(rgs, tmp, &usbssp_data->regset_list, list)
		usbssp_debugfs_free_regset(rgs);
}

void __init usbssp_debugfs_create_root(void)
{
	usbssp_debugfs_root = debugfs_create_dir("usbssp", usb_debug_root);
}

void __exit usbssp_debugfs_remove_root(void)
{
	debugfs_remove_recursive(usbssp_debugfs_root);
	usbssp_debugfs_root = NULL;
}
