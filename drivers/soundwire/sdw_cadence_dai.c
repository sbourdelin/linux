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
 * First written by Hardik T Shah
 * Rewrite by Vinod
 */

#include <linux/pm_runtime.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/soundwire/soundwire.h>
#include "sdw_intel_shim.h"
#include "sdw_cadence.h"

struct cdns_dma_data {
	int stream_tag;
	int nr_ports;
	struct cdns_ports **port;
	struct sdw_bus *bus;
	enum sdw_stream_type stream_type;
	int link_id;
};

static int cdns_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai, bool pcm)
{
	struct cdns_sdw *sdw = snd_soc_dai_get_drvdata(dai);
	struct cdns_dma_data *dma;
	int ret;

	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;

	if (pcm)
		dma->stream_type = SDW_STREAM_PCM;
	else
		dma->stream_type = SDW_STREAM_PDM;

	dma->bus = &sdw->bus;
	dma->link_id = sdw->instance;

	snd_soc_dai_set_dma_data(dai, substream, dma);

	ret = sdw_alloc_stream_tag(&dma->stream_tag);
	if (ret) {
		dev_err(dai->dev, "allocate stream tag failed for DAI %s: %d\n",
					dai->name, ret);
		goto fail_stream_tag;
	}

	snd_soc_dai_program_stream_tag(substream, dai, dma->stream_tag);

	return 0;

fail_stream_tag:
	kfree(dma);
	snd_soc_dai_set_dma_data(dai, substream, NULL);
	return ret;
}

static int cdns_pcm_startup(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	return cdns_startup(substream, dai, true);
}

static int cdns_pdm_startup(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	return cdns_startup(substream, dai, false);
}

static struct sdw_cdns_pdi *cdns_find_pdi(struct cdns_sdw *sdw,
		unsigned int num, struct sdw_cdns_pdi *pdi)
{
	int i;

	for (i = 0; i < num; i++) {
		if (pdi[i].assigned == true)
			continue;

		pdi[i].assigned = true;
		return &pdi[i];
	}

	return NULL;
}

static int cdns_alloc_stream(struct cdns_sdw *sdw,
			struct sdw_cdns_streams *stream,
			struct cdns_ports *port, u32 ch, u32 dir)
{
	struct sdw_cdns_pdi *pdi = NULL;
	u32 offset, val;

	spin_lock(&sdw->bus.lock);
	/* check for streams based on direction if not use bidir */
	if (dir == SDW_DATA_DIR_IN)
		pdi = cdns_find_pdi(sdw, stream->num_in, stream->in);
	else
		pdi = cdns_find_pdi(sdw, stream->num_out, stream->out);

	/* check if we got, otherwise use bd */
	if (!pdi)
		pdi = cdns_find_pdi(sdw, stream->num_bd, stream->bd);

	spin_unlock(&sdw->bus.lock);

	if (!pdi)
		return -EIO;

	pdi->pdi_num = port->idx;
	port->pdi = pdi;
	pdi->l_ch_num = 0;
	pdi->h_ch_num = ch - 1;

	val = 0;
	if (dir == SDW_DATA_DIR_IN)
		val = CDNS_PORTCTRL_DIRN;

	offset = CDNS_PORTCTRL + port->idx * CDNS_PORT_OFFSET;
	cdns_sdw_updatel(sdw, offset, CDNS_PORTCTRL_DIRN, val);

	val = port->idx;
	val |= ((1 << ch) - 1) << fls(CDNS_PDI_CONFIG_CHANNEL);
	cdns_sdw_writel(sdw, val, CDNS_PDI_CONFIG(pdi->pdi_num));

	return 0;
}


static struct cdns_ports *cdns_alloc_port(struct cdns_sdw *sdw,
				u32 ch, u32 dir, bool pcm)
{
	struct cdns_ports *port = NULL;
	int i, ret;

	/* hold the bus lock */
	spin_lock(&sdw->bus.lock);
	for (i = 1; i <= CDNS_MAX_PORTS; i++) {
		if (sdw->ports[i].allocated == true)
			continue;

		port = &sdw->ports[i];
		port->allocated = true;
		port->direction = dir;
		port->ch = ch;
		break;
	}
	spin_unlock(&sdw->bus.lock);

	if (!port) {
		dev_err(sdw->dev, "Unable to find a free port\n");
		return NULL;
	}

	if (pcm) {
		ret = cdns_alloc_stream(sdw, &sdw->pcm, port, ch, dir);
		/* configure shim for PCM only */
		sdw->res->ops->config_pdi(sdw->res->shim, sdw->instance, port->pdi);
	} else {
		ret = cdns_alloc_stream(sdw, &sdw->pdm, port, ch, dir);
	}

	if (!ret) {
		/* failed so freeup the port */
		spin_lock(&sdw->bus.lock);
		port->allocated = false;
		spin_unlock(&sdw->bus.lock);
		port = NULL;
	}
	return port;
}

static int cdns_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct cdns_sdw *sdw = snd_soc_dai_get_drvdata(dai);
	struct cdns_dma_data *dma;
	struct sdw_stream_config sconfig;
	struct sdw_ports_config pconfig;
	struct sdw_port_config *port;
	int ret, i, ch, dir;
	bool pcm = true;

	ret = pm_runtime_get_sync(dai->dev);
	if (!ret)
		return ret;

	dma = snd_soc_dai_get_dma_data(dai, substream);
	ch = params_channels(params);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		dir = SDW_DATA_DIR_IN;
	else
		dir = SDW_DATA_DIR_OUT;

	if (dma->stream_type == SDW_STREAM_PDM) {
		dma->nr_ports = ch;
		pcm = false;
		/* upscale factor for PDM */
	} else {
		dma->nr_ports = 1;
	}

	dma->port = kcalloc(dma->nr_ports, sizeof(struct cdns_ports), GFP_KERNEL);
	if (!dma->port)
		return -ENOMEM;

	for (i = 0; i < dma->nr_ports; i++) {
		dma->port[i] = cdns_alloc_port(sdw, ch, dir, pcm);
		if (!dma->port[i])
			return -EIO;
	}

	/* now tell the shim and DSP */
	sdw->res->ops->config_stream(sdw->res->shim, sdw->instance,
					substream, dai, params);


	/* configure the stream */
	sconfig.direction = dir;
	sconfig.channel_count = ch;
	sconfig.frame_rate = params_rate(params);
	sconfig.type = dma->stream_type;
	if (dma->stream_type == SDW_STREAM_PDM) {
		sconfig.frame_rate *= 16;
		sconfig.bps = 1;
	} else {
		sconfig.bps = snd_pcm_format_width(params_format(params));
	}
	ret = sdw_config_stream(&sdw->bus, NULL, &sconfig, dma->stream_tag);
	if (ret) {
		dev_err(dai->dev, "sdw_config_stream failed: %d\n", ret);
		return ret;
	}

	/* now port configuration */
	port = kcalloc(dma->nr_ports, sizeof(*port), GFP_KERNEL);
	if(!port)
		return -ENOMEM;

	pconfig.num_ports = dma->nr_ports;
	pconfig.port_config = port;

	for (i = 0; i < dma->nr_ports; i++) {
		port[i].num = dma->port[i]->idx;
		if (dma->stream_type == SDW_STREAM_PDM)
			port[i].ch_mask = 1;
		else
			port[i].ch_mask = (1 << ch) - 1;
	}

	ret = sdw_config_ports(&sdw->bus, NULL, &pconfig, dma->stream_tag);
	if (ret) {
		dev_err(dai->dev, "sdw_config_ports failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int cdns_free_port(struct cdns_sdw *sdw, struct cdns_ports *port)
{
	spin_lock(&sdw->bus.lock);
	port->pdi->assigned = false;
	port->pdi = NULL;
	port->allocated = false;
	spin_unlock(&sdw->bus.lock);

	return 0;
}

static int cdns_hw_free(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	struct cdns_sdw *sdw = snd_soc_dai_get_drvdata(dai);
	struct cdns_dma_data *dma;
	int ret, i;

	dma = snd_soc_dai_get_dma_data(dai, substream);

	ret = sdw_release_stream(&sdw->bus, NULL, dma->stream_tag);
	if (ret)
		dev_err(dai->dev, "sdw_release_stream failed: %d\n", ret);

	for (i = 0; i < dma->nr_ports; i++) {
		cdns_free_port(sdw, dma->port[i]);
		dma->port[i] = NULL;
	}

	return 0;
}

static int cdns_trigger(struct snd_pcm_substream *substream,
				int cmd, struct snd_soc_dai *dai)
{
	struct cdns_dma_data *dma;
	int ret;

	dma = snd_soc_dai_get_dma_data(dai, substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		ret = sdw_prepare_and_enable(dma->stream_tag);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		ret = sdw_disable_and_deprepare(dma->stream_tag);
		break;

	default:
		return -EINVAL;
	}
	if (ret)
		dev_err(dai->dev, "sdw_prepare/disable failed: %d\n", ret);

	return ret;
}

static void cdns_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct cdns_dma_data *dma;

	dma = snd_soc_dai_get_dma_data(dai, substream);
	snd_soc_dai_remove_stream_tag(substream, dai);

	sdw_release_stream_tag(dma->stream_tag);
	snd_soc_dai_set_dma_data(dai, substream, NULL);
	kfree(dma);

	pm_runtime_mark_last_busy(dai->dev);
	pm_runtime_put_autosuspend(dai->dev);
}

static struct snd_soc_dai_ops cdns_pcm_dai_ops = {
	.startup = cdns_pcm_startup,
	.hw_params = cdns_hw_params,
	.hw_free = cdns_hw_free,
	.trigger = cdns_trigger,
	.shutdown = cdns_shutdown,
};

static struct snd_soc_dai_ops cdns_pdm_dai_ops = {
	.startup = cdns_pdm_startup,
	.hw_params = cdns_hw_params,
	.hw_free = cdns_hw_free,
	.trigger = cdns_trigger,
	.shutdown = cdns_shutdown,
};


static struct snd_soc_dai_driver cdns_dai[] = {
{
	/*
	 * to start with add single PCM & PDM dai and scale this later
	 */
	.name = "SDW Pin",
	.ops = &cdns_pcm_dai_ops,
	.playback = {
		.stream_name = "SDW Tx",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "SDW Rx",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
{
	.name = "SDW PDM Pin",
	.ops = &cdns_pdm_dai_ops,
	.capture = {
		.stream_name = "SDW Rx1",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
};

static const struct snd_soc_component_driver cdns_component = {
	.name           = "soundwire",
};

int cdns_register_dai(struct cdns_sdw *sdw)
{
	return snd_soc_register_component(sdw->dev, &cdns_component,
				cdns_dai, ARRAY_SIZE(cdns_dai));
}

void cdns_deregister_dai(struct cdns_sdw *sdw)
{
	snd_soc_unregister_component(sdw->dev);
}
