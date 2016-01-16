/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/string.h>
#include <xen/interface/io/clkif.h>

#define to_clk_xen(_hw) container_of(_hw, struct xen_clk, hw)

int xen_clkfront_do_request(int id, const char *name, unsigned long rate);
int xen_clkfront_wait_response(int id, const char *name, unsigned long *rate);
static int xen_clkfront_prepare(struct clk_hw *hw)
{
	struct clk *clk = hw->clk;
	const char *name = __clk_get_name(clk);
	unsigned long rate;
	int err;

	err = xen_clkfront_do_request(XENCLK_PREPARE, name, 0);
	if (err)
		return 0;

	err = xen_clkfront_wait_response(XENCLK_PREPARE, name, NULL);
	if (err)
		return -EIO;

	return 0;
}

void xen_clkfront_unprepare(struct clk_hw *hw)
{
	struct clk *clk = hw->clk;
	const char *name = __clk_get_name(clk);
	unsigned long rate;
	int err;

	err = xen_clkfront_do_request(XENCLK_UNPREPARE, name, 0);
	if (err)
		return 0;

	xen_clkfront_wait_response(XENCLK_UNPREPARE, name, NULL);

	return 0;
}

/* clk_enable */
int xen_clkfront_enable(struct clk_hw *hw)
{
	/*
	 * clk_enable API can be used in interrupt context,
	 * but here the pvclk framework only works in sleepable context.
	 * So in DomU frontend, clk_prepare takes the responsibility
	 * for enable clk in backend.
	 */
	return 0;
}

/* clk_disable */
void xen_clkfront_disable(struct clk_hw *hw)
{
}

/* clk_get_rate */
static unsigned long xen_clkfront_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct clk *clk = hw->clk;
	const char *name = __clk_get_name(clk);
	unsigned long rate;
	int err;
	
	if (!name) {
		BUG_ON(!name);
		return 0;
	}

	err = xen_clkfront_do_request(XENCLK_GET_RATE, name, 0);
	if (err)
		return 0;

	err = xen_clkfront_wait_response(XENCLK_GET_RATE, name, &rate);
	if (err)
		return 0;

	return rate;
}

/* clk_set_rate */
int xen_clkfront_set_rate(struct clk_hw *hw, unsigned long rate,
			  unsigned long parent_rate)
{
	struct clk *clk = hw->clk;
	const char *name = __clk_get_name(clk);
	int err;

	if (!name) {
		BUG_ON(!name);
		return 0;
	}

	err = xen_clkfront_do_request(XENCLK_SET_RATE, name, rate);
	if (err)
		return 0;

	err = xen_clkfront_wait_response(XENCLK_SET_RATE, name, NULL);
	if (err)
		return -EINVAL;

	return 0;
}

long xen_clkfront_determine_rate(struct clk_hw *hw,
				 unsigned long rate,
				 unsigned long min_rate,
				 unsigned long max_rate,
				 unsigned long *best_parent_rate,
				 struct clk_hw **best_parent_hw)
{
	/* directly return rate, let backend does this work */
	return rate;
}

const struct clk_ops xen_clkfront_ops = {
	.prepare = xen_clkfront_prepare,
	.unprepare = xen_clkfront_unprepare,
	.enable = xen_clkfront_enable,
	.disable = xen_clkfront_disable,
	.recalc_rate = xen_clkfront_recalc_rate,
	.determine_rate = xen_clkfront_determine_rate,
	.set_rate = xen_clkfront_set_rate,
};
EXPORT_SYMBOL_GPL(xen_clkfront_ops);

struct xen_clk {
	struct clk_hw hw;
	u8 flags;
	spinlock_t *lock;
};

struct clk *clk_register_xen(struct device *dev, const char *name,
			     const char *parent_name, unsigned long flags,
			     spinlock_t *lock)
{
	struct clk *clk;
	struct clk_init_data init;
	struct xen_clk *xenclk;

	xenclk = kzalloc(sizeof(struct xen_clk), GFP_KERNEL);
	if (!xenclk) {
		pr_err("%s: cound not allocate xen clk\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	init.name = name;
	init.ops = &xen_clkfront_ops;
	/* register as root clk in frontend */
	init.flags = flags | CLK_GET_RATE_NOCACHE | CLK_GET_ACCURACY_NOCACHE | CLK_IS_ROOT;
	init.parent_names = NULL;
	init.num_parents = 0;

	xenclk->hw.init = &init;

	clk = clk_register(dev, &xenclk->hw);
	if (IS_ERR(clk)) {
		pr_err("clk_register failure %s\n", name);
		kfree(xenclk);
	}

	return clk;
}
EXPORT_SYMBOL_GPL(clk_register_xen);

void clk_unregister_xen(struct clk *clk)
{
	struct xen_clk *xenclk;
	struct clk_hw *hw;

	hw = __clk_get_hw(clk);
	if (!hw)
		return;

	xenclk = to_clk_xen(hw);

	clk_unregister(clk);
	kfree(xenclk);
}
EXPORT_SYMBOL_GPL(clk_unregister_xen);
