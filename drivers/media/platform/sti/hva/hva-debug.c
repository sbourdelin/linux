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
	static char str[200] = "";
	char *cur = str;
	size_t left = sizeof(str);
	int cnt = 0;
	int ret = 0;

	/* frame info */
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

	/* encoding info */
	cur += cnt;
	left -= cnt;
	ret = snprintf(cur, left, "%d frames encoded", ctx->encoded_frames);
	cnt = (left > ret ? ret : left);

	/* error info */
	if (ctx->sys_errors) {
		cur += cnt;
		left -= cnt;
		ret = snprintf(cur, left, ", %d system errors",
			       ctx->sys_errors);
		cnt = (left > ret ? ret : left);
	}

	if (ctx->encode_errors) {
		cur += cnt;
		left -= cnt;
		ret = snprintf(cur, left, ", %d encoding errors",
			       ctx->encode_errors);
		cnt = (left > ret ? ret : left);
	}

	if (ctx->frame_errors) {
		cur += cnt;
		left -= cnt;
		ret = snprintf(cur, left, ", %d frame errors",
			       ctx->frame_errors);
		cnt = (left > ret ? ret : left);
	}

	return str;
}
