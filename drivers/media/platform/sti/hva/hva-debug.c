/*
 * Copyright (C) STMicroelectronics SA 2015
 * Authors: Yannick Fertre <yannick.fertre@st.com>
 *          Hugues Fruchet <hugues.fruchet@st.com>
 * License terms:  GNU General Public License (GPL), version 2
 */

#include "hva.h"

/*
 * encoding summary
 */

char *hva_dbg_summary(struct hva_ctx *ctx)
{
	struct hva_streaminfo *stream = &ctx->streaminfo;
	struct hva_frameinfo *frame = &ctx->frameinfo;
	struct hva_ctx_dbg *dbg = &ctx->dbg;
	static char str[200] = "";
	char *cur = str;
	size_t left = sizeof(str);
	int cnt = 0;
	int ret = 0;
	u32 errors;

	/* frame info */
	cur += cnt;
	left -= cnt;
	ret = snprintf(cur, left, "%4.4s %dx%d > ",
		       (char *)&frame->pixelformat,
		       frame->aligned_width, frame->aligned_height);
	cnt = (left > ret ? ret : left);

	/* stream info */
	cur += cnt;
	left -= cnt;
	ret = snprintf(cur, left, "%4.4s %dx%d %s %s: ",
		       (char *)&stream->streamformat,
		       stream->width, stream->height,
		       stream->profile, stream->level);
	cnt = (left > ret ? ret : left);

	/* performance info */
	cur += cnt;
	left -= cnt;
	ret = snprintf(cur, left, "%d frames encoded", dbg->cnt_duration);
	cnt = (left > ret ? ret : left);

	if (dbg->cnt_duration && dbg->total_duration) {
		u64 div;
		u32 fps;

		div = (u64)dbg->cnt_duration * 100000;
		do_div(div, dbg->total_duration);
		fps = (u32)div;
		cur += cnt;
		left -= cnt;
		ret = snprintf(cur, left, ", max fps (0.1Hz)=%d", fps);
		cnt = (left > ret ? ret : left);
	}

	/* error info */
	errors = dbg->sys_errors + dbg->encode_errors + dbg->frame_errors;
	if (errors) {
		cur += cnt;
		left -= cnt;
		ret = snprintf(cur, left, ", %d errors", errors);
		cnt = (left > ret ? ret : left);
	}

	return str;
}

/*
 * performance debug info
 */

void hva_dbg_perf_begin(struct hva_ctx *ctx)
{
	struct hva_ctx_dbg *dbg = &ctx->dbg;

	dbg->begin = ktime_get();

	/*
	 * filter sequences valid for performance:
	 * - begin/begin (no stream available) is an invalid sequence
	 * - begin/end is a valid sequence
	 */
	dbg->is_valid_period = false;
}

void hva_dbg_perf_end(struct hva_ctx *ctx, struct hva_stream *stream)
{
	struct device *dev = ctx_to_dev(ctx);
	u64 div;
	u32 duration;
	u32 bytesused;
	u32 timestamp;
	struct hva_ctx_dbg *dbg = &ctx->dbg;
	ktime_t end = ktime_get();

	/* stream bytesused and timestamp in us */
	bytesused = vb2_get_plane_payload(&stream->vbuf.vb2_buf, 0);
	div = stream->vbuf.vb2_buf.timestamp;
	do_div(div, 1000);
	timestamp = (u32)div;

	/* encoding duration */
	div = (u64)ktime_us_delta(end, dbg->begin);

	dev_dbg(dev,
		"%s perf stream[%d] dts=%d encoded using %d bytes in %d us",
		ctx->name,
		stream->vbuf.sequence,
		timestamp,
		bytesused, (u32)div);

	do_div(div, 100);
	duration = (u32)div;

	dbg->total_duration += duration;
	dbg->cnt_duration++;

	dbg->is_valid_period = true;
}
