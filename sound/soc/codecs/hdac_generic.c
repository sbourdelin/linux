/*
 *  hdac_generic.c - ASoc HDA generic codec driver
 *
 *  Copyright (C) 2016-2017 Intel Corp
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
#include <linux/kernel.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/hdaudio_ext.h>
#include <sound/hda_regmap.h>
#include "../../hda/local.h"
#include "../../hda/ext/hdac_codec.h"
#include "hdac_generic.h"

#define HDA_MAX_CVTS	10

struct hdac_generic_dai_map {
	struct hdac_codec_widget *cvt;
};

struct hdac_generic_priv {
	struct hdac_generic_dai_map dai_map[HDA_MAX_CVTS];
	unsigned int num_pins;
	unsigned int num_adcs;
	unsigned int num_dacs;
	unsigned int num_dapm_widgets;
};

static char *wid_names[] = {
		"dac", "adc", "mixer", "mux", "pin", "power",
		"volme knob", "beep", NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, "vendor",
};

struct route_map {
	struct list_head head;
	const char *sink;
	char *control;
	const char *src;
};

struct widget_node_entries {
	struct hdac_codec_widget *wid;
	struct snd_soc_dapm_widget *w;
	int num_nodes;
};

static inline struct hdac_ext_device *to_hda_ext_device(struct device *dev)
{
	struct hdac_device *hdac = dev_to_hdac_dev(dev);

	return to_ehdac_device(hdac);
}

static void hdac_generic_set_power_state(struct hdac_ext_device *edev,
			hda_nid_t nid, unsigned int pwr_state)
{
	/* TODO: check D0sup bit before setting this */
	if (!snd_hdac_check_power_state(&edev->hdac, nid, pwr_state))
		snd_hdac_codec_write(&edev->hdac, nid, 0,
				AC_VERB_SET_POWER_STATE, pwr_state);
}

static void hdac_generic_set_eapd(struct hdac_ext_device *edev,
			hda_nid_t nid, bool enable)
{
	u32 pin_caps = snd_hdac_read_parm_uncached(&edev->hdac,
					nid, AC_PAR_PIN_CAP);

	if (pin_caps & AC_PINCAP_EAPD)
		snd_hdac_codec_write(&edev->hdac, nid, 0,
				AC_VERB_SET_EAPD_BTLENABLE, enable ? 2 : 0);
}

static int hdac_generic_pin_io_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kc, int event)
{
	struct hdac_ext_device *edev = to_hda_ext_device(w->dapm->dev);
	struct hdac_codec_widget *wid = w->priv;
	int val;

	dev_dbg(&edev->hdac.dev, "%s: widget: %s event: %x\n",
			__func__, w->name, event);

	val = snd_hdac_codec_read(&edev->hdac, wid->nid, 0,
			AC_VERB_GET_PIN_WIDGET_CONTROL, 0);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (w->id == snd_soc_dapm_output) {
			/* TODO: program using a eapd widget */
			hdac_generic_set_eapd(edev, wid->nid, true);
			val |= AC_PINCTL_OUT_EN;
		} else {

			/* TODO: program vref using a mixer control */
			val |= AC_PINCTL_VREF_80;
			val |= AC_PINCTL_IN_EN;
		}

		break;

	case SND_SOC_DAPM_POST_PMD:
		if (w->id == snd_soc_dapm_output) {
			/* TODO: program using a eapd widget */
			hdac_generic_set_eapd(edev, wid->nid, false);
			val &= ~AC_PINCTL_OUT_EN;
		} else {
			val &= AC_PINCTL_VREF_HIZ;
			val &= ~AC_PINCTL_IN_EN;
		}

		break;

	default:
		dev_warn(&edev->hdac.dev, "Event %d not handled\n", event);
		return 0;
	}

	snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
			AC_VERB_SET_PIN_WIDGET_CONTROL, val);

	return 0;
}

static int hdac_generic_pin_mux_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kc, int event)
{
	struct hdac_ext_device *edev = to_hda_ext_device(w->dapm->dev);
	struct hdac_codec_widget *wid = w->priv;
	int mux_idx;

	dev_dbg(&edev->hdac.dev, "%s: widget: %s event: %x\n",
			__func__, w->name, event);
	if (!kc)
		kc  = w->kcontrols[0];

	mux_idx = dapm_kcontrol_get_value(kc);
	if (mux_idx > 0) {
		snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
			AC_VERB_SET_CONNECT_SEL, (mux_idx - 1));
	}

	return 0;
}

static int hdac_generic_pin_pga_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kc, int event)
{
	struct hdac_ext_device *edev = to_hda_ext_device(w->dapm->dev);
	struct hdac_codec_widget *wid = w->priv;

	dev_dbg(&edev->hdac.dev, "%s: widget: %s event: %x\n",
			__func__, w->name, event);

	if (event == SND_SOC_DAPM_POST_PMD) {
		hdac_generic_set_power_state(edev, wid->nid, AC_PWRST_D3);
		snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE);
	} else {
		hdac_generic_set_power_state(edev, wid->nid, AC_PWRST_D0);
		snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);
	}

	return 0;
}

static int hdac_generic_widget_power_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kc, int event)
{
	struct hdac_ext_device *edev = to_hda_ext_device(w->dapm->dev);
	struct hdac_codec_widget *wid = w->priv;

	dev_dbg(&edev->hdac.dev, "%s: widget: %s event: %x\n",
			__func__, w->name, event);
	hdac_generic_set_power_state(edev, wid->nid,
		(event == SND_SOC_DAPM_POST_PMD ? AC_PWRST_D3:AC_PWRST_D0));

	return 0;
}

static int hdac_generic_cvt_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kc, int event)
{
	struct hdac_ext_device *edev = to_hda_ext_device(w->dapm->dev);
	struct hdac_codec_widget *wid = w->priv;

	hdac_generic_widget_power_event(w, kc, event);

	if (event == SND_SOC_DAPM_POST_PMD)
		snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE,
				AMP_IN_MUTE(0));
	else
		snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE,
				AMP_IN_UNMUTE(0) | 0x5b);

	return 0;
}

static int get_mixer_control_index(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kc)
{
	int i;

	for (i = 0; i < w->num_kcontrols; i++) {
		if (w->kcontrols[i] == kc)
			return i;
	}

	return -EINVAL;
}

static int hdac_generic_mixer_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kc, int event)
{
	struct hdac_ext_device *edev = to_hda_ext_device(w->dapm->dev);
	struct hdac_codec_widget *wid = w->priv;
	bool no_input = true;
	int i;

	dev_dbg(&edev->hdac.dev, "%s: widget: %s event: %x\n",
			__func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		hdac_generic_set_power_state(edev, wid->nid, AC_PWRST_D0);

		/* TODO: Check capability and program amp */
		snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);

		for (i = 0; i < w->num_kcontrols; i++) {
			if (dapm_kcontrol_get_value(w->kcontrols[i])) {
				snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
					AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(i));
			}
		}

		return 0;

	case SND_SOC_DAPM_POST_PMD:
		/* TODO: Check capability and program amp */
		snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE);

		for (i = 0; i < w->num_kcontrols; i++) {
			if (dapm_kcontrol_get_value(w->kcontrols[i])) {
				snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
					AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(i));
			}
		}

		hdac_generic_set_power_state(edev, wid->nid, AC_PWRST_D3);

		return 0;

	case SND_SOC_DAPM_POST_REG:
		i = get_mixer_control_index(w, kc);
		if (i < 0) {
			dev_err(&edev->hdac.dev,
					"%s: Wrong kcontrol event: %s\n",
					__func__, kc->id.name);
			return i;
		}
		if (dapm_kcontrol_get_value(kc)) {
			snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(i));
			no_input = false;
		} else {
			snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(i));
		}

		if (no_input)
			snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE);

		return 0;

	default:
		dev_warn(&edev->hdac.dev, "Event %d not handled\n", event);
		return 0;
	}

	return 0;
}

/* TODO: Check capability and program amp */
static void update_mux_amp_switch(struct hdac_ext_device *edev,
		hda_nid_t nid, struct snd_kcontrol *kc, bool enable)
{
	struct soc_enum *e = (struct soc_enum *)kc->private_value;
	int mux_idx, i;

	mux_idx = dapm_kcontrol_get_value(kc);

	if (!enable || !mux_idx) {
		for (i = 1; i < (e->items - 1); i++)
			snd_hdac_codec_write(&edev->hdac, nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(i -1));

		snd_hdac_codec_write(&edev->hdac, nid, 0,
			AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE);
	} else {
		for (i = 1; i < (e->items - 1); i++) {
			if (i == mux_idx)
				snd_hdac_codec_write(&edev->hdac, nid, 0,
					AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(i - 1));
			else
				snd_hdac_codec_write(&edev->hdac, nid, 0,
					AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(i -1));
		}

		snd_hdac_codec_write(&edev->hdac, nid, 0,
			AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);
	}
}

static int hdac_generic_selector_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kc, int event)
{
	struct hdac_ext_device *edev = to_hda_ext_device(w->dapm->dev);
	struct hdac_codec_widget *wid = w->priv;

	dev_dbg(&edev->hdac.dev, "%s: widget: %s event: %x\n",
			__func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		hdac_generic_set_power_state(edev, wid->nid, AC_PWRST_D0);
		snd_hdac_codec_write(&edev->hdac, wid->nid, 0,
				AC_VERB_SET_CONNECT_SEL,
				(dapm_kcontrol_get_value(w->kcontrols[0]) - 1));
		update_mux_amp_switch(edev, wid->nid, w->kcontrols[0], true);

		return 0;

	case SND_SOC_DAPM_POST_REG:
		update_mux_amp_switch(edev, wid->nid, kc, true);

		return 0;

	case SND_SOC_DAPM_POST_PMD:
		update_mux_amp_switch(edev, wid->nid, w->kcontrols[0], false);
		hdac_generic_set_power_state(edev, wid->nid, AC_PWRST_D3);

		return 0;

	default:
		dev_warn(&edev->hdac.dev, "Event %d not handled\n", event);
		return 0;
	}

	return 0;
}

static bool is_duplicate_route(struct list_head *route_list,
		const char *sink, const char *control, const char *src)
{
	struct route_map *map;

	list_for_each_entry(map, route_list, head) {

		if (strcmp(src, map->src))
			continue;
		if (strcmp(sink, map->sink))
			continue;
		if (!control && !map->control)
			return true;
		if ((control && map->control) &&
				!strcmp(control, map->control))
			return true;
	}

	return false;
}

static int hdac_generic_add_route(struct snd_soc_dapm_context *dapm,
		const char *sink, const char *control, const char *src,
		struct list_head *route_list)
{
	struct snd_soc_dapm_route route;
	struct route_map *map;

	/*
	 * During parsing a loop can happen from input pin to output pin.
	 * An input pin is represented with pga and input dapm widgets.
	 * There is possibility of duplicate route between these two pga and
	 * input widgets as the input can appear for multiple output pins or
	 * adcs during connection list query.
	 */
	if (is_duplicate_route(route_list, sink, control, src))
		return 0;

	route.sink = sink;
	route.source = src;
	route.control = control;
	route.connected = NULL;

	snd_soc_dapm_add_route_single(dapm, &route);

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;


	map->sink = sink;
	map->src = src;
	if (control) {
		map->control =
			kzalloc(sizeof(char) * SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
								GFP_KERNEL);
		if (!map->control)
			return -ENOMEM;

		strcpy(map->control, control);
	}

	list_add_tail(&map->head, route_list);

	return 0;
}

/* Returns the only dapm widget which can be connected to other hda widgets */
static struct snd_soc_dapm_widget *hda_widget_to_dapm_widget(
			struct hdac_ext_device *edev,
			struct hdac_codec_widget *wid)
{
	struct snd_soc_dapm_widget **wid_ref;

	switch (wid->type) {
	case AC_WID_PIN:
		wid_ref = wid->priv;

		if (is_input_pin(&edev->hdac, wid->nid))
			return wid_ref[1];

		if (wid->num_inputs == 1)
			return wid_ref[1];

		return wid_ref[2];

	case AC_WID_BEEP:
		wid_ref = wid->priv;

		return wid_ref[1];

	case AC_WID_AUD_OUT:
	case AC_WID_AUD_IN:
	case AC_WID_AUD_MIX:
	case AC_WID_AUD_SEL:
	case AC_WID_POWER:
		return wid->priv;

	default:
		dev_info(&edev->hdac.dev, "Widget type %d not handled\n", wid->type);
		return NULL;
	}

	return NULL;
}

static void fill_pinout_next_wid_entry(struct hdac_ext_device *edev,
			struct widget_node_entries *next,
			struct widget_node_entries *wid_entry,
			const char **control, int index)
{
	struct snd_soc_dapm_widget **wid_ref = wid_entry->wid->priv;
	const struct snd_kcontrol_new *kc;
	struct soc_enum *se;

	switch (wid_entry->w->id) {
	case snd_soc_dapm_output:
		next->w = wid_ref[1];
		next->num_nodes = 1;
		next->wid = wid_entry->wid;

		break;
	case snd_soc_dapm_pga:
		if (wid_entry->wid->num_inputs == 1) {
			next->wid = wid_entry->wid->conn_list[index].input_w;
			next->w = hda_widget_to_dapm_widget(
					edev, next->wid);
			next->num_nodes = next->wid->num_inputs;
		} else {
			next->wid = wid_entry->wid;
			next->w = wid_ref[2];
			next->num_nodes = wid_entry->wid->num_inputs;
		}

		break;

	case snd_soc_dapm_mux:
		kc = wid_entry->w->kcontrol_news;
		se = (struct soc_enum *)kc->private_value;

		next->wid = wid_entry->wid->conn_list[index].input_w;
		next->num_nodes = next->wid->num_inputs;
		next->w = hda_widget_to_dapm_widget(edev, next->wid);

		*control = se->texts[index + 1];

		break;
	default:
		dev_warn(&edev->hdac.dev, "widget nid: %d id: %d not handled\n",
				wid_entry->wid->nid, wid_entry->w->id);
		break;
	}
}

static int parse_node_and_add_route(struct snd_soc_dapm_context *dapm,
				struct widget_node_entries *wid_entry,
				struct list_head *route_list)
{
	struct hdac_ext_device *edev = to_hda_ext_device(dapm->dev);
	int i, ret;
	struct widget_node_entries next;
	const char *control = NULL;

	if (!wid_entry->num_nodes)
		return 0;

	if ((wid_entry->w->id == snd_soc_dapm_dac) ||
			 (wid_entry->w->id == snd_soc_dapm_input) ||
			 (wid_entry->w->id == snd_soc_dapm_siggen)) {

		return 0;
	}

	for (i = 0; i < wid_entry->num_nodes; i++) {

		if (wid_entry->wid->type == AC_WID_PIN) {
			control = NULL;

			if (is_input_pin(&edev->hdac, wid_entry->wid->nid)) {

				struct snd_soc_dapm_widget **wid_ref =
							wid_entry->wid->priv;

				if (wid_entry->w->id == snd_soc_dapm_pga) {
					next.w = wid_ref[0];
					next.num_nodes = 1;
					next.wid = wid_entry->wid;
				}
			} else { /* if output pin */
				fill_pinout_next_wid_entry(edev, &next,
						wid_entry, &control, i);
			}
		} else {
			struct snd_soc_dapm_widget *w = wid_entry->wid->priv;
			const struct snd_kcontrol_new *kc;
			struct soc_enum *se;

			next.wid = wid_entry->wid->conn_list[i].input_w;
			next.w = hda_widget_to_dapm_widget(edev, next.wid);
			if (next.wid->type == AC_WID_PIN &&
					is_input_pin(&edev->hdac, next.wid->nid)) 
				next.num_nodes = 1;
			else
				next.num_nodes = next.wid->num_inputs;

			switch (w->id) {
			case snd_soc_dapm_mux:
				kc = &w->kcontrol_news[0];
				se = (struct soc_enum *)kc->private_value;
				control = se->texts[i + 1];

				break;

			case snd_soc_dapm_mixer:
				kc = &w->kcontrol_news[i];
				control = kc->name;

				break;
			default:
				break;
			}
		}

		ret = hdac_generic_add_route(dapm, wid_entry->w->name,
					control, next.w->name, route_list);
		if (ret < 0)
			return ret;

		ret = parse_node_and_add_route(dapm, &next, route_list);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * Example graph connection from a output PIN to a DAC:
 * DAC1->
 *         Mixer 1 ------->
 * DAC2->                   Virtual Mux -> PIN PGA -> OUTPUT PIN
 *                       ->
 * LOUT1 ----------------|
 *
 * Widget connection map can be created by querying the connection list for
 * each widget. The parsing can happen from two endpoints:
 * 1) PIN widget 2) ADC widget.
 *
 * This goes through both pin list and adc list and builds the graph.
 */

static int hdac_generic_add_route_to_list(struct snd_soc_dapm_context *dapm,
				struct snd_soc_dapm_widget *widgets)
{
	struct hdac_ext_device *edev = to_hda_ext_device(dapm->dev);
	struct hdac_codec_widget *wid;
	struct snd_soc_dapm_widget **wid_ref;
	struct widget_node_entries wid_entry;
	struct list_head route_list;
	struct route_map *map, *tmp;
	int ret =  0;

	/*
	 * manage the routes through a temp list to identify duplicate
	 * routes from being added.
	 */
	INIT_LIST_HEAD(&route_list);
	list_for_each_entry(wid, &edev->hdac.widget_list, head) {
		if ((wid->type != AC_WID_PIN) && (wid->type != AC_WID_AUD_IN))
			continue;
		/*
		 * input capable pins don't have a connection list, so skip
		 * them.
		 */
		if ((wid->type == AC_WID_PIN) &&
				(is_input_pin(&edev->hdac, wid->nid)))
			continue;

		if (wid->type == AC_WID_PIN) {
			wid_ref = wid->priv;

			wid_entry.wid = wid;
			wid_entry.num_nodes = 1;
			wid_entry.w = wid_ref[0];
		} else {
			wid_entry.wid = wid;
			wid_entry.num_nodes = wid->num_inputs;
			wid_entry.w = wid->priv;
		}

		ret = parse_node_and_add_route(dapm, &wid_entry, &route_list);
		if (ret < 0)
			goto fail;
	}

fail:
	list_for_each_entry_safe(map, tmp, &route_list, head) {
		kfree(map->control);
		list_del(&map->head);
		kfree(map);
	}

	return ret;
}

static int hdac_generic_fill_widget_info(struct device *dev,
		struct snd_soc_dapm_widget *w, enum snd_soc_dapm_type id,
		void *priv, const char *wname, const char *stream,
		struct snd_kcontrol_new *wc, int numkc,
		int (*event)(struct snd_soc_dapm_widget *, struct snd_kcontrol *, int),
		unsigned short event_flags)
{
	w->id = id;
	w->name = devm_kstrdup(dev, wname, GFP_KERNEL);
	if (!w->name)
		return -ENOMEM;

	w->sname = stream;
	w->reg = SND_SOC_NOPM;
	w->shift = 0;
	w->kcontrol_news = wc;
	w->num_kcontrols = numkc;
	w->priv = priv;
	w->event = event;
	w->event_flags = event_flags;

	return 0;
}

static int hdac_generic_alloc_mux_widget(struct snd_soc_dapm_context *dapm,
		struct snd_soc_dapm_widget *widgets, int index,
		struct hdac_codec_widget *wid)

{
	struct snd_kcontrol_new *kc;
	struct soc_enum *se;
	char kc_name[HDAC_GENERIC_NAME_SIZE];
	char mux_items[HDAC_GENERIC_NAME_SIZE];
	char widget_name[HDAC_GENERIC_NAME_SIZE];
	const char *name;
	/* To hold inputs to the Pin mux */
	char *items[HDA_MAX_CONNECTIONS];
	int i = 0, ret;
	int num_items = wid->num_inputs + 1;

	if (wid->type == AC_WID_AUD_SEL)
		sprintf(widget_name, "Mux %x", wid->nid);
	else if (wid->type == AC_WID_PIN)
		sprintf(widget_name, "Pin %x Mux", wid->nid);
	else
		return -EINVAL;

	kc = devm_kzalloc(dapm->dev, sizeof(*kc), GFP_KERNEL);
	if (!kc)
		return -ENOMEM;

	se = devm_kzalloc(dapm->dev, sizeof(*se), GFP_KERNEL);
	if (!se)
		return -ENOMEM;

	sprintf(kc_name, "Mux %d Input", wid->nid);
	kc->name = devm_kstrdup(dapm->dev, kc_name, GFP_KERNEL);
	if (!kc->name)
		return -ENOMEM;

	kc->private_value = (long)se;
	kc->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	kc->access = 0;
	kc->info = snd_soc_info_enum_double;
	kc->put = snd_soc_dapm_put_enum_double;
	kc->get = snd_soc_dapm_get_enum_double;

	se->reg = SND_SOC_NOPM;

	se->items = num_items;
	se->mask = roundup_pow_of_two(se->items) - 1;

	sprintf(mux_items, "NONE");
	items[i] = devm_kstrdup(dapm->dev, mux_items, GFP_KERNEL);
	if (!items[i])
		return -ENOMEM;

	for (i = 0; i < wid->num_inputs; i++)	{
		name = wid_names[wid->conn_list[i].type];
		if (!name)
			return -EINVAL;

		sprintf(mux_items, "%s %x", name, wid->conn_list[i].nid);
		items[i + 1] = devm_kstrdup(dapm->dev, mux_items, GFP_KERNEL);
		if (!items[i])
			return -ENOMEM;
	}

	se->texts = devm_kmemdup(dapm->dev, items,
			(num_items  * sizeof(char *)), GFP_KERNEL);
	if (!se->texts)
		return -ENOMEM;

	ret = hdac_generic_fill_widget_info(dapm->dev, &widgets[index],
			snd_soc_dapm_mux, wid, widget_name, NULL, kc, 1,
			hdac_generic_selector_event, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMD | SND_SOC_DAPM_POST_REG);

	if (ret < 0)
		return ret;

	wid->priv = &widgets[index];

	return 0;
}

static const char *get_dai_stream(struct snd_soc_dai_driver *dai_drv,
			int num_dais, struct hdac_codec_widget *wid)
{
	int i;
	struct hdac_codec_widget *tmp;

	for (i = 0; i < num_dais; i++) {
		tmp = dai_drv[i].dobj.private;
		if (tmp->nid == wid->nid) {
			if (wid->type == AC_WID_AUD_IN)
				return dai_drv[i].capture.stream_name;
			else
				return dai_drv[i].playback.stream_name;
		}
	}

	return NULL;
}

static int hdac_codec_alloc_cvt_widget(struct snd_soc_dapm_context *dapm,
			struct snd_soc_dapm_widget *widgets, int index,
			struct hdac_codec_widget *wid)
{
	struct snd_soc_dai_driver *dai_drv = dapm->component->dai_drv;
	char widget_name[HDAC_GENERIC_NAME_SIZE];
	const char *dai_strm_name;
	int ret = 0;

	dai_strm_name = get_dai_stream(dai_drv,
				dapm->component->num_dai, wid);
	if (!dai_strm_name)
		return -EINVAL;

	if (wid->type == AC_WID_AUD_IN) {
		sprintf(widget_name, "ADC %x", wid->nid);
	} else {
		sprintf(widget_name, "%s DAC %x",
			(wid->caps & AC_WCAP_DIGITAL) ? "Digital" : "Analog",
			wid->nid);
	}

	ret = hdac_generic_fill_widget_info(dapm->dev, &widgets[index],
			wid->type == AC_WID_AUD_IN ?
			snd_soc_dapm_aif_in : snd_soc_dapm_aif_out,
			wid, widget_name, dai_strm_name, NULL, 0,
			hdac_generic_cvt_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD);
	if (ret < 0)
		return ret;

	wid->priv = &widgets[index];

	return 0;
}

static int hdac_codec_alloc_mixer_widget(struct snd_soc_dapm_context *dapm,
				struct snd_soc_dapm_widget *w, int index,
				struct hdac_codec_widget *wid)
{
	struct snd_kcontrol_new *kc;
	struct soc_mixer_control *mc;
	char kc_name[HDAC_GENERIC_NAME_SIZE];
	char widget_name[HDAC_GENERIC_NAME_SIZE];
	const char *name;
	int i, ret;

	kc = devm_kzalloc(dapm->dev,
			(sizeof(*kc) * wid->num_inputs),
			GFP_KERNEL);
	if (!kc)
		return -ENOMEM;

	for (i = 0; i < wid->num_inputs; i++) {
		name = wid_names[wid->conn_list[i].type];
		if (!name)
			return -EINVAL;

		sprintf(kc_name, "%s %x in Switch",
				name, wid->conn_list[i].nid);
		kc[i].name = devm_kstrdup(dapm->dev, kc_name, GFP_KERNEL);
		if (!kc[i].name)
			return -ENOMEM;

		mc = devm_kzalloc(dapm->dev, (sizeof(*mc)), GFP_KERNEL);
		if (!mc)
			return -ENOMEM;

		mc->reg = SND_SOC_NOPM;
		mc->rreg = SND_SOC_NOPM;
		mc->max = 1;

		kc[i].private_value = (long)mc;
		kc[i].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		kc[i].info = snd_soc_info_volsw;
		kc[i].put = snd_soc_dapm_put_volsw;
		kc[i].get = snd_soc_dapm_get_volsw;
	}

	sprintf(widget_name, "Mixer %x", wid->nid);
	ret = hdac_generic_fill_widget_info(dapm->dev, &w[index],
			snd_soc_dapm_mixer, wid, widget_name, NULL,
			kc, wid->num_inputs, hdac_generic_mixer_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD |
			SND_SOC_DAPM_POST_REG);
	if (ret < 0)
		return ret;

	wid->priv = &w[index];

	return 0;
}

/*
 * Each Pin widget will be represented with:
 *	DAPM input/output - Based on out/in capability queried
 *	DAPM PGA - To program the PIN configuration
 *	DAPM Mux - Create a virtual Mux widget, if output capable pin can
 *		   select from multiple inputs.
 *
 * Returns number of dapm widgets created on success else returns -ve error
 * code.
 */
static int hdac_codec_alloc_pin_widget(struct snd_soc_dapm_context *dapm,
			struct snd_soc_dapm_widget *widgets, int index,
			struct hdac_codec_widget *wid)
{
	struct hdac_ext_device *edev = to_hda_ext_device(dapm->dev);
	char widget_name[HDAC_GENERIC_NAME_SIZE];
	int i = index;
	int ret;
	bool input;
	/*
	 * Pin complex are represented with multiple dapm widgets. Cache them
	 * for easy reference. wid_ref[0]->input/output, wid_ref[1]->pga,
	 * wid_ref[2]->mux.
	 */
	struct snd_soc_dapm_widget **wid_ref;

	input = is_input_pin(&edev->hdac, wid->nid);

	wid_ref = devm_kzalloc(dapm->dev,
			3 * sizeof(struct snd_soc_dapm_widget),
			GFP_KERNEL);
	if (!wid_ref)
		return -ENOMEM;

	/* Create output/input widget */
	sprintf(widget_name, "Pin %x %s", wid->nid,
				input ? "Input" : "Output");

	ret = hdac_generic_fill_widget_info(dapm->dev, &widgets[i],
			input ? snd_soc_dapm_input : snd_soc_dapm_output,
			wid, widget_name, NULL, NULL, 0,
			hdac_generic_pin_io_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD);
	if (ret < 0)
		return ret;

	wid_ref[0] = &widgets[i++];

	/* Create PGA widget */
	sprintf(widget_name, "Pin %x PGA", wid->nid);
	ret = hdac_generic_fill_widget_info(dapm->dev, &widgets[i],
			snd_soc_dapm_pga, wid, widget_name, NULL,
			NULL, 0, hdac_generic_pin_pga_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD);
	if (ret < 0)
		return ret;

	wid_ref[1] = &widgets[i++];

	/* Create Mux if Pin widget can select from multiple inputs */
	if (!input && wid->num_inputs > 1) {
		sprintf(widget_name, "Pin %x Mux", wid->nid);
		ret = hdac_generic_alloc_mux_widget(dapm, widgets, i, wid);
		if (ret < 0)
			return ret;
		/*
		 * Pin mux will not use generic selector handler, so
		 * override. Also mux widget create will increment the
		 * index, so assign the previous widget.
		 */
		widgets[i].event_flags = SND_SOC_DAPM_PRE_PMU |
						SND_SOC_DAPM_POST_PMD |
						SND_SOC_DAPM_POST_REG;
		widgets[i].event = hdac_generic_pin_mux_event;

		wid_ref[2] = &widgets[i++];
	}

	/* override hda widget private with dapm widget group */
	wid->priv = wid_ref;

	/* Return number of dapm widgets created */
	return i - index;
}

static int hdac_codec_alloc_power_widget(struct snd_soc_dapm_context *dapm,
			struct snd_soc_dapm_widget *widgets, int index,
			struct hdac_codec_widget *wid)
{
	char widget_name[HDAC_GENERIC_NAME_SIZE];
	int ret = 0;

	sprintf(widget_name, "Power %x", wid->nid);
	ret = hdac_generic_fill_widget_info(dapm->dev, &widgets[index],
			snd_soc_dapm_supply, wid, widget_name,
			NULL, NULL, 0, hdac_generic_widget_power_event,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD);
	if (ret < 0)
		return ret;

	wid->priv = &widgets[index];

	return 0;
}

/*
 * Each Beep hda widget will be represented with two dapm widget a siggen
 * and a PGA. A virtual switch control will be added to turn on/off DAPM.
 */
static int hdac_codec_alloc_beep_widget(struct snd_soc_dapm_context *dapm,
			struct snd_soc_dapm_widget *widgets, int index,
			struct hdac_codec_widget *wid)
{
	char widget_name[HDAC_GENERIC_NAME_SIZE];
	int i = index, ret = 0;
	struct soc_mixer_control *mc;
	struct snd_kcontrol_new *kc;
	char kc_name[HDAC_GENERIC_NAME_SIZE];
	/*
	 * Beep widgets are represented with multiple dapm widgets. Cache them
	 * for each reference. wid_ref[0]->siggen, wid_ref[1]->pga.
	 */
	struct snd_soc_dapm_widget **wid_ref;

	wid_ref = devm_kzalloc(dapm->dev,
			2 * sizeof(struct snd_soc_dapm_widget),
			GFP_KERNEL);
	if (!wid_ref)
		return -ENOMEM;

	sprintf(widget_name, "Beep Gen %x", wid->nid);
	ret = hdac_generic_fill_widget_info(dapm->dev, &widgets[i++],
			snd_soc_dapm_siggen, wid, widget_name,
			NULL, NULL, 0, NULL, 0);
		if (ret < 0)
			return ret;

	kc = devm_kzalloc(dapm->dev,
			(sizeof(*kc) * wid->num_inputs),
			GFP_KERNEL);
	if (!kc)
		return -ENOMEM;

	sprintf(kc_name, "%s %x in Switch", wid_names[wid->type], wid->nid);
	kc[i].name = devm_kstrdup(dapm->dev, kc_name, GFP_KERNEL);
	if (!kc[i].name)
		return -ENOMEM;
	mc = devm_kzalloc(dapm->dev, (sizeof(*mc)), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;

	mc->reg = SND_SOC_NOPM;
	mc->rreg = SND_SOC_NOPM;
	mc->max = 1;

	kc[i].private_value = (long)mc;
	kc[i].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	kc[i].info = snd_soc_info_volsw;
	kc[i].put = snd_soc_dapm_get_volsw;
	kc[i].get = snd_soc_dapm_put_volsw;

	sprintf(widget_name, "Beep Gen %x PGA", wid->nid);
	ret = hdac_generic_fill_widget_info(dapm->dev, &widgets[i],
			snd_soc_dapm_pga, wid, widget_name,
			NULL, kc, 1, NULL, 0);
	if (ret < 0)
		return ret;

	wid->priv = wid_ref;

	return 0;
}

/* Create DAPM widgets to represent each codec widget */
static int hdac_codec_alloc_widgets(struct snd_soc_dapm_context *dapm,
		struct snd_soc_dapm_widget *widgets)
{
	struct hdac_ext_device *edev = to_hda_ext_device(dapm->dev);
	struct hdac_codec_widget *wid;
	int index = 0;
	int ret = 0;

	list_for_each_entry(wid, &edev->hdac.widget_list, head) {
		switch (wid->type) {
		case AC_WID_AUD_IN:
		case AC_WID_AUD_OUT:
			ret = hdac_codec_alloc_cvt_widget(dapm, widgets,
							index, wid);
			if (ret < 0)
				return ret;
			index++;
			break;

		case AC_WID_PIN:
			ret = hdac_codec_alloc_pin_widget(dapm, widgets,
							index, wid);
			if (ret < 0)
				return ret;
			index += ret;
			break;

		case AC_WID_AUD_MIX:
			ret = hdac_codec_alloc_mixer_widget(dapm, widgets,
							index, wid);
			if (ret < 0)
				return ret;
			index++;
			break;

		case AC_WID_AUD_SEL:
			ret = hdac_generic_alloc_mux_widget(dapm, widgets,
							index, wid);
			if (ret < 0)
				return ret;
			index++;
			break;

		case AC_WID_POWER:
			ret = hdac_codec_alloc_power_widget(dapm, widgets,
							index, wid);
			if (ret < 0)
				return ret;
			index++;
			break;

		case AC_WID_BEEP:
			ret = hdac_codec_alloc_beep_widget(dapm, widgets,
							index, wid);
			if (ret < 0)
				return ret;
			index += 2;
			break;

		default:
			dev_warn(&edev->hdac.dev,
					"dapm widget not allocated for type: %d\n",
					wid->type);
			break;
		}
	}

	return ret;
}

static int hdac_generic_create_fill_widget_route_map(
		struct snd_soc_dapm_context *dapm)
{
	struct snd_soc_dapm_widget *widgets;
	struct hdac_ext_device *edev = to_hda_ext_device(dapm->dev);
	struct hdac_generic_priv *hdac_priv = edev->private_data;

	widgets = devm_kzalloc(dapm->dev,
			(sizeof(*widgets) * hdac_priv->num_dapm_widgets),
			GFP_KERNEL);
	if (!widgets)
		return -ENOMEM;

	/* Create DAPM widgets */
	hdac_codec_alloc_widgets(dapm, widgets);

	snd_soc_dapm_new_controls(dapm, widgets, hdac_priv->num_dapm_widgets);

	/* Add each path to dapm graph when enumerated */
	hdac_generic_add_route_to_list(dapm, widgets);

	snd_soc_dapm_new_widgets(dapm->card);


	return 0;
}

static void hdac_generic_calc_dapm_widgets(struct hdac_ext_device *edev)
{
	struct hdac_generic_priv *hdac_priv = edev->private_data;
	struct hdac_codec_widget *wid;

	if (list_empty(&edev->hdac.widget_list))
		return;

	/*
	 * PIN widget with output capable are represented with an additional
	 * virtual mux widgets.
	 */
	list_for_each_entry(wid, &edev->hdac.widget_list, head) {
		switch (wid->type) {
		case AC_WID_AUD_IN:
			hdac_priv->num_dapm_widgets++;
			hdac_priv->num_adcs++;
			break;

		case AC_WID_AUD_OUT:
			hdac_priv->num_dapm_widgets++;
			hdac_priv->num_dacs++;
			break;

		case AC_WID_PIN:
			hdac_priv->num_pins++;
			/*
			 * PIN widgets are represented with dapm_pga and
			 * dapm_output.
			 */
			hdac_priv->num_dapm_widgets += 2;

			if (is_input_pin(&edev->hdac, wid->nid))
				continue;

			/*
			 * PIN widget with output capable are represented
			 * with an additional virtual mux widgets.
			 */
			if (wid->num_inputs > 1)
				hdac_priv->num_dapm_widgets++;

			break;

		case AC_WID_AUD_MIX:
			hdac_priv->num_dapm_widgets++;
			break;

		case AC_WID_AUD_SEL:
			hdac_priv->num_dapm_widgets++;
			break;

		case AC_WID_POWER:
			hdac_priv->num_dapm_widgets++;
			break;

		case AC_WID_BEEP:
			/*
			 * Beep widgets are represented with a siggen and
			 * pga dapm widgets
			 */
			hdac_priv->num_dapm_widgets += 2;
			break;

		default:
			dev_warn(&edev->hdac.dev, "no dapm widget for type: %d\n",
						wid->type);
			break;
		}
	}
}

static int hdac_generic_set_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *hparams, struct snd_soc_dai *dai)
{
	struct hdac_ext_device *edev = snd_soc_dai_get_drvdata(dai);
	struct hdac_generic_priv *hdac_priv = edev->private_data;
	struct hdac_generic_dai_map *dai_map = &hdac_priv->dai_map[dai->id];
	u32 format;

	format = snd_hdac_calc_stream_format(params_rate(hparams),
			params_channels(hparams), params_format(hparams),
			32, 0);

	snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
				AC_VERB_SET_STREAM_FORMAT, format);

	return 0;
}

static int hdac_generic_set_tdm_slot(struct snd_soc_dai *dai,
		unsigned int tx_mask, unsigned int rx_mask,
		int slots, int slot_width)
{
	struct hdac_ext_device *edev = snd_soc_dai_get_drvdata(dai);
	struct hdac_generic_priv *hdac_priv = edev->private_data;
	struct hdac_generic_dai_map *dai_map = &hdac_priv->dai_map[dai->id];
	int val;

	dev_dbg(&edev->hdac.dev, "%s: strm_tag: %d\n", __func__, tx_mask);

	val = snd_hdac_codec_read(&edev->hdac, dai_map->cvt->nid, 0,
					AC_VERB_GET_CONV, 0);
	snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
				AC_VERB_SET_CHANNEL_STREAMID,
				(val & 0xf0) | (tx_mask << 4));

	return 0;
}

static int hdac_codec_set_bias_level(struct snd_soc_codec *codec,
			enum snd_soc_bias_level level)
{
	struct hdac_ext_device *edev = snd_soc_codec_get_drvdata(codec);
	struct hdac_device *hdac = &edev->hdac;

	dev_dbg(&edev->hdac.dev, "%s: level: %d\n", __func__, level); 

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		hdac_generic_set_power_state(edev, hdac->afg, AC_PWRST_D0);
		break;

	case SND_SOC_BIAS_OFF:
		hdac_generic_set_power_state(edev, hdac->afg, AC_PWRST_D3);
		break;

	default:
		dev_info(&edev->hdac.dev, "Bias level %d not handled\n", level);
		break;
	}

	return 0;
}

static int hdac_codec_probe(struct snd_soc_codec *codec)
{
	struct hdac_ext_device *edev = snd_soc_codec_get_drvdata(codec);
	struct snd_soc_dapm_context *dapm =
		snd_soc_component_get_dapm(&codec->component);
	int ret;

	edev->scodec = codec;

	/* create widget, route and controls */
	ret = hdac_generic_create_fill_widget_route_map(dapm);
	if (ret < 0)
		return ret;

	/* TODO: jack sense */

	/* Imp: Store the card pointer in hda_codec */
	edev->card = dapm->card->snd_card;

	/* TODO: runtime PM */
	return 0;
}

static int hdac_codec_remove(struct snd_soc_codec *codec)
{
	/* TODO: disable runtime pm */
	return 0;
}

static struct snd_soc_codec_driver hdac_generic_codec = {
	.probe		= hdac_codec_probe,
	.remove		= hdac_codec_remove,
	.set_bias_level = hdac_codec_set_bias_level,
};

static struct snd_soc_dai_ops hdac_generic_ops = {
	.hw_params = hdac_generic_set_hw_params,
	.set_tdm_slot = hdac_generic_set_tdm_slot,
};

static int hdac_generic_create_dais(struct hdac_ext_device *edev,
		struct snd_soc_dai_driver **dais, int num_dais)
{
	struct hdac_device *hdac = &edev->hdac;
	struct hdac_generic_priv *hdac_priv = edev->private_data;
	struct snd_soc_dai_driver *codec_dais;
	char stream_name[HDAC_GENERIC_NAME_SIZE];
	char dai_name[HDAC_GENERIC_NAME_SIZE];
	struct hdac_codec_widget *widget;
	int i = 0;
	u32 rates, bps;
	unsigned int rate_max = 192000, rate_min = 8000;
	u64 formats;
	int ret;

	codec_dais = devm_kzalloc(&hdac->dev,
			(sizeof(*codec_dais) * num_dais),
			GFP_KERNEL);
	if (!codec_dais)
		return -ENOMEM;

	/* Iterate over the input adc and dac list to create DAIs */
	list_for_each_entry(widget, &edev->hdac.widget_list, head) {

		if ((widget->type != AC_WID_AUD_IN) &&
				(widget->type != AC_WID_AUD_OUT))
			continue;

		ret = snd_hdac_query_supported_pcm(hdac, widget->nid,
				&rates,	&formats, &bps);
		if (ret)
			return ret;

		sprintf(dai_name, "%x-aif%d", hdac->vendor_id, widget->nid);

		codec_dais[i].name = devm_kstrdup(&hdac->dev, dai_name,
							GFP_KERNEL);
		if (!codec_dais[i].name)
			return -ENOMEM;

		codec_dais[i].ops = &hdac_generic_ops;
		codec_dais[i].dobj.private = widget;
		hdac_priv->dai_map[i].cvt = widget;

		switch (widget->type) {
		case AC_WID_AUD_IN:
			snprintf(stream_name, sizeof(stream_name),
					"Analog Capture-%d", widget->nid);
			codec_dais[i].capture.stream_name =
					devm_kstrdup(&hdac->dev, stream_name,
								GFP_KERNEL);
			if (!codec_dais[i].capture.stream_name)
				return -ENOMEM;

			 /*
			  * Set caps based on capability queried from the
			  * converter.
			  */
			codec_dais[i].capture.formats = formats;
			codec_dais[i].capture.rates = rates;
			codec_dais[i].capture.rate_max = rate_max;
			codec_dais[i].capture.rate_min = rate_min;
			codec_dais[i].capture.channels_min = 2;
			codec_dais[i].capture.channels_max = 2;

			i++;
			break;

		case AC_WID_AUD_OUT:
			if (widget->caps & AC_WCAP_DIGITAL)
				snprintf(stream_name, sizeof(stream_name),
					"Digital Playback-%d", widget->nid);
			else
				snprintf(stream_name, sizeof(stream_name),
					"Analog Playback-%d", widget->nid);

			codec_dais[i].playback.stream_name =
					devm_kstrdup(&hdac->dev, stream_name,
								GFP_KERNEL);
			if (!codec_dais[i].playback.stream_name)
				return -ENOMEM;

			/*
			 * Set caps based on capability queried from the
			 * converter.
			 */
			codec_dais[i].playback.formats = formats;
			codec_dais[i].playback.rates = rates;
			codec_dais[i].playback.rate_max = rate_max;
			codec_dais[i].playback.rate_min = rate_min;
			codec_dais[i].playback.channels_min = 2;
			codec_dais[i].playback.channels_max = 2;

			i++;

			break;
		default:
			dev_warn(&hdac->dev, "Invalid widget type: %d\n",
						widget->type);
			break;
		}
	}

	*dais = codec_dais;

	return 0;
}

static int hdac_generic_dev_probe(struct hdac_ext_device *edev)
{
	struct hdac_device *codec = &edev->hdac;
	struct hdac_generic_priv *hdac_priv;
	struct snd_soc_dai_driver *codec_dais = NULL;
	int num_dais = 0;
	int ret = 0;

	hdac_priv = devm_kzalloc(&codec->dev, sizeof(*hdac_priv), GFP_KERNEL);
	if (hdac_priv == NULL)
		return -ENOMEM;

	ret = snd_hdac_codec_init(codec);
	if (ret < 0)
		return ret;

	edev->private_data = hdac_priv;
	dev_set_drvdata(&codec->dev, edev);

	ret = snd_hdac_parse_widgets(codec);
	if (ret < 0) {
		dev_err(&codec->dev, "Failed to parse widgets with err: %d\n",
							ret);
		return ret;
	}

	hdac_generic_calc_dapm_widgets(edev);

	if (!hdac_priv->num_pins || ((!hdac_priv->num_adcs) &&
					 (!hdac_priv->num_dacs))) {

		dev_err(&codec->dev, "No port widgets or cvt widgets");
		return -EIO;
	}

	num_dais = hdac_priv->num_adcs + hdac_priv->num_dacs;

	ret = hdac_generic_create_dais(edev, &codec_dais, num_dais);
	if (ret < 0) {
		dev_err(&codec->dev, "Failed to create dais with err: %d\n",
							ret);
		return ret;
	}

	/* ASoC specific initialization */
	return snd_soc_register_codec(&codec->dev, &hdac_generic_codec,
			codec_dais, num_dais);
}

static int hdac_generic_dev_remove(struct hdac_ext_device *edev)
{
	snd_hdac_codec_cleanup(&edev->hdac);
	return 0;
}

/*
 * TODO:
 * Driver_data will be used to perform any vendor specific init, register
 * specific dai ops.
 * Driver will implement it's own match function to retrieve driver data.
 */
static const struct hda_device_id codec_list[] = {
	HDA_CODEC_EXT_ENTRY(0x10ec0286, 0x100002, "ALC286", 0),
	{}
};

MODULE_DEVICE_TABLE(hdaudio, codec_list);

static struct hdac_ext_driver hdac_codec_driver = {
	. hdac = {
		.driver = {
			.name   = "HDA ASoC Codec",
			/* Add PM */
		},
		.id_table       = codec_list,
	},
	.probe          = hdac_generic_dev_probe,
	.remove         = hdac_generic_dev_remove,
};

static int __init hdac_generic_init(void)
{
	return snd_hda_ext_driver_register(&hdac_codec_driver);
}

static void __exit hdac_generic_exit(void)
{
	snd_hda_ext_driver_unregister(&hdac_codec_driver);
}

module_init(hdac_generic_init);
module_exit(hdac_generic_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("HDA ASoC codec");
MODULE_AUTHOR("Subhransu S. Prusty<subhransu.s.prusty@intel.com>");
