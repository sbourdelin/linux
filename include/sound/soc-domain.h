/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ASoC Sample Rate Domain Support
 *
 * Copyright (c) 2018 Cirrus Logic, Inc. and
 *                    Cirrus Logic International Semiconductor Ltd.
 *
 * Author: Charles Keepax <ckeepax@opensource.cirrus.com>
 */

#ifndef LINUX_SND_SOC_DOMAIN_H
#define LINUX_SND_SOC_DOMAIN_H

#define SND_SOC_DOMAIN_CURRENT -1

struct snd_soc_domain;
struct snd_soc_domain_group;

struct snd_soc_domain_ops {
	int (*set_rate)(struct snd_soc_domain *domain, int rate);
	int (*get_rate)(struct snd_soc_domain *domain);
};

struct snd_soc_domain_driver {
	const char * const name;

	const struct snd_soc_domain_ops *ops;

	void *private_data;
};

struct snd_soc_domain {
	const struct snd_soc_domain_driver *driver;
	struct snd_soc_component *component;

	/* TODO: Probably should be a snd_pcm_hw_params */
	int rate;

	int active_groups;
};

struct snd_soc_domain_group_ops {
	int (*set_domain)(struct snd_soc_domain_group *group, int dom);

	int (*mask_domains)(struct snd_soc_domain_group *group,
			    unsigned long *domain_mask);
	/* optional */
	int (*pick_domain)(struct snd_soc_domain_group *group,
			   const unsigned long *domain_mask);
};

struct snd_soc_domain_group_driver {
	const char * const name;

	const struct snd_soc_domain_group_ops *ops;

	void *private_data;
};

struct snd_soc_domain_group {
	const struct snd_soc_domain_group_driver *driver;
	struct snd_soc_component *component;

	int domain_index;
	int attach_count;

	struct list_head peers;

	unsigned int walking:1;
	unsigned int power:1;
};

int devm_snd_soc_domain_init(struct snd_soc_component *component);

struct snd_soc_domain_group *
devm_snd_soc_domain_group_new(struct snd_soc_component *component,
			      const struct snd_soc_domain_group_driver *drv);

struct snd_soc_domain *snd_soc_domain_get(struct snd_soc_domain_group *group,
					  int index);
bool snd_soc_domain_active(struct snd_soc_domain *domain);
int snd_soc_domain_get_rate(struct snd_soc_domain *domain);

int snd_soc_domain_set_rate(struct snd_soc_domain_group *group, int rate);

/* TODO: API to force a particular domain onto a group? */
int snd_soc_domain_attach(struct snd_soc_domain_group *group);
int snd_soc_domain_detach(struct snd_soc_domain_group *group);

int snd_soc_domain_event(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol,
			 int event);

int snd_soc_domain_connect_widgets(struct snd_soc_dapm_widget *a,
				   struct snd_soc_dapm_widget *b,
				   bool connect);

#endif
