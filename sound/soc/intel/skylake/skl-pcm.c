/*
 *  skl-pcm.c -ASoC HDA Platform driver file implementing PCM functionality
 *
 *  Copyright (C) 2014-2015 Intel Corp
 *  Author:  Jeeja KP <jeeja.kp@intel.com>
 *
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
 *
 */

#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/clocksource.h>
#include <linux/timekeeping.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "skl.h"
#include "skl-topology.h"
#include "skl-sst-dsp.h"
#include "skl-sst-ipc.h"

#define HDA_MONO 1
#define HDA_STEREO 2
#define HDA_QUAD 4
#define SKL_ADSP_FWREG_PPLBASE     (0x8000 + 0x40)

static struct snd_pcm_hardware azx_pcm_hw = {
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_PAUSE |
				 SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_SYNC_START |
				 SNDRV_PCM_INFO_HAS_WALL_CLOCK | /* legacy */
				 SNDRV_PCM_INFO_HAS_LINK_ATIME |
				 SNDRV_PCM_INFO_NO_PERIOD_WAKEUP),
	.formats =		SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S32_LE |
				SNDRV_PCM_FMTBIT_S24_LE,
	.rates =		SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_16000 |
				SNDRV_PCM_RATE_8000,
	.rate_min =		8000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		8,
	.buffer_bytes_max =	AZX_MAX_BUF_SIZE,
	.period_bytes_min =	128,
	.period_bytes_max =	AZX_MAX_BUF_SIZE / 2,
	.periods_min =		2,
	.periods_max =		AZX_MAX_FRAG,
	.fifo_size =		0,
};

static inline
struct hdac_ext_stream *get_hdac_ext_stream(struct snd_pcm_substream *substream)
{
	return substream->runtime->private_data;
}

static struct hdac_ext_bus *get_bus_ctx(struct snd_pcm_substream *substream)
{
	struct hdac_ext_stream *stream = get_hdac_ext_stream(substream);
	struct hdac_stream *hstream = hdac_stream(stream);
	struct hdac_bus *bus = hstream->bus;

	return hbus_to_ebus(bus);
}

static int skl_substream_alloc_pages(struct hdac_ext_bus *ebus,
				 struct snd_pcm_substream *substream,
				 size_t size)
{
	struct hdac_ext_stream *stream = get_hdac_ext_stream(substream);

	hdac_stream(stream)->bufsize = 0;
	hdac_stream(stream)->period_bytes = 0;
	hdac_stream(stream)->format_val = 0;

	return snd_pcm_lib_malloc_pages(substream, size);
}

static int skl_substream_free_pages(struct hdac_bus *bus,
				struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static void skl_set_pcm_constrains(struct hdac_ext_bus *ebus,
				 struct snd_pcm_runtime *runtime)
{
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);

	/* avoid wrap-around with wall-clock */
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_TIME,
				     20, 178000000);
}

static enum hdac_ext_stream_type skl_get_host_stream_type(struct hdac_ext_bus *ebus)
{
	if ((ebus_to_hbus(ebus))->ppcap)
		return HDAC_EXT_STREAM_TYPE_HOST;
	else
		return HDAC_EXT_STREAM_TYPE_COUPLED;
}

/*
 * check if the stream opened is marked as ignore_suspend by machine, if so
 * then enable suspend_active refcount
 *
 * The count supend_active does not need lock as it is used in open/close
 * and suspend context
 */
static void skl_set_suspend_active(struct snd_pcm_substream *substream,
					 struct snd_soc_dai *dai, bool enable)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct snd_soc_dapm_widget *w;
	struct skl *skl = ebus_to_skl(ebus);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		w = dai->playback_widget;
	else
		w = dai->capture_widget;

	if (w->ignore_suspend && enable)
		skl->supend_active++;
	else if (w->ignore_suspend && !enable)
		skl->supend_active--;
}

static int skl_pcm_open(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct hdac_ext_stream *stream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct skl_dma_params *dma_params;
	struct skl *skl = get_skl_ctx(dai->dev);
	struct skl_module_cfg *mconfig;

	dev_dbg(dai->dev, "%s: %s\n", __func__, dai->name);

	stream = snd_hdac_ext_stream_assign(ebus, substream,
					skl_get_host_stream_type(ebus));
	if (stream == NULL)
		return -EBUSY;

	skl_set_pcm_constrains(ebus, runtime);

	/*
	 * disable WALLCLOCK timestamps for capture streams
	 * until we figure out how to handle digital inputs
	 */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		runtime->hw.info &= ~SNDRV_PCM_INFO_HAS_WALL_CLOCK; /* legacy */
		runtime->hw.info &= ~SNDRV_PCM_INFO_HAS_LINK_ATIME;
	}

	runtime->private_data = stream;

	dma_params = kzalloc(sizeof(*dma_params), GFP_KERNEL);
	if (!dma_params)
		return -ENOMEM;

	dma_params->stream_tag = hdac_stream(stream)->stream_tag;
	snd_soc_dai_set_dma_data(dai, substream, dma_params);

	dev_dbg(dai->dev, "stream tag set in dma params=%d\n",
				 dma_params->stream_tag);
	skl_set_suspend_active(substream, dai, true);
	snd_pcm_set_sync(substream);

	mconfig = skl_tplg_fe_get_cpr_module(dai, substream->stream);
	skl_tplg_d0i3_get(skl, mconfig->d0i3_caps);

	return 0;
}

static int skl_get_format(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct skl_dma_params *dma_params;
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	int format_val = 0;

	if ((ebus_to_hbus(ebus))->ppcap) {
		struct snd_pcm_runtime *runtime = substream->runtime;

		format_val = snd_hdac_calc_stream_format(runtime->rate,
						runtime->channels,
						runtime->format,
						32, 0);
	} else {
		struct snd_soc_dai *codec_dai = rtd->codec_dai;

		dma_params = snd_soc_dai_get_dma_data(codec_dai, substream);
		if (dma_params)
			format_val = dma_params->format;
	}

	return format_val;
}

static int skl_be_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct skl *skl = get_skl_ctx(dai->dev);
	struct skl_sst *ctx = skl->skl_sst;
	struct skl_module_cfg *mconfig;

	if (dai->playback_widget->power || dai->capture_widget->power)
		return 0;

	mconfig = skl_tplg_be_get_cpr_module(dai, substream->stream);
	if (mconfig == NULL)
		return -EINVAL;

	return skl_dsp_set_dma_control(ctx, mconfig);
}

static int skl_pcm_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *stream = get_hdac_ext_stream(substream);
	struct skl *skl = get_skl_ctx(dai->dev);
	unsigned int format_val;
	int err;
	struct skl_module_cfg *mconfig;

	dev_dbg(dai->dev, "%s: %s\n", __func__, dai->name);

	mconfig = skl_tplg_fe_get_cpr_module(dai, substream->stream);

	format_val = skl_get_format(substream, dai);
	dev_dbg(dai->dev, "stream_tag=%d formatvalue=%d\n",
				hdac_stream(stream)->stream_tag, format_val);
	snd_hdac_stream_reset(hdac_stream(stream));

	/* In case of XRUN recovery, reset the FW pipe to clean state */
	if (mconfig && (substream->runtime->status->state ==
					SNDRV_PCM_STATE_XRUN))
		skl_reset_pipe(skl->skl_sst, mconfig->pipe);

	err = snd_hdac_stream_set_params(hdac_stream(stream), format_val);
	if (err < 0)
		return err;

	err = snd_hdac_stream_setup(hdac_stream(stream));
	if (err < 0)
		return err;

	hdac_stream(stream)->prepared = 1;

	return err;
}

static int skl_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct hdac_ext_stream *stream = get_hdac_ext_stream(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct skl_pipe_params p_params = {0};
	struct skl_module_cfg *m_cfg;
	int ret, dma_id;

	dev_dbg(dai->dev, "%s: %s\n", __func__, dai->name);
	ret = skl_substream_alloc_pages(ebus, substream,
					  params_buffer_bytes(params));
	if (ret < 0)
		return ret;

	dev_dbg(dai->dev, "format_val, rate=%d, ch=%d, format=%d\n",
			runtime->rate, runtime->channels, runtime->format);

	dma_id = hdac_stream(stream)->stream_tag - 1;
	dev_dbg(dai->dev, "dma_id=%d\n", dma_id);

	p_params.s_fmt = snd_pcm_format_width(params_format(params));
	p_params.ch = params_channels(params);
	p_params.s_freq = params_rate(params);
	p_params.host_dma_id = dma_id;
	p_params.stream = substream->stream;

	m_cfg = skl_tplg_fe_get_cpr_module(dai, p_params.stream);
	if (m_cfg)
		skl_tplg_update_pipe_params(dai->dev, m_cfg, &p_params);

	return 0;
}

static void skl_pcm_close(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *stream = get_hdac_ext_stream(substream);
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct skl_dma_params *dma_params = NULL;
	struct skl *skl = ebus_to_skl(ebus);
	struct skl_module_cfg *mconfig;

	dev_dbg(dai->dev, "%s: %s\n", __func__, dai->name);

	snd_hdac_ext_stream_release(stream, skl_get_host_stream_type(ebus));

	dma_params = snd_soc_dai_get_dma_data(dai, substream);
	/*
	 * now we should set this to NULL as we are freeing by the
	 * dma_params
	 */
	snd_soc_dai_set_dma_data(dai, substream, NULL);
	skl_set_suspend_active(substream, dai, false);

	/*
	 * check if close is for "Reference Pin" and set back the
	 * CGCTL.MISCBDCGE if disabled by driver
	 */
	if (!strncmp(dai->name, "Reference Pin", 13) &&
			skl->skl_sst->miscbdcg_disabled) {
		skl->skl_sst->enable_miscbdcge(dai->dev, true);
		skl->skl_sst->miscbdcg_disabled = false;
	}

	mconfig = skl_tplg_fe_get_cpr_module(dai, substream->stream);
	skl_tplg_d0i3_put(skl, mconfig->d0i3_caps);

	kfree(dma_params);
}

static int skl_pcm_hw_free(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct hdac_ext_stream *stream = get_hdac_ext_stream(substream);

	dev_dbg(dai->dev, "%s: %s\n", __func__, dai->name);

	snd_hdac_stream_cleanup(hdac_stream(stream));
	hdac_stream(stream)->prepared = 0;

	return skl_substream_free_pages(ebus_to_hbus(ebus), substream);
}

static int skl_be_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct skl_pipe_params p_params = {0};

	p_params.s_fmt = snd_pcm_format_width(params_format(params));
	p_params.ch = params_channels(params);
	p_params.s_freq = params_rate(params);
	p_params.stream = substream->stream;

	return skl_tplg_be_update_params(dai, &p_params);
}

static struct snd_soc_dai *skl_get_be_dai(struct snd_soc_pcm_runtime *fe,
		int stream)
{
	struct snd_soc_dpcm *dpcm;
	struct snd_soc_pcm_runtime *be;

	dpcm = list_first_entry(&fe->dpcm[stream].be_clients,
			struct snd_soc_dpcm, list_be);
	if (!dpcm)
		return NULL;
	be = dpcm->be;

	return be->cpu_dai;
}

/*
 * skl_azx_scale64: Scale base by mult/div while not overflowing sanely
 *
 * copied from sound/pci/hda/hda_controller.c
 *
 * The tmestamps for a 48Khz stream can overflow after (2^64/10^9)/48K which
 * is about 384307 ie ~4.5 days.
 *
 * This scales the calculation so that overflow will happen but after 2^64 /
 * 48000 secs, which is pretty large!
 *
 * In caln below:
 *	base may overflow, but since there isn’t any additional division
 *	performed on base it’s OK
 *	rem can’t overflow because both are 32-bit values
 */

static u64 skl_azx_scale64(u64 base, u32 num, u32 den)
{
	u64 rem;

	rem = do_div(base, den);

	base *= num;
	rem *= num;

	do_div(rem, den);

	return base + rem;
}

/*
 * Reads start stream offset for the gateway from the fw register. FW
 * registers store both start stream offset and end stream offset in 4
 * dwords. First 2 dwords for start stream and 2nd 2 dwords for end stream
 * offset.
 */
static int skl_get_startstreamoffset(struct skl *skl, struct skl_module_cfg *mconfig,
			struct snd_pcm_substream *ss, u64 *ss_offset_ns)
{
	void __iomem *offset_addr, *mmio_base;
	struct skl_pipe_params *params = mconfig->pipe->p_params;
	u64 soffset;
	u32 ssesoffset[4];
	u8 gtw_id;

	switch (mconfig->dev_type) {
	case SKL_DEVICE_I2S:
		gtw_id = mconfig->vbus_id;
		break;
	case SKL_DEVICE_DMIC:
		gtw_id = mconfig->vbus_id;
		break;
	case SKL_DEVICE_HDALINK:
		gtw_id = params->link_dma_id;
	case SKL_DEVICE_HDAHOST:
		gtw_id = params->host_dma_id;
		break;
	default:
		return -EINVAL;

	}

	mmio_base = pci_ioremap_bar(skl->pci, 4);

	/* 16 bytes is stored for each gateway */
	offset_addr = mmio_base + SKL_ADSP_FWREG_PPLBASE + (gtw_id * 16);
	memcpy_fromio(&ssesoffset, offset_addr, 16);

	/* Only 1st 2 dwords for start stream offset */
	soffset = (u64) ssesoffset[1] << 32 | ssesoffset[0];

	/*
	 * Convert into samples with link is transmitting in 32 bit
	 * container and 2 channel per pipeline.
	 */
	soffset = soffset/8;

	skl_azx_scale64(soffset, NSEC_PER_SEC, ss->runtime->rate);

	return 0;
}

static struct skl_module_cfg *get_mconfig_for_be_dai(
		struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct snd_soc_dai *cpu_dai_be;
	
	cpu_dai_be = skl_get_be_dai(rtd, substream->stream);
	if (!cpu_dai_be) {
		dev_err(rtd->cpu_dai->dev, "Back End DAI not found\n");
		return NULL;
	}

	/* Get Back End Copier Config */
	return skl_tplg_be_get_cpr_module(cpu_dai_be, substream->stream);
}

struct timestamp_context {
	struct skl *skl;
	struct snd_pcm_substream *substream;
	struct skl_module_cfg *m_cfg;
	struct system_counterval_t sys;
	ktime_t device_time;
	struct system_time_snapshot snapshot;
	u64 wallclk;
};

static int skl_get_dsp_timestamp(ktime_t *device,
		struct system_counterval_t *system, void *ctx)
{
	u32 array[9];
	int ret;
	u64 t_local_sample, t_wallclk, t_tscc;
	struct timestamp_context *context = ctx;
	struct snd_pcm_runtime *runtime = context->substream->runtime;

	ret  = skl_get_timestamp_info(context->skl->skl_sst,
				context->m_cfg, (u32 *)&array);
	if (ret < 0)
		return ret;

	t_local_sample = (u64) array[4] << 32 | array[3];
	t_wallclk = (u64) array[6] << 32 | array[5];
	t_tscc = (u64) array[8] << 32 | array[7];

	*device = ns_to_ktime(skl_azx_scale64(t_local_sample, NSEC_PER_SEC,
						runtime->rate));
	*system = convert_art_to_tsc(t_tscc);

	context->wallclk= skl_azx_scale64(t_wallclk, NSEC_PER_SEC, 24000000);

	return 0;
}

int skl_get_sync_time( ktime_t *device_time, struct system_counterval_t *sys,
								void *ctx )
{
	struct timestamp_context *ddev = ctx;

	*device_time = ddev->device_time;
	*sys = ddev->sys;

	return 0;
}

static int skl_get_crossstamp(struct system_device_crosststamp *xstamp,
						struct timestamp_context *ctx)
{
	int ret;

	ktime_get_snapshot(&ctx->snapshot );
	ret = skl_get_dsp_timestamp(&ctx->device_time, &ctx->sys, ctx);
	if (ret < 0)
		return ret;

	return get_device_system_crosststamp(skl_get_sync_time, ctx,
						&ctx->snapshot, xstamp);
}

/*
 * Read timestamp from firmware and return values in ns for wallclk and
 * sample counter. For tscc it return correlated system time.
 */
static int skl_read_timestamp_info(struct skl_module_cfg *m_cfg,
			struct snd_pcm_substream *substream,
			struct system_device_crosststamp *xstamp,
			struct skl *skl, u64 *wallclk_ns)
{


	int ret;
	struct timestamp_context context;

	context.skl = skl;
	context.substream = substream;
	context.m_cfg = m_cfg;

	ret = skl_get_crossstamp(xstamp, &context);
	if (ret < 0)
		return ret;

	*wallclk_ns = context.wallclk;

	return 0;
}

/* Return tscc in ns and timespec reference */
static int skl_convert_tscc(struct snd_pcm_substream *substream,
		struct system_device_crosststamp *xstamp,
		struct timespec *system_ts, u64 *system_ns)
{
	switch (substream->runtime->tstamp_type) {
	case SNDRV_PCM_TSTAMP_TYPE_MONOTONIC:
		return -EINVAL;

	case SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW:
		if (system_ns)
			*system_ns = ktime_to_ns(xstamp->sys_monoraw);
		if (system_ts)
			*system_ts = ktime_to_timespec(xstamp->sys_monoraw);
		break;

	default:
		if (system_ns)
			*system_ns = ktime_to_ns(xstamp->sys_realtime);
		if (system_ts)
			*system_ts = ktime_to_timespec(xstamp->sys_realtime);
		break;
	}

	return 0;
}

/*
 * Reading the timestamp value from the DSP immediately after the DMA start
 * may not reflect the correct trigger timestamp. So two different
 * timestamps (T1 and T2) are read with 10ms delay, a ratio is identified to
 * compute trigger tstamp(T0).
 */
static int skl_pcm_trigger_calc_ttime(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct hdac_ext_bus *ebus = get_bus_ctx(substream);
	struct skl *skl = ebus_to_skl(ebus);
	struct skl_module_cfg *m_cfg_fe, *m_cfg_be;
	struct system_device_crosststamp xstamp;
	u64 t1sample_ns, t1wallclk_ns, t1tscc_ns;
	u64 t2sample_ns, t2wallclk_ns;
	u64 t0wallclk_ns;
	u64 startstreamoffset_ns;
	u64 ratio, operator1;
	s64 trigger_value;
	int ret;

	dev_dbg(rtd->cpu_dai->dev, "In %s: CPU Dai: %s\n", __func__,
			rtd->cpu_dai->name);

	m_cfg_fe = skl_tplg_fe_get_cpr_module(rtd->cpu_dai, substream->stream);
	if (!m_cfg_fe) {
		dev_err(rtd->cpu_dai->dev, "Front End Copier Gateway not found\n");
		return -EINVAL;
	}

	/*
	 * The link may be enabled before the stream start. A snapshot of
	 * the link counter is taken when dma starts and stored in a stream
	 * start offset register. This will be used as a reference to
	 * calculate trigger timestamp.
	 */
	ret = skl_get_startstreamoffset(skl, m_cfg_fe, substream, &startstreamoffset_ns);
	if (ret < 0) {
		dev_err(rtd->cpu_dai->dev,
			"Error in getting stream offset for device type=%d\n",
			m_cfg_fe->dev_type);
		return -EINVAL;
	}

	/* Get Back End Copier Config */
	m_cfg_be = get_mconfig_for_be_dai(substream);
	if (!m_cfg_be) {
		dev_err(rtd->cpu_dai->dev, "Back End Copier not found\n");
		return -EINVAL;
	}

	/*
	 * If the fw timestamp values are read immediately after the dma is
	 * started, there is a possibility that num samples will be less
	 * than stream start offset and may result in a negative
	 * calculation. So wait a while before reading the first (T1)
	 * timestamp values.
	 */
	msleep(5);

	/* Read T1 from FW */
	ret = skl_read_timestamp_info(m_cfg_be, substream, &xstamp,
					skl, &t1wallclk_ns);
	if (ret < 0)
		return ret;

	t1sample_ns = ktime_to_ns(xstamp.device);
	ret = skl_convert_tscc(substream, &xstamp, NULL, &t1tscc_ns);
	if (ret < 0)
		return ret;

	/* Read T2 after 10 ms */
	msleep(10);
	ret = skl_read_timestamp_info(m_cfg_be, substream, &xstamp,
					skl, &t2wallclk_ns);
	if (ret < 0)
		return ret;
	t2sample_ns = ktime_to_ns(xstamp.device);

	/*
	 * Multiply with 1000000 to include fractional part. Dropped later
	 * before calculating final value
	 */
	ratio = ((t2wallclk_ns - t1wallclk_ns) * 1000000) /
			(t2sample_ns - t1sample_ns);
	dev_dbg(rtd->cpu_dai->dev, "ratio: %lld\n", ratio);

	/*
	 * T0_WallClock = T1_WallClock -
	 * 	(Ratio * (T1_LLPU_LLPL - StreamStartOffset))
	 */
	operator1 =  (ratio * (t1sample_ns - startstreamoffset_ns));
	operator1 = operator1/1000000;

	t0wallclk_ns = t1wallclk_ns - operator1;
	dev_dbg(rtd->cpu_dai->dev, "T0 wallclock Value: %lld\n", t0wallclk_ns);

	ret = skl_convert_tscc(substream, &xstamp, NULL, &t1tscc_ns);
	if (ret < 0)
		return ret;

	/* Trigger Time = T1_System - (T1_WallClock - T0_WallClock) */
	trigger_value = t1tscc_ns - (t1wallclk_ns - t0wallclk_ns);
	substream->runtime->trigger_tstamp = ns_to_timespec64(trigger_value);
	dev_dbg(rtd->cpu_dai->dev, "Trigger Value: %lld\n", trigger_value);

	/*
	 * Store t0 wallclock as reference to compute audio timestamp in
	 * get_time_info callback
	 */
	m_cfg_be->pipe->p_params->t0_wallclk = t0wallclk_ns;

	return 0;
}

static int skl_decoupled_trigger(struct snd_pcm_substream *substream,
		int cmd)
{
	struct hdac_ext_bus *ebus = get_bus_ctx(substream);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct hdac_ext_stream *stream;
	int start;
	unsigned long cookie;
	struct hdac_stream *hstr;

	stream = get_hdac_ext_stream(substream);
	hstr = hdac_stream(stream);

	if (!hstr->prepared)
		return -EPIPE;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		start = 1;
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		start = 0;
		break;

	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&bus->reg_lock, cookie);

	if (start) {
		snd_hdac_stream_start(hdac_stream(stream), true);
		snd_hdac_stream_timecounter_init(hstr, 0);
	} else {
		snd_hdac_stream_stop(hdac_stream(stream));
	}

	spin_unlock_irqrestore(&bus->reg_lock, cookie);

	return 0;
}

static int skl_pcm_trigger(struct snd_pcm_substream *substream, int cmd,
		struct snd_soc_dai *dai)
{
	struct skl *skl = get_skl_ctx(dai->dev);
	struct skl_sst *ctx = skl->skl_sst;
	struct skl_module_cfg *mconfig;
	struct hdac_ext_bus *ebus = get_bus_ctx(substream);
	struct hdac_ext_stream *stream = get_hdac_ext_stream(substream);
	struct snd_soc_dapm_widget *w;
	int ret;
	int start, ttime;

	mconfig = skl_tplg_fe_get_cpr_module(dai, substream->stream);
	if (!mconfig)
		return -EIO;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		w = dai->playback_widget;
	else
		w = dai->capture_widget;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!w->ignore_suspend) {
			skl_pcm_prepare(substream, dai);
			/*
			 * enable DMA Resume enable bit for the stream, set the
			 * dpib & lpib position to resume before starting the
			 * DMA
			 */
			snd_hdac_ext_stream_drsm_enable(ebus, true,
						hdac_stream(stream)->index);
			snd_hdac_ext_stream_set_dpibr(ebus, stream,
							stream->dpib);
			snd_hdac_ext_stream_set_lpib(stream, stream->lpib);
		}
		start = 1, ttime = 0;
		break;
	case SNDRV_PCM_TRIGGER_START:
		start = ttime = 1;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		start = 1;
		ttime = 0;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		start = ttime = 0;
		/*
		 * Stop FE Pipe first and stop DMA. This is to make sure that
		 * there are no underrun/overrun in the case if there is a delay
		 * between the two operations.
		 */
		ret = skl_stop_pipe(ctx, mconfig->pipe);
		if (ret < 0)
			return ret;

		ret = skl_decoupled_trigger(substream, cmd);
		if ((cmd == SNDRV_PCM_TRIGGER_SUSPEND) && !w->ignore_suspend) {
			/* save the dpib and lpib positions */
			stream->dpib = readl(ebus->bus.remap_addr +
					AZX_REG_VS_SDXDPIB_XBASE +
					(AZX_REG_VS_SDXDPIB_XINTERVAL *
					hdac_stream(stream)->index));

			stream->lpib = snd_hdac_stream_get_pos_lpib(
							hdac_stream(stream));
			snd_hdac_ext_stream_decouple(ebus, stream, false);
		}
		break;

	default:
		return -EINVAL;
	}

	if (start) {
		/*
		 * Start HOST DMA and Start FE Pipe.This is to make sure that
		 * there are no underrun/overrun in the case when the FE
		 * pipeline is started but there is a delay in starting the
		 * DMA channel on the host.
		 */
		snd_hdac_ext_stream_decouple(ebus, stream, true);
		ret = skl_decoupled_trigger(substream, cmd);
		if (ret < 0)
			return ret;
		ret = skl_run_pipe(ctx, mconfig->pipe);
		if (ret < 0)
			return ret;
	}

	if ((ttime) && ((ebus_to_hbus(ebus))->gtscap)) {

		ret = skl_pcm_trigger_calc_ttime(substream);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int skl_link_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct hdac_ext_stream *link_dev;
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct hdac_ext_dma_params *dma_params;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct skl_pipe_params p_params = {0};

	link_dev = snd_hdac_ext_stream_assign(ebus, substream,
					HDAC_EXT_STREAM_TYPE_LINK);
	if (!link_dev)
		return -EBUSY;

	snd_soc_dai_set_dma_data(dai, substream, (void *)link_dev);

	/* set the stream tag in the codec dai dma params  */
	dma_params = snd_soc_dai_get_dma_data(codec_dai, substream);
	if (dma_params)
		dma_params->stream_tag =  hdac_stream(link_dev)->stream_tag;

	p_params.s_fmt = snd_pcm_format_width(params_format(params));
	p_params.ch = params_channels(params);
	p_params.s_freq = params_rate(params);
	p_params.stream = substream->stream;
	p_params.link_dma_id = hdac_stream(link_dev)->stream_tag - 1;

	return skl_tplg_be_update_params(dai, &p_params);
}

static int skl_link_pcm_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct hdac_ext_stream *link_dev =
			snd_soc_dai_get_dma_data(dai, substream);
	unsigned int format_val = 0;
	struct skl_dma_params *dma_params;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct hdac_ext_link *link;
	struct skl *skl = get_skl_ctx(dai->dev);
	struct skl_module_cfg *mconfig = NULL;

	dma_params  = (struct skl_dma_params *)
			snd_soc_dai_get_dma_data(codec_dai, substream);
	if (dma_params)
		format_val = dma_params->format;
	dev_dbg(dai->dev, "stream_tag=%d formatvalue=%d codec_dai_name=%s\n",
			hdac_stream(link_dev)->stream_tag, format_val, codec_dai->name);

	link = snd_hdac_ext_bus_get_link(ebus, rtd->codec->component.name);
	if (!link)
		return -EINVAL;

	snd_hdac_ext_link_stream_reset(link_dev);

	/* In case of XRUN recovery, reset the FW pipe to clean state */
	mconfig = skl_tplg_be_get_cpr_module(dai, substream->stream);
	if (mconfig && (substream->runtime->status->state ==
					SNDRV_PCM_STATE_XRUN))
		skl_reset_pipe(skl->skl_sst, mconfig->pipe);

	snd_hdac_ext_link_stream_setup(link_dev, format_val);

	snd_hdac_ext_link_set_stream_id(link, hdac_stream(link_dev)->stream_tag);
	link_dev->link_prepared = 1;

	return 0;
}

static int skl_link_pcm_trigger(struct snd_pcm_substream *substream,
	int cmd, struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *link_dev =
				snd_soc_dai_get_dma_data(dai, substream);
	struct hdac_ext_bus *ebus = get_bus_ctx(substream);
	struct hdac_ext_stream *stream = get_hdac_ext_stream(substream);

	dev_dbg(dai->dev, "In %s cmd=%d\n", __func__, cmd);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_RESUME:
		skl_link_pcm_prepare(substream, dai);
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		snd_hdac_ext_stream_decouple(ebus, stream, true);
		snd_hdac_ext_link_stream_start(link_dev);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		snd_hdac_ext_link_stream_clear(link_dev);
		if (cmd == SNDRV_PCM_TRIGGER_SUSPEND)
			snd_hdac_ext_stream_decouple(ebus, stream, false);
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static int skl_link_hw_free(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct hdac_ext_stream *link_dev =
				snd_soc_dai_get_dma_data(dai, substream);
	struct hdac_ext_link *link;

	dev_dbg(dai->dev, "%s: %s\n", __func__, dai->name);

	link_dev->link_prepared = 0;

	link = snd_hdac_ext_bus_get_link(ebus, rtd->codec->component.name);
	if (!link)
		return -EINVAL;

	snd_hdac_ext_link_clear_stream_id(link, hdac_stream(link_dev)->stream_tag);
	snd_hdac_ext_stream_release(link_dev, HDAC_EXT_STREAM_TYPE_LINK);
	return 0;
}

static struct snd_soc_dai_ops skl_pcm_dai_ops = {
	.startup = skl_pcm_open,
	.shutdown = skl_pcm_close,
	.prepare = skl_pcm_prepare,
	.hw_params = skl_pcm_hw_params,
	.hw_free = skl_pcm_hw_free,
	.trigger = skl_pcm_trigger,
};

static struct snd_soc_dai_ops skl_dmic_dai_ops = {
	.hw_params = skl_be_hw_params,
};

static struct snd_soc_dai_ops skl_be_ssp_dai_ops = {
	.hw_params = skl_be_hw_params,
	.prepare = skl_be_prepare,
};

static struct snd_soc_dai_ops skl_link_dai_ops = {
	.prepare = skl_link_pcm_prepare,
	.hw_params = skl_link_hw_params,
	.hw_free = skl_link_hw_free,
	.trigger = skl_link_pcm_trigger,
};

static struct snd_soc_dai_driver skl_platform_dai[] = {
{
	.name = "System Pin",
	.ops = &skl_pcm_dai_ops,
	.playback = {
		.stream_name = "System Playback",
		.channels_min = HDA_MONO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_8000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture = {
		.stream_name = "System Capture",
		.channels_min = HDA_MONO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "Reference Pin",
	.ops = &skl_pcm_dai_ops,
	.capture = {
		.stream_name = "Reference Capture",
		.channels_min = HDA_MONO,
		.channels_max = HDA_QUAD,
		.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "Deepbuffer Pin",
	.ops = &skl_pcm_dai_ops,
	.playback = {
		.stream_name = "Deepbuffer Playback",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "LowLatency Pin",
	.ops = &skl_pcm_dai_ops,
	.playback = {
		.stream_name = "Low Latency Playback",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "DMIC Pin",
	.ops = &skl_pcm_dai_ops,
	.capture = {
		.stream_name = "DMIC Capture",
		.channels_min = HDA_MONO,
		.channels_max = HDA_QUAD,
		.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "HDMI1 Pin",
	.ops = &skl_pcm_dai_ops,
	.playback = {
		.stream_name = "HDMI1 Playback",
		.channels_min = HDA_STEREO,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_32000 |	SNDRV_PCM_RATE_44100 |
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
			SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
			SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			SNDRV_PCM_FMTBIT_S32_LE,
	},
},
{
	.name = "HDMI2 Pin",
	.ops = &skl_pcm_dai_ops,
	.playback = {
		.stream_name = "HDMI2 Playback",
		.channels_min = HDA_STEREO,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_32000 |	SNDRV_PCM_RATE_44100 |
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
			SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
			SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			SNDRV_PCM_FMTBIT_S32_LE,
	},
},
{
	.name = "HDMI3 Pin",
	.ops = &skl_pcm_dai_ops,
	.playback = {
		.stream_name = "HDMI3 Playback",
		.channels_min = HDA_STEREO,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_32000 |	SNDRV_PCM_RATE_44100 |
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
			SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
			SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			SNDRV_PCM_FMTBIT_S32_LE,
	},
},

/* BE CPU  Dais */
{
	.name = "SSP0 Pin",
	.ops = &skl_be_ssp_dai_ops,
	.playback = {
		.stream_name = "ssp0 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "ssp0 Rx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
{
	.name = "SSP1 Pin",
	.ops = &skl_be_ssp_dai_ops,
	.playback = {
		.stream_name = "ssp1 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "ssp1 Rx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
{
	.name = "SSP2 Pin",
	.ops = &skl_be_ssp_dai_ops,
	.playback = {
		.stream_name = "ssp2 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "ssp2 Rx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
{
	.name = "SSP3 Pin",
	.ops = &skl_be_ssp_dai_ops,
	.playback = {
		.stream_name = "ssp3 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "ssp3 Rx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
{
	.name = "SSP4 Pin",
	.ops = &skl_be_ssp_dai_ops,
	.playback = {
		.stream_name = "ssp4 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "ssp4 Rx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
{
	.name = "SSP5 Pin",
	.ops = &skl_be_ssp_dai_ops,
	.playback = {
		.stream_name = "ssp5 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "ssp5 Rx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
{
	.name = "iDisp1 Pin",
	.ops = &skl_link_dai_ops,
	.playback = {
		.stream_name = "iDisp1 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_8000|SNDRV_PCM_RATE_16000|SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE |
			SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "iDisp2 Pin",
	.ops = &skl_link_dai_ops,
	.playback = {
		.stream_name = "iDisp2 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_8000|SNDRV_PCM_RATE_16000|
			SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE |
			SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "iDisp3 Pin",
	.ops = &skl_link_dai_ops,
	.playback = {
		.stream_name = "iDisp3 Tx",
		.channels_min = HDA_STEREO,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_8000|SNDRV_PCM_RATE_16000|
			SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE |
			SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "DMIC01 Pin",
	.ops = &skl_dmic_dai_ops,
	.capture = {
		.stream_name = "DMIC01 Rx",
		.channels_min = HDA_MONO,
		.channels_max = HDA_QUAD,
		.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
},
{
	.name = "HD-Codec Pin",
	.ops = &skl_link_dai_ops,
	.playback = {
		.stream_name = "HD-Codec Tx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "HD-Codec Rx",
		.channels_min = HDA_STEREO,
		.channels_max = HDA_STEREO,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
};

static int skl_platform_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai_link *dai_link = rtd->dai_link;

	dev_dbg(rtd->cpu_dai->dev, "In %s:%s\n", __func__,
					dai_link->cpu_dai_name);

	runtime = substream->runtime;
	snd_soc_set_runtime_hwparams(substream, &azx_pcm_hw);

	return 0;
}

static int skl_coupled_trigger(struct snd_pcm_substream *substream,
					int cmd)
{
	struct hdac_ext_bus *ebus = get_bus_ctx(substream);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct hdac_ext_stream *stream;
	struct snd_pcm_substream *s;
	bool start;
	int sbits = 0;
	unsigned long cookie;
	struct hdac_stream *hstr;

	stream = get_hdac_ext_stream(substream);
	hstr = hdac_stream(stream);

	dev_dbg(bus->dev, "In %s cmd=%d\n", __func__, cmd);

	if (!hstr->prepared)
		return -EPIPE;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		start = true;
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		start = false;
		break;

	default:
		return -EINVAL;
	}

	snd_pcm_group_for_each_entry(s, substream) {
		if (s->pcm->card != substream->pcm->card)
			continue;
		stream = get_hdac_ext_stream(s);
		sbits |= 1 << hdac_stream(stream)->index;
		snd_pcm_trigger_done(s, substream);
	}

	spin_lock_irqsave(&bus->reg_lock, cookie);

	/* first, set SYNC bits of corresponding streams */
	snd_hdac_stream_sync_trigger(hstr, true, sbits, AZX_REG_SSYNC);

	snd_pcm_group_for_each_entry(s, substream) {
		if (s->pcm->card != substream->pcm->card)
			continue;
		stream = get_hdac_ext_stream(s);
		if (start)
			snd_hdac_stream_start(hdac_stream(stream), true);
		else
			snd_hdac_stream_stop(hdac_stream(stream));
	}
	spin_unlock_irqrestore(&bus->reg_lock, cookie);

	snd_hdac_stream_sync(hstr, start, sbits);

	spin_lock_irqsave(&bus->reg_lock, cookie);

	/* reset SYNC bits */
	snd_hdac_stream_sync_trigger(hstr, false, sbits, AZX_REG_SSYNC);
	if (start)
		snd_hdac_stream_timecounter_init(hstr, sbits);
	spin_unlock_irqrestore(&bus->reg_lock, cookie);

	return 0;
}

static int skl_platform_pcm_trigger(struct snd_pcm_substream *substream,
					int cmd)
{
	struct hdac_ext_bus *ebus = get_bus_ctx(substream);

	if (!(ebus_to_hbus(ebus))->ppcap)
		return skl_coupled_trigger(substream, cmd);

	return 0;
}

static snd_pcm_uframes_t skl_platform_pcm_pointer
			(struct snd_pcm_substream *substream)
{
	struct hdac_ext_stream *hstream = get_hdac_ext_stream(substream);
	struct hdac_ext_bus *ebus = get_bus_ctx(substream);
	unsigned int pos;

	/*
	 * Use DPIB for Playback stream as the periodic DMA Position-in-
	 * Buffer Writes may be scheduled at the same time or later than
	 * the MSI and does not guarantee to reflect the Position of the
	 * last buffer that was transferred. Whereas DPIB register in
	 * HAD space reflects the actual data that is transferred.
	 * Use the position buffer for capture, as DPIB write gets
	 * completed earlier than the actual data written to the DDR.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		pos = readl(ebus->bus.remap_addr + AZX_REG_VS_SDXDPIB_XBASE +
				(AZX_REG_VS_SDXDPIB_XINTERVAL *
				hdac_stream(hstream)->index));
	else
		pos = snd_hdac_stream_get_pos_posbuf(hdac_stream(hstream));

	if (pos >= hdac_stream(hstream)->bufsize)
		pos = 0;

	return bytes_to_frames(substream->runtime, pos);
}

static u64 skl_adjust_codec_delay(struct snd_pcm_substream *substream,
				u64 nsec)
{
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	u64 codec_frames, codec_nsecs;

	if (!codec_dai->driver->ops->delay)
		return nsec;

	codec_frames = codec_dai->driver->ops->delay(substream, codec_dai);
	codec_nsecs = div_u64(codec_frames * 1000000000LL,
			      substream->runtime->rate);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return nsec + codec_nsecs;

	return (nsec > codec_nsecs) ? nsec - codec_nsecs : 0;
}

static int skl_get_time_info(struct snd_pcm_substream *substream,
			struct timespec *system_ts, struct timespec *audio_ts,
			struct snd_pcm_audio_tstamp_config *audio_tstamp_config,
			struct snd_pcm_audio_tstamp_report *audio_tstamp_report)
{
	struct hdac_ext_stream *sstream = get_hdac_ext_stream(substream);
	struct hdac_stream *hstr = hdac_stream(sstream);
	u64 nsec;

	if ((substream->runtime->hw.info & SNDRV_PCM_INFO_HAS_LINK_ATIME) &&
		(audio_tstamp_config->type_requested == SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK)) {

		snd_pcm_gettime(substream->runtime, system_ts);

		nsec = timecounter_read(&hstr->tc);
		nsec = div_u64(nsec, 3); /* can be optimized */
		if (audio_tstamp_config->report_delay)
			nsec = skl_adjust_codec_delay(substream, nsec);

		*audio_ts = ns_to_timespec(nsec);

		audio_tstamp_report->actual_type = SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK;
		audio_tstamp_report->accuracy_report = 1; /* rest of struct is valid */
		audio_tstamp_report->accuracy = 42; /* 24MHzWallClk == 42ns resolution */

	} else {
		audio_tstamp_report->actual_type = SNDRV_PCM_AUDIO_TSTAMP_TYPE_DEFAULT;
	}

	return 0;
}

static const struct snd_pcm_ops skl_platform_ops = {
	.open = skl_platform_open,
	.ioctl = snd_pcm_lib_ioctl,
	.trigger = skl_platform_pcm_trigger,
	.pointer = skl_platform_pcm_pointer,
	.get_time_info =  skl_get_time_info,
	.mmap = snd_pcm_lib_default_mmap,
	.page = snd_pcm_sgbuf_ops_page,
};

static void skl_pcm_free(struct snd_pcm *pcm)
{
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

#define MAX_PREALLOC_SIZE	(32 * 1024 * 1024)

static int skl_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *dai = rtd->cpu_dai;
	struct hdac_ext_bus *ebus = dev_get_drvdata(dai->dev);
	struct snd_pcm *pcm = rtd->pcm;
	unsigned int size;
	int retval = 0;
	struct skl *skl = ebus_to_skl(ebus);

	if (dai->driver->playback.channels_min ||
		dai->driver->capture.channels_min) {
		/* buffer pre-allocation */
		size = CONFIG_SND_HDA_PREALLOC_SIZE * 1024;
		if (size > MAX_PREALLOC_SIZE)
			size = MAX_PREALLOC_SIZE;
		retval = snd_pcm_lib_preallocate_pages_for_all(pcm,
						SNDRV_DMA_TYPE_DEV_SG,
						snd_dma_pci_data(skl->pci),
						size, MAX_PREALLOC_SIZE);
		if (retval) {
			dev_err(dai->dev, "dma buffer allocationf fail\n");
			return retval;
		}
	}

	return retval;
}

static int skl_populate_modules(struct skl *skl)
{
	struct skl_pipeline *p;
	struct skl_pipe_module *m;
	struct snd_soc_dapm_widget *w;
	struct skl_module_cfg *mconfig;
	int ret;

	list_for_each_entry(p, &skl->ppl_list, node) {
		list_for_each_entry(m, &p->pipe->w_list, node) {

			w = m->w;
			mconfig = w->priv;

			ret = snd_skl_get_module_info(skl->skl_sst, mconfig);
			if (ret < 0) {
				dev_err(skl->skl_sst->dev,
					"query module info failed:%d\n", ret);
				goto err;
			}
		}
	}
err:
	return ret;
}

static int skl_platform_soc_probe(struct snd_soc_platform *platform)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(platform->dev);
	struct skl *skl = ebus_to_skl(ebus);
	const struct skl_dsp_ops *ops;
	int ret;

	pm_runtime_get_sync(platform->dev);
	if ((ebus_to_hbus(ebus))->ppcap) {
		ret = skl_tplg_init(platform, ebus);
		if (ret < 0) {
			dev_err(platform->dev, "Failed to init topology!\n");
			return ret;
		}
		skl->platform = platform;

		/* load the firmwares, since all is set */
		ops = skl_get_dsp_ops(skl->pci->device);
		if (!ops)
			return -EIO;

		if (skl->skl_sst->is_first_boot == false) {
			dev_err(platform->dev, "DSP reports first boot done!!!\n");
			return -EIO;
		}

		ret = ops->init_fw(platform->dev, skl->skl_sst);
		if (ret < 0) {
			dev_err(platform->dev, "Failed to boot first fw: %d\n", ret);
			return ret;
		}
		skl_populate_modules(skl);
		skl->skl_sst->update_d0i3c = skl_update_d0i3c;
	}
	pm_runtime_mark_last_busy(platform->dev);
	pm_runtime_put_autosuspend(platform->dev);

	return 0;
}
static struct snd_soc_platform_driver skl_platform_drv  = {
	.probe		= skl_platform_soc_probe,
	.ops		= &skl_platform_ops,
	.pcm_new	= skl_pcm_new,
	.pcm_free	= skl_pcm_free,
};

static const struct snd_soc_component_driver skl_component = {
	.name           = "pcm",
};

int skl_platform_register(struct device *dev)
{
	int ret;
	struct hdac_ext_bus *ebus = dev_get_drvdata(dev);
	struct skl *skl = ebus_to_skl(ebus);

	INIT_LIST_HEAD(&skl->ppl_list);

	ret = snd_soc_register_platform(dev, &skl_platform_drv);
	if (ret) {
		dev_err(dev, "soc platform registration failed %d\n", ret);
		return ret;
	}
	ret = snd_soc_register_component(dev, &skl_component,
				skl_platform_dai,
				ARRAY_SIZE(skl_platform_dai));
	if (ret) {
		dev_err(dev, "soc component registration failed %d\n", ret);
		snd_soc_unregister_platform(dev);
	}

	return ret;

}

int skl_platform_unregister(struct device *dev)
{
	snd_soc_unregister_component(dev);
	snd_soc_unregister_platform(dev);
	return 0;
}
