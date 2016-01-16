/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/list.h>
#include <linux/io.h>

#include <xen/xen.h>
#include <xen/xenbus.h>
#include <xen/grant_table.h>
#include <xen/events.h>
#include <xen/page.h>

#include <xen/interface/io/clkif.h>

#define GRANT_INVALID_REF	0
DEFINE_SPINLOCK(xen_clk_lock);
static struct clk **clks;
static struct clk_onecell_data clk_data;
static struct xen_clkfront_info *ginfo;
extern struct clk *clk_register_xen(struct device *dev, const char *name,
				    const char *parent_name,
				    unsigned long flags,
				    spinlock_t *lock);
static int __init xen_clkfront_register(int num, const char **clks_name)
{
	int i;

	for (i = 0; i < num; i++) {
		clks[i] = clk_register_xen(NULL, clks_name[i], NULL,
					   CLK_GATE_SET_TO_DISABLE,
					   &xen_clk_lock);
		if (IS_ERR_OR_NULL(clks[i]))
			return PTR_ERR(clks[i]);
	}

	return 0;
}

static void __init xen_clkfront_deregister(int num)
{
	int i;

	for (i = 0; i < num; i++) {
		if (clks[i])
			clk_unregister_gate(clks[i]);
	}
}

static const struct xenbus_device_id xen_clkfront_ids[] = {
	{ "vclk" },
	{ "" },
};

struct xen_clkfront_comp {
	struct completion completion;
	unsigned long rate;
	int success;
	int id;
	char clk_name[32];
};

struct xen_clkfront_info {
	spinlock_t lock;
	struct xenbus_device *clkdev;
	int clk_ring_ref;
	struct xen_clkif_front_ring clk_ring;
	unsigned int evtchn;
	unsigned int irq;
	struct xen_clkfront_comp comp[XENCLK_END];
};

static int xen_clkfront_probe(struct xenbus_device *dev,
			      const struct xenbus_device_id *id)
{
	struct xen_clkfront_info *info;
	int i;

	info = kzalloc(sizeof(struct xen_clkfront_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->clkdev = dev;
	dev_set_drvdata(&dev->dev, info);

	for (i = 0; i < ARRAY_SIZE(info->comp); i++)
		init_completion(&(info->comp[i].completion));

	ginfo = info;

	return 0;
}

static void xen_clkfront_destroy_rings(struct xen_clkfront_info *info)
{
	if (info->irq)
		unbind_from_irqhandler(info->irq, info);
	info->irq = 0;

	if (info->clk_ring_ref != GRANT_INVALID_REF) {
		gnttab_end_foreign_access(info->clk_ring_ref, 0,
					  (unsigned long)info->clk_ring.sring);
		info->clk_ring_ref = GRANT_INVALID_REF;
	}
	info->clk_ring.sring = NULL;
}

static int xen_clkfront_handle_int(struct xen_clkfront_info *info)
{
	struct xen_clkif_response *res;
	RING_IDX i, rp;
	int more_to_do = 0;
	unsigned long flags;
	struct xen_clkfront_comp *comp;

	spin_lock_irqsave(&info->lock, flags);
	rp = info->clk_ring.sring->rsp_prod;
	rmb(); /* ensure we see queued responses up to "rp" */

	for (i = info->clk_ring.rsp_cons; i != rp; i++) {
		res = RING_GET_RESPONSE(&info->clk_ring, i);
		BUG_ON(res->id >= XENCLK_END);
		comp = &info->comp[res->id];
		comp->id = res->id;
		comp->success = res->success;
		comp->rate = res->rate;
		strncpy(comp->clk_name, res->clk_name, sizeof(res->clk_name));
		complete(&comp->completion);
	}
	info->clk_ring.rsp_cons = i;

	if (i != info->clk_ring.req_prod_pvt)
		RING_FINAL_CHECK_FOR_RESPONSES(&info->clk_ring, more_to_do);
	else
		info->clk_ring.sring->rsp_event = i + 1;

	spin_unlock_irqrestore(&info->lock, flags);

	return more_to_do;
}

static irqreturn_t xen_clkfront_int(int irq, void *dev_id)
{
	struct xen_clkfront_info *info = dev_id;

	while (xen_clkfront_handle_int(info))
		cond_resched();

	return IRQ_HANDLED;
}

static int xen_clkfront_setup_rings(struct xenbus_device *dev,
				    struct xen_clkfront_info *info)
{
	struct xen_clkif_sring *clk_sring;
	grant_ref_t gref;
	int err;

	info->clk_ring_ref = GRANT_INVALID_REF;

	clk_sring = (struct xen_clkif_sring *)get_zeroed_page(
						GFP_NOIO | __GFP_HIGH);
	if (!clk_sring) {
		xenbus_dev_fatal(dev, -ENOMEM, "allocating clk sring");
		return -ENOMEM;
	}

	SHARED_RING_INIT(clk_sring);
	FRONT_RING_INIT(&info->clk_ring, clk_sring, PAGE_SIZE);

	err = xenbus_grant_ring(dev, clk_sring, 1, &gref);
	if (err < 0) {
		free_page((unsigned long)clk_sring);
		info->clk_ring.sring = NULL;
		goto fail;
	}
	info->clk_ring_ref = gref;

	err = xenbus_alloc_evtchn(dev, &info->evtchn);
	if (err) {
		xenbus_dev_fatal(dev, err, "xenbus_alloc_evtchn");
		goto fail;
	}

	err = bind_evtchn_to_irqhandler(info->evtchn, xen_clkfront_int, 0,
					"xen_clkif", info);
	if (err <= 0) {
		xenbus_dev_fatal(dev, err, "bind_evtchn_to_irqhandler failed");
		goto fail;
	}

	info->irq = err;

	return 0;

fail:
	xen_clkfront_destroy_rings(info);
	return err;
}

int xen_clkfront_wait_response(int id, const char *name, unsigned long *rate)
{
	struct xen_clkfront_info *info = ginfo;
	struct xen_clkfront_comp *comp = &info->comp[id];

	if (!ginfo) {
		pr_err("Not initialized\n");
		return -EIO;
	}

	wait_for_completion(&comp->completion);

	if ((id == comp->id) && !strncmp(name, comp->clk_name, sizeof(comp->clk_name))) {
		if (rate)
			*rate = comp->rate;
		return 0;
	}

	return -EIO;
}

int xen_clkfront_do_request(int id, const char *name, unsigned long rate)
{
	struct xen_clkfront_info *info = ginfo;
	struct xen_clkif_request *req;
	int notify;

	if (!info) {
		pr_err("Not initialized\n");
		return -EIO;
	}

	req = RING_GET_REQUEST(&info->clk_ring, info->clk_ring.req_prod_pvt);
	req->id = id;
	req->rate = rate;
	strncpy(req->clk_name, name, sizeof(req->clk_name));

	info->clk_ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&info->clk_ring, notify);

	if (notify)
		notify_remote_via_irq(info->irq);

	return 0;
}

static int xen_clkfront_connect(struct xenbus_device *dev)
{
	struct xen_clkfront_info *info = dev_get_drvdata(&dev->dev);
	struct xenbus_transaction xbt;
	int err;
	char *message;

	err = xen_clkfront_setup_rings(dev, info);
	if (err) {
		pr_err("%s: failure....", __func__);
		return err;
	}
again:
	err = xenbus_transaction_start(&xbt);
	if (err) {
		xenbus_dev_fatal(dev, err, "starting transaction");
		goto destroy_ring;
	}

	err = xenbus_printf(xbt, dev->nodename, "clk-ring-ref", "%u",
			    info->clk_ring_ref);
	if (err) {
		message = "writing clk-ring-ref";
		goto abort_transaction;
	}

	err = xenbus_printf(xbt, dev->nodename, "event-channel", "%u",
			    info->evtchn);
	if (err) {
		message = "writing event-channel";
		goto abort_transaction;
	}

	err = xenbus_transaction_end(xbt, 0);
	if (err) {
		if (err == -EAGAIN)
			goto again;
		xenbus_dev_fatal(dev, err, "completing transaction");
		goto destroy_ring;
	}

	return 0;

abort_transaction:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, err, "%s", message);

destroy_ring:
	xen_clkfront_destroy_rings(info);

	return err;
}

static void xen_clkfront_disconnect(struct xenbus_device *dev)
{
	xenbus_frontend_closed(dev);
}

static void xen_clkfront_backend_changed(struct xenbus_device *dev,
					 enum xenbus_state backend_state)
{
	switch (backend_state) {
	case XenbusStateInitialising:
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateUnknown:
		break;

	case XenbusStateInitWait:
	case XenbusStateInitialised:
	case XenbusStateConnected:
		if (dev->state != XenbusStateInitialising)
			break;
		if (!xen_clkfront_connect(dev))
			xenbus_switch_state(dev, XenbusStateConnected);
		break;

	case XenbusStateClosed:
		if (dev->state == XenbusStateClosed)
			break;
		/* Missed the backend's Closing state -- fallthrough */
	case XenbusStateClosing:
		xen_clkfront_disconnect(dev);
		break;

	default:
		xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
				 backend_state);
		break;
	}
}

static int xen_clkfront_remove(struct xenbus_device *dev)
{
	struct xen_clkfront_info *info = dev_get_drvdata(&dev->dev);

	xen_clkfront_destroy_rings(info);

	kfree(info);

	ginfo = NULL;

	return -EINVAL;
}

static struct xenbus_driver xen_clkfront_driver = {
	.ids = xen_clkfront_ids,
	.probe = xen_clkfront_probe,
	.otherend_changed = xen_clkfront_backend_changed,
	.remove = xen_clkfront_remove,
};

static int __init xen_clkfront_init(void)
{
	struct device_node *np;
	int nr, ret;
	const char **clks_name;

	if (!xen_domain())
		return -ENODEV;

	np = of_find_compatible_node(NULL, NULL, "xen,xen-clk");
	if (!np) {
		printk("error node\n");
		return -EINVAL;
	}

	ret = of_property_count_strings(np, "clock-output-names");
	if (ret <= 0) {
		of_node_put(np);
		return ret;
	}

	nr = ret;

	clks_name = kzalloc(sizeof(char *) * nr, GFP_KERNEL);
	if (!clks_name)
		return -ENOMEM;

	ret = of_property_read_string_array(np, "clock-output-names",
					    clks_name, nr);

	if (ret < 0)
		goto free_clks_name;

	clks = kzalloc(sizeof(struct clk *) * nr, GFP_KERNEL);
	if (!clks) {
		ret = PTR_ERR(clks);
		goto free_clks_name;
	}

	ret = xen_clkfront_register(nr, clks_name);
	if (ret != 0)
		goto free_clks;

	clk_data.clks = clks;
	clk_data.clk_num = nr;
	of_clk_add_provider(np, of_clk_src_onecell_get, &clk_data);

	ret = xenbus_register_frontend(&xen_clkfront_driver);
	if (ret) {
		pr_err("register frontend failure\n");
		goto free_clks;
	}

	of_node_put(np);

	return 0;

free_clks:
	xen_clkfront_deregister(nr);
	kfree(clks);
free_clks_name:
	of_node_put(np);
	kfree(clks_name);
	return ret;
}
subsys_initcall(xen_clkfront_init);
