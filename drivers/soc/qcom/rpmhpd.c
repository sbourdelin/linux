// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <soc/qcom/cmd-db.h>
#include <soc/qcom/rpmh.h>

#define domain_to_rpmhpd(domain) container_of(domain, struct rpmhpd, pd)

#define DEFINE_RPMHPD_AO(_platform, _name, _active)			\
	static struct rpmhpd _platform##_##_active;			\
	static struct rpmhpd _platform##_##_name = {			\
		.pd = {	.name = #_name,	},				\
		.peer = &_platform##_##_active,				\
		.res_name = #_name".lvl",				\
	};								\
	static struct rpmhpd _platform##_##_active = {			\
		.pd = { .name = #_active, },				\
		.peer = &_platform##_##_name,				\
		.active_only = true,					\
		.res_name = #_name".lvl",				\
	}

#define DEFINE_RPMHPD(_platform, _name)					\
	static struct rpmhpd _platform##_##_name = {			\
		.pd = { .name = #_name, },				\
		.res_name = #_name".lvl",				\
	}

/*
 * This is the number of bytes used for each command DB aux data entry of an
 * ARC resource.
 */
#define RPMH_ARC_LEVEL_SIZE	2
#define RPMH_ARC_MAX_LEVELS	16

struct rpmhpd {
	struct device *dev;
	struct generic_pm_domain pd;
	struct rpmhpd *peer;
	const bool active_only;
	unsigned long corner;
	u32	level[RPMH_ARC_MAX_LEVELS];
	int	level_count;
	bool enabled;
	const char *res_name;
	u32	addr;
};

struct rpmhpd_desc {
	struct rpmhpd **rpmhpds;
	size_t num_pds;
};

static DEFINE_MUTEX(rpmhpd_lock);

/* sdm845 RPMh powerdomains */
DEFINE_RPMHPD(sdm845, ebi);
DEFINE_RPMHPD_AO(sdm845, mx, mx_ao);
DEFINE_RPMHPD_AO(sdm845, cx, cx_ao);
DEFINE_RPMHPD(sdm845, lmx);
DEFINE_RPMHPD(sdm845, lcx);
DEFINE_RPMHPD(sdm845, gfx);
DEFINE_RPMHPD(sdm845, mss);

static struct rpmhpd *sdm845_rpmhpds[] = {
	[0] = &sdm845_ebi,
	[1] = &sdm845_mx,
	[2] = &sdm845_mx_ao,
	[3] = &sdm845_cx,
	[4] = &sdm845_cx_ao,
	[5] = &sdm845_lmx,
	[6] = &sdm845_lcx,
	[7] = &sdm845_gfx,
	[8] = &sdm845_mss,
};

static const struct rpmhpd_desc sdm845_desc = {
	.rpmhpds = sdm845_rpmhpds,
	.num_pds = ARRAY_SIZE(sdm845_rpmhpds),
};

static const struct of_device_id rpmhpd_match_table[] = {
	{ .compatible = "qcom,sdm845-rpmhpd", .data = &sdm845_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, rpmhpd_match_table);

static int rpmhpd_send_corner(struct rpmhpd *pd, int state, unsigned int corner)
{
	struct tcs_cmd cmd = {
		.addr = pd->addr,
		.data = corner,
	};

	return rpmh_write(pd->dev, state, &cmd, 1);
};

static void to_active_sleep(struct rpmhpd *pd, unsigned long corner,
			    unsigned long *active, unsigned long *sleep)
{
	*active = corner;

	if (pd->active_only)
		*sleep = 0;
	else
		*sleep = *active;
}

static int rpmhpd_aggregate_corner(struct rpmhpd *pd)
{
	int ret;
	struct rpmhpd *peer = pd->peer;
	unsigned long active_corner, sleep_corner;
	unsigned long this_corner = 0, this_sleep_corner = 0;
	unsigned long peer_corner = 0, peer_sleep_corner = 0;

	to_active_sleep(pd, pd->corner, &this_corner, &this_sleep_corner);

	if (peer && peer->enabled)
		to_active_sleep(peer, peer->corner, &peer_corner,
				&peer_sleep_corner);

	active_corner = max(this_corner, peer_corner);

	ret = rpmhpd_send_corner(pd, RPMH_ACTIVE_ONLY_STATE, active_corner);
	if (ret)
		return ret;

	sleep_corner = max(this_sleep_corner, peer_sleep_corner);

	return rpmhpd_send_corner(pd, RPMH_SLEEP_STATE, sleep_corner);
}

static int rpmhpd_power_on(struct generic_pm_domain *domain)
{
	int ret = 0;
	struct rpmhpd *pd = domain_to_rpmhpd(domain);

	mutex_lock(&rpmhpd_lock);

	pd->enabled = true;

	if (pd->corner)
		ret = rpmhpd_aggregate_corner(pd);

	mutex_unlock(&rpmhpd_lock);

	return ret;
}

static int rpmhpd_power_off(struct generic_pm_domain *domain)
{
	int ret;
	struct rpmhpd *pd = domain_to_rpmhpd(domain);

	mutex_lock(&rpmhpd_lock);

	if (pd->level[0] == 0) {
		ret = rpmhpd_send_corner(pd, RPMH_ACTIVE_ONLY_STATE, 0);
		if (ret)
			return ret;

		ret = rpmhpd_send_corner(pd, RPMH_SLEEP_STATE, 0);
		if (ret)
			return ret;
	}

	pd->enabled = false;

	mutex_unlock(&rpmhpd_lock);

	return 0;
}

static int rpmhpd_set_performance(struct generic_pm_domain *domain,
			     unsigned int state)
{
	int ret = 0;
	struct rpmhpd *pd = domain_to_rpmhpd(domain);

	mutex_lock(&rpmhpd_lock);

	pd->corner = state;

	if (!pd->enabled)
		goto out;

	ret = rpmhpd_aggregate_corner(pd);
out:
	mutex_unlock(&rpmhpd_lock);

	return ret;
}

static unsigned int rpmhpd_get_performance(struct generic_pm_domain *genpd,
					   struct dev_pm_opp *opp)
{
	struct device_node *np;
	unsigned int corner = 0;

	np = dev_pm_opp_get_of_node(opp);
	if (of_property_read_u32(np, "qcom,level", &corner)) {
		pr_err("%s: missing 'qcom,level' property\n", __func__);
		return 0;
	}

	of_node_put(np);

	return corner;
}

static int rpmhpd_update_level_mapping(struct rpmhpd *rpmhpd)
{
	u8 *buf;
	int i, j, len, ret;

	len = cmd_db_read_aux_data_len(rpmhpd->res_name);
	if (len <= 0)
		return len;

	buf = kzalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = cmd_db_read_aux_data(rpmhpd->res_name, buf, len);
	if (ret < 0)
		return ret;

	rpmhpd->level_count = len / RPMH_ARC_LEVEL_SIZE;

	for (i = 0; i < rpmhpd->level_count; i++) {
		for (j = 0; j < RPMH_ARC_LEVEL_SIZE; j++)
			rpmhpd->level[i] |=
				buf[i * RPMH_ARC_LEVEL_SIZE + j] << (8 * j);

		/*
		 * The AUX data may be zero padded.  These 0 valued entries at
		 * the end of the map must be ignored.
		 */
		if (i > 0 && rpmhpd->level[i] == 0) {
			rpmhpd->level_count = i;
			break;
		}
		pr_dbg("%s: ARC hlvl=%2d --> vlvl=%4u\n", rpmhpd->res_name, i,
		       rpmhpd->level[i]);
	}

	kfree(buf);
	return 0;
}

static int rpmhpd_probe(struct platform_device *pdev)
{
	int i, ret;
	size_t num;
	struct genpd_onecell_data *data;
	struct rpmhpd **rpmhpds;
	const struct rpmhpd_desc *desc;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	rpmhpds = desc->rpmhpds;
	num = desc->num_pds;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->domains = devm_kcalloc(&pdev->dev, num, sizeof(*data->domains),
				     GFP_KERNEL);
	data->num_domains = num;

	ret = cmd_db_ready();
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Command DB unavailable, ret=%d\n",
				ret);
		return ret;
	}

	for (i = 0; i < num; i++) {
		if (!rpmhpds[i])
			continue;

		rpmhpds[i]->dev = &pdev->dev;
		rpmhpds[i]->addr = cmd_db_read_addr(rpmhpds[i]->res_name);
		if (!rpmhpds[i]->addr) {
			dev_err(&pdev->dev, "Could not find RPMh address for resource %s\n",
				rpmhpds[i]->res_name);
			return -ENODEV;
		}

		ret = cmd_db_read_slave_id(rpmhpds[i]->res_name);
		if (ret != CMD_DB_HW_ARC) {
			dev_err(&pdev->dev, "RPMh slave ID mismatch\n");
			return -EINVAL;
		}

		ret = rpmhpd_update_level_mapping(rpmhpds[i]);
		if (ret)
			return ret;

		rpmhpds[i]->pd.power_off = rpmhpd_power_off;
		rpmhpds[i]->pd.power_on = rpmhpd_power_on;
		rpmhpds[i]->pd.set_performance_state = rpmhpd_set_performance;
		rpmhpds[i]->pd.opp_to_performance_state = rpmhpd_get_performance;
		pm_genpd_init(&rpmhpds[i]->pd, NULL, true);

		data->domains[i] = &rpmhpds[i]->pd;

		/*
		 * Until we have all consumers voting on corners
		 * just vote the max corner on all PDs
		 * This should ideally be *removed* once we have
		 * all (most) consumers being able to vote
		 */
		rpmhpd_power_on(&rpmhpds[i]->pd);
		rpmhpd_set_performance(&rpmhpds[i]->pd, rpmhpds[i]->level_count - 1);
	}

	return of_genpd_add_provider_onecell(pdev->dev.of_node, data);
}

static int rpmhpd_remove(struct platform_device *pdev)
{
	of_genpd_del_provider(pdev->dev.of_node);
	return 0;
}

static struct platform_driver rpmhpd_driver = {
	.driver = {
		.name = "qcom-rpmhpd",
		.of_match_table = rpmhpd_match_table,
	},
	.probe = rpmhpd_probe,
	.remove = rpmhpd_remove,
};

static int __init rpmhpd_init(void)
{
	return platform_driver_register(&rpmhpd_driver);
}
core_initcall(rpmhpd_init);

static void __exit rpmhpd_exit(void)
{
	platform_driver_unregister(&rpmhpd_driver);
}
module_exit(rpmhpd_exit);

MODULE_DESCRIPTION("Qualcomm RPMh Power Domain Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:qcom-rpmhpd");
