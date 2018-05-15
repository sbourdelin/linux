// SPDX-License-Identifier: GPL-2.0
// bpf-rawir-event.c - handles bpf
//
// Copyright (C) 2018 Sean Young <sean@mess.org>

#include <linux/bpf.h>
#include <linux/filter.h>
#include "rc-core-priv.h"

/*
 * BPF interface for raw IR
 */
const struct bpf_prog_ops rawir_event_prog_ops = {
};

BPF_CALL_1(bpf_rc_repeat, struct bpf_rawir_event*, event)
{
	struct ir_raw_event_ctrl *ctrl;

	ctrl = container_of(event, struct ir_raw_event_ctrl, bpf_rawir_event);

	rc_repeat(ctrl->dev);

	return 0;
}

static const struct bpf_func_proto rc_repeat_proto = {
	.func	   = bpf_rc_repeat,
	.gpl_only  = true, /* rc_repeat is EXPORT_SYMBOL_GPL */
	.ret_type  = RET_INTEGER,
	.arg1_type = ARG_PTR_TO_CTX,
};

BPF_CALL_4(bpf_rc_keydown, struct bpf_rawir_event*, event, u32, protocol,
	   u32, scancode, u32, toggle)
{
	struct ir_raw_event_ctrl *ctrl;

	ctrl = container_of(event, struct ir_raw_event_ctrl, bpf_rawir_event);

	rc_keydown(ctrl->dev, protocol, scancode, toggle != 0);

	return 0;
}

static const struct bpf_func_proto rc_keydown_proto = {
	.func	   = bpf_rc_keydown,
	.gpl_only  = true, /* rc_keydown is EXPORT_SYMBOL_GPL */
	.ret_type  = RET_INTEGER,
	.arg1_type = ARG_PTR_TO_CTX,
	.arg2_type = ARG_ANYTHING,
	.arg3_type = ARG_ANYTHING,
	.arg4_type = ARG_ANYTHING,
};

static const struct bpf_func_proto *
rawir_event_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_rc_repeat:
		return &rc_repeat_proto;
	case BPF_FUNC_rc_keydown:
		return &rc_keydown_proto;
	case BPF_FUNC_map_lookup_elem:
		return &bpf_map_lookup_elem_proto;
	case BPF_FUNC_map_update_elem:
		return &bpf_map_update_elem_proto;
	case BPF_FUNC_map_delete_elem:
		return &bpf_map_delete_elem_proto;
	case BPF_FUNC_ktime_get_ns:
		return &bpf_ktime_get_ns_proto;
	case BPF_FUNC_tail_call:
		return &bpf_tail_call_proto;
	case BPF_FUNC_get_prandom_u32:
		return &bpf_get_prandom_u32_proto;
	default:
		return NULL;
	}
}

static bool rawir_event_is_valid_access(int off, int size,
					enum bpf_access_type type,
					const struct bpf_prog *prog,
					struct bpf_insn_access_aux *info)
{
	/* struct bpf_rawir_event has two u32 fields */
	if (type == BPF_WRITE)
		return false;

	if (size != sizeof(__u32))
		return false;

	if (!(off == offsetof(struct bpf_rawir_event, duration) ||
	      off == offsetof(struct bpf_rawir_event, type)))
		return false;

	return true;
}

const struct bpf_verifier_ops rawir_event_verifier_ops = {
	.get_func_proto  = rawir_event_func_proto,
	.is_valid_access = rawir_event_is_valid_access
};

#define BPF_MAX_PROGS 64

static int rc_dev_bpf_attach(struct rc_dev *rcdev, struct bpf_prog *prog)
{
	struct ir_raw_event_ctrl *raw;
	struct bpf_prog_array __rcu *old_array;
	struct bpf_prog_array *new_array;
	int ret, i, size;

	if (rcdev->driver_type != RC_DRIVER_IR_RAW)
		return -EINVAL;

	ret = mutex_lock_interruptible(&rcdev->lock);
	if (ret)
		return ret;

	raw = rcdev->raw;

	if (raw->progs) {
		size = bpf_prog_array_length(raw->progs);
		for (i = 0; i < size; i++) {
			if (prog == raw->progs->progs[i]) {
				ret = -EEXIST;
				goto out;
			}
		}

		if (size >= BPF_MAX_PROGS) {
			ret = -E2BIG;
			goto out;
		}
	}

	old_array = raw->progs;
	ret = bpf_prog_array_copy(old_array, NULL, prog, &new_array);
	if (ret < 0)
		goto out;

	rcu_assign_pointer(raw->progs, new_array);
	bpf_prog_array_free(old_array);
out:
	mutex_unlock(&rcdev->lock);
	return ret;
}

static int rc_dev_bpf_detach(struct rc_dev *rcdev, struct bpf_prog *prog)
{
	struct ir_raw_event_ctrl *raw;
	struct bpf_prog_array __rcu *old_array;
	struct bpf_prog_array *new_array;
	int ret;

	if (rcdev->driver_type != RC_DRIVER_IR_RAW)
		return -EINVAL;

	ret = mutex_lock_interruptible(&rcdev->lock);
	if (ret)
		return ret;

	raw = rcdev->raw;

	old_array = raw->progs;
	ret = bpf_prog_array_copy(old_array, prog, NULL, &new_array);
	if (ret < 0) {
		bpf_prog_array_delete_safe(old_array, prog);
	} else {
		rcu_assign_pointer(raw->progs, new_array);
		bpf_prog_array_free(old_array);
	}

	bpf_prog_put(prog);
	mutex_unlock(&rcdev->lock);
	return 0;
}

void rc_dev_bpf_run(struct rc_dev *rcdev, struct ir_raw_event ev)
{
	struct ir_raw_event_ctrl *raw = rcdev->raw;

	if (!raw->progs)
		return;

	if (unlikely(ev.carrier_report)) {
		raw->bpf_rawir_event.carrier = ev.carrier;
		raw->bpf_rawir_event.type = BPF_RAWIR_EVENT_CARRIER;
	} else {
		raw->bpf_rawir_event.duration = ev.duration;

		if (ev.pulse)
			raw->bpf_rawir_event.type = BPF_RAWIR_EVENT_PULSE;
		else if (ev.timeout)
			raw->bpf_rawir_event.type = BPF_RAWIR_EVENT_TIMEOUT;
		else if (ev.reset)
			raw->bpf_rawir_event.type = BPF_RAWIR_EVENT_RESET;
		else
			raw->bpf_rawir_event.type = BPF_RAWIR_EVENT_SPACE;
	}

	BPF_PROG_RUN_ARRAY(raw->progs, &raw->bpf_rawir_event, BPF_PROG_RUN);
}

void rc_dev_bpf_put(struct rc_dev *rcdev)
{
	struct bpf_prog_array *progs = rcdev->raw->progs;
	int i, size;

	if (!progs)
		return;

	size = bpf_prog_array_length(progs);
	for (i = 0; i < size; i++)
		bpf_prog_put(progs->progs[i]);

	bpf_prog_array_free(rcdev->raw->progs);
}

int rc_dev_prog_attach(const union bpf_attr *attr)
{
	struct bpf_prog *prog;
	struct rc_dev *rcdev;
	int ret;

	if (attr->attach_flags)
		return -EINVAL;

	prog = bpf_prog_get_type(attr->attach_bpf_fd,
				 BPF_PROG_TYPE_RAWIR_EVENT);
	if (IS_ERR(prog))
		return PTR_ERR(prog);

	rcdev = rc_dev_get_from_fd(attr->target_fd);
	if (IS_ERR(rcdev)) {
		bpf_prog_put(prog);
		return PTR_ERR(rcdev);
	}

	ret = rc_dev_bpf_attach(rcdev, prog);
	if (ret)
		bpf_prog_put(prog);

	put_device(&rcdev->dev);

	return ret;
}

int rc_dev_prog_detach(const union bpf_attr *attr)
{
	struct bpf_prog *prog;
	struct rc_dev *rcdev;
	int ret;

	if (attr->attach_flags)
		return -EINVAL;

	prog = bpf_prog_get_type(attr->attach_bpf_fd,
				 BPF_PROG_TYPE_RAWIR_EVENT);
	if (IS_ERR(prog))
		return PTR_ERR(prog);

	rcdev = rc_dev_get_from_fd(attr->target_fd);
	if (IS_ERR(rcdev)) {
		bpf_prog_put(prog);
		return PTR_ERR(rcdev);
	}

	ret = rc_dev_bpf_detach(rcdev, prog);

	bpf_prog_put(prog);
	put_device(&rcdev->dev);

	return ret;
}

int rc_dev_prog_query(const union bpf_attr *attr, union bpf_attr __user *uattr)
{
	__u32 __user *prog_ids = u64_to_user_ptr(attr->query.prog_ids);
	struct bpf_prog_array *progs;
	struct rc_dev *rcdev;
	u32 cnt, flags = 0;
	int ret;

	if (attr->query.query_flags)
		return -EINVAL;

	rcdev = rc_dev_get_from_fd(attr->query.target_fd);
	if (IS_ERR(rcdev))
		return PTR_ERR(rcdev);

	if (rcdev->driver_type != RC_DRIVER_IR_RAW) {
		ret = -EINVAL;
		goto out;
	}

	ret = mutex_lock_interruptible(&rcdev->lock);
	if (ret)
		goto out;

	progs = rcdev->raw->progs;
	cnt = progs ? bpf_prog_array_length(progs) : 0;

	if (copy_to_user(&uattr->query.prog_cnt, &cnt, sizeof(cnt))) {
		ret = -EFAULT;
		goto out;
	}
	if (copy_to_user(&uattr->query.attach_flags, &flags, sizeof(flags))) {
		ret = -EFAULT;
		goto out;
	}

	if (attr->query.prog_cnt != 0 && prog_ids && cnt)
		ret = bpf_prog_array_copy_to_user(progs, prog_ids, cnt);

out:
	mutex_unlock(&rcdev->lock);
	put_device(&rcdev->dev);

	return ret;
}
