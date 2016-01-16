/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>

#include <xen/xen.h>
#include <xen/events.h>
#include <xen/xenbus.h>
#include <xen/grant_table.h>
#include <xen/page.h>

#include <linux/clk.h>

#include <xen/interface/grant_table.h>
#include <xen/interface/io/clkif.h>

extern struct clk *__clk_lookup(const char *name);
extern bool __clk_is_prepared(struct clk *clk);

struct xen_clkback_info {
	domid_t domid;
	unsigned irq;
	unsigned long handle;
	struct xenbus_device *clkdev;
	spinlock_t clk_ring_lock;
	struct xen_clkif_back_ring clk_ring;
	atomic_t refcnt;
	int is_connected;
	int ring_error;
};

static void xen_clkback_do_response(struct xen_clkback_info *info,
				    int id, char *name, unsigned long rate,
				    int success)
{
	struct xen_clkif_response *res;
	unsigned long flags;
	int notify;

	spin_lock_irqsave(&info->clk_ring_lock, flags);
	res = RING_GET_RESPONSE(&info->clk_ring, info->clk_ring.rsp_prod_pvt);

	res->success = success;
	res->id = id;
	res->rate = rate;
	strncpy(res->clk_name, name, sizeof(res->clk_name));
	info->clk_ring.rsp_prod_pvt++;

	/* More stuff */
	barrier();
	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&info->clk_ring, notify);
	spin_unlock_irqrestore(&info->clk_ring_lock, flags);

	if (notify)
		notify_remote_via_irq(info->irq);
}

static int xen_clkback_handle_int(struct xen_clkback_info *info)
{
	struct xen_clkif_back_ring *clk_ring = &info->clk_ring;
	struct xen_clkif_request req;
	RING_IDX rc, rp;
	int more_to_do;
	struct clk *clk;
	int err;
	unsigned long rate;

	rc = clk_ring->req_cons;
	rp = clk_ring->sring->req_prod;
	rmb();	/* req_cons is written by frontend. */
	
	if (RING_REQUEST_PROD_OVERFLOW(clk_ring, rp)) {
		rc = clk_ring->rsp_prod_pvt;
		info->ring_error = 1;
		return 0;
	}

	while (rc != rp) {
		if (RING_REQUEST_CONS_OVERFLOW(clk_ring, rc))
			break;

		req = *RING_GET_REQUEST(clk_ring, rc);
		clk = __clk_lookup(req.clk_name);
		if (!clk)
			pr_err("no clk node for %s\n", req.clk_name);

		switch (req.id) {
		case XENCLK_PREPARE:
			err = clk_prepare_enable(clk);
			xen_clkback_do_response(info, req.id, req.clk_name, 0, err);
			break;
		case XENCLK_UNPREPARE:
			clk_disable_unprepare(clk);
			err = !__clk_is_prepared(clk);
			xen_clkback_do_response(info, req.id, req.clk_name, 0, err);
			break;
		case XENCLK_GET_RATE:
			rate = clk_get_rate(clk);
			xen_clkback_do_response(info, req.id, req.clk_name, rate, 0);
			break;
		case XENCLK_SET_RATE:
			err = clk_set_rate(clk, req.rate);
			xen_clkback_do_response(info, req.id, req.clk_name, 0, err);
			break;
		}
		clk_ring->req_cons = ++rc;

		cond_resched();
	}

	RING_FINAL_CHECK_FOR_REQUESTS(clk_ring, more_to_do);

	return !!more_to_do;
}

static irqreturn_t xen_clkback_be_int(int irq, void *dev_id)
{
	struct xen_clkback_info *info = dev_id;

	if (info->ring_error)
		return IRQ_HANDLED;

	while (xen_clkback_handle_int(info))
		cond_resched();

	return IRQ_HANDLED;
}

static int xen_clkback_map(struct xen_clkback_info *info,
			   grant_ref_t *clk_ring_ref, evtchn_port_t evtchn)
{
	int err;
	void *addr;
	struct xen_clkif_sring *clk_sring;

	if (info->irq)
		return 0;

	err = xenbus_map_ring_valloc(info->clkdev, clk_ring_ref, 1, &addr);
	if (err)
		return err;

	clk_sring = addr;

	BACK_RING_INIT(&info->clk_ring, clk_sring, PAGE_SIZE);

	err = bind_interdomain_evtchn_to_irq(info->domid, evtchn);
	if (err < 0)
		goto fail_evtchn;
	info->irq = err;

	err = request_threaded_irq(info->irq, NULL, xen_clkback_be_int,
				   IRQF_ONESHOT, "xen-clkback", info);
	if (err) {
		pr_err("bind evtchn to irq failure!\n");
		goto free_irq;
	}

	return 0;
free_irq:
	unbind_from_irqhandler(info->irq, info);
	info->irq = 0;
	info->clk_ring.sring = NULL;
fail_evtchn:
	xenbus_unmap_ring_vfree(info->clkdev, clk_sring);
	return err;
}

static int xen_clkback_connect_rings(struct xen_clkback_info *info)
{
	struct xenbus_device *dev = info->clkdev;
	unsigned clk_ring_ref, evtchn;
	int err;

	err = xenbus_gather(XBT_NIL, dev->otherend,
			    "clk-ring-ref", "%u", &clk_ring_ref,
			    "event-channel", "%u", &evtchn, NULL);
	if (err) {
		xenbus_dev_fatal(dev, err,
				 "reading %s/ring-ref and event-channel",
				 dev->otherend);
		return err;
	}

	pr_info("xen-pvclk: clk-ring-ref %u, event-channel %u\n",
		clk_ring_ref, evtchn);

	err = xen_clkback_map(info, &clk_ring_ref, evtchn);
	if (err)
		xenbus_dev_fatal(dev, err, "mapping urb-ring-ref %u evtchn %u",
			clk_ring_ref, evtchn);

	return err;
}

static void xen_clkback_disconnect(struct xen_clkback_info *info)
{
	if (info->irq) {
		unbind_from_irqhandler(info->irq, info);
		info->irq = 0;
	}

	if (info->clk_ring.sring) {
		xenbus_unmap_ring_vfree(info->clkdev, info->clk_ring.sring);
		info->clk_ring.sring = NULL;
	}
}

static void xen_clkback_frontend_changed(struct xenbus_device *dev,
					 enum xenbus_state frontend_state)
{
	struct xen_clkback_info *info = dev_get_drvdata(&dev->dev);

	switch (frontend_state) {
	case XenbusStateInitialised:
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
		break;

	case XenbusStateInitialising:
		if (dev->state == XenbusStateClosed) {
			pr_info("xen-pvclk: %s: prepare for reconnect\n",
				dev->nodename);
			xenbus_switch_state(dev, XenbusStateInitWait);
		}
		break;
	case XenbusStateConnected:
		if (dev->state != XenbusStateConnected)
			xenbus_switch_state(dev, XenbusStateConnected);

		xen_clkback_connect_rings(info);
		break;
	case XenbusStateClosing:
		xen_clkback_disconnect(info);
		xenbus_switch_state(dev, XenbusStateClosing);
		break;
	case XenbusStateClosed:
		xenbus_switch_state(dev, XenbusStateClosed);
		if (xenbus_dev_is_online(dev))
			break;
		/* fall through if not online */
	case XenbusStateUnknown:
		device_unregister(&dev->dev);
		break;

	default:
		xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
				 frontend_state);
		break;
	}
}

static struct xen_clkback_info *xen_clkback_alloc(domid_t domid,
						  unsigned long handle)
{
	struct xen_clkback_info *info;

	info = kzalloc(sizeof(struct xen_clkback_info), GFP_KERNEL);
	if (!info)
		return NULL;

	info->domid = domid;
	info->handle = handle;
	spin_lock_init(&info->clk_ring_lock);
	atomic_set(&info->refcnt, 0);
	info->ring_error = 0;

	return info;
}

static int xen_clkback_probe(struct xenbus_device *dev,
			     const struct xenbus_device_id *id)
{
	struct xen_clkback_info *info;
	unsigned long handle;
	int err;

	if (kstrtoul(strrchr(dev->otherend, '/') + 1, 0, &handle))
		return -EINVAL;

	info = xen_clkback_alloc(dev->otherend_id, handle);
	if (!info) {
		xenbus_dev_fatal(dev, -ENOMEM, "Allocating backend interface");
		return -ENOMEM;
	}

	info->clkdev = dev;
	dev_set_drvdata(&dev->dev, info);

	err = xenbus_switch_state(dev, XenbusStateInitWait);
	if (err)
		return err;

	return 0;
}

static int xen_clkback_remove(struct xenbus_device *dev)
{
	struct xen_clkback_info *info = dev_get_drvdata(&dev->dev);

	if (!info)
		return 0;

	xen_clkback_disconnect(info);

	kfree(info);
	dev_set_drvdata(&dev->dev, NULL);

	return 0;
}

static const struct xenbus_device_id xen_clkback_ids[] = {
	{ "vclk" },
	{ "" },
};

static struct xenbus_driver xen_clkback_driver = {
	.ids			= xen_clkback_ids,
	.probe			= xen_clkback_probe,
	.otherend_changed	= xen_clkback_frontend_changed,
	.remove			= xen_clkback_remove,
};

static int __init xen_clkback_init(void)
{
	int err;

	if (!xen_domain())
		return -ENODEV;

	err = xenbus_register_backend(&xen_clkback_driver);
	if (err)
		return err;

	/* Can we avoid libxl pvclk? doing xenstore in kernel ? */
	return 0;
}
module_init(xen_clkback_init);

static void __exit xen_clkback_exit(void)
{
	xenbus_unregister_driver(&xen_clkback_driver);
}
module_exit(xen_clkback_exit);

MODULE_ALIAS("xen-clkback:vclk");
MODULE_AUTHOR("Peng Fan <van.freenix@gmail.com>");
MODULE_DESCRIPTION("Xen CLK backend driver (clkback)");
MODULE_LICENSE("Dual BSD/GPL");
