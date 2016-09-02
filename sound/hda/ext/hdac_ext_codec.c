/*
 *  hdac_codec.c - HDA codec library
 *
 *  Copyright (C) 2016 Intel Corp
 *  Author: Subhransu S. Prusty <subhransu.s.prusty@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <sound/soc.h>
#include <sound/hdaudio_ext.h>
#include "../../hda/local.h"
#include "hdac_ext_codec.h"
#include <sound/hda_verbs.h>

static int hdac_generic_query_connlist(struct hdac_device *hdac,
				struct hdac_ext_codec_widget *wid)
{
	hda_nid_t mux_nids[HDA_MAX_CONNECTIONS];
	unsigned int caps;
	int i;

	if (!(get_wcaps(hdac, wid->nid) & AC_WCAP_CONN_LIST)) {
		dev_dbg(&hdac->dev,
			"HDAC: wid %d wcaps %#x doesn't support connection list\n",
			wid->nid, get_wcaps(hdac, wid->nid));

		return 0;
	}

	wid->num_inputs = snd_hdac_get_connections(hdac, wid->nid,
					mux_nids, HDA_MAX_CONNECTIONS);

	if (wid->num_inputs == 0) {
		dev_warn(&hdac->dev, "No connections found for wid: %d\n",
							wid->nid);
		return 0;
	}

	for (i = 0; i < wid->num_inputs; i++) {
		wid->conn_list[i].nid = mux_nids[i];
		caps = get_wcaps(hdac, mux_nids[i]);
		wid->conn_list[i].type = get_wcaps_type(caps);
	}

	dev_dbg(&hdac->dev, "num_inputs %d for wid: %d\n",
			wid->num_inputs, wid->nid);

	return wid->num_inputs;
}

static int hdac_codec_add_widget(struct hdac_ext_device *edev, hda_nid_t nid,
					unsigned int type, unsigned int caps)
{
	struct hdac_device *codec = &edev->hdac;
	struct hdac_ext_codec_widget *widget;
	unsigned int *cfg;

	widget = kzalloc(sizeof(*widget), GFP_KERNEL);
	if (!widget)
		return -ENOMEM;

	widget->nid = nid;
	widget->type = type;
	widget->caps = caps;
	list_add_tail(&widget->head, &edev->widget_list);

	if (type == AC_WID_PIN) {
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg)
			return -ENOMEM;

		*cfg = snd_hdac_codec_read(codec, nid, 0,
					AC_VERB_GET_CONFIG_DEFAULT, 0);
		widget->params = cfg;
	}

	return hdac_generic_query_connlist(codec, widget);
}

int snd_hdac_ext_parse_widgets(struct hdac_ext_device *edev)
{
	struct hdac_device *hdac = &edev->hdac;
	hda_nid_t nid;
	int num_nodes, i;
	struct hdac_ext_codec_widget *wid;
	struct hdac_ext_codec_widget *tmp;
	int ret = 0;

	num_nodes = snd_hdac_get_sub_nodes(hdac, hdac->afg, &nid);
	if (!nid || num_nodes <= 0) {
		dev_err(&hdac->dev, "HDAC: failed to get afg sub nodes\n");
		return -EINVAL;
	}
	hdac->num_nodes = num_nodes;
	hdac->start_nid = nid;

	for (i = 0; i < hdac->num_nodes; i++, nid++) {
		unsigned int caps;
		unsigned int type;

		caps = get_wcaps(hdac, nid);
		type = get_wcaps_type(caps);

		ret = hdac_codec_add_widget(edev, nid, type, caps);
		if (ret < 0)
			goto fail_add_widget;

	}

	hdac->end_nid = nid;

	/* Cache input connection to a widget */
	list_for_each_entry(wid, &edev->widget_list, head) {
		if (!wid->num_inputs)
			continue;

		for (i = 0; i < wid->num_inputs; i++) {
			list_for_each_entry(tmp, &edev->widget_list, head) {
				if (wid->conn_list[i].nid == tmp->nid) {
					wid->conn_list[i].input_w = tmp;
					break;
				}
			}
		}
	}

	return 0;

fail_add_widget:
	snd_hdac_ext_codec_cleanup(edev);
	return ret;
}

void snd_hdac_ext_codec_cleanup(struct hdac_ext_device *edev)
{
	struct hdac_ext_codec_widget *wid, *tmp;

	list_for_each_entry_safe(wid, tmp, &edev->widget_list, head) {
		kfree(wid->params);
		list_del(&wid->head);
		kfree(wid);
	}
}
