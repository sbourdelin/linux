// SPDX-License-Identifier: GPL-2.0
/*
 * ASoC Sample Rate Domain Support
 *
 * Copyright (c) 2018 Cirrus Logic, Inc. and
 *                    Cirrus Logic International Semiconductor Ltd.
 *
 * Author: Charles Keepax <ckeepax@opensource.cirrus.com>
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <sound/soc.h>

struct domain_group_peer {
	struct list_head list;
	int link_count;
	struct snd_soc_domain_group *group;
};

static inline void domain_mutex_lock(struct snd_soc_component *component)
{
	mutex_lock(&component->card->domain_mutex);
}

static inline void domain_mutex_unlock(struct snd_soc_component *component)
{
	mutex_unlock(&component->card->domain_mutex);
}

static inline void domain_mutex_assert_held(struct snd_soc_component *component)
{
	lockdep_assert_held(&component->card->domain_mutex);
}

int devm_snd_soc_domain_init(struct snd_soc_component *component)
{
	int i;

	if (!component->driver->num_domains)
		return 0;

	component->num_domains = component->driver->num_domains;
	component->domains = devm_kcalloc(component->card->dev,
					  component->num_domains,
					  sizeof(*component->domains),
					  GFP_KERNEL);
	if (!component->domains)
		return -ENOMEM;

	for (i = 0; i < component->num_domains; i++) {
		component->domains[i].component = component;
		component->domains[i].driver = &component->driver->domains[i];
	}

	return 0;
}
EXPORT_SYMBOL_GPL(devm_snd_soc_domain_init);

struct snd_soc_domain_group *
devm_snd_soc_domain_group_new(struct snd_soc_component *component,
			      const struct snd_soc_domain_group_driver *driver)
{
	struct snd_soc_domain_group *group;

	group = devm_kzalloc(component->card->dev, sizeof(*group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&group->peers);

	group->component = component;
	group->driver = driver;

	return group;
}
EXPORT_SYMBOL_GPL(devm_snd_soc_domain_group_new);

struct snd_soc_domain *snd_soc_domain_get(struct snd_soc_domain_group *group,
					  int index)
{
	int ndomains = group->component->num_domains;

	domain_mutex_assert_held(group->component);

	if (index == SND_SOC_DOMAIN_CURRENT)
		index = group->domain_index;

	if (index < 0 || index >= ndomains)
		return NULL;

	return &group->component->domains[index];
}
EXPORT_SYMBOL_GPL(snd_soc_domain_get);

bool snd_soc_domain_active(struct snd_soc_domain *domain)
{
	bool active;

	domain_mutex_assert_held(domain->component);

	active = !!domain->active_groups;

	return active;
}
EXPORT_SYMBOL_GPL(snd_soc_domain_active);

int snd_soc_domain_get_rate(struct snd_soc_domain *domain)
{
	domain_mutex_assert_held(domain->component);

	return domain->rate;
}
EXPORT_SYMBOL_GPL(snd_soc_domain_get_rate);

int snd_soc_domain_set_rate(struct snd_soc_domain_group *group, int rate)
{
	struct snd_soc_domain *domain;
	int ret = -ENODEV;

	domain_mutex_lock(group->component);

	domain = snd_soc_domain_get(group, SND_SOC_DOMAIN_CURRENT);
	if (domain) {
		domain->rate = rate;
		ret = domain->driver->ops->set_rate(domain, rate);
	}

	domain_mutex_unlock(group->component);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_domain_set_rate);

static struct snd_soc_domain_group *
group_walk(struct snd_soc_domain_group *group, bool local,
	   bool (*cond)(struct snd_soc_domain_group *g, void *c), void *cookie)
{
	struct domain_group_peer *link;
	struct snd_soc_domain_group *target = NULL;

	domain_mutex_assert_held(group->component);

	if (group->walking)
		return NULL;

	dev_vdbg(group->component->dev, "Walking %s\n", group->driver->name);

	if (cond(group, cookie))
		return group;

	group->walking = true;
	list_for_each_entry(link, &group->peers, list) {
		if (!link->group->power)
			continue;

		if (local && link->group->component != group->component)
			continue;

		target = group_walk(link->group, local, cond, cookie);
		if (target)
			break;
	}
	group->walking = false;

	return target;
}

static bool group_mask(struct snd_soc_domain_group *group, void *cookie)
{
	unsigned long *mask = cookie;

	if (group->attach_count)
		*mask &= 1 << group->domain_index;
	else if (group->driver->ops->mask_domains)
		group->driver->ops->mask_domains(group, mask);

	return false;
}

static int group_pick(struct snd_soc_domain_group *group,
				const unsigned long *domain_mask)
{
	int ndomains = group->component->num_domains;
	int i;

	domain_mutex_assert_held(group->component);

	for_each_set_bit(i, domain_mask, ndomains) {
		struct snd_soc_domain *domain = &group->component->domains[i];

		if (!snd_soc_domain_active(domain))
			return i;
	}

	return find_first_bit(domain_mask, ndomains);
}

int snd_soc_domain_attach(struct snd_soc_domain_group *group)
{
	int ret = 0;

	domain_mutex_lock(group->component);

	dev_dbg(group->component->dev, "Attaching domain to %s: %d\n",
		group->driver->name, group->attach_count);

	if (!group->attach_count) {
		const struct snd_soc_domain_group_ops *ops = group->driver->ops;
		unsigned long dom_map = ~0UL;
		struct snd_soc_domain *domain;

		group_walk(group, true, group_mask, &dom_map);

		if (ops->pick_domain)
			group->domain_index = ops->pick_domain(group, &dom_map);
		else
			group->domain_index = group_pick(group, &dom_map);

		domain = snd_soc_domain_get(group, SND_SOC_DOMAIN_CURRENT);
		if (!domain) {
			dev_err(group->component->dev,
				"No suitable domain to attach for %s\n",
				group->driver->name);
			ret = -ENODEV;
			goto error;
		}

		dev_dbg(group->component->dev, "Apply domain %s to %s\n",
			domain->driver->name, group->driver->name);

		ret = ops->set_domain(group, group->domain_index);
		if (ret)
			goto error;

		domain->active_groups++;
	}

	group->attach_count++;

error:
	domain_mutex_unlock(group->component);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_domain_attach);

int snd_soc_domain_detach(struct snd_soc_domain_group *group)
{
	int ret = 0;

	domain_mutex_lock(group->component);

	dev_dbg(group->component->dev, "Detaching domain from %s: %d\n",
		group->driver->name, group->attach_count);

	if (!group->attach_count) {
		dev_err(group->component->dev, "Unbalanced detach on %s\n",
			group->driver->name);
		ret = -EPERM;
	} else {
		struct snd_soc_domain *domain;

		domain = snd_soc_domain_get(group, SND_SOC_DOMAIN_CURRENT);
		if (!domain) {
			dev_err(group->component->dev,
				"Group %s has missing domain\n",
				group->driver->name);
			ret = -ENODEV;
			goto error;
		}

		domain->active_groups--;
		group->attach_count--;
	}

error:
	domain_mutex_unlock(group->component);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_domain_detach);

int snd_soc_domain_event(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol,
			 int event)
{
	switch (event) {
	case SND_SOC_DAPM_WILL_PMU:
		w->dgroup->power = true;
		return 0;
	case SND_SOC_DAPM_PRE_PMU:
		return snd_soc_domain_attach(w->dgroup);
	case SND_SOC_DAPM_POST_PMD:
		w->dgroup->power = false;
		return snd_soc_domain_detach(w->dgroup);
	default:
		return 0;
	}
}
EXPORT_SYMBOL_GPL(snd_soc_domain_event);

static struct domain_group_peer *
group_peer_find(struct snd_soc_domain_group *group,
		struct snd_soc_domain_group *peer)
{
	struct domain_group_peer *link;

	domain_mutex_assert_held(group->component);

	list_for_each_entry(link, &group->peers, list) {
		if (link->group == peer)
			return link;
	}

	return NULL;
}

static int group_peer_new(struct snd_soc_domain_group *group,
			  struct snd_soc_domain_group *peer)
{
	struct domain_group_peer *link;

	domain_mutex_lock(group->component);

	link = group_peer_find(group, peer);
	if (!link) {
		dev_dbg(group->component->dev, "New peer: %s -> %s\n",
			group->driver->name, peer->driver->name);

		link = kzalloc(sizeof(*link), GFP_KERNEL);
		if (!link)
			return -ENOMEM;

		INIT_LIST_HEAD(&link->list);
		link->group = peer;

		list_add_tail(&link->list, &group->peers);
	}

	link->link_count++;

	domain_mutex_unlock(group->component);

	return 0;
}

static int group_peer_delete(struct snd_soc_domain_group *group,
			     struct snd_soc_domain_group *peer)
{
	struct domain_group_peer *link;
	int ret = 0;

	domain_mutex_lock(group->component);

	link = group_peer_find(group, peer);
	if (!link) {
		dev_err(group->component->dev,
			"Delete on invalid peer: %s -> %s\n",
			group->driver->name, peer->driver->name);
		ret = -ENOENT;
		goto error;
	}

	link->link_count--;
	if (!link->link_count) {
		dev_dbg(group->component->dev, "Delete peer: %s -> %s\n",
			group->driver->name, peer->driver->name);

		list_del(&link->list);
		kfree(link);
	}

error:
	domain_mutex_unlock(group->component);

	return ret;
}

int snd_soc_domain_connect_widgets(struct snd_soc_dapm_widget *a,
				   struct snd_soc_dapm_widget *b,
				   bool connect)
{
	int (*op)(struct snd_soc_domain_group *group,
		  struct snd_soc_domain_group *peer);
	int ret;

	if (!a->dgroup || !b->dgroup)
		return 0;

	dev_dbg(a->dapm->dev, "%s %s,%s - %s,%s\n",
		connect ? "Connecting" : "Disconnecting",
		a->name, a->dgroup->driver->name,
		b->name, b->dgroup->driver->name);

	if (connect)
		op = group_peer_new;
	else
		op = group_peer_delete;

	ret = op(a->dgroup, b->dgroup);
	if (ret)
		return ret;

	ret = op(b->dgroup, a->dgroup);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_domain_connect_widgets);
