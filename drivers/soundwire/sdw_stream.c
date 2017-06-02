/*
 *  This file is provided under a dual BSD/GPLv2 license.  When using or
 *  redistributing this file, you may do so under either license.
 *
 *  GPL LICENSE SUMMARY
 *
 *  Copyright(c) 2015-17 Intel Corporation.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of version 2 of the GNU General Public License as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  BSD LICENSE
 *
 *  Copyright(c) 2015-17 Intel Corporation.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *    * Neither the name of Intel Corporation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *  sdw_stream.c - SoundWire Bus stream operations.
 *
 *  Author:  Sanyog Kale <sanyog.r.kale@intel.com>
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/soundwire/soundwire.h>
#include "sdw_bus.h"

/**
 * sdw_release_stream_tag: Free the already assigned stream tag.
 *
 * @stream_tag: Stream tag to be freed.
 *
 * Reverses effect of "sdw_alloc_stream_tag"
 */
void sdw_release_stream_tag(unsigned int stream_tag)
{

	int i;
	struct sdw_stream_tag *stream_tags = sdw_core.stream_tags;

	/* Acquire core lock */
	mutex_lock(&sdw_core.core_lock);

	/* Get stream tag data structure */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tag == stream_tags[i].stream_tag) {

			/* Reference count update */
			sdw_dec_ref_count(&stream_tags[i].ref_count);

			if (stream_tags[i].ref_count == 0)
				/* Free up resources */
				kfree(stream_tags[i].sdw_rt);
		}
	}

	/* Release core lock */
	mutex_unlock(&sdw_core.core_lock);
}
EXPORT_SYMBOL(sdw_release_stream_tag);

/**
 * sdw_alloc_stream_tag: Allocates unique stream_tag
 *
 * @stream_tag: Stream tag returned by bus
 *
 * Stream tag is a unique identifier for each SoundWire stream across all
 * SoundWire bus instances. Stream tag is a software concept defined by bus
 * for stream management and not by MIPI SoundWire Spec. Each
 * SoundWire Stream is individually configured and controlled using the
 * stream tag. Multiple Master(s) and Slave(s) associated with the stream,
 * uses stream tag as an identifier. All the operations on the stream e.g.
 * stream configuration, port configuration, prepare and enable of the ports
 * are done based on stream tag. This API shall be called once per SoundWire
 * stream either by the Master or Slave associated with the stream.
 */
int sdw_alloc_stream_tag(unsigned int *stream_tag)
{
	int i;
	int ret = -EINVAL;
	struct sdw_runtime *sdw_rt;
	struct sdw_stream_tag *stream_tags = sdw_core.stream_tags;

	/* Acquire core lock */
	mutex_lock(&sdw_core.core_lock);

	/* Allocate new stream tag and initialize resources */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (!stream_tags[i].ref_count) {

			*stream_tag = stream_tags[i].stream_tag;

			/* Initialize stream lock */
			mutex_init(&stream_tags[i].stream_lock);

			/* Allocate resources for stream runtime handle */
			sdw_rt = kzalloc(sizeof(*sdw_rt), GFP_KERNEL);
			if (!sdw_rt) {
				ret = -ENOMEM;
				goto out;
			}

			/* Reference count update */
			sdw_inc_ref_count(&stream_tags[i].ref_count);

			/* Initialize Master and Slave list */
			INIT_LIST_HEAD(&sdw_rt->slv_rt_list);
			INIT_LIST_HEAD(&sdw_rt->mstr_rt_list);

			/* Change stream state to ALLOC */
			sdw_rt->stream_state = SDW_STATE_STRM_ALLOC;

			stream_tags[i].sdw_rt = sdw_rt;

			ret = 0;
			break;
		}
	}
out:
	/* Release core lock */
	mutex_unlock(&sdw_core.core_lock);

	return ret;
}
EXPORT_SYMBOL(sdw_alloc_stream_tag);

/**
 * sdw_config_mstr_stream: Checks if master runtime handle already
 * available, if not allocates and initialize Master runtime handle.
 *
 * @bus: Bus handle
 * @stream_config: Stream configuration for the SoundWire audio stream.
 * @sdw_rt: Stream runtime handle.
 *
 * Returns Master runtime handle.
 */
static struct sdw_mstr_runtime *sdw_config_mstr_stream(struct sdw_bus *bus,
				struct sdw_stream_config *stream_config,
				struct sdw_runtime *sdw_rt)
{
	struct sdw_mstr_runtime *mstr_rt = NULL;
	struct sdw_stream_params *str_p;

	/* Retrieve Bus handle if already available */
	list_for_each_entry(mstr_rt, &sdw_rt->mstr_rt_list, mstr_strm_node) {
		if (mstr_rt->bus == bus)
			return mstr_rt;
	}

	/* Allocate resources for Master runtime handle */
	mstr_rt = kzalloc(sizeof(*mstr_rt), GFP_KERNEL);
	if (!mstr_rt)
		goto out;

	/* Initialization of Master runtime handle */
	INIT_LIST_HEAD(&mstr_rt->port_rt_list);
	INIT_LIST_HEAD(&mstr_rt->slv_rt_list);
	list_add_tail(&mstr_rt->mstr_strm_node, &sdw_rt->mstr_rt_list);
	list_add_tail(&mstr_rt->mstr_node, &bus->mstr_rt_list);

	/* Update PCM parameters for Master */
	mstr_rt->direction = stream_config->direction;
	str_p = &mstr_rt->stream_params;
	str_p->rate = stream_config->frame_rate;
	str_p->channel_count = stream_config->channel_count;
	str_p->bps = stream_config->bps;

	/* Add reference for bus device handle */
	mstr_rt->bus = bus;

	/* Add reference for stream runtime handle */
	mstr_rt->sdw_rt = sdw_rt;

out:
	return mstr_rt;
}

/**
 * sdw_config_slave_stream: Allocate and initialize slave runtime handle.
 *
 * @slave: Slave handle
 * @stream_config: Stream configuration for the SoundWire audio stream.
 * @sdw_rt: Stream runtime handle.
 *
 * Returns Slave runtime handle.
 */
static struct sdw_slv_runtime *sdw_config_slv_stream(struct sdw_slave *slave,
				struct sdw_stream_config *stream_config,
				struct sdw_runtime *sdw_rt)
{
	struct sdw_slv_runtime *slv_rt;
	struct sdw_stream_params *str_p;

	/* Allocate resources for Slave runtime handle */
	slv_rt = kzalloc(sizeof(*slv_rt), GFP_KERNEL);
	if (!slv_rt)
		return NULL;

	/* Initialization of Slave runtime handle */
	INIT_LIST_HEAD(&slv_rt->port_rt_list);

	/* Update PCM parameters for Slave */
	slv_rt->direction = stream_config->direction;
	str_p = &slv_rt->stream_params;
	str_p->rate = stream_config->frame_rate;
	str_p->channel_count = stream_config->channel_count;
	str_p->bps = stream_config->bps;

	/* Add reference for Slave device handle */
	slv_rt->slv = slave;

	/* Add reference for stream runtime handle */
	slv_rt->sdw_rt = sdw_rt;

	return slv_rt;
}

/**
 * sdw_release_mstr_stream: Removes entry from master runtime list and free
 * up resources.
 *
 * @bus: Bus handle.
 * @sdw_rt: Master runtime handle.
 */
static void sdw_release_mstr_stream(struct sdw_bus *bus,
				struct sdw_runtime *sdw_rt)
{
	struct sdw_mstr_runtime *mstr_rt, *__mstr_rt;

	/* Retrieve Master runtime handle */
	list_for_each_entry_safe(mstr_rt, __mstr_rt, &sdw_rt->mstr_rt_list,
			mstr_strm_node) {

		if (mstr_rt->bus == bus) {

			if (mstr_rt->direction == SDW_DATA_DIR_OUT)
				/* Reference count update */
				sdw_dec_ref_count(&sdw_rt->tx_ref_count);
			else
				/* Reference count update */
				sdw_dec_ref_count(&sdw_rt->rx_ref_count);

			/* Remove node from the list */
			list_del(&mstr_rt->mstr_strm_node);
			list_del(&mstr_rt->mstr_node);

			pm_runtime_mark_last_busy(bus->dev);
			pm_runtime_put_sync_autosuspend(bus->dev);

			/* Free up Master runtime handle resources */
			kfree(mstr_rt);
		}
	}
}

/**
 * sdw_release_slv_stream: Removes entry from slave runtime list and free up
 *	resources.
 *
 * @slave: Slave handle.
 * @sdw_rt: Stream runtime handle.
 */
static void sdw_release_slv_stream(struct sdw_slave *slave,
			struct sdw_runtime *sdw_rt)
{
	struct sdw_slv_runtime *slv_rt, *__slv_rt;

	/* Retrieve Slave runtime handle */
	list_for_each_entry_safe(slv_rt, __slv_rt, &sdw_rt->slv_rt_list,
			slave_strm_node) {

		if (slv_rt->slv == slave) {

			if (slv_rt->direction == SDW_DATA_DIR_OUT)
				/* Reference count update */
				sdw_dec_ref_count(&sdw_rt->tx_ref_count);
			else
				/* Reference count update */
				sdw_dec_ref_count(&sdw_rt->rx_ref_count);

			/* Remove node from the list */
			list_del(&slv_rt->slave_strm_node);

			pm_runtime_mark_last_busy(&slave->dev);
			pm_runtime_put_sync_autosuspend(&slave->dev);

			/* Free up Slave runtime handle resources */
			kfree(slv_rt);
		}
	}
}

/**
 * sdw_release_stream: De-associates Master(s) and Slave(s) from stream.
 *	Reverse effect of the sdw_config_stream. Master calls this with
 *	Slave handle as NULL, Slave calls this with Bus handle as NULL.
 *
 * @bus: Bus handle,
 * @slave: SoundWire Slave handle, Null if stream configuration is called by
 * Master driver.
 * @stream_tag: Stream_tag representing the audio stream. All Masters and
 * Slaves part of the same stream has same stream tag. So Bus holds
 * information of all Masters and Slaves associated with stream tag.
 */

int sdw_release_stream(struct sdw_bus *bus, struct sdw_slave *slave,
					unsigned int stream_tag)
{
	int i;
	struct sdw_runtime *sdw_rt = NULL;
	struct sdw_stream_tag *stream_tags = sdw_core.stream_tags;

	/* Retrieve bus handle if called by Slave */
	if (!bus)
		bus = slave->bus;

	/* Retrieve stream runtime handle */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tags[i].stream_tag == stream_tag) {
			sdw_rt = stream_tags[i].sdw_rt;
			break;
		}
	}

	if (!sdw_rt) {
		dev_err(bus->dev, "Invalid stream tag\n");
		return -EINVAL;
	}

	/* Call release API of Master/Slave */
	if (!slave)
		sdw_release_mstr_stream(bus, sdw_rt);
	else
		sdw_release_slv_stream(slave, sdw_rt);

	return 0;
}
EXPORT_SYMBOL(sdw_release_stream);

/**
 * sdw_config_stream: Configures the SoundWire stream. 
 * *
 * @bus: Bus handle.
 * @slave: SoundWire Slave handle, Null if stream configuration is called by
 * Master driver.
 * @stream_config: Stream configuration for the SoundWire audio stream.
 * @stream_tag: Stream_tag representing the audio stream
 *
 * All the Master(s) and Slave(s) associated with the stream calls this API
 * with "sdw_stream_config". This API configures SoundWire stream based on
 * "sdw_stream_config" provided by each Master(s) and Slave(s) associated
 * with the stream. Master calls this function with Slave handle as NULL,
 * Slave calls this with Bus handle as NULL.  All Masters and Slaves part of
 * the same stream has same stream tag. So Bus holds information of all
 * Masters and Slaves associated with stream tag.
 */
int sdw_config_stream(struct sdw_bus *bus, struct sdw_slave *slave,
				struct sdw_stream_config *stream_config,
				unsigned int stream_tag)
{
	int i;
	int ret = 0;
	struct sdw_runtime *sdw_rt = NULL;
	struct sdw_mstr_runtime *mstr_rt = NULL;
	struct sdw_slv_runtime *slv_rt = NULL;
	struct sdw_stream_tag *stream_tags = sdw_core.stream_tags;
	struct sdw_stream_tag *stream = NULL;

	/* Retrieve bus handle if called by Slave */
	if (!bus)
		bus = slave->bus;

	/* Retrieve stream runtime handle */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tags[i].stream_tag == stream_tag) {
			sdw_rt = stream_tags[i].sdw_rt;
			stream = &stream_tags[i];
			break;
		}
	}

	if (!sdw_rt) {
		dev_err(bus->dev, "Valid stream tag not found\n");
		ret = -EINVAL;
		goto out;
	}

	/* Acquire stream lock */
	mutex_lock(&stream->stream_lock);

	/* Get and Initialize Master runtime handle */
	mstr_rt = sdw_config_mstr_stream(bus, stream_config, sdw_rt);
	if (!mstr_rt) {
		dev_err(bus->dev, "Master runtime configuration failed\n");
		ret = -EINVAL;
		goto error;
	}

	/* Initialize Slave runtime handle */
	if (slave) {
		slv_rt = sdw_config_slv_stream(slave, stream_config, sdw_rt);
		if (!slv_rt) {
			dev_err(bus->dev, "Slave runtime configuration failed\n");
			ret = -EINVAL;
			goto error;
		}
	}

	/*
	 * Stream params will be stored based on Tx only, since there can be
	 * only one Tx and multiple Rx, There can be multiple Tx if there is
	 * aggregation on Tx. That is handled by adding the channels to
	 * stream_params for each aggregated Tx slaves
	 */
	if (!sdw_rt->tx_ref_count && stream_config->direction ==
					SDW_DATA_DIR_OUT) {
		sdw_rt->stream_params.rate = stream_config->frame_rate;
		sdw_rt->stream_params.channel_count =
						stream_config->channel_count;
		sdw_rt->stream_params.bps = stream_config->bps;
		/* Reference count update */
		sdw_inc_ref_count(&sdw_rt->tx_ref_count);
	}

	/*
	 * Normally there will be only one Tx in system, multiple Tx can
	 * only be there if we support aggregation. In that case there may
	 * be multiple slave or masters handing different channels of same
	 * Tx stream.
	 */
	else if (sdw_rt->tx_ref_count && stream_config->direction ==
						SDW_DATA_DIR_OUT) {
		if (sdw_rt->stream_params.rate !=
			stream_config->frame_rate) {
			dev_err(bus->dev, "Frame rate for aggregated devices not matching\n");
			ret = -EINVAL;
			goto error;
		}

		if (sdw_rt->stream_params.bps != stream_config->bps) {
			dev_err(bus->dev, "bps for aggregated devices not matching\n");
			ret = -EINVAL;
			goto error;
		}

		/*
		 * Number of channels gets added, since both devices will be
		 * supporting different channels. Like one Codec supporting
		 * L and other supporting R channel.
		 */
		sdw_rt->stream_params.channel_count +=
			stream_config->channel_count;

		/* Reference count update */
		sdw_inc_ref_count(&sdw_rt->tx_ref_count);
	} else
		/* Reference count update */
		sdw_inc_ref_count(&sdw_rt->rx_ref_count);

	sdw_rt->type = stream_config->type;

	/* Change stream state to CONFIG */
	sdw_rt->stream_state = SDW_STATE_STRM_CONFIG;

	/*
	 * Slaves are added to two list, This is because bandwidth is
	 * calculated for two masters individually, while Ports are enabled
	 * of all the aggregated masters and slaves part of the same stream
	 * tag simultaneously.
	 */
	if (slave) {
		list_add_tail(&slv_rt->slave_strm_node, &sdw_rt->slv_rt_list);
		list_add_tail(&slv_rt->slave_mstr_node, &mstr_rt->slv_rt_list);
	}

	/* Release stream lock */
	mutex_unlock(&stream->stream_lock);

	if (slave)
		pm_runtime_get_sync(&slave->dev);
	else
		pm_runtime_get_sync(bus->dev);

	return ret;

error:
	mutex_unlock(&stream->stream_lock);
	kfree(mstr_rt);
	kfree(slv_rt);
out:
	return ret;
}
EXPORT_SYMBOL(sdw_config_stream);

/**
 * sdw_check_dpn_prop: Check Master and Slave port properties. This performs
 * PCM parameter check based on PCM parameters received in stream.
 *
 * @dpn_prop: Properties of Master or Slave port.
 * @strm_params: Stream PCM parameters.
 */
static int sdw_check_dpn_prop(struct sdw_dpn_prop *dpn_prop,
			struct sdw_stream_params *strm_prms)
{
	struct sdw_dpn_audio_mode *prop = &dpn_prop->audio_mode;
	int i, value;

	/* Check for sampling frequency */
	if (prop->num_freq) {
		for (i = 0; i < prop->num_freq; i++) {
			value = prop->freq[i];
			if (strm_prms->rate == value)
				break;
		}

		if (i == prop->num_freq)
			return -EINVAL;
	} else {

		if ((strm_prms->rate < prop->min_freq)
				|| (strm_prms->rate > prop->max_freq))
			return -EINVAL;
	}

	/* Check for bit rate */
	if (dpn_prop->num_words) {
		for (i = 0; i < dpn_prop->num_words; i++) {
			value = dpn_prop->words[i];
			if (strm_prms->bps == value)
				break;
		}

		if (i == dpn_prop->num_words)
			return -EINVAL;

	} else {

		if ((strm_prms->bps < dpn_prop->min_word)
				|| (strm_prms->bps > dpn_prop->max_word))
			return -EINVAL;
	}

	/* Check for number of channels */
	if (dpn_prop->num_ch) {
		for (i = 0; i < dpn_prop->num_ch; i++) {
			value = dpn_prop->ch[i];
			if (strm_prms->bps == value)
				break;
		}

		if (i == dpn_prop->num_ch)
			return -EINVAL;

	} else {

		if ((strm_prms->channel_count < dpn_prop->min_ch) ||
			(strm_prms->channel_count > dpn_prop->max_ch))
			return -EINVAL;
	}

	return 0;
}

/**
 * sdw_mstr_port_configuration: Master Port configuration. This performs
 * all the port related configuration including allocation port
 * structure memory, assign PCM parameters and add port node in master
 * runtime list.
 *
 * @bus: Bus handle.
 * @sdw_rt: Stream runtime information.
 * @ports_config: Port configuration for Slave.
 */
static int sdw_mstr_port_configuration(struct sdw_bus *bus,
				struct sdw_runtime *sdw_rt,
				struct sdw_ports_config *ports_config)
{
	struct sdw_mstr_runtime *mstr_rt = NULL;
	struct sdw_port_runtime *port_rt;
	int found = 0;
	int i;
	int ret = 0, pn = 0;
	struct sdw_dpn_prop *dpn_prop = bus->prop.dpn_prop;

	/* Get bus device handle */
	list_for_each_entry(mstr_rt, &sdw_rt->mstr_rt_list, mstr_strm_node) {
		if (mstr_rt->bus == bus) {
			found = 1;
			break;
		}
	}

	if (!found) {
		dev_err(bus->dev, "Master not found for this port\n");
		return -EINVAL;
	}

	/* Allocate resources for port runtime handle */
	port_rt = kzalloc((sizeof(*port_rt) * ports_config->num_ports),
				GFP_KERNEL);
	if (!port_rt)
		return -ENOMEM;

	/* Check master capabilities */
	if (!dpn_prop)
		return -EINVAL;

	/* Iterate for number of ports to perform initialization */
	for (i = 0; i < ports_config->num_ports; i++) {
		port_rt[i].channel_mask = ports_config->port_config[i].ch_mask;
		port_rt[i].port_num = pn = ports_config->port_config[i].num;

		/* Perform capability check for master port */
		ret = sdw_check_dpn_prop(&dpn_prop[pn],
						&mstr_rt->stream_params);
		if (ret < 0) {
			dev_err(bus->dev, "Master capabilities check failed ret = %d\n", ret);
			goto error;
		}

		/* Add node to port runtime list */
		list_add_tail(&port_rt[i].port_node, &mstr_rt->port_rt_list);
	}

	return ret;

error:
	kfree(port_rt);
	return ret;
}

/**
 * sdw_get_slv_dpn_prop: Retrieve Slave port capabilities.
 *
 * @slave: Slave handle.
 * @direction: Data direction.
 * @port_num: Port for which capabilities to be retrieved.
 */
struct sdw_dpn_prop *sdw_get_slv_dpn_prop(struct sdw_slave *slave,
				enum sdw_data_direction direction,
				unsigned int port_num)
{
	int i;
	struct sdw_dpn_prop *dpn_prop;
	u8 num_ports;
	bool port_found = 0;

	if (direction == SDW_DATA_DIR_OUT) {
		num_ports = slave->prop.source_ports;
		dpn_prop = slave->prop.src_dpn_prop;
	} else {
		num_ports = slave->prop.sink_ports;
		dpn_prop = slave->prop.sink_dpn_prop;
	}

	for (i = 0; i < num_ports; i++) {
		dpn_prop = &dpn_prop[i];

		if (dpn_prop->port == port_num) {
			port_found = 1;
			break;
		}
	}

	if (!port_found)
		return NULL;

	return dpn_prop;

}

/**
 * sdw_config_slv_port: Slave Port configuration. This performs
 * all the port related configuration including allocation port
 * structure memory, assign PCM parameters and add port node in slave
 * runtime list.
 *
 * @slave: Slave handle.
 * @sdw_rt: Stream runtime information.
 * @ports_config: Port configuration for Slave.
 */
static int sdw_config_slv_port(struct sdw_slave *slave,
			struct sdw_runtime *sdw_rt,
			struct sdw_ports_config *ports_config)
{
	struct sdw_slv_runtime *slv_rt;
	struct sdw_port_runtime *port_rt;
	struct sdw_dpn_prop *dpn_prop;
	int found = 0, ret = 0;
	int i, pn;

	/* Get Slave device handle */
	list_for_each_entry(slv_rt, &sdw_rt->slv_rt_list, slave_strm_node) {
		if (slv_rt->slv == slave) {
			found = 1;
			break;
		}
	}

	if (!found) {
		dev_err(&slave->dev, "Slave not found for this port\n");
		return -EINVAL;
	}


	/* Allocate resources for port runtime handle */
	port_rt = kzalloc((sizeof(*port_rt) * ports_config->num_ports),
					GFP_KERNEL);
	if (!port_rt)
		return -ENOMEM;

	/* Assign PCM parameters */
	for (i = 0; i < ports_config->num_ports; i++) {
		port_rt[i].channel_mask = ports_config->port_config[i].ch_mask;
		port_rt[i].port_num = pn =
				ports_config->port_config[i].num;

		dpn_prop = sdw_get_slv_dpn_prop(slave, slv_rt->direction,
				ports_config->port_config[i].num);
		if (!dpn_prop) {
			ret = -EINVAL;
			dev_err(&slave->dev, "Slave port capabilities not found ret = %d\n", ret);
			goto error;
		}

		/* Perform capability check for slave port */
		ret = sdw_check_dpn_prop(dpn_prop, &slv_rt->stream_params);
		if (ret < 0) {
			dev_err(&slave->dev, "Slave capabilities check failed ret = %d\n", ret);
			goto error;
		}

		/* Add node to port runtime list */
		list_add_tail(&port_rt[i].port_node, &slv_rt->port_rt_list);
	}

	return ret;

error:
	kfree(port_rt);
	return ret;
}

/**
 * sdw_config_ports: Configures Master or Slave Port(s) associated with
 * the stream. All the Master(s) and Slave(s) associated with the
 * stream calls this API with "sdw_ports_config". Master calls this
 * function with Slave handle as NULL, Slave calls this with bus
 * handle as NULL.
 *
 * @bus: Bus handle where the Slave is connected.
 * @slave: Slave handle.
 * @ports_config: Port configuration for each Port of SoundWire Slave.
 * @stream_tag: Stream tag, where this Port is connected.
 */
int sdw_config_ports(struct sdw_bus *bus, struct sdw_slave *slave,
				struct sdw_ports_config *ports_config,
				unsigned int stream_tag)
{
	int ret;
	int i;
	struct sdw_stream_tag *stream_tags = sdw_core.stream_tags;
	struct sdw_runtime *sdw_rt = NULL;
	struct sdw_stream_tag *stream = NULL;

	/* Retrieve bus handle if called by Slave */
	if (!bus)
		bus = slave->bus;

	/* Retrieve stream runtime handle */
	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tags[i].stream_tag == stream_tag) {
			sdw_rt = stream_tags[i].sdw_rt;
			stream = &stream_tags[i];
			break;
		}
	}

	if (!sdw_rt) {
		dev_err(bus->dev, "Invalid stream tag\n");
		return -EINVAL;
	}

	/* Acquire stream lock */
	mutex_lock(&stream->stream_lock);

	/* Perform Master/Slave port configuration */
	if (!slave)
		ret = sdw_mstr_port_configuration(bus, sdw_rt, ports_config);
	else
		ret = sdw_config_slv_port(slave, sdw_rt, ports_config);

	/* Release stream lock */
	mutex_unlock(&stream->stream_lock);

	return ret;
}
EXPORT_SYMBOL(sdw_config_ports);

/**
 * sdw_acquire_mstr_lock: Acquire Master lock for the Master(s) used by the
 * given stream. The advantage of using Master lock over core lock is Master
 * lock will lock only those Master(s) associated with given stream giving
 * the advantage of simultaneous configuration of stream(s) running on
 * different Master(s). On the other hand, core lock will not allow multiple
 * stream configuration simultaneously.
 *
 * @stream_tag: Stream tag on which operations needs to be performed.
 */
static void sdw_acquire_mstr_lock(struct sdw_stream_tag *stream_tag)
{
	struct sdw_runtime *sdw_rt = stream_tag->sdw_rt;
	struct sdw_mstr_runtime *sdw_mstr_rt = NULL;
	struct sdw_bus *bus = NULL;

	/* Acquire core lock */
	mutex_lock(&sdw_core.core_lock);

	/* Iterate for all Master(s) in Master list */
	list_for_each_entry(sdw_mstr_rt, &sdw_rt->mstr_rt_list,
			mstr_strm_node) {

		/* Get Bus structure */
		bus = sdw_mstr_rt->bus;

		/* Acquire Master lock */
		spin_lock(&bus->lock);
	}

	/* Release core lock */
	mutex_unlock(&sdw_core.core_lock);

}

/**
 * sdw_release_mstr_lock: This function releases Master lock for the
 * Master(s) used by the given stream acquired in sdw_acquire_mstr_lock API.
 *
 * @stream_tag: Stream tag on which operations needs to be performed.
 */
static void sdw_release_mstr_lock(struct sdw_stream_tag *stream_tag)
{
	struct sdw_runtime *sdw_rt = stream_tag->sdw_rt;
	struct sdw_mstr_runtime *sdw_mstr_rt = NULL;
	struct sdw_bus *bus = NULL;

	/* Acquire core lock */
	mutex_lock(&sdw_core.core_lock);

	/* Iterate for all Master(s) in Master list */
	list_for_each_entry(sdw_mstr_rt, &sdw_rt->mstr_rt_list,
			mstr_strm_node) {

		/* Get Bus structure */
		bus = sdw_mstr_rt->bus;

		/* Release Master lock */
		spin_unlock(&bus->lock);
	}

	/* Release core lock */
	mutex_unlock(&sdw_core.core_lock);

}

/**
 * sdw_find_stream: Retrieves stream tag handle by matching stream tag.
 *
 * @stream_tag: Stream tag.
 */
static struct sdw_stream_tag *sdw_find_stream(int stream_tag)
{
	int i;
	struct sdw_stream_tag *stream_tags = sdw_core.stream_tags;
	struct sdw_stream_tag *stream = NULL;

	/* Acquire core lock */
	mutex_lock(&sdw_core.core_lock);

	for (i = 0; i < SDW_NUM_STREAM_TAGS; i++) {
		if (stream_tag == stream_tags[i].stream_tag) {
			stream = &stream_tags[i];
			break;
		}
	}

	if (stream == NULL) {
		/* Release core lock */
		mutex_unlock(&sdw_core.core_lock);
		WARN_ON(1);
		return NULL;
	}

	/* Release core lock */
	mutex_unlock(&sdw_core.core_lock);

	return stream;
}

/**
 * sdw_prepare_and_enable: Prepare and enable all the ports of all the
 *	Master(s) and Slave(s) associated with this stream tag. Following
 *	will be done as part of prepare operation.
 *	1. Bus parameters such as bandwidth, frame shape, clock frequency,
 *	SSP interval are computed based on current stream as well as already
 *	active streams on bus. Re-computation is required to accommodate
 *	current stream on the bus.
 *	2. Transport parameters of all Master and Slave ports are computed
 *	for the current as well as already active stream based on above
 *	calculated frame shape and clock frequency.
 *	3. Computed bus and transport parameters are programmed in Master
 *	and Slave registers. The banked registers programming is done on the
 *	alternate bank (bank currently unused). Port channels are enabled
 *	for the already active streams on the alternate bank (bank currently
 *	unused). This is done in order to not to disrupt already active
 *	stream.
 *	4. Once all the new values are programmed, switch is made to
 *	alternate bank. Once switch is successful, the port channels enabled
 *	on previous bank for already active streams are disabled.
 *	5. Ports of Master and Slave for new stream are prepared.
 *
 *	Following will be done as part of enable operation.
 *	1. All the values computed in SDW_STATE_STRM_PREPARE state are
 *	programmed in alternate bank (bank currently unused). It includes
 *	programming of already active streams as well.
 *	2. All the Master and Slave port channels for the new stream are
 *	enabled on alternate bank (bank currently unused).
 *	3. Once all the new values are programmed, switch is made on the
 *	alternate bank. Once the switch is successful, the port channels
 *	enabled on previous bank for already active streams are disabled.
 *
 *	This shall be called either by Master or Slave, which is responsible
 *	for doing data transfer between SoundWire link and the system
 *	memory.
 *
 * @stream_tag: Audio stream to be enabled. Each stream has unique
 *	stream_tag. All the channels of all the ports of Slave(s) and
 *	Master(s) attached to this stream will be prepared and enabled
 *	simultaneously with bank switch.
 */
int sdw_prepare_and_enable(unsigned int stream_tag)
{

	int ret;

	struct sdw_stream_tag *stream = NULL;

	stream = sdw_find_stream(stream_tag);
	if (!stream)
		return -EINVAL;

	/* Acquire Master lock */
	sdw_acquire_mstr_lock(stream);

	/*
	 * All the operations related to Prepare and enable is
	 * performed here. Prepare operation is by default and enable
	 * is based on enable flag
	 */
	ret = sdw_prepare_and_enable_ops(stream);
	if (ret < 0)
		pr_err("Error: prepare/enable operation failed\n");

	/* Release Master lock */
	sdw_release_mstr_lock(stream);

	return ret;
}
EXPORT_SYMBOL(sdw_prepare_and_enable);

/**
 * sdw_disable_and_deprepare: Disable and de-prepare all the ports of
 *	all the Master(s) and Slave(s) associated with stream tag. Following
 *	will be done as part of disable operation.
 *	1. Disable for Master and Slave ports channels is performed on
 *	alternate bank (bank currently unused) registers for current stream.
 *	2. All the current configuration of bus and Master and Slave ports
 *	are programmed into alternate bank (bank currently unused). It
 *	includes programming of already active streams port channels on
 *	alternate bank (bank currently unused).
 *	3. Switch is made on new bank. Once the switch is successful, the
 *	port channels of current stream are disabled. All the port channels
 *	enabled on previous bank for active stream are disabled.
 *
 *	Following will be done as part of de-prepare operation.
 *	1. Check the bandwidth required per Master. If its zero, de-prepare
 *	current stream and move stream state SDW_STATE_STRM_UNPREPARE, rest
 *	of the steps are not required. If bandwidth required per Master is
 *	non zero that means some more streams are running on Master and
 *	continue with next step.
 *	2. Bus parameters and transport parameters are computed for the
 *	streams active on the given Master.
 *	3. All the computed values for active stream are programmed into
 *	alternate bank (bank currently unused) in Master and Slave registers
 *	including already active streams port channels on alternate bank
 *	(bank currently unused).
 *	4. Switch is made to alternate bank where all the values for active
 *	stream were programmed. On successful switch of bank, all the port
 *	channels enabled on previous bank for active stream are disabled.
 *	5. De-prepare ports of the Master and Slave associated with current
 *	stream.
 *
 *	This shall be called either by Master or Slave, which is
 *	responsible for doing data transfer between SoundWire link and the
 *	system memory.
 *	Note: Both disable and de-prepare operations are performed in single
 *	call. De-prepare operation can be deferred for some specific timeout
 *	value after disable operation, to avoid bus re-configurations
 *	between short play and pause periods.
 *
 * @stream_tag: Audio stream to be disabled. Each stream has unique
 *	stream_tag. All the channels of all the ports of Slave(s) and
 *	Master(s) attached to this stream will be disabled and de-prepared
 *	simultaneously with bank switch.
 */
int sdw_disable_and_deprepare(unsigned int stream_tag)
{
	int ret;
	struct sdw_stream_tag *stream = NULL;

	stream = sdw_find_stream(stream_tag);
	if (!stream)
		return -EINVAL;

	/* Acquire Master lock */
	sdw_acquire_mstr_lock(stream);

	/*
	 * All the operations related to disable and de-prepare is
	 * performed here. Disable operation is by default and de-prepare
	 * is based on de-prepare flag
	 */
	ret = sdw_disable_and_deprepare_ops(stream);
	if (ret < 0)
		pr_err("Error: disable/de-prepare operations failed\n");

	/* Release Master lock */
	sdw_release_mstr_lock(stream);

	return ret;
}
EXPORT_SYMBOL(sdw_disable_and_deprepare);
