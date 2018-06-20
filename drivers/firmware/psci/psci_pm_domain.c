// SPDX-License-Identifier: GPL-2.0
/*
 * PM domains for CPUs via genpd - managed by PSCI.
 *
 * Copyright (C) 2018 Linaro Ltd.
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "psci.h"

#ifdef CONFIG_PM_GENERIC_DOMAINS_OF
static int psci_pd_power_off(struct generic_pm_domain *pd)
{
	struct genpd_power_state *state = &pd->states[pd->state_idx];
	u32 *pd_state;
	u32 composite_pd_state;

	if (!state->data)
		return 0;

	pd_state = state->data;
	composite_pd_state = *pd_state | psci_get_domain_state();
	psci_set_domain_state(composite_pd_state);

	return 0;
}

static int psci_dt_parse_pd_states(struct genpd_power_state *states,
				   int state_count)
{
	int i, err;
	u32 *psci_states;

	if (!state_count)
		return 0;

	psci_states = kcalloc(state_count, sizeof(psci_states), GFP_KERNEL);
	if (!psci_states)
		return -ENOMEM;

	for (i = 0; i < state_count; i++) {
		err = psci_dt_parse_state_node(to_of_node(states[i].fwnode),
					       &psci_states[i]);
		if (err) {
			kfree(psci_states);
			return err;
		}
	}

	for (i = 0; i < state_count; i++)
		states[i].data = &psci_states[i];

	return 0;
}

static int psci_dt_init_genpd(struct device_node *np,
			      struct genpd_power_state *states,
			      unsigned int state_count)
{
	struct generic_pm_domain *pd;
	struct dev_power_governor *pd_gov;
	int ret = -ENOMEM;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->name = kasprintf(GFP_KERNEL, "%pOF", np);
	if (!pd->name)
		goto free_pd;

	pd->name = kbasename(pd->name);
	pd->power_off = psci_pd_power_off;
	pd->states = states;
	pd->state_count = state_count;
	pd->flags |= GENPD_FLAG_IRQ_SAFE | GENPD_FLAG_CPU_DOMAIN;

	/* Use governor for CPU PM domains if it has some states to manage. */
	pd_gov = state_count > 0 ? &pm_domain_cpu_gov : NULL;

	ret = pm_genpd_init(pd, pd_gov, false);
	if (ret)
		goto free_name;

	ret = of_genpd_add_provider_simple(np, pd);
	if (ret)
		goto remove_pd;

	pr_info("init PM domain %s\n", pd->name);
	return 0;

remove_pd:
	pm_genpd_remove(pd);
free_name:
	kfree(pd->name);
free_pd:
	kfree(pd);
	pr_err("failed to init PM domain ret=%d %pOF\n", ret, np);
	return ret;
}

static int psci_dt_set_genpd_topology(struct device_node *np)
{
	struct device_node *node;
	struct of_phandle_args child, parent;
	int ret;

	for_each_child_of_node(np, node) {
		if (of_parse_phandle_with_args(node, "power-domains",
					       "#power-domain-cells", 0,
					       &parent))
			continue;

		child.np = node;
		child.args_count = 0;

		ret = of_genpd_add_subdomain(&parent, &child);
		of_node_put(parent.np);
		if (ret) {
			of_node_put(node);
			return ret;
		}
	}

	return 0;
}

int psci_dt_init_pm_domains(struct device_node *np)
{
	struct device_node *node;
	struct genpd_power_state *states;
	int state_count;
	int pd_count = 0;
	int ret;

	/* Parse child nodes for "#power-domain-cells". */
	for_each_child_of_node(np, node) {
		if (!of_find_property(node, "#power-domain-cells", NULL))
			continue;

		ret = of_genpd_parse_idle_states(node, &states, &state_count);
		if (ret)
			goto err_put;

		ret = psci_dt_parse_pd_states(states, state_count);
		if (ret)
			goto err_put;

		ret = psci_dt_init_genpd(node, states, state_count);
		if (ret)
			goto err_put;

		pd_count++;
	}

	if (!pd_count)
		return 0;

	ret = psci_dt_set_genpd_topology(np);
	if (ret)
		goto err_msg;

	return pd_count;

err_put:
	of_node_put(node);
err_msg:
	pr_err("failed to create PM domains ret=%d\n", ret);
	return ret;
}
#endif
