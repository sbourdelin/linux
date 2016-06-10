#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/sched_clock.h>
#include <soc/at91/atmel_tcb.h>

struct atmel_tcb_clksrc {
	struct clocksource clksrc;
	struct clock_event_device clkevt;
	struct regmap *regmap;
	struct clk *clk[2];
	int channels[2];
	u8 bits;
	unsigned int irq;
	bool registered;
	bool irq_requested;
};

static struct atmel_tcb_clksrc tc = {
	.clksrc = {
		.name		= "tcb_clksrc",
		.rating		= 200,
		.mask		= CLOCKSOURCE_MASK(32),
		.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
	},
	.clkevt	= {
		.name			= "tcb_clkevt",
		.features		= CLOCK_EVT_FEAT_ONESHOT,
		/* Should be lower than at91rm9200's system timer */
		.rating			= 125,
	},
};

static cycle_t tc_get_cycles(struct clocksource *cs)
{
	unsigned long	flags;
	u32		lower, upper, tmp;

	raw_local_irq_save(flags);
	do {
		regmap_read(tc.regmap, ATMEL_TC_CV(1), &upper);
		regmap_read(tc.regmap, ATMEL_TC_CV(0), &lower);
		regmap_read(tc.regmap, ATMEL_TC_CV(1), &tmp);
	} while (upper != tmp);

	raw_local_irq_restore(flags);
	return (upper << 16) | lower;
}

static cycle_t tc_get_cycles32(struct clocksource *cs)
{
	u32 val;

	regmap_read(tc.regmap, ATMEL_TC_CV(tc.channels[0]), &val);

	return val;
}

static u64 tc_sched_clock_read(void)
{
	return tc_get_cycles(&tc.clksrc);
}

static u64 tc_sched_clock_read32(void)
{
	return tc_get_cycles32(&tc.clksrc);
}

static int tcb_clkevt_next_event(unsigned long delta,
				 struct clock_event_device *d)
{
	u32 val;

	regmap_read(tc.regmap, ATMEL_TC_CV(tc.channels[0]), &val);
	regmap_write(tc.regmap, ATMEL_TC_RC(tc.channels[0]), val + delta);
	regmap_write(tc.regmap, ATMEL_TC_IER(tc.channels[0]), ATMEL_TC_CPCS);

	return 0;
}

static irqreturn_t tc_clkevt_irq(int irq, void *handle)
{
	unsigned int sr;

	regmap_read(tc.regmap, ATMEL_TC_SR(tc.channels[0]), &sr);
	if (sr & ATMEL_TC_CPCS) {
		tc.clkevt.event_handler(&tc.clkevt);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int tcb_clkevt_oneshot(struct clock_event_device *dev)
{
	int ret;

	if (tc.irq_requested)
		return 0;

	ret = request_irq(tc.irq, tc_clkevt_irq, IRQF_TIMER | IRQF_SHARED,
			  "tcb_clkevt", &tc);
	if (!ret)
		tc.irq_requested = true;

	return ret;
}

static int tcb_clkevt_shutdown(struct clock_event_device *dev)
{
	regmap_write(tc.regmap, ATMEL_TC_IDR(tc.channels[0]), 0xff);
	if (tc.bits == 16)
		regmap_write(tc.regmap, ATMEL_TC_IDR(tc.channels[1]), 0xff);

	if (tc.irq_requested) {
		free_irq(tc.irq, &tc);
		tc.irq_requested = false;
	}

	return 0;
}

static void __init tcb_setup_dual_chan(struct atmel_tcb_clksrc *tc,
				       int mck_divisor_idx)
{
	/* first channel: waveform mode, input mclk/8, clock TIOA on overflow */
	regmap_write(tc->regmap, ATMEL_TC_CMR(tc->channels[0]),
		     mck_divisor_idx	/* likely divide-by-8 */
			| ATMEL_TC_CMR_WAVE
			| ATMEL_TC_CMR_WAVESEL_UP	/* free-run */
			| ATMEL_TC_CMR_ACPA(SET)	/* TIOA rises at 0 */
			| ATMEL_TC_CMR_ACPC(CLEAR));	/* (duty cycle 50%) */
	regmap_write(tc->regmap, ATMEL_TC_RA(tc->channels[0]), 0x0000);
	regmap_write(tc->regmap, ATMEL_TC_RC(tc->channels[0]), 0x8000);
	regmap_write(tc->regmap, ATMEL_TC_IDR(tc->channels[0]), 0xff);	/* no irqs */
	regmap_write(tc->regmap, ATMEL_TC_CCR(tc->channels[0]),
		     ATMEL_TC_CCR_CLKEN);

	/* second channel: waveform mode, input TIOA */
	regmap_write(tc->regmap, ATMEL_TC_CMR(tc->channels[1]),
		     ATMEL_TC_CMR_XC(tc->channels[1])	/* input: TIOA */
		     | ATMEL_TC_CMR_WAVE
		     | ATMEL_TC_CMR_WAVESEL_UP);	/* free-run */
	regmap_write(tc->regmap, ATMEL_TC_IDR(tc->channels[1]), 0xff);	/* no irqs */
	regmap_write(tc->regmap, ATMEL_TC_CCR(tc->channels[1]),
		     ATMEL_TC_CCR_CLKEN);

	/* chain both channel, we assume the previous channel */
	regmap_write(tc->regmap, ATMEL_TC_BMR,
		     ATMEL_TC_BMR_TCXC(1 + tc->channels[1], tc->channels[1]));
	/* then reset all the timers */
	regmap_write(tc->regmap, ATMEL_TC_BCR, ATMEL_TC_BCR_SYNC);
}

static void __init tcb_setup_single_chan(struct atmel_tcb_clksrc *tc,
					 int mck_divisor_idx)
{
	/* channel 0:  waveform mode, input mclk/8 */
	regmap_write(tc->regmap, ATMEL_TC_CMR(tc->channels[0]),
		     mck_divisor_idx	/* likely divide-by-8 */
			| ATMEL_TC_CMR_WAVE
			| ATMEL_TC_CMR_WAVESEL_UP	/* free-run */
			);
	regmap_write(tc->regmap, ATMEL_TC_IDR(tc->channels[0]), 0xff);	/* no irqs */
	regmap_write(tc->regmap, ATMEL_TC_CCR(tc->channels[0]),
		     ATMEL_TC_CCR_CLKEN);

	/* then reset all the timers */
	regmap_write(tc->regmap, ATMEL_TC_BCR, ATMEL_TC_BCR_SYNC);
}

static void __init tcb_clksrc_init(struct device_node *node)
{
	const struct of_device_id *match;
	u32 rate, divided_rate = 0;
	int best_divisor_idx = -1;
	int i, err;

	if (tc.registered)
		return;

	tc.regmap = syscon_node_to_regmap(node->parent);
	if (IS_ERR(tc.regmap))
		return;

	match = of_match_node(atmel_tcb_dt_ids, node->parent);
	tc.bits = (int)match->data;

	err = of_property_read_u32_index(node, "reg", 0, &tc.channels[0]);
	if (err)
		return;

	tc.channels[1] = -1;

	if (tc.bits == 16) {
		of_property_read_u32_index(node, "reg", 1, &tc.channels[1]);
		if (tc.channels[1] == -1) {
			pr_err("%s: clocksource needs two channels\n",
			       node->parent->full_name);
		}
	}

	tc.irq = tcb_irq_get(node, tc.channels[0]);
	if (tc.irq < 0)
		return;

	tc.clk[0] = tcb_clk_get(node, tc.channels[0]);
	if (IS_ERR(tc.clk[0]))
		return;
	err = clk_prepare_enable(tc.clk[0]);
	if (err) {
		pr_debug("can't enable T0 clk\n");
		goto err_clk;
	}

	if (tc.bits == 16) {
		tc.clk[1] = tcb_clk_get(node, tc.channels[1]);
		if (IS_ERR(tc.clk[1]))
			goto err_disable_t0;
	}

	/* How fast will we be counting?  Pick something over 5 MHz.  */
	rate = (u32)clk_get_rate(tc.clk[0]);
	for (i = 0; i < 5; i++) {
		unsigned int divisor = atmel_tc_divisors[i];
		unsigned int tmp;

		if (!divisor)
			continue;

		tmp = rate / divisor;
		pr_debug("TC: %u / %-3u [%d] --> %u\n", rate, divisor, i, tmp);
		if (best_divisor_idx > 0) {
			if (tmp < 5 * 1000 * 1000)
				continue;
		}
		divided_rate = tmp;
		best_divisor_idx = i;
	}

	pr_debug("%s: %s at %d.%03d MHz\n", tc.clksrc.name,
		 node->parent->full_name, divided_rate / 1000000,
		 ((divided_rate + 500000) % 1000000) / 1000);

	if (tc.bits == 32) {
		tc.clksrc.read = tc_get_cycles32;
		tcb_setup_single_chan(&tc, best_divisor_idx);
	} else {
		err = clk_prepare_enable(tc.clk[1]);
		if (err) {
			pr_debug("can't enable T1 clk\n");
			goto err_clk1;
		}
		tc.clksrc.read = tc_get_cycles,
		tcb_setup_dual_chan(&tc, best_divisor_idx);
	}

	err = clocksource_register_hz(&tc.clksrc, divided_rate);
	if (err)
		goto err_disable_t1;

	if (tc.bits == 32)
		sched_clock_register(tc_sched_clock_read32, 32, divided_rate);
	else
		sched_clock_register(tc_sched_clock_read, 32, divided_rate);

	tc.registered = true;

	/* Set up and register clockevents */
	tc.clkevt.cpumask = cpumask_of(0);
	tc.clkevt.set_next_event = tcb_clkevt_next_event;
	tc.clkevt.set_state_oneshot = tcb_clkevt_oneshot;
	tc.clkevt.set_state_shutdown = tcb_clkevt_shutdown;
	if (tc.bits == 16)
		clockevents_config_and_register(&tc.clkevt, divided_rate, 1,
						0xffff);
	else
		clockevents_config_and_register(&tc.clkevt, divided_rate, 1,
						0xffffffff);
	return;

err_disable_t1:
	if (tc.bits == 16)
		clk_disable_unprepare(tc.clk[1]);

err_clk1:
	if (tc.bits == 16)
		clk_put(tc.clk[1]);

err_disable_t0:
	clk_disable_unprepare(tc.clk[0]);

err_clk:
	clk_put(tc.clk[0]);

	pr_err("%s: unable to register clocksource/clockevent\n",
	       tc.clksrc.name);
}
CLOCKSOURCE_OF_DECLARE(atmel_tcb_clksrc, "atmel,tcb-clksrc",
		       tcb_clksrc_init);
