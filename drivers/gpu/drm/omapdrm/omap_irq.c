/*
 * drivers/gpu/drm/omapdrm/omap_irq.c
 *
 * Copyright (C) 2012 Texas Instruments
 * Author: Rob Clark <rob.clark@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "omap_drv.h"

struct omap_irq_wait {
	struct list_head node;
	wait_queue_head_t wq;
	struct dss_irq irqmask;
	int count;
};

static void dss_irq_or(struct omap_drm_private *priv,
		       struct dss_irq *res, const struct dss_irq *arg1,
		       const struct dss_irq *arg2)
{
	int i;

	for (i = 0; i < priv->dispc_ops->get_num_mgrs(); i++)
		res->channel[i] = arg1->channel[i] | arg2->channel[i];

	for (i = 0; i < priv->dispc_ops->get_num_ovls(); i++)
		res->ovl[i] = arg1->ovl[i] | arg2->ovl[i];
}

static void dss_irq_and(struct omap_drm_private *priv,
			struct dss_irq *res, const struct dss_irq *arg1,
			const struct dss_irq *arg2)
{
	int i;

	for (i = 0; i < priv->dispc_ops->get_num_mgrs(); i++)
		res->channel[i] = arg1->channel[i] & arg2->channel[i];

	for (i = 0; i < priv->dispc_ops->get_num_ovls(); i++)
		res->ovl[i] = arg1->ovl[i] & arg2->ovl[i];
}

static bool dss_irq_nonzero(struct omap_drm_private *priv,
			    struct dss_irq *status)
{
	int i;

	for (i = 0; i < priv->dispc_ops->get_num_mgrs(); i++)
		if (status->channel[i])
			return true;

	for (i = 0; i < priv->dispc_ops->get_num_ovls(); i++)
		if (status->ovl[i])
			return true;

	return false;
}

/* call with wait_lock and dispc runtime held */
static void omap_irq_full_mask(struct drm_device *dev, struct dss_irq *irqmask)
{
	struct omap_drm_private *priv = dev->dev_private;
	struct omap_irq_wait *wait;

	assert_spin_locked(&priv->wait_lock);

	*irqmask = priv->irq_mask;

	list_for_each_entry(wait, &priv->wait_list, node)
		dss_irq_or(priv, irqmask, irqmask, &wait->irqmask);

	DBG("irqmask ch %02x %02x %02x %02x ovl %02x %02x %02x %02x",
	    irqmask->channel[0], irqmask->channel[1],
	    irqmask->channel[2], irqmask->channel[3],
	    irqmask->ovl[0], irqmask->ovl[1], irqmask->ovl[2], irqmask->ovl[3]);
}

static void omap_irq_wait_handler(struct omap_irq_wait *wait)
{
	wait->count--;
	wake_up(&wait->wq);
}

struct omap_irq_wait * omap_irq_wait_init(struct drm_device *dev,
		struct dss_irq *waitmask, int count)
{
	struct omap_drm_private *priv = dev->dev_private;
	struct omap_irq_wait *wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	struct dss_irq irqmask;
	unsigned long flags;

	init_waitqueue_head(&wait->wq);
	wait->irqmask = *waitmask;
	wait->count = count;

	spin_lock_irqsave(&priv->wait_lock, flags);
	list_add(&wait->node, &priv->wait_list);
	omap_irq_full_mask(dev, &irqmask);
	priv->dispc_ops->write_irqenable(&irqmask);
	spin_unlock_irqrestore(&priv->wait_lock, flags);

	return wait;
}

int omap_irq_wait(struct drm_device *dev, struct omap_irq_wait *wait,
		unsigned long timeout)
{
	struct omap_drm_private *priv = dev->dev_private;
	struct dss_irq irqmask;
	unsigned long flags;
	int ret;

	ret = wait_event_timeout(wait->wq, (wait->count <= 0), timeout);

	spin_lock_irqsave(&priv->wait_lock, flags);
	list_del(&wait->node);
	omap_irq_full_mask(dev, &irqmask);
	priv->dispc_ops->write_irqenable(&irqmask);
	spin_unlock_irqrestore(&priv->wait_lock, flags);

	kfree(wait);

	return ret == 0 ? -1 : 0;
}

/**
 * enable_vblank - enable vblank interrupt events
 * @dev: DRM device
 * @pipe: which irq to enable
 *
 * Enable vblank interrupts for @crtc.  If the device doesn't have
 * a hardware vblank counter, this routine should be a no-op, since
 * interrupts will have to stay on to keep the count accurate.
 *
 * RETURNS
 * Zero on success, appropriate errno if the given @crtc's vblank
 * interrupt cannot be enabled.
 */
int omap_irq_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct omap_drm_private *priv = dev->dev_private;
	unsigned long flags;
	enum omap_channel channel = omap_crtc_channel(crtc);
	struct dss_irq irqmask;

	DBG("dev=%p, crtc=%u", dev, channel);

	spin_lock_irqsave(&priv->wait_lock, flags);
	priv->irq_mask.channel[channel] |=
		DSS_IRQ_MGR_VSYNC_EVEN | DSS_IRQ_MGR_VSYNC_ODD;
	omap_irq_full_mask(dev, &irqmask);
	priv->dispc_ops->write_irqenable(&irqmask);
	spin_unlock_irqrestore(&priv->wait_lock, flags);

	return 0;
}

/**
 * disable_vblank - disable vblank interrupt events
 * @dev: DRM device
 * @pipe: which irq to enable
 *
 * Disable vblank interrupts for @crtc.  If the device doesn't have
 * a hardware vblank counter, this routine should be a no-op, since
 * interrupts will have to stay on to keep the count accurate.
 */
void omap_irq_disable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct omap_drm_private *priv = dev->dev_private;
	unsigned long flags;
	enum omap_channel channel = omap_crtc_channel(crtc);
	struct dss_irq irqmask;

	DBG("dev=%p, crtc=%u", dev, channel);

	spin_lock_irqsave(&priv->wait_lock, flags);
	priv->irq_mask.channel[channel] &=
		~(DSS_IRQ_MGR_VSYNC_EVEN | DSS_IRQ_MGR_VSYNC_ODD);
	omap_irq_full_mask(dev, &irqmask);
	priv->dispc_ops->write_irqenable(&irqmask);
	spin_unlock_irqrestore(&priv->wait_lock, flags);
}

static void omap_irq_fifo_underflow(struct omap_drm_private *priv,
				    struct dss_irq *irqstatus)
{
	static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	bool ovl_undeflow[DSS_MAX_OVLS] = { false };
	bool underflow = false;
	unsigned int i;

	spin_lock(&priv->wait_lock);
	for (i = 0; i < priv->dispc_ops->get_num_ovls(); i++)
		if (irqstatus->ovl[i] & priv->irq_mask.ovl[i] &
		    DSS_IRQ_OVL_FIFO_UNDERFLOW)
			underflow = ovl_undeflow[i] = true;
	spin_unlock(&priv->wait_lock);

	if (!underflow)
		return;

	if (!__ratelimit(&_rs))
		return;

	DRM_ERROR("FIFO underflow on ");

	for (i = 0; i < DSS_MAX_OVLS; ++i) {
		if (ovl_undeflow[i])
			pr_cont("%u:%s ", i, priv->dispc_ops->get_ovl_name(i));
	}

	pr_cont("\n");
}

static void omap_irq_ocp_error_handler(struct drm_device *dev,
				       struct dss_irq *irqstatus)
{
	if (!(irqstatus->device & DSS_IRQ_DEVICE_OCP_ERR))
		return;

	dev_err_ratelimited(dev->dev, "OCP error\n");
}

static irqreturn_t omap_irq_handler(int irq, void *arg)
{
	struct drm_device *dev = (struct drm_device *) arg;
	struct omap_drm_private *priv = dev->dev_private;
	struct omap_irq_wait *wait, *n;
	unsigned long flags;
	unsigned int id;
	struct dss_irq irqstatus, clearmask;

	spin_lock_irqsave(&priv->wait_lock, flags);
	omap_irq_full_mask(dev, &clearmask);
	priv->dispc_ops->read_irqstatus(&irqstatus, &clearmask);

	list_for_each_entry_safe(wait, n, &priv->wait_list, node) {
		struct dss_irq waitstatus;

		dss_irq_and(priv, &waitstatus, &irqstatus, &wait->irqmask);
		if (dss_irq_nonzero(priv, &waitstatus))
			omap_irq_wait_handler(wait);
	}
	spin_unlock_irqrestore(&priv->wait_lock, flags);

	VERB("irqs: ch 0x%02x 0x%02x 0x%02x 0x%02x ovl 0x%02x 0x%02x 0x%02x 0x%02x\n",
	       irqstatus.channel[0], irqstatus.channel[1],
	       irqstatus.channel[2], irqstatus.channel[3],
	       irqstatus.ovl[0], irqstatus.ovl[1],
	       irqstatus.ovl[2], irqstatus.ovl[3]);


	for (id = 0; id < priv->num_crtcs; id++) {
		struct drm_crtc *crtc = priv->crtcs[id];
		enum omap_channel channel = omap_crtc_channel(crtc);

		if (irqstatus.channel[channel] & 
		    (DSS_IRQ_MGR_VSYNC_EVEN | DSS_IRQ_MGR_VSYNC_ODD)) {
			drm_handle_vblank(dev, id);
			omap_crtc_vblank_irq(crtc);
		}

		if (irqstatus.channel[channel] & DSS_IRQ_MGR_SYNC_LOST)
			omap_crtc_error_irq(crtc, irqstatus.channel[channel]);
	}

	omap_irq_ocp_error_handler(dev, &irqstatus);
	omap_irq_fifo_underflow(priv, &irqstatus);

	return IRQ_HANDLED;
}

/*
 * We need a special version, instead of just using drm_irq_install(),
 * because we need to register the irq via omapdss.  Once omapdss and
 * omapdrm are merged together we can assign the dispc hwmod data to
 * ourselves and drop these and just use drm_irq_{install,uninstall}()
 */

int omap_drm_irq_install(struct drm_device *dev)
{
	struct omap_drm_private *priv = dev->dev_private;
	unsigned int i;
	int ret;

	spin_lock_init(&priv->wait_lock);
	INIT_LIST_HEAD(&priv->wait_list);

	priv->irq_mask.device = DSS_IRQ_DEVICE_OCP_ERR;

	for (i = 0; i < priv->num_planes; ++i)
		priv->irq_mask.ovl[omap_plane_get_id(priv->planes[i])] |=
			DSS_IRQ_OVL_FIFO_UNDERFLOW;

	for (i = 0; i < priv->num_crtcs; ++i)
		priv->irq_mask.channel[omap_crtc_channel(priv->crtcs[i])] |=
			DSS_IRQ_MGR_SYNC_LOST;

	ret = priv->dispc_ops->request_irq(omap_irq_handler, dev);
	if (ret < 0)
		return ret;

	dev->irq_enabled = true;

	return 0;
}

void omap_drm_irq_uninstall(struct drm_device *dev)
{
	struct omap_drm_private *priv = dev->dev_private;

	if (!dev->irq_enabled)
		return;

	dev->irq_enabled = false;

	priv->dispc_ops->free_irq(dev);
}
