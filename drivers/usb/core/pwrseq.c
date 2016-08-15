/*
 * pwrseq.c	USB device power sequence management
 *
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 * Author: Peter Chen <peter.chen@nxp.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/list.h>
#include <linux/of.h>
#include <linux/power/pwrseq.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "hub.h"

struct usb_pwrseq_node {
	struct pwrseq *pwrseq;
	struct list_head list;
};

static int hub_of_pwrseq_on(struct device_node *np, struct usb_hub *hub)
{
	struct pwrseq *pwrseq;
	struct usb_pwrseq_node *pwrseq_node;
	int ret;

	pwrseq = pwrseq_alloc_generic();
	if (IS_ERR(pwrseq))
		return PTR_ERR(pwrseq);

	ret = pwrseq_get(np, pwrseq);
	if (ret)
		goto pwr_free;

	ret = pwrseq_on(np, pwrseq);
	if (ret)
		goto pwr_put;

	pwrseq_node = kzalloc(sizeof(*pwrseq_node), GFP_KERNEL);
	pwrseq_node->pwrseq = pwrseq;
	list_add(&pwrseq_node->list, &hub->pwrseq_on_list);

	return 0;

pwr_put:
	pwrseq_put(pwrseq);
pwr_free:
	pwrseq_free(pwrseq);
	return ret;
}

int hub_pwrseq_on(struct usb_hub *hub)
{
	struct device *parent;
	struct usb_device *hdev = hub->hdev;
	struct device_node *np;
	int ret;

	if (hdev->parent)
		parent = &hdev->dev;
	else
		parent = bus_to_hcd(hdev->bus)->self.controller;

	for_each_child_of_node(parent->of_node, np) {
		ret = hub_of_pwrseq_on(np, hub);
		if (ret)
			return ret;
	}

	return 0;
}

void hub_pwrseq_off(struct usb_hub *hub)
{
	struct pwrseq *pwrseq;
	struct usb_pwrseq_node *pwrseq_node, *tmp_node;

	list_for_each_entry_safe(pwrseq_node, tmp_node,
			&hub->pwrseq_on_list, list) {
		pwrseq = pwrseq_node->pwrseq;
		pwrseq_off(pwrseq);
		pwrseq_put(pwrseq);
		pwrseq_free(pwrseq);
		list_del(&pwrseq_node->list);
		kfree(pwrseq_node);
	}
}
