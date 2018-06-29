// SPDX-License-Identifier: GPL-2.0+
//
// DVB USB compliant linux driver for Conexant USB reference design -
// (analog part).
//
// Copyright (C) 2011, 2017 Maciej S. Szmigiero (mail@maciej.szmigiero.name)
//
// TODO:
//  * audio support,
//  * finish radio support (requires audio of course),
//  * VBI support,
//  * controls support

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>
#include <media/drv-intf/cx25840.h>
#include <media/tuner.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "cxusb.h"

static int cxusb_medion_v_queue_setup(struct vb2_queue *q,
				      unsigned int *num_buffers,
				      unsigned int *num_planes,
				      unsigned int sizes[],
				      struct device *alloc_devs[])
{
	struct dvb_usb_device *dvbdev = vb2_get_drv_priv(q);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	unsigned int size = cxdev->raw_mode ?
		CXUSB_VIDEO_MAX_FRAME_SIZE :
		cxdev->width * cxdev->height * 2;

	if (*num_planes > 0) {
		if (*num_planes != 1)
			return -EINVAL;

		if (sizes[0] < size)
			return -EINVAL;
	} else {
		*num_planes = 1;
		sizes[0] = size;
	}

	if (q->num_buffers + *num_buffers < 6)
		*num_buffers = 6 - q->num_buffers;

	return 0;
}

static int cxusb_medion_v_buf_init(struct vb2_buffer *vb)
{
	struct dvb_usb_device *dvbdev = vb2_get_drv_priv(vb->vb2_queue);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	cxusb_vprintk(dvbdev, OPS, "buffer init\n");

	if (cxdev->raw_mode) {
		if (vb2_plane_size(vb, 0) < CXUSB_VIDEO_MAX_FRAME_SIZE)
			return -ENOMEM;
	} else {
		if (vb2_plane_size(vb, 0) < cxdev->width * cxdev->height * 2)
			return -ENOMEM;
	}

	cxusb_vprintk(dvbdev, OPS, "buffer OK\n");

	return 0;
}

static void cxusb_auxbuf_init(struct cxusb_medion_auxbuf *auxbuf,
			      u8 *buf, unsigned int len)
{
	auxbuf->buf = buf;
	auxbuf->len = len;
	auxbuf->paylen = 0;
}

static void cxusb_auxbuf_head_trim(struct dvb_usb_device *dvbdev,
				   struct cxusb_medion_auxbuf *auxbuf,
				   unsigned int pos)
{
	if (pos == 0)
		return;

	if (WARN_ON(pos > auxbuf->paylen))
		return;

	cxusb_vprintk(dvbdev, AUXB,
		      "trimming auxbuf len by %u to %u\n",
		      pos, auxbuf->paylen - pos);

	memmove(auxbuf->buf, auxbuf->buf + pos, auxbuf->paylen - pos);
	auxbuf->paylen -= pos;
}

static unsigned int cxusb_auxbuf_paylen(struct cxusb_medion_auxbuf *auxbuf)
{
	return auxbuf->paylen;
}

static bool cxusb_auxbuf_make_space(struct dvb_usb_device *dvbdev,
				    struct cxusb_medion_auxbuf *auxbuf,
				    unsigned int howmuch)
{
	unsigned int freespace;

	if (WARN_ON(howmuch >= auxbuf->len))
		howmuch = auxbuf->len - 1;

	freespace = auxbuf->len - cxusb_auxbuf_paylen(auxbuf);

	cxusb_vprintk(dvbdev, AUXB, "freespace is %u\n", freespace);

	if (freespace >= howmuch)
		return true;

	howmuch -= freespace;

	cxusb_vprintk(dvbdev, AUXB, "will overwrite %u bytes of buffer\n",
		      howmuch);

	cxusb_auxbuf_head_trim(dvbdev, auxbuf, howmuch);

	return false;
}

/* returns false if some data was overwritten */
static bool cxusb_auxbuf_append_urb(struct dvb_usb_device *dvbdev,
				    struct cxusb_medion_auxbuf *auxbuf,
				    struct urb *urb)
{
	unsigned long len = 0;
	int i;
	bool ret;

	for (i = 0; i < urb->number_of_packets; i++)
		len += urb->iso_frame_desc[i].actual_length;

	ret = cxusb_auxbuf_make_space(dvbdev, auxbuf, len);

	for (i = 0; i < urb->number_of_packets; i++) {
		unsigned int to_copy;

		to_copy = urb->iso_frame_desc[i].actual_length;

		memcpy(auxbuf->buf + auxbuf->paylen, urb->transfer_buffer +
		       urb->iso_frame_desc[i].offset, to_copy);

		auxbuf->paylen += to_copy;
	}

	return ret;
}

static bool cxusb_auxbuf_copy(struct cxusb_medion_auxbuf *auxbuf,
			      unsigned int pos, unsigned char *dest,
			      unsigned int len)
{
	if (pos + len > auxbuf->paylen)
		return false;

	memcpy(dest, auxbuf->buf + pos, len);

	return true;
}

static unsigned int cxusb_auxbuf_advance(struct cxusb_medion_auxbuf *auxbuf,
					 unsigned int pos,
					 unsigned int increment)
{
	return pos + increment;
}

static unsigned int cxusb_auxbuf_begin(struct cxusb_medion_auxbuf *auxbuf)
{
	return 0;
}

static bool cxusb_auxbuf_isend(struct cxusb_medion_auxbuf *auxbuf,
			       unsigned int pos)
{
	return pos >= auxbuf->paylen;
}

static bool cxusb_medion_cf_refc_fld_chg(struct dvb_usb_device *dvbdev,
					 struct cxusb_bt656_params *bt656,
					 bool firstfield,
					 unsigned int maxlines,
					 unsigned int maxlinesamples,
					 unsigned char buf[4])
{
	bool firstfield_code = (buf[3] & CXUSB_BT656_FIELD_MASK) ==
		CXUSB_BT656_FIELD_1;
	unsigned int remlines;

	if (bt656->line == 0 || firstfield == firstfield_code)
		return false;

	if (bt656->fmode == LINE_SAMPLES) {
		unsigned int remsamples = maxlinesamples -
			bt656->linesamples;

		cxusb_vprintk(dvbdev, BT656,
			      "field %c after line %u field change\n",
			      firstfield ? '1' : '2', bt656->line);

		if (bt656->buf != NULL && remsamples > 0) {
			memset(bt656->buf, 0, remsamples);
			bt656->buf += remsamples;

			cxusb_vprintk(dvbdev, BT656,
				      "field %c line %u %u samples still remaining (of %u)\n",
				      firstfield ? '1' : '2',
				      bt656->line, remsamples,
				      maxlinesamples);
		}

		bt656->line++;
	}

	remlines = maxlines - bt656->line;
	if (bt656->buf != NULL && remlines > 0) {
		memset(bt656->buf, 0, remlines * maxlinesamples);
		bt656->buf += remlines * maxlinesamples;

		cxusb_vprintk(dvbdev, BT656,
			      "field %c %u lines still remaining (of %u)\n",
			      firstfield ? '1' : '2', remlines,
			      maxlines);
	}

	return true;
}

static bool cxusb_medion_cf_refc_start_sch(struct dvb_usb_device *dvbdev,
					   struct cxusb_medion_auxbuf *auxbuf,
					   struct cxusb_bt656_params *bt656,
					   bool firstfield,
					   unsigned char buf[4])
{
	bool firstfield_code = (buf[3] & CXUSB_BT656_FIELD_MASK) ==
		CXUSB_BT656_FIELD_1;
	bool sav_code = (buf[3] & CXUSB_BT656_SEAV_MASK) ==
		CXUSB_BT656_SEAV_SAV;
	bool vbi_code = (buf[3] & CXUSB_BT656_VBI_MASK) ==
		CXUSB_BT656_VBI_ON;

	if (bt656->fmode != START_SEARCH)
		return false;

	if (sav_code && firstfield == firstfield_code) {
		if (!vbi_code) {
			cxusb_vprintk(dvbdev, BT656, "line start @ pos %x\n",
				      bt656->pos);

			bt656->linesamples = 0;
			bt656->fmode = LINE_SAMPLES;
		} else {
			cxusb_vprintk(dvbdev, BT656, "VBI start @ pos %x\n",
				      bt656->pos);

			bt656->fmode = VBI_SAMPLES;
		}
	}

	bt656->pos = cxusb_auxbuf_advance(auxbuf, bt656->pos, 4);

	return true;
}

static bool cxusb_medion_cf_refc_line_smpl(struct dvb_usb_device *dvbdev,
					   struct cxusb_bt656_params *bt656,
					   bool firstfield,
					   unsigned int maxlinesamples,
					   unsigned char buf[4])
{
	bool sav_code = (buf[3] & CXUSB_BT656_SEAV_MASK) ==
		CXUSB_BT656_SEAV_SAV;
	unsigned int remsamples;

	if (bt656->fmode != LINE_SAMPLES)
		return false;

	if (sav_code)
		cxusb_vprintk(dvbdev, BT656,
			      "SAV in line samples @ line %u, pos %x\n",
			      bt656->line, bt656->pos);

	remsamples = maxlinesamples - bt656->linesamples;
	if (bt656->buf != NULL && remsamples > 0) {
		memset(bt656->buf, 0, remsamples);
		bt656->buf += remsamples;

		cxusb_vprintk(dvbdev, BT656,
			      "field %c line %u %u samples still remaining (of %u)\n",
			      firstfield ? '1' : '2', bt656->line, remsamples,
			      maxlinesamples);
	}

	bt656->fmode = START_SEARCH;
	bt656->line++;

	return true;
}

static bool cxusb_medion_cf_refc_vbi_smpl(struct dvb_usb_device *dvbdev,
					  struct cxusb_bt656_params *bt656,
					  unsigned char buf[4])
{
	bool sav_code = (buf[3] & CXUSB_BT656_SEAV_MASK) ==
		CXUSB_BT656_SEAV_SAV;

	if (bt656->fmode != VBI_SAMPLES)
		return false;

	if (sav_code)
		cxusb_vprintk(dvbdev, BT656, "SAV in VBI samples @ pos %x\n",
			      bt656->pos);

	bt656->fmode = START_SEARCH;

	return true;
}

static void cxusb_medion_cf_ref_code(struct dvb_usb_device *dvbdev,
				     struct cxusb_medion_auxbuf *auxbuf,
				     struct cxusb_bt656_params *bt656,
				     bool firstfield,
				     unsigned int maxlines,
				     unsigned int maxlinesamples,
				     unsigned char buf[4])
{
	if (cxusb_medion_cf_refc_start_sch(dvbdev, auxbuf, bt656, firstfield,
					   buf))
		return;

	if (cxusb_medion_cf_refc_line_smpl(dvbdev, bt656, firstfield,
					   maxlinesamples, buf))
		return;

	if (cxusb_medion_cf_refc_vbi_smpl(dvbdev, bt656, buf))
		return;

	bt656->pos = cxusb_auxbuf_advance(auxbuf, bt656->pos, 4);
}

static bool cxusb_medion_cs_start_sch(struct dvb_usb_device *dvbdev,
				      struct cxusb_medion_auxbuf *auxbuf,
				      struct cxusb_bt656_params *bt656,
				      unsigned int maxlinesamples)
{
	unsigned char buf[64];
	unsigned int idx;
	unsigned int tocheck = clamp_t(size_t, maxlinesamples / 4, 3,
				       sizeof(buf));

	if (bt656->fmode != START_SEARCH || bt656->line == 0)
		return false;

	if (!cxusb_auxbuf_copy(auxbuf, bt656->pos + 1, buf, tocheck)) {
		bt656->pos = cxusb_auxbuf_advance(auxbuf, bt656->pos, 1);
		return true;
	}

	for (idx = 0; idx <= tocheck - 3; idx++)
		if (memcmp(buf + idx, CXUSB_BT656_PREAMBLE, 3) == 0)
			break;

	if (idx <= tocheck - 3) {
		bt656->pos = cxusb_auxbuf_advance(auxbuf, bt656->pos, 1);
		return true;
	}

	cxusb_vprintk(dvbdev, BT656, "line %u early start, pos %x\n",
		      bt656->line, bt656->pos);

	bt656->linesamples = 0;
	bt656->fmode = LINE_SAMPLES;

	return true;
}

static bool cxusb_medion_cs_line_smpl(struct dvb_usb_device *dvbdev,
				      struct cxusb_medion_auxbuf *auxbuf,
				      struct cxusb_bt656_params *bt656,
				      unsigned int maxlinesamples,
				      unsigned char val)
{
	if (bt656->fmode != LINE_SAMPLES)
		return false;

	if (bt656->buf != NULL)
		*(bt656->buf++) = val;

	bt656->linesamples++;
	bt656->pos = cxusb_auxbuf_advance(auxbuf, bt656->pos, 1);

	if (bt656->linesamples >= maxlinesamples) {
		bt656->fmode = START_SEARCH;
		bt656->line++;
	}

	return true;
}

static void cxusb_medion_copy_samples(struct dvb_usb_device *dvbdev,
				      struct cxusb_medion_auxbuf *auxbuf,
				      struct cxusb_bt656_params *bt656,
				      unsigned int maxlinesamples,
				      unsigned char val)
{
	if (cxusb_medion_cs_start_sch(dvbdev, auxbuf, bt656, maxlinesamples))
		return;

	if (cxusb_medion_cs_line_smpl(dvbdev, auxbuf, bt656, maxlinesamples,
				      val))
		return;

	/* TODO: copy VBI samples */
	bt656->pos = cxusb_auxbuf_advance(auxbuf, bt656->pos, 1);
}

static bool cxusb_medion_copy_field(struct dvb_usb_device *dvbdev,
				    struct cxusb_medion_auxbuf *auxbuf,
				    struct cxusb_bt656_params *bt656,
				    bool firstfield,
				    unsigned int maxlines,
				    unsigned int maxlinesmpls)
{
	while (bt656->line < maxlines &&
	       !cxusb_auxbuf_isend(auxbuf, bt656->pos)) {
		unsigned char val;

		if (!cxusb_auxbuf_copy(auxbuf, bt656->pos, &val, 1))
			return false;

		if (val == CXUSB_BT656_PREAMBLE[0]) {
			unsigned char buf[4];

			buf[0] = val;
			if (!cxusb_auxbuf_copy(auxbuf, bt656->pos + 1,
					       buf + 1, 3))
				return false;

			if (buf[1] == CXUSB_BT656_PREAMBLE[1] &&
			    buf[2] == CXUSB_BT656_PREAMBLE[2]) {
				if (cxusb_medion_cf_refc_fld_chg(dvbdev,
								 bt656,
								 firstfield,
								 maxlines,
								 maxlinesmpls,
								 buf))
					return true;

				cxusb_medion_cf_ref_code(dvbdev, auxbuf,
							 bt656, firstfield,
							 maxlines,
							 maxlinesmpls, buf);

				continue;
			}
		}

		cxusb_medion_copy_samples(dvbdev, auxbuf, bt656, maxlinesmpls,
					  val);
	}

	if (bt656->line < maxlines) {
		cxusb_vprintk(dvbdev, BT656,
			      "end of buffer pos = %u, line = %u\n",
			      bt656->pos, bt656->line);
		return false;
	}

	return true;
}

static void cxusb_medion_v_process_urb_raw_mode(struct cxusb_medion_dev *cxdev,
						struct urb *urb)
{
	struct dvb_usb_device *dvbdev = cxdev->dvbdev;
	u8 *buf;
	struct cxusb_medion_vbuffer *vbuf;
	int i;
	unsigned long len = 0;

	if (list_empty(&cxdev->buflist)) {
		dev_warn(&dvbdev->udev->dev, "no free buffers\n");
		return;
	}

	vbuf = list_first_entry(&cxdev->buflist, struct cxusb_medion_vbuffer,
				list);
	list_del(&vbuf->list);

	vbuf->vb2.timestamp = ktime_get_ns();

	buf = vb2_plane_vaddr(&vbuf->vb2, 0);

	for (i = 0; i < urb->number_of_packets; i++) {
		memcpy(buf, urb->transfer_buffer +
		       urb->iso_frame_desc[i].offset,
		       urb->iso_frame_desc[i].actual_length);

		buf += urb->iso_frame_desc[i].actual_length;
		len += urb->iso_frame_desc[i].actual_length;
	}

	vb2_set_plane_payload(&vbuf->vb2, 0, len);

	vb2_buffer_done(&vbuf->vb2, VB2_BUF_STATE_DONE);
}

static void cxusb_medion_v_process_urb(struct cxusb_medion_dev *cxdev,
				       struct urb *urb)
{
	struct dvb_usb_device *dvbdev = cxdev->dvbdev;
	struct cxusb_bt656_params *bt656 = &cxdev->bt656;
	bool reset;

	cxusb_vprintk(dvbdev, URB, "appending urb\n");

	/*
	 * append new data to circ. buffer
	 * overwrite old data if necessary
	 */
	reset = !cxusb_auxbuf_append_urb(dvbdev, &cxdev->auxbuf, urb);

	/*
	 * if this is a new frame
	 * fetch a buffer from list
	 */
	if (bt656->mode == NEW_FRAME) {
		if (!list_empty(&cxdev->buflist)) {
			cxdev->vbuf =
				list_first_entry(&cxdev->buflist,
						 struct cxusb_medion_vbuffer,
						 list);
			list_del(&cxdev->vbuf->list);

			cxdev->vbuf->vb2.timestamp = ktime_get_ns();
		} else
			dev_warn(&dvbdev->udev->dev, "no free buffers\n");
	}

	if (bt656->mode == NEW_FRAME || reset) {
		bt656->pos = cxusb_auxbuf_begin(&cxdev->auxbuf);
		bt656->mode = FIRST_FIELD;
		bt656->fmode = START_SEARCH;
		bt656->line = 0;

		if (cxdev->vbuf != NULL)
			bt656->buf = vb2_plane_vaddr(&cxdev->vbuf->vb2, 0);
	}

	cxusb_vprintk(dvbdev, URB, "auxbuf payload len %u",
		      cxusb_auxbuf_paylen(&cxdev->auxbuf));

	if (bt656->mode == FIRST_FIELD) {
		cxusb_vprintk(dvbdev, URB, "copying field 1\n");

		if (!cxusb_medion_copy_field(dvbdev, &cxdev->auxbuf, bt656,
					     true, cxdev->height / 2,
					     cxdev->width * 2))
			return;

		/*
		 * do not trim buffer there in case
		 * we need to reset search later
		 */
		bt656->mode = SECOND_FIELD;
		bt656->fmode = START_SEARCH;
		bt656->line = 0;
	}

	if (bt656->mode == SECOND_FIELD) {
		cxusb_vprintk(dvbdev, URB, "copying field 2\n");

		if (!cxusb_medion_copy_field(dvbdev, &cxdev->auxbuf, bt656,
					     false, cxdev->height / 2,
					     cxdev->width * 2))
			return;

		cxusb_auxbuf_head_trim(dvbdev, &cxdev->auxbuf, bt656->pos);

		bt656->mode = NEW_FRAME;

		if (cxdev->vbuf != NULL) {
			vb2_set_plane_payload(&cxdev->vbuf->vb2, 0,
					      cxdev->width * cxdev->height * 2);

			vb2_buffer_done(&cxdev->vbuf->vb2, VB2_BUF_STATE_DONE);

			cxdev->vbuf = NULL;
			cxdev->bt656.buf = NULL;

			cxusb_vprintk(dvbdev, URB, "frame done\n");
		} else
			cxusb_vprintk(dvbdev, URB, "frame skipped\n");
	}
}

static void cxusb_medion_v_complete_work(struct work_struct *work)
{
	struct cxusb_medion_dev *cxdev = container_of(work,
						      struct cxusb_medion_dev,
						      urbwork);
	struct dvb_usb_device *dvbdev = cxdev->dvbdev;
	struct urb *urb;
	int ret;
	unsigned int i, urbn;

	mutex_lock(cxdev->videodev->lock);

	cxusb_vprintk(dvbdev, URB, "worker called, streaming = %d\n",
		      (int)cxdev->streaming);

	if (!cxdev->streaming)
		goto unlock;

	urbn = cxdev->nexturb;
	if (test_bit(urbn, &cxdev->urbcomplete)) {
		urb = cxdev->streamurbs[urbn];
		clear_bit(urbn, &cxdev->urbcomplete);

		cxdev->nexturb++;
		cxdev->nexturb %= CXUSB_VIDEO_URBS;
	} else {
		for (i = 0, urbn++; i < CXUSB_VIDEO_URBS - 1; i++, urbn++) {
			urbn %= CXUSB_VIDEO_URBS;
			if (test_bit(urbn, &cxdev->urbcomplete)) {
				urb = cxdev->streamurbs[urbn];
				clear_bit(urbn, &cxdev->urbcomplete);
				break;
			}
		}

		if (i >= CXUSB_VIDEO_URBS - 1) {
			cxusb_vprintk(dvbdev, URB,
				      "URB worker called but no URB ready\n");
			goto unlock;
		}

		cxusb_vprintk(dvbdev, URB,
			      "out-of-order URB: expected %u but %u is ready\n",
			      cxdev->nexturb, urbn);

		cxdev->nexturb = urbn + 1;
		cxdev->nexturb %= CXUSB_VIDEO_URBS;
	}

	cxusb_vprintk(dvbdev, URB, "URB %u status = %d\n", urbn, urb->status);

	if (urb->status == 0 || urb->status == -EXDEV) {
		int i;
		unsigned long len = 0;

		for (i = 0; i < urb->number_of_packets; i++)
			len += urb->iso_frame_desc[i].actual_length;

		cxusb_vprintk(dvbdev, URB, "URB %u data len = %lu\n",
			      urbn, len);

		if (len > 0) {
			if (cxdev->raw_mode)
				cxusb_medion_v_process_urb_raw_mode(cxdev, urb);
			else
				cxusb_medion_v_process_urb(cxdev, urb);
		}
	}

	cxusb_vprintk(dvbdev, URB, "URB %u submit\n", urbn);

	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret != 0)
		dev_err(&dvbdev->udev->dev,
			"unable to submit URB (%d), you'll have to restart streaming\n",
			ret);

	for (i = 0; i < CXUSB_VIDEO_URBS; i++)
		if (test_bit(i, &cxdev->urbcomplete)) {
			schedule_work(&cxdev->urbwork);
			break;
		}

unlock:
	mutex_unlock(cxdev->videodev->lock);
}

static void cxusb_medion_v_complete(struct urb *u)
{
	struct dvb_usb_device *dvbdev = u->context;
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	unsigned int i;

	for (i = 0; i < CXUSB_VIDEO_URBS; i++)
		if (cxdev->streamurbs[i] == u)
			break;

	if (i >= CXUSB_VIDEO_URBS) {
		dev_err(&dvbdev->udev->dev,
			"complete on unknown URB\n");
		return;
	}

	cxusb_vprintk(dvbdev, URB, "URB %d complete\n", i);

	set_bit(i, &cxdev->urbcomplete);
	schedule_work(&cxdev->urbwork);
}

static bool cxusb_medion_stream_busy(struct cxusb_medion_dev *cxdev)
{
	int i;

	if (cxdev->streaming)
		return true;

	for (i = 0; i < CXUSB_VIDEO_URBS; i++)
		/*
		 * not streaming but URB is still active -
		 * stream is being stopped
		 */
		if (cxdev->streamurbs[i] != NULL)
			return true;

	return false;
}

static void cxusb_medion_return_buffers(struct cxusb_medion_dev *cxdev,
					bool requeue)
{
	struct cxusb_medion_vbuffer *vbuf, *vbuf_tmp;

	list_for_each_entry_safe(vbuf, vbuf_tmp, &cxdev->buflist,
				 list) {
		list_del(&vbuf->list);
		vb2_buffer_done(&vbuf->vb2, requeue ? VB2_BUF_STATE_QUEUED :
				VB2_BUF_STATE_ERROR);
	}

	if (cxdev->vbuf != NULL) {
		vb2_buffer_done(&cxdev->vbuf->vb2, requeue ?
				VB2_BUF_STATE_QUEUED :
				VB2_BUF_STATE_ERROR);

		cxdev->vbuf = NULL;
		cxdev->bt656.buf = NULL;
	}
}

static int cxusb_medion_v_start_streaming(struct vb2_queue *q,
					  unsigned int count)
{
	struct dvb_usb_device *dvbdev = vb2_get_drv_priv(q);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	u8 streamon_params[2] = { 0x03, 0x00 };
	int npackets, i;
	int ret;

	cxusb_vprintk(dvbdev, OPS, "should start streaming\n");

	/* already streaming */
	if (cxdev->streaming)
		return 0;

	if (cxusb_medion_stream_busy(cxdev)) {
		ret = -EBUSY;
		goto ret_retbufs;
	}

	ret = v4l2_subdev_call(cxdev->cx25840, video, s_stream, 1);
	if (ret != 0) {
		dev_err(&dvbdev->udev->dev,
			"unable to start stream (%d)\n", ret);
		goto ret_retbufs;
	}

	ret = cxusb_ctrl_msg(dvbdev, CMD_STREAMING_ON, streamon_params, 2,
			     NULL, 0);
	if (ret != 0) {
		dev_err(&dvbdev->udev->dev,
			"unable to start streaming (%d)\n", ret);
		goto ret_unstream_cx;
	}

	if (cxdev->raw_mode)
		npackets = CXUSB_VIDEO_MAX_FRAME_PKTS;
	else {
		u8 *buf;
		unsigned int urblen, auxbuflen;

		/* has to be less than full frame size */
		urblen = (cxdev->width * 2 + 4 + 4) * cxdev->height;
		npackets = urblen / CXUSB_VIDEO_PKT_SIZE;
		urblen = npackets * CXUSB_VIDEO_PKT_SIZE;

		auxbuflen = (cxdev->width * 2 + 4 + 4) *
			(cxdev->height + 50 /* VBI lines */) + urblen;

		buf = vmalloc(auxbuflen);
		if (buf == NULL) {
			ret = -ENOMEM;
			goto ret_unstream_md;
		}

		cxusb_auxbuf_init(&cxdev->auxbuf, buf, auxbuflen);
	}

	for (i = 0; i < CXUSB_VIDEO_URBS; i++) {
		int framen;
		u8 *streambuf;
		struct urb *surb;

		streambuf = kmalloc(npackets * CXUSB_VIDEO_PKT_SIZE,
				    GFP_KERNEL);
		if (streambuf == NULL) {
			if (i == 0) {
				ret = -ENOMEM;
				goto ret_freeab;
			} else
				break;
		}

		surb = usb_alloc_urb(npackets, GFP_KERNEL);
		if (surb == NULL) {
			kfree(streambuf);
			ret = -ENOMEM;
			goto ret_freeu;
		}

		cxdev->streamurbs[i] = surb;
		surb->dev = dvbdev->udev;
		surb->context = dvbdev;
		surb->pipe = usb_rcvisocpipe(dvbdev->udev, 2);

		surb->interval = 1;
		surb->transfer_flags = URB_ISO_ASAP;

		surb->transfer_buffer = streambuf;

		surb->complete = cxusb_medion_v_complete;
		surb->number_of_packets = npackets;
		surb->transfer_buffer_length = npackets * CXUSB_VIDEO_PKT_SIZE;

		for (framen = 0; framen < npackets; framen++) {
			surb->iso_frame_desc[framen].offset =
				CXUSB_VIDEO_PKT_SIZE * framen;

			surb->iso_frame_desc[framen].length =
				CXUSB_VIDEO_PKT_SIZE;
		}
	}

	cxdev->urbcomplete = 0;
	cxdev->nexturb = 0;
	cxdev->vbuf = NULL;
	cxdev->bt656.mode = NEW_FRAME;
	cxdev->bt656.buf = NULL;

	for (i = 0; i < CXUSB_VIDEO_URBS; i++)
		if (cxdev->streamurbs[i] != NULL) {
			ret = usb_submit_urb(cxdev->streamurbs[i],
					GFP_KERNEL);
			if (ret != 0)
				dev_err(&dvbdev->udev->dev,
					"URB %d submission failed (%d)\n", i,
					ret);
		}

	cxdev->streaming = true;

	return 0;

ret_freeu:
	for (i = 0; i < CXUSB_VIDEO_URBS; i++)
		if (cxdev->streamurbs[i] != NULL) {
			kfree(cxdev->streamurbs[i]->transfer_buffer);
			usb_free_urb(cxdev->streamurbs[i]);
			cxdev->streamurbs[i] = NULL;
		}

ret_freeab:
	if (!cxdev->raw_mode)
		vfree(cxdev->auxbuf.buf);

ret_unstream_md:
	cxusb_ctrl_msg(dvbdev, CMD_STREAMING_OFF, NULL, 0, NULL, 0);

ret_unstream_cx:
	v4l2_subdev_call(cxdev->cx25840, video, s_stream, 0);

ret_retbufs:
	cxusb_medion_return_buffers(cxdev, true);

	return ret;
}

static void cxusb_medion_v_stop_streaming(struct vb2_queue *q)
{
	struct dvb_usb_device *dvbdev = vb2_get_drv_priv(q);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	int i, ret;

	cxusb_vprintk(dvbdev, OPS, "should stop streaming\n");

	if (!cxdev->streaming)
		return;

	cxdev->streaming = false;

	cxusb_ctrl_msg(dvbdev, CMD_STREAMING_OFF, NULL, 0, NULL, 0);

	ret = v4l2_subdev_call(cxdev->cx25840, video, s_stream, 0);
	if (ret != 0)
		dev_err(&dvbdev->udev->dev, "unable to stop stream (%d)\n",
			ret);

	/* let URB completion run */
	mutex_unlock(cxdev->videodev->lock);

	for (i = 0; i < CXUSB_VIDEO_URBS; i++)
		if (cxdev->streamurbs[i] != NULL)
			usb_kill_urb(cxdev->streamurbs[i]);

	flush_work(&cxdev->urbwork);

	mutex_lock(cxdev->videodev->lock);

	/* free transfer buffer and URB */
	if (!cxdev->raw_mode)
		vfree(cxdev->auxbuf.buf);

	for (i = 0; i < CXUSB_VIDEO_URBS; i++)
		if (cxdev->streamurbs[i] != NULL) {
			kfree(cxdev->streamurbs[i]->transfer_buffer);
			usb_free_urb(cxdev->streamurbs[i]);
			cxdev->streamurbs[i] = NULL;
		}

	cxusb_medion_return_buffers(cxdev, false);
}

static void cxusub_medion_v_buf_queue(struct vb2_buffer *vb)
{
	struct cxusb_medion_vbuffer *vbuf =
		container_of(vb, struct cxusb_medion_vbuffer, vb2);
	struct dvb_usb_device *dvbdev = vb2_get_drv_priv(vb->vb2_queue);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	/* cxusb_vprintk(dvbdev, OPS, "mmmm.. fresh buffer...\n"); */

	list_add_tail(&vbuf->list, &cxdev->buflist);
}

static void cxusub_medion_v_wait_prepare(struct vb2_queue *q)
{
	struct dvb_usb_device *dvbdev = vb2_get_drv_priv(q);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	mutex_unlock(cxdev->videodev->lock);
}

static void cxusub_medion_v_wait_finish(struct vb2_queue *q)
{
	struct dvb_usb_device *dvbdev = vb2_get_drv_priv(q);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	mutex_lock(cxdev->videodev->lock);
}

static const struct vb2_ops cxdev_video_qops = {
	.queue_setup = cxusb_medion_v_queue_setup,
	.buf_init = cxusb_medion_v_buf_init,
	.start_streaming = cxusb_medion_v_start_streaming,
	.stop_streaming = cxusb_medion_v_stop_streaming,
	.buf_queue = cxusub_medion_v_buf_queue,
	.wait_prepare = cxusub_medion_v_wait_prepare,
	.wait_finish = cxusub_medion_v_wait_finish
};

static int cxusb_medion_v_querycap(struct file *file, void *fh,
				   struct v4l2_capability *cap)
{
	const __u32 videocaps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_TUNER |
		V4L2_CAP_READWRITE | V4L2_CAP_STREAMING;
	const __u32 radiocaps = V4L2_CAP_TUNER | V4L2_CAP_RADIO;
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	strncpy(cap->driver, dvbdev->udev->dev.driver->name,
		sizeof(cap->driver) - 1);
	strcpy(cap->card, "Medion 95700");
	usb_make_path(dvbdev->udev, cap->bus_info, sizeof(cap->bus_info));

	if (vdev->vfl_type == VFL_TYPE_GRABBER)
		cap->device_caps = videocaps;
	else
		cap->device_caps = radiocaps;

	cap->capabilities = videocaps | radiocaps | V4L2_CAP_DEVICE_CAPS;

	memset(cap->reserved, 0, sizeof(cap->reserved));

	return 0;
}

static int cxusb_medion_v_enum_fmt_vid_cap(struct file *file, void *fh,
					   struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;

	f->flags = 0;
	strcpy(f->description, "YUV 4:2:2");
	f->pixelformat = V4L2_PIX_FMT_UYVY;
	memset(f->reserved, 0, sizeof(f->reserved));

	return 0;
}

static int cxusb_medion_g_fmt_vid_cap(struct file *file, void *fh,
				      struct v4l2_format *f)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	f->fmt.pix.width = cxdev->width;
	f->fmt.pix.height = cxdev->height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
	f->fmt.pix.field = V4L2_FIELD_SEQ_TB;
	f->fmt.pix.bytesperline = cxdev->raw_mode ? 0 : cxdev->width * 2;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
	f->fmt.pix.sizeimage =
		cxdev->raw_mode ? CXUSB_VIDEO_MAX_FRAME_SIZE :
		f->fmt.pix.bytesperline * f->fmt.pix.height;
	f->fmt.pix.priv = 0;

	return 0;
}

static int cxusb_medion_g_parm(struct file *file, void *fh,
			       struct v4l2_streamparm *param)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(&param->parm.capture, 0, sizeof(param->parm.capture));

	if (cxdev->raw_mode)
		param->parm.capture.extendedmode |=
			CXUSB_EXTENDEDMODE_CAPTURE_RAW;

	param->parm.capture.readbuffers = 1;

	return 0;
}

static int cxusb_medion_s_parm(struct file *file, void *fh,
			       struct v4l2_streamparm *param)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	bool want_raw;

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	want_raw = param->parm.capture.extendedmode &
		CXUSB_EXTENDEDMODE_CAPTURE_RAW;

	if (want_raw != cxdev->raw_mode) {
		if (cxusb_medion_stream_busy(cxdev) ||
		    vb2_is_busy(&cxdev->videoqueue))
			return -EBUSY;

		cxdev->raw_mode = want_raw;
	}

	param->parm.capture.readbuffers = 1;

	return 0;
}

static int cxusb_medion_try_s_fmt_vid_cap(struct file *file,
					  struct v4l2_format *f,
					  bool isset)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	struct v4l2_subdev_format subfmt;
	int ret;

	if (isset && (cxusb_medion_stream_busy(cxdev) ||
		      vb2_is_busy(&cxdev->videoqueue)))
		return -EBUSY;

	memset(&subfmt, 0, sizeof(subfmt));
	subfmt.which = isset ? V4L2_SUBDEV_FORMAT_ACTIVE :
		V4L2_SUBDEV_FORMAT_TRY;
	subfmt.format.width = f->fmt.pix.width & ~1;
	subfmt.format.height = f->fmt.pix.height & ~1;
	subfmt.format.code = MEDIA_BUS_FMT_FIXED;
	subfmt.format.field = V4L2_FIELD_SEQ_TB;
	subfmt.format.colorspace = V4L2_COLORSPACE_SMPTE170M;

	ret = v4l2_subdev_call(cxdev->cx25840, pad, set_fmt, NULL, &subfmt);
	if (ret != 0) {
		if (ret != -ERANGE)
			return ret;

		/* try some common formats */
		subfmt.format.width = 720;
		subfmt.format.height = 576;
		ret = v4l2_subdev_call(cxdev->cx25840, pad, set_fmt, NULL,
				       &subfmt);
		if (ret != 0) {
			if (ret != -ERANGE)
				return ret;

			subfmt.format.width = 640;
			subfmt.format.height = 480;
			ret = v4l2_subdev_call(cxdev->cx25840, pad, set_fmt,
					       NULL, &subfmt);
			if (ret != 0)
				return ret;
		}
	}

	f->fmt.pix.width = subfmt.format.width;
	f->fmt.pix.height = subfmt.format.height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
	f->fmt.pix.field = V4L2_FIELD_SEQ_TB;
	f->fmt.pix.bytesperline = cxdev->raw_mode ? 0 : f->fmt.pix.width * 2;
	f->fmt.pix.sizeimage =
		cxdev->raw_mode ? CXUSB_VIDEO_MAX_FRAME_SIZE :
		f->fmt.pix.bytesperline * f->fmt.pix.height;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
	f->fmt.pix.priv = 0;

	if (isset) {
		cxdev->width = f->fmt.pix.width;
		cxdev->height = f->fmt.pix.height;
	}

	return 0;
}

static int cxusb_medion_try_fmt_vid_cap(struct file *file, void *fh,
					struct v4l2_format *f)
{
	return cxusb_medion_try_s_fmt_vid_cap(file, f, false);
}

static int cxusb_medion_s_fmt_vid_cap(struct file *file, void *fh,
				      struct v4l2_format *f)
{
	return cxusb_medion_try_s_fmt_vid_cap(file, f, true);
}

static const struct {
	struct v4l2_input input;
	u32 inputcfg;
} cxusb_medion_inputs[] = {
	{ .input = { .name = "TV tuner", .type = V4L2_INPUT_TYPE_TUNER,
		     .tuner = 0, .std = V4L2_STD_PAL },
	  .inputcfg = CX25840_COMPOSITE2, },

	{  .input = { .name = "Composite", .type = V4L2_INPUT_TYPE_CAMERA,
		     .std = V4L2_STD_ALL },
	   .inputcfg = CX25840_COMPOSITE1, },

	{  .input = { .name = "S-Video", .type = V4L2_INPUT_TYPE_CAMERA,
		      .std = V4L2_STD_ALL },
	   .inputcfg = CX25840_SVIDEO_LUMA3 | CX25840_SVIDEO_CHROMA4 }
};

#define CXUSB_INPUT_CNT ARRAY_SIZE(cxusb_medion_inputs)

static int cxusb_medion_enum_input(struct file *file, void *fh,
				   struct v4l2_input *inp)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	u32 index = inp->index;

	if (index >= CXUSB_INPUT_CNT)
		return -EINVAL;

	*inp = cxusb_medion_inputs[index].input;
	inp->index = index;
	inp->capabilities |= V4L2_IN_CAP_STD;

	if (index == cxdev->input) {
		int ret;
		u32 status = 0;

		ret = v4l2_subdev_call(cxdev->cx25840, video, g_input_status,
				       &status);
		if (ret != 0)
			dev_warn(&dvbdev->udev->dev,
				 "cx25840 input status query failed (%d)\n",
				 ret);
		else
			inp->status = status;
	}

	return 0;
}

static int cxusb_medion_g_input(struct file *file, void *fh,
				unsigned int *i)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	*i = cxdev->input;

	return 0;
}

static int cxusb_medion_s_input(struct file *file, void *fh,
				unsigned int i)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	int ret;

	if (i >= CXUSB_INPUT_CNT)
		return -EINVAL;

	ret = v4l2_subdev_call(cxdev->cx25840, video, s_routing,
			       cxusb_medion_inputs[i].inputcfg, 0, 0);
	if (ret != 0)
		return ret;

	cxdev->input = i;

	return 0;
}

static int cxusb_medion_g_tuner(struct file *file, void *fh,
				struct v4l2_tuner *tuner)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	struct video_device *vdev = video_devdata(file);
	int ret;

	if (tuner->index != 0)
		return -EINVAL;

	if (vdev->vfl_type == VFL_TYPE_GRABBER)
		tuner->type = V4L2_TUNER_ANALOG_TV;
	else
		tuner->type = V4L2_TUNER_RADIO;

	tuner->capability = 0;
	tuner->afc = 0;

	/*
	 * fills:
	 * always: capability (static), rangelow (static), rangehigh (static)
	 * radio mode: afc (may fail silently), rxsubchans (static), audmode
	 */
	ret = v4l2_subdev_call(cxdev->tda9887, tuner, g_tuner, tuner);
	if (ret != 0)
		return ret;

	/*
	 * fills:
	 * always: capability (static), rangelow (static), rangehigh (static)
	 * radio mode: rxsubchans (always stereo), audmode,
	 * signal (might be wrong)
	 */
	ret = v4l2_subdev_call(cxdev->tuner, tuner, g_tuner, tuner);
	if (ret != 0)
		return ret;

	tuner->signal = 0;

	/*
	 * fills: TV mode: capability, rxsubchans, audmode, signal
	 */
	ret = v4l2_subdev_call(cxdev->cx25840, tuner, g_tuner, tuner);
	if (ret != 0)
		return ret;

	if (vdev->vfl_type == VFL_TYPE_GRABBER)
		strcpy(tuner->name, "TV tuner");
	else
		strcpy(tuner->name, "Radio tuner");

	memset(tuner->reserved, 0, sizeof(tuner->reserved));

	return 0;
}

static int cxusb_medion_s_tuner(struct file *file, void *fh,
				const struct v4l2_tuner *tuner)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	struct video_device *vdev = video_devdata(file);
	int ret;

	if (tuner->index != 0)
		return -EINVAL;

	ret = v4l2_subdev_call(cxdev->tda9887, tuner, s_tuner, tuner);
	if (ret != 0)
		return ret;

	ret = v4l2_subdev_call(cxdev->tuner, tuner, s_tuner, tuner);
	if (ret != 0)
		return ret;

	/*
	 * make sure that cx25840 is in a correct TV / radio mode,
	 * since calls above may have changed it for tuner / IF demod
	 */
	if (vdev->vfl_type == VFL_TYPE_GRABBER)
		v4l2_subdev_call(cxdev->cx25840, video, s_std, cxdev->norm);
	else
		v4l2_subdev_call(cxdev->cx25840, tuner, s_radio);

	return v4l2_subdev_call(cxdev->cx25840, tuner, s_tuner, tuner);
}

static int cxusb_medion_g_frequency(struct file *file, void *fh,
				    struct v4l2_frequency *freq)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	if (freq->tuner != 0)
		return -EINVAL;

	return v4l2_subdev_call(cxdev->tuner, tuner, g_frequency, freq);
}

static int cxusb_medion_s_frequency(struct file *file, void *fh,
				    const struct v4l2_frequency *freq)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	struct video_device *vdev = video_devdata(file);
	int ret;

	if (freq->tuner != 0)
		return -EINVAL;

	ret = v4l2_subdev_call(cxdev->tda9887, tuner, s_frequency, freq);
	if (ret != 0)
		return ret;

	ret = v4l2_subdev_call(cxdev->tuner, tuner, s_frequency, freq);
	if (ret != 0)
		return ret;

	/*
	 * make sure that cx25840 is in a correct TV / radio mode,
	 * since calls above may have changed it for tuner / IF demod
	 */
	if (vdev->vfl_type == VFL_TYPE_GRABBER)
		v4l2_subdev_call(cxdev->cx25840, video, s_std, cxdev->norm);
	else
		v4l2_subdev_call(cxdev->cx25840, tuner, s_radio);

	return v4l2_subdev_call(cxdev->cx25840, tuner, s_frequency, freq);
}

static int cxusb_medion_g_std(struct file *file, void *fh,
			      v4l2_std_id *norm)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	int ret;

	ret = v4l2_subdev_call(cxdev->cx25840, video, g_std, norm);
	if (ret != 0) {
		cxusb_vprintk(dvbdev, OPS, "cannot get standard for input %u\n",
			      (unsigned int)cxdev->input);

		return ret;
	}

	cxusb_vprintk(dvbdev, OPS,
		      "current standard for input %u is %lx\n",
		      (unsigned int)cxdev->input,
		      (unsigned long)*norm);

	if (cxdev->input == 0)
		/*
		 * make sure we don't have improper std bits set
		 * for TV tuner (could happen when no signal was
		 * present yet after reset)
		 */
		*norm &= V4L2_STD_PAL;

	if (*norm == V4L2_STD_UNKNOWN)
		return -ENODATA;

	return 0;
}

static int cxusb_medion_s_std(struct file *file, void *fh,
			      v4l2_std_id norm)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	int ret;

	cxusb_vprintk(dvbdev, OPS,
		      "trying to set standard for input %u to %lx\n",
		      (unsigned int)cxdev->input,
		      (unsigned long)norm);

	/* on composite or S-Video any std is acceptable */
	if (cxdev->input != 0) {
		ret = v4l2_subdev_call(cxdev->cx25840, video, s_std, norm);
		if (ret)
			return ret;

		goto ret_savenorm;
	}

	/* TV tuner is only able to demodulate PAL */
	if ((norm & ~V4L2_STD_PAL) != 0)
		return -EINVAL;

	/* no autodetection support */
	if (norm == 0)
		return -EINVAL;

	ret = v4l2_subdev_call(cxdev->tda9887, video, s_std, norm);
	if (ret != 0) {
		dev_err(&dvbdev->udev->dev,
			"tda9887 norm setup failed (%d)\n",
			ret);
		return ret;
	}

	ret = v4l2_subdev_call(cxdev->tuner, video, s_std, norm);
	if (ret != 0) {
		dev_err(&dvbdev->udev->dev,
			"tuner norm setup failed (%d)\n",
			ret);
		return ret;
	}

	ret = v4l2_subdev_call(cxdev->cx25840, video, s_std, norm);
	if (ret != 0) {
		dev_err(&dvbdev->udev->dev,
			"cx25840 norm setup failed (%d)\n",
			ret);
		return ret;
	}

ret_savenorm:
	cxdev->norm = norm;

	return 0;
}

static int cxusb_medion_log_status(struct file *file, void *fh)
{
	struct dvb_usb_device *dvbdev = video_drvdata(file);
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	v4l2_device_call_all(&cxdev->v4l2dev, 0, core, log_status);

	return 0;
}

static const struct v4l2_ioctl_ops cxusb_video_ioctl = {
	.vidioc_querycap = cxusb_medion_v_querycap,
	.vidioc_enum_fmt_vid_cap = cxusb_medion_v_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = cxusb_medion_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = cxusb_medion_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = cxusb_medion_try_fmt_vid_cap,
	.vidioc_enum_input = cxusb_medion_enum_input,
	.vidioc_g_input = cxusb_medion_g_input,
	.vidioc_s_input = cxusb_medion_s_input,
	.vidioc_g_parm = cxusb_medion_g_parm,
	.vidioc_s_parm = cxusb_medion_s_parm,
	.vidioc_g_tuner = cxusb_medion_g_tuner,
	.vidioc_s_tuner = cxusb_medion_s_tuner,
	.vidioc_g_frequency = cxusb_medion_g_frequency,
	.vidioc_s_frequency = cxusb_medion_s_frequency,
	.vidioc_g_std = cxusb_medion_g_std,
	.vidioc_s_std = cxusb_medion_s_std,
	.vidioc_log_status = cxusb_medion_log_status,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff
};

static const struct v4l2_ioctl_ops cxusb_radio_ioctl = {
	.vidioc_querycap = cxusb_medion_v_querycap,
	.vidioc_g_tuner = cxusb_medion_g_tuner,
	.vidioc_s_tuner = cxusb_medion_s_tuner,
	.vidioc_g_frequency = cxusb_medion_g_frequency,
	.vidioc_s_frequency = cxusb_medion_s_frequency,
	.vidioc_log_status = cxusb_medion_log_status
};

/*
 * in principle, this should be const, but s_io_pin_config is declared
 * to take non-const, and gcc complains
 */
static struct v4l2_subdev_io_pin_config cxusub_medion_pin_config[] = {
	{ .pin = CX25840_PIN_DVALID_PRGM0, .function = CX25840_PAD_DEFAULT,
	  .strength = CX25840_PIN_DRIVE_MEDIUM },
	{ .pin = CX25840_PIN_PLL_CLK_PRGM7, .function = CX25840_PAD_AUX_PLL },
	{ .pin = CX25840_PIN_HRESET_PRGM2, .function = CX25840_PAD_ACTIVE,
	  .strength = CX25840_PIN_DRIVE_MEDIUM }
};

int cxusb_medion_analog_init(struct dvb_usb_device *dvbdev)
{
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	u8 tuner_analog_msg_data[] = { 0x9c, 0x60, 0x85, 0x54 };
	struct i2c_msg tuner_analog_msg = { .addr = 0x61, .flags = 0,
					    .buf = tuner_analog_msg_data,
					    .len =
					    sizeof(tuner_analog_msg_data) };
	struct v4l2_subdev_format subfmt;
	int ret;

	/* switch tuner to analog mode so IF demod will become accessible */
	ret = i2c_transfer(&dvbdev->i2c_adap, &tuner_analog_msg, 1);
	if (ret != 1)
		dev_warn(&dvbdev->udev->dev,
			 "tuner analog switch failed (%d)\n", ret);

	ret = v4l2_subdev_call(cxdev->cx25840, core, load_fw);
	if (ret != 0)
		dev_warn(&dvbdev->udev->dev,
			 "cx25840 fw load failed (%d)\n", ret);

	ret = v4l2_subdev_call(cxdev->cx25840, video, s_routing,
			       CX25840_COMPOSITE1, 0,
			       CX25840_VCONFIG_FMT_BT656 |
			       CX25840_VCONFIG_RES_8BIT |
			       CX25840_VCONFIG_VBIRAW_DISABLED |
			       CX25840_VCONFIG_ANCDATA_DISABLED |
			       CX25840_VCONFIG_ACTIVE_COMPOSITE |
			       CX25840_VCONFIG_VALID_ANDACTIVE |
			       CX25840_VCONFIG_HRESETW_NORMAL |
			       CX25840_VCONFIG_CLKGATE_NONE |
			       CX25840_VCONFIG_DCMODE_DWORDS);
	if (ret != 0)
		dev_warn(&dvbdev->udev->dev,
			 "cx25840 mode set failed (%d)\n", ret);

	/* composite */
	cxdev->input = 1;
	cxdev->norm = 0;

	/* TODO: setup audio samples insertion */

	ret = v4l2_subdev_call(cxdev->cx25840, core, s_io_pin_config,
			       sizeof(cxusub_medion_pin_config) /
			       sizeof(cxusub_medion_pin_config[0]),
			       cxusub_medion_pin_config);
	if (ret != 0)
		dev_warn(&dvbdev->udev->dev,
			"cx25840 pin config failed (%d)\n", ret);

	/* make sure that we aren't in radio mode */
	v4l2_subdev_call(cxdev->tda9887, video, s_std, V4L2_STD_PAL);
	v4l2_subdev_call(cxdev->tuner, video, s_std, V4L2_STD_PAL);
	v4l2_subdev_call(cxdev->cx25840, video, s_std, cxdev->norm);

	memset(&subfmt, 0, sizeof(subfmt));
	subfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	subfmt.format.width = cxdev->width;
	subfmt.format.height = cxdev->height;
	subfmt.format.code = MEDIA_BUS_FMT_FIXED;
	subfmt.format.field = V4L2_FIELD_SEQ_TB;
	subfmt.format.colorspace = V4L2_COLORSPACE_SMPTE170M;

	ret = v4l2_subdev_call(cxdev->cx25840, pad, set_fmt, NULL, &subfmt);
	if (ret != 0) {
		subfmt.format.width = 640;
		subfmt.format.height = 480;
		ret = v4l2_subdev_call(cxdev->cx25840, pad, set_fmt, NULL,
				       &subfmt);
		if (ret != 0)
			dev_warn(&dvbdev->udev->dev,
				 "cx25840 format set failed (%d)\n", ret);
	}

	if (ret == 0) {
		cxdev->width = subfmt.format.width;
		cxdev->height = subfmt.format.height;
	}

	return 0;
}

static int cxusb_videoradio_open(struct file *f)
{
	struct dvb_usb_device *dvbdev = video_drvdata(f);
	int ret;

	/*
	 * no locking needed since this call only modifies analog
	 * state if there are no other analog handles currenly
	 * opened so ops done via them cannot create a conflict
	 */
	ret = cxusb_medion_get(dvbdev, CXUSB_OPEN_ANALOG);
	if (ret != 0)
		return ret;

	ret = v4l2_fh_open(f);
	if (ret != 0)
		goto ret_release;

	cxusb_vprintk(dvbdev, OPS, "got open\n");

	return 0;

ret_release:
	cxusb_medion_put(dvbdev);

	return ret;
}

static int cxusb_videoradio_release(struct file *f)
{
	struct video_device *vdev = video_devdata(f);
	struct dvb_usb_device *dvbdev = video_drvdata(f);
	int ret;

	cxusb_vprintk(dvbdev, OPS, "got release\n");

	if (vdev->vfl_type == VFL_TYPE_GRABBER)
		ret = vb2_fop_release(f);
	else
		ret = v4l2_fh_release(f);

	cxusb_medion_put(dvbdev);

	return ret;
}

static const struct v4l2_file_operations cxusb_video_fops = {
	.owner = THIS_MODULE,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.open = cxusb_videoradio_open,
	.release = cxusb_videoradio_release
};

static const struct v4l2_file_operations cxusb_radio_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open = cxusb_videoradio_open,
	.release = cxusb_videoradio_release
};

static void cxusb_medion_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct cxusb_medion_dev *cxdev =
		container_of(v4l2_dev, struct cxusb_medion_dev, v4l2dev);
	struct dvb_usb_device *dvbdev = cxdev->dvbdev;

	cxusb_vprintk(dvbdev, OPS, "v4l2 device release\n");

	v4l2_device_unregister(&cxdev->v4l2dev);

	mutex_destroy(&cxdev->dev_lock);

	while (completion_done(&cxdev->v4l2_release))
		schedule();

	complete(&cxdev->v4l2_release);
}

static void cxusb_medion_videodev_release(struct video_device *vdev)
{
	struct dvb_usb_device *dvbdev = video_get_drvdata(vdev);

	cxusb_vprintk(dvbdev, OPS, "video device release\n");

	vb2_queue_release(vdev->queue);

	video_device_release(vdev);
}

static int cxusb_medion_register_analog_video(struct dvb_usb_device *dvbdev)
{
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	int ret;

	cxdev->videoqueue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cxdev->videoqueue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	cxdev->videoqueue.ops = &cxdev_video_qops;
	cxdev->videoqueue.mem_ops = &vb2_vmalloc_memops;
	cxdev->videoqueue.drv_priv = dvbdev;
	cxdev->videoqueue.buf_struct_size =
		sizeof(struct cxusb_medion_vbuffer);
	cxdev->videoqueue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	ret = vb2_queue_init(&cxdev->videoqueue);
	if (ret) {
		dev_err(&dvbdev->udev->dev,
			"video queue init failed, ret = %d\n", ret);
		return ret;
	}

	cxdev->videodev = video_device_alloc();
	if (cxdev->videodev == NULL) {
		dev_err(&dvbdev->udev->dev, "video device alloc failed\n");
		ret = -ENOMEM;
		goto ret_qrelease;
	}

	cxdev->videodev->fops = &cxusb_video_fops;
	cxdev->videodev->v4l2_dev = &cxdev->v4l2dev;
	cxdev->videodev->queue = &cxdev->videoqueue;
	strcpy(cxdev->videodev->name, "cxusb");
	cxdev->videodev->vfl_dir = VFL_DIR_RX;
	cxdev->videodev->ioctl_ops = &cxusb_video_ioctl;
	cxdev->videodev->tvnorms = V4L2_STD_ALL;
	cxdev->videodev->release = cxusb_medion_videodev_release;
	cxdev->videodev->lock = &cxdev->dev_lock;
	video_set_drvdata(cxdev->videodev, dvbdev);

	ret = video_register_device(cxdev->videodev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		dev_err(&dvbdev->udev->dev,
			"video device register failed, ret = %d\n", ret);
		goto ret_vrelease;
	}

	return 0;

ret_vrelease:
	video_device_release(cxdev->videodev);

ret_qrelease:
	vb2_queue_release(&cxdev->videoqueue);

	return ret;
}

static int cxusb_medion_register_analog_radio(struct dvb_usb_device *dvbdev)
{
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	int ret;

	cxdev->radiodev = video_device_alloc();
	if (cxdev->radiodev == NULL) {
		dev_err(&dvbdev->udev->dev, "radio device alloc failed\n");
		return -ENOMEM;
	}

	cxdev->radiodev->fops = &cxusb_radio_fops;
	cxdev->radiodev->v4l2_dev = &cxdev->v4l2dev;
	strcpy(cxdev->radiodev->name, "cxusb");
	cxdev->radiodev->vfl_dir = VFL_DIR_RX;
	cxdev->radiodev->ioctl_ops = &cxusb_radio_ioctl;
	cxdev->radiodev->release = video_device_release;
	cxdev->radiodev->lock = &cxdev->dev_lock;
	video_set_drvdata(cxdev->radiodev, dvbdev);

	ret = video_register_device(cxdev->radiodev, VFL_TYPE_RADIO, -1);
	if (ret) {
		dev_err(&dvbdev->udev->dev,
			"radio device register failed, ret = %d\n", ret);
		video_device_release(cxdev->radiodev);
		return ret;
	}

	return 0;
}

static int cxusb_medion_register_analog_subdevs(struct dvb_usb_device *dvbdev)
{
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	struct tuner_setup tun_setup;
	struct i2c_board_info cx25840_board;
	struct cx25840_platform_data cx25840_platform;

	/* attach capture chip */
	memset(&cx25840_platform, 0, sizeof(cx25840_platform));
	cx25840_platform.generic_mode = 1;

	memset(&cx25840_board, 0, sizeof(cx25840_board));
	strcpy(cx25840_board.type, "cx25840");
	cx25840_board.addr = 0x44;
	cx25840_board.platform_data = &cx25840_platform;

	cxdev->cx25840 = v4l2_i2c_new_subdev_board(&cxdev->v4l2dev,
						   &dvbdev->i2c_adap,
						   &cx25840_board, NULL);
	if (cxdev->cx25840 == NULL) {
		dev_err(&dvbdev->udev->dev, "cx25840 not found\n");
		return -ENODEV;
	}

	/* attach analog tuner */
	cxdev->tuner = v4l2_i2c_new_subdev(&cxdev->v4l2dev,
					   &dvbdev->i2c_adap,
					   "tuner", 0x61, NULL);
	if (cxdev->tuner == NULL) {
		dev_err(&dvbdev->udev->dev, "tuner not found\n");
		return -ENODEV;
	}

	/* configure it */
	memset(&tun_setup, 0, sizeof(tun_setup));
	tun_setup.addr = 0x61;
	tun_setup.type = TUNER_PHILIPS_FMD1216ME_MK3;
	tun_setup.mode_mask = T_RADIO | T_ANALOG_TV;
	v4l2_subdev_call(cxdev->tuner, tuner, s_type_addr, &tun_setup);

	/* attach IF demod */
	cxdev->tda9887 = v4l2_i2c_new_subdev(&cxdev->v4l2dev,
					     &dvbdev->i2c_adap,
					     "tuner", 0x43, NULL);
	if (cxdev->tda9887 == NULL) {
		dev_err(&dvbdev->udev->dev, "tda9887 not found\n");
		return -ENODEV;
	}

	return 0;
}

int cxusb_medion_register_analog(struct dvb_usb_device *dvbdev)
{
	struct cxusb_medion_dev *cxdev = dvbdev->priv;
	int ret;

	mutex_init(&cxdev->dev_lock);

	init_completion(&cxdev->v4l2_release);

	cxdev->v4l2dev.release = cxusb_medion_v4l2_release;

	ret = v4l2_device_register(&dvbdev->udev->dev, &cxdev->v4l2dev);
	if (ret != 0) {
		dev_err(&dvbdev->udev->dev,
			"V4L2 device registration failed, ret = %d\n", ret);
		mutex_destroy(&cxdev->dev_lock);
		return ret;
	}

	ret = cxusb_medion_register_analog_subdevs(dvbdev);
	if (ret)
		goto ret_unregister;

	INIT_WORK(&cxdev->urbwork, cxusb_medion_v_complete_work);
	INIT_LIST_HEAD(&cxdev->buflist);

	cxdev->width = 320;
	cxdev->height = 240;

	ret = cxusb_medion_register_analog_video(dvbdev);
	if (ret)
		goto ret_unregister;

	ret = cxusb_medion_register_analog_radio(dvbdev);
	if (ret)
		goto ret_vunreg;

	return 0;

ret_vunreg:
	video_unregister_device(cxdev->videodev);

ret_unregister:
	v4l2_device_put(&cxdev->v4l2dev);
	wait_for_completion(&cxdev->v4l2_release);

	return ret;
}

void cxusb_medion_unregister_analog(struct dvb_usb_device *dvbdev)
{
	struct cxusb_medion_dev *cxdev = dvbdev->priv;

	cxusb_vprintk(dvbdev, OPS, "unregistering analog\n");

	video_unregister_device(cxdev->radiodev);
	video_unregister_device(cxdev->videodev);

	v4l2_device_put(&cxdev->v4l2dev);
	wait_for_completion(&cxdev->v4l2_release);

	cxusb_vprintk(dvbdev, OPS, "analog unregistered\n");
}
