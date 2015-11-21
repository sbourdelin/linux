/*
 *  Force feedback support for Logitech HID++ devices [feature 0x8123]
 *
 *  Currently only used for the G920 Driving Force Racing Wheel
 *
 *  Copyright (c) 2015 Edwin Velds <e.velds@gmail.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 2 of the License.
 */

#include <linux/device.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>
#include <linux/atomic.h>
#include <linux/fixp-arith.h>
#include "hid-logitech-hidpp-ff.h"


#define HIDPP_FF_GET_INFO		0x01
#define HIDPP_FF_RESET_ALL		0x11
#define HIDPP_FF_DOWNLOAD_EFFECT	0x21
#define HIDPP_FF_SET_EFFECT_STATE	0x31
#define HIDPP_FF_DESTROY_EFFECT		0x41
#define HIDPP_FF_GET_APERTURE		0x51
#define HIDPP_FF_SET_APERTURE		0x61
#define HIDPP_FF_GET_GLOBAL_GAINS	0x71
#define HIDPP_FF_SET_GLOBAL_GAINS	0x81

#define HIDPP_FF_EFFECT_STATE_GET	0x00
#define HIDPP_FF_EFFECT_STATE_STOP	0x01
#define HIDPP_FF_EFFECT_STATE_PLAY	0x02
#define HIDPP_FF_EFFECT_STATE_PAUSE	0x03

#define HIDPP_FF_EFFECT_CONSTANT	0x00
#define HIDPP_FF_EFFECT_PERIODIC_SINE		0x01
#define HIDPP_FF_EFFECT_PERIODIC_SQUARE		0x02
#define HIDPP_FF_EFFECT_PERIODIC_TRIANGLE	0x03
#define HIDPP_FF_EFFECT_PERIODIC_SAWTOOTHUP	0x04
#define HIDPP_FF_EFFECT_PERIODIC_SAWTOOTHDOWN	0x05
#define HIDPP_FF_EFFECT_SPRING		0x06
#define HIDPP_FF_EFFECT_DAMPER		0x07

#define HIDPP_FF_EFFECT_AUTOSTART	0x80

#define HIDPP_FF_EFFECTID_NONE		-1
#define HIDPP_FF_EFFECTID_AUTOCENTER	-2

#define HIDPP_FF_MAX_PARAMS	20

#define to_hid_device(pdev) container_of(pdev, struct hid_device, dev)


struct hidpp_ff_private_data {
	struct hidpp_device *hidpp;
	u8 feature_index;
	u16 gain;
	s16 range;
	u8 slot_autocenter;
	u8 num_effects;
	int *effect_ids;
	struct workqueue_struct *wq;
	atomic_t workqueue_size;
};

struct hidpp_ff_work_data {
	struct work_struct work;
	struct hidpp_ff_private_data *data;
	int effect_id;
	u8 command;
	u8 params[HIDPP_FF_MAX_PARAMS];
	u8 size;
};

static const signed short hiddpp_ff_effects[] = {
	FF_CONSTANT,
	FF_PERIODIC,
	FF_SINE,
	FF_SQUARE,
	FF_SAW_UP,
	FF_SAW_DOWN,
	FF_TRIANGLE,
	FF_SPRING,
	FF_DAMPER,
	FF_AUTOCENTER,
	FF_GAIN,
	-1
};


static u8 hidpp_ff_find_effect(struct hidpp_ff_private_data *data, int effect_id)
{
	int i;

	for (i = 0; i < data->num_effects; i++)
		if (data->effect_ids[i] == effect_id)
			return i+1;

	return 0;
}

static void hidpp_ff_work_handler(struct work_struct *w)
{
	struct hidpp_ff_work_data *wd = container_of(w, struct hidpp_ff_work_data, work);
	struct hidpp_ff_private_data *data = wd->data;
	struct hidpp_report response;
	u8 slot;
	int ret;

	/* add slot number if needed */
	switch (wd->effect_id) {
	case HIDPP_FF_EFFECTID_AUTOCENTER:
		wd->params[0] = data->slot_autocenter;
		break;
	case HIDPP_FF_EFFECTID_NONE:
		/* leave slot as zero */
		break;
	default:
		/* find current slot for effect */
		wd->params[0] = hidpp_ff_find_effect(data, wd->effect_id);
		break;
	}

	/* send command and wait for reply */
	ret = hidpp_send_fap_command_sync(data->hidpp, data->feature_index,
		wd->command, wd->params, wd->size, &response);

	if (ret) {
		hid_err(data->hidpp->hid_dev, "Failed to send command to device!\n");
		goto out;
	}

	/* parse return data */
	switch (wd->command) {
	case HIDPP_FF_DOWNLOAD_EFFECT:
		slot = response.fap.params[0];
		if (slot > 0 && slot <= data->num_effects) {
			if (wd->effect_id >= 0)
				/* regular effect uploaded */
				data->effect_ids[slot-1] = wd->effect_id;
			else if (wd->effect_id >= HIDPP_FF_EFFECTID_AUTOCENTER)
				/* autocenter spring uploaded */
				data->slot_autocenter = slot;
		}
		break;
	case HIDPP_FF_DESTROY_EFFECT:
		if (wd->effect_id >= 0)
			/* regular effect destroyed */
			data->effect_ids[wd->params[0]-1] = -1;
		else if (wd->effect_id >= HIDPP_FF_EFFECTID_AUTOCENTER)
			/* autocenter spring destoyed */
			data->slot_autocenter = 0;
		break;
	case HIDPP_FF_SET_GLOBAL_GAINS:
		data->gain = (wd->params[0] << 8) + wd->params[1];
		break;
	case HIDPP_FF_SET_APERTURE:
		data->range = (wd->params[0] << 8) + wd->params[1];
		break;
	default:
		/* no action needed */
		break;
	}

out:
	atomic_dec(&data->workqueue_size);
	kfree(wd);
}

static int hidpp_ff_queue_work(struct hidpp_ff_private_data *data, int effect_id, u8 command, u8 *params, u8 size)
{
	struct hidpp_ff_work_data *wd = kzalloc(sizeof(struct hidpp_ff_work_data), GFP_KERNEL);
	int s;

	if (!wd)
		return -ENOMEM;

	INIT_WORK(&wd->work, hidpp_ff_work_handler);

	wd->data = data;
	wd->effect_id = effect_id;
	wd->command = command;
	wd->size = size;
	memcpy(wd->params, params, size);

	atomic_inc(&data->workqueue_size);
	queue_work(data->wq, &wd->work);

	/* warn about excessive queue size */
	s = atomic_read(&data->workqueue_size);
	if (s >= 20 && s % 20 == 0)
		hid_warn(data->hidpp->hid_dev, "Force feedback command queue contains %d commands, causing substantial delays.", s);

	return 0;
}

static int hidpp_ff_upload_effect(struct input_dev *dev, struct ff_effect *effect, struct ff_effect *old)
{
	struct hidpp_ff_private_data *data = dev->ff->private;
	u8 params[20];
	u8 size;
	int force;

	/* set common parameters */
	params[2] = effect->replay.length >> 8;
	params[3] = effect->replay.length & 255;
	params[4] = effect->replay.delay >> 8;
	params[5] = effect->replay.delay & 255;

	switch (effect->type) {
	case FF_CONSTANT:
		force = (effect->u.constant.level * fixp_sin16((effect->direction * 360) >> 16)) >> 15;
		params[1] = HIDPP_FF_EFFECT_CONSTANT;
		params[6] = force >> 8;
		params[7] = force & 255;
		params[8] = effect->u.constant.envelope.attack_level >> 8;
		params[9] = effect->u.constant.envelope.attack_length >> 8;
		params[10] = effect->u.constant.envelope.attack_length & 255;
		params[11] = effect->u.constant.envelope.fade_level >> 8;
		params[12] = effect->u.constant.envelope.fade_length >> 8;
		params[13] = effect->u.constant.envelope.fade_length & 255;
		size = 14;
		dbg_hid("Uploading constant force level=%d in dir %d = %d\n",
				effect->u.constant.level,
				effect->direction, force);
		dbg_hid("          envelope attack=(%d, %d ms) fade=(%d, %d ms)\n",
				effect->u.constant.envelope.attack_level,
				effect->u.constant.envelope.attack_length,
				effect->u.constant.envelope.fade_level,
				effect->u.constant.envelope.fade_length);
		break;
	case FF_PERIODIC:
	{
		switch (effect->u.periodic.waveform) {
		case FF_SINE:
			params[1] = HIDPP_FF_EFFECT_PERIODIC_SINE;
			break;
		case FF_SQUARE:
			params[1] = HIDPP_FF_EFFECT_PERIODIC_SQUARE;
			break;
		case FF_SAW_UP:
			params[1] = HIDPP_FF_EFFECT_PERIODIC_SAWTOOTHUP;
			break;
		case FF_SAW_DOWN:
			params[1] = HIDPP_FF_EFFECT_PERIODIC_SAWTOOTHDOWN;
			break;
		case FF_TRIANGLE:
			params[1] = HIDPP_FF_EFFECT_PERIODIC_TRIANGLE;
			break;
		default:
			hid_err(data->hidpp->hid_dev, "Unexpected periodic waveform type %i!\n", effect->u.periodic.waveform);
			return -EINVAL;
		}
		force = (effect->u.periodic.magnitude * fixp_sin16((effect->direction * 360) >> 16)) >> 15;
		params[6] = effect->u.periodic.magnitude >> 8;
		params[7] = effect->u.periodic.magnitude & 255;
		params[8] = effect->u.periodic.offset >> 8;
		params[9] = effect->u.periodic.offset & 255;
		params[10] = effect->u.periodic.period >> 8;
		params[11] = effect->u.periodic.period & 255;
		params[12] = effect->u.periodic.phase >> 8;
		params[13] = effect->u.periodic.phase & 255;
		params[14] = effect->u.periodic.envelope.attack_level >> 7;
		params[15] = effect->u.periodic.envelope.attack_length >> 8;
		params[16] = effect->u.periodic.envelope.attack_length & 255;
		params[17] = effect->u.periodic.envelope.fade_level >> 7;
		params[18] = effect->u.periodic.envelope.fade_length >> 8;
		params[19] = effect->u.periodic.envelope.fade_length & 255;
		size = 20;
		dbg_hid("Uploading periodic force mag=%d/dir=%d, offset=%d, period=%d ms, phase=%d\n",
				effect->u.periodic.magnitude, effect->direction,
				effect->u.periodic.offset,
				effect->u.periodic.period,
				effect->u.periodic.phase);
		dbg_hid("          envelope attack=(%d, %d ms) fade=(%d, %d ms)\n",
				effect->u.periodic.envelope.attack_level,
				effect->u.periodic.envelope.attack_length,
				effect->u.periodic.envelope.fade_level,
				effect->u.periodic.envelope.fade_length);
		break;
	}
	case FF_SPRING:
	case FF_DAMPER:
		params[1] = effect->type == FF_SPRING ? HIDPP_FF_EFFECT_SPRING : HIDPP_FF_EFFECT_DAMPER;
		params[6] = effect->u.condition[0].left_saturation >> 9;
		params[7] = (effect->u.condition[0].left_saturation >> 1) & 255;
		params[8] = effect->u.condition[0].left_coeff >> 8;
		params[9] = effect->u.condition[0].left_coeff & 255;
		params[10] = effect->u.condition[0].deadband >> 9;
		params[11] = (effect->u.condition[0].deadband >> 1) & 255;
		params[12] = effect->u.condition[0].center >> 8;
		params[13] = effect->u.condition[0].center & 255;
		params[14] = effect->u.condition[0].right_coeff >> 8;
		params[15] = effect->u.condition[0].right_coeff & 255;
		params[16] = effect->u.condition[0].right_saturation >> 9;
		params[17] = (effect->u.condition[0].right_saturation >> 1) & 255;
		size = 18;
		dbg_hid("Uploading %s force left coeff=%d, left sat=%d, right coeff=%d, right sat=%d\n",
				effect->type == FF_SPRING ? "spring" : "damper",
				effect->u.condition[0].left_coeff,
				effect->u.condition[0].left_saturation,
				effect->u.condition[0].right_coeff,
				effect->u.condition[0].right_saturation);
		dbg_hid("          deadband=%d, center=%d\n",
				effect->u.condition[0].deadband,
				effect->u.condition[0].center);
		break;
	default:
		hid_err(data->hidpp->hid_dev, "Unexpected force type %i!\n", effect->type);
		return -EINVAL;
	}

	return hidpp_ff_queue_work(data, effect->id, HIDPP_FF_DOWNLOAD_EFFECT, params, size);
}

static int hidpp_ff_playback(struct input_dev *dev, int effect_id, int value)
{
	struct hidpp_ff_private_data *data = dev->ff->private;
	u8 params[2];

	params[1] = value ? HIDPP_FF_EFFECT_STATE_PLAY : HIDPP_FF_EFFECT_STATE_STOP;

	dbg_hid("St%sing playback of effect %d!\n", value?"art":"opp", effect_id);

	return hidpp_ff_queue_work(data, effect_id, HIDPP_FF_SET_EFFECT_STATE, params, 2);
}

static int hidpp_ff_erase_effect(struct input_dev *dev, int effect_id)
{
	struct hidpp_ff_private_data *data = dev->ff->private;
	u8 slot = 0;

	dbg_hid("Erasing effect %d!\n", effect_id);

	return hidpp_ff_queue_work(data, effect_id, HIDPP_FF_DESTROY_EFFECT, &slot, 1);
}

static void hidpp_ff_set_autocenter(struct input_dev *dev, u16 magnitude)
{
	struct hidpp_ff_private_data *data = dev->ff->private;
	u8 params[18];

	dbg_hid("Setting autocenter to %d!\n", magnitude);

	if (magnitude) {
		/* start a standard spring effect */
		params[1] = HIDPP_FF_EFFECT_SPRING | HIDPP_FF_EFFECT_AUTOSTART;
		/* zero delay and duration */
		params[2] = params[3] = params[4] = params[5] = 0;
		/* saturation and coeff to equal value */
		params[6] = params[8] = params[14] = params[16] = magnitude >> 9;
		params[7] = params[9] = params[15] = params[17] = (magnitude & 255) >> 1;
		/* zero deadband and center */
		params[10] = params[11] = params[12] = params[13] = 0;

		hidpp_ff_queue_work(data, HIDPP_FF_EFFECTID_AUTOCENTER, HIDPP_FF_DOWNLOAD_EFFECT, params, ARRAY_SIZE(params));
	} else {
		if (data->slot_autocenter) {
			/* remove spring effect */
			hidpp_ff_queue_work(data, HIDPP_FF_EFFECTID_AUTOCENTER, HIDPP_FF_DESTROY_EFFECT, params, 1);
		}
	}
}

static void hidpp_ff_set_gain(struct input_dev *dev, u16 gain)
{
	struct hidpp_ff_private_data *data = dev->ff->private;
	u8 params[4];

	dbg_hid("Setting gain to %d!\n", gain);

	params[0] = gain >> 8;
	params[1] = gain & 255;
	params[2] = 0; /* no boost */
	params[3] = 0;

	hidpp_ff_queue_work(data, HIDPP_FF_EFFECTID_NONE, HIDPP_FF_SET_GLOBAL_GAINS, params, 4);
}

static ssize_t hidpp_ff_range_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hid = to_hid_device(dev);
	struct hid_input *hidinput = list_entry(hid->inputs.next, struct hid_input, list);
	struct input_dev *idev = hidinput->input;
	struct hidpp_ff_private_data *data = idev->ff->private;

	return scnprintf(buf, PAGE_SIZE, "%u\n", data->range);
}

static ssize_t hidpp_ff_range_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hid = to_hid_device(dev);
	struct hid_input *hidinput = list_entry(hid->inputs.next, struct hid_input, list);
	struct input_dev *idev = hidinput->input;
	struct hidpp_ff_private_data *data = idev->ff->private;
	u8 params[2];
	int range = simple_strtoul(buf, NULL, 10);

	range = clamp(range, 180, 900);

	params[0] = range >> 8;
	params[1] = range & 0x00FF;

	hidpp_ff_queue_work(data, -1, HIDPP_FF_SET_APERTURE, params, 2);

	return count;
}

static DEVICE_ATTR(range, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, hidpp_ff_range_show, hidpp_ff_range_store);

static void hidpp_ff_destroy(struct ff_device *ff)
{
	struct hidpp_ff_private_data *data = ff->private;

	kfree(data->effect_ids);
}

int hidpp_ff_init(struct hidpp_device *hidpp, u8 feature_index)
{
	struct hid_device *hid = hidpp->hid_dev;
	struct hid_input *hidinput = list_entry(hid->inputs.next, struct hid_input, list);
	struct input_dev *dev = hidinput->input;
	struct ff_device *ff;
	struct hidpp_report response;
	struct hidpp_ff_private_data *data;
	int error, j, num_slots;

	if (!dev) {
		hid_err(hid, "Struct input_dev not set!\n");
		return -EINVAL;
	}

	/* Set supported force feedback capabilities */
	for (j = 0; hiddpp_ff_effects[j] >= 0; j++)
		set_bit(hiddpp_ff_effects[j], dev->ffbit);

	/* Read number of slots available in device */
	error = hidpp_send_fap_command_sync(hidpp, feature_index,
		HIDPP_FF_GET_INFO, NULL, 0, &response);
	if (error) {
		if (error < 0)
			return error;
		hid_err(hidpp->hid_dev, "%s: received protocol error 0x%02x\n",
			__func__, error);
		return -EPROTO;
	}

	num_slots = response.fap.params[0];

	error = input_ff_create(dev, num_slots);

	if (error) {
		hid_err(dev, "Failed to create FF device\n");
		return error;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->effect_ids = kzalloc(num_slots*sizeof(int), GFP_KERNEL);
	if (!data->effect_ids) {
		kfree(data);
		return -ENOMEM;
	}
	data->hidpp = hidpp;
	data->feature_index = feature_index;
	data->slot_autocenter = 0;
	data->num_effects = num_slots;
	for (j = 0; j < num_slots; j++)
		data->effect_ids[j] = -1;

	ff = dev->ff;
	ff->private = data;

	ff->upload = hidpp_ff_upload_effect;
	ff->erase = hidpp_ff_erase_effect;
	ff->playback = hidpp_ff_playback;
	ff->set_gain = hidpp_ff_set_gain;
	ff->set_autocenter = hidpp_ff_set_autocenter;
	ff->destroy = hidpp_ff_destroy;


	/* reset all forces */
	error = hidpp_send_fap_command_sync(hidpp, feature_index,
		HIDPP_FF_RESET_ALL, NULL, 0, &response);

	/* Read current Range */
	error = hidpp_send_fap_command_sync(hidpp, feature_index,
		HIDPP_FF_GET_APERTURE, NULL, 0, &response);
	if (error)
		hid_warn(hidpp->hid_dev, "Failed to read range from device.\n");
	data->range = error ? 900 : get_unaligned_be16(&response.fap.params[0]);

	/* Create sysfs interface */
	error = device_create_file(&(hidpp->hid_dev->dev), &dev_attr_range);
	if (error)
		hid_warn(hidpp->hid_dev, "Unable to create sysfs interface for \"range\", errno %d\n", error);

	/* Read the current gain values */
	error = hidpp_send_fap_command_sync(hidpp, feature_index,
		HIDPP_FF_GET_GLOBAL_GAINS, NULL, 0, &response);
	if (error)
		hid_warn(hidpp->hid_dev, "Failed to read gain values from device.\n");
	data->gain = error ? 0xffff : get_unaligned_be16(&response.fap.params[0]);
	/* ignore boost value at response.fap.params[2] */

	/* init the hardware command queue */
	data->wq = create_singlethread_workqueue("hidpp-ff-sendqueue");
	atomic_set(&data->workqueue_size, 0);

	return 0;
}

int hidpp_ff_deinit(struct hid_device *hid)
{
	struct hid_input *hidinput = list_entry(hid->inputs.next, struct hid_input, list);
	struct input_dev *dev = hidinput->input;
	struct hidpp_ff_private_data *data;

	if (!dev) {
		hid_err(hid, "Struct input_dev not found!\n");
		return -EINVAL;
	}

	hid_info(hid, "Unloading HID++ force feedback.\n");
	data = dev->ff->private;
	if (!data) {
		hid_err(hid, "Private data not found!\n");
		return -EINVAL;
	}

	destroy_workqueue(data->wq);
	device_remove_file(&hid->dev, &dev_attr_range);

	return 0;
}
