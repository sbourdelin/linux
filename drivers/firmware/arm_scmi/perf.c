/*
 * System Control and Management Interface (SCMI) Performance Protocol
 *
 * Copyright (C) 2017 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"

enum scmi_performance_protocol_cmd {
	PERF_DOMAIN_ATTRIBUTES = 0x3,
	PERF_DESCRIBE_LEVELS = 0x4,
	PERF_LIMITS_SET = 0x5,
	PERF_LIMITS_GET = 0x6,
	PERF_LEVEL_SET = 0x7,
	PERF_LEVEL_GET = 0x8,
	PERF_NOTIFY_LIMITS = 0x9,
	PERF_NOTIFY_LEVEL = 0xa,
};

struct scmi_msg_resp_perf_attributes {
	__le16 num_domains;
	__le16 flags;
#define POWER_SCALE_IN_MILLIWATT(x)	((x) & BIT(0))
	__le32 stats_addr_low;
	__le32 stats_addr_high;
	__le32 stats_size;
} __packed;

struct scmi_msg_resp_perf_domain_attributes {
	__le32 flags;
#define SUPPORTS_SET_LIMITS(x)		((x) & BIT(31))
#define SUPPORTS_SET_PERF_LVL(x)	((x) & BIT(30))
#define SUPPORTS_PERF_LIMIT_NOTIFY(x)	((x) & BIT(29))
#define SUPPORTS_PERF_LEVEL_NOTIFY(x)	((x) & BIT(28))
	__le16 rate_limit_us;
	__le16 reserved3;
	__le32 max_freq;
	__le32 min_freq;
#define FREQUENCY_BASE(x)		((x) >> 16)
#define FREQUENCY_SCALE(x)		((x) & 0x3f)
	    u8 name[SCMI_MAX_STR_SIZE];
} __packed;

struct scmi_msg_perf_describe_levels {
	__le32 domain;
	__le32 level_index;
} __packed;

struct scmi_perf_set_limits {
	__le32 domain;
	__le32 max_level;
	__le32 min_level;
} __packed;

struct scmi_perf_get_limits {
	__le32 max_level;
	__le32 min_level;
} __packed;

struct scmi_perf_set_level {
	__le32 domain;
	__le32 level;
} __packed;

struct scmi_perf_notify_level_or_limits {
	__le32 domain;
	__le32 notify_enable;
} __packed;

struct scmi_msg_resp_perf_describe_levels {
	__le16 num_returned;
	__le16 num_remaining;
	struct {
		__le32 perf_val;
		__le32 power;
		__le16 transition_latency_us;
		__le16 reserved;
	} opp[0];
} __packed;

struct perf_dom_info {
	bool set_limits;
	bool set_perf;
	bool perf_limit_notify;
	bool perf_level_notify;
	char name[SCMI_MAX_STR_SIZE];
	struct scmi_opp opp[MAX_OPPS];
};

struct scmi_perf_info {
	int num_domains;
	bool power_scale_mw;
	u64 stats_addr;
	u32 stats_size;
	struct perf_dom_info *dom_info;
};

static struct scmi_perf_info perf_info;

static int scmi_perf_attributes_get(struct scmi_handle *handle,
				    struct scmi_perf_info *perf_info)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_perf_attributes *attr;

	ret = scmi_one_xfer_init(handle, PROTOCOL_ATTRIBUTES,
				 SCMI_PROTOCOL_PERF, 0, sizeof(*attr), &t);
	if (ret)
		return ret;

	attr = (struct scmi_msg_resp_perf_attributes *)t->rx.buf;

	ret = scmi_do_xfer(handle, t);
	if (!ret) {
		u16 flags = le16_to_cpu(attr->flags);

		perf_info->num_domains = le16_to_cpu(attr->num_domains);
		perf_info->power_scale_mw = POWER_SCALE_IN_MILLIWATT(flags);
		perf_info->stats_addr = le32_to_cpu(attr->stats_addr_low) |
				(u64)le32_to_cpu(attr->stats_addr_high) << 32;
		perf_info->stats_size = le32_to_cpu(attr->stats_size);
	}

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int
scmi_perf_domain_attributes_get(struct scmi_handle *handle, u32 domain,
				struct perf_dom_info *dom_info)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_perf_domain_attributes *attr;

	ret = scmi_one_xfer_init(handle, PERF_DOMAIN_ATTRIBUTES,
				 SCMI_PROTOCOL_PERF, sizeof(domain),
				 sizeof(*attr), &t);
	if (ret)
		return ret;

	*(__le32 *)t->tx.buf = cpu_to_le32(domain);
	attr = (struct scmi_msg_resp_perf_domain_attributes *)t->rx.buf;

	ret = scmi_do_xfer(handle, t);
	if (!ret) {
		u32 flags = le32_to_cpu(attr->flags);

		dom_info->set_limits = SUPPORTS_SET_LIMITS(flags);
		dom_info->set_perf = SUPPORTS_SET_PERF_LVL(flags);
		dom_info->perf_limit_notify = SUPPORTS_PERF_LIMIT_NOTIFY(flags);
		dom_info->perf_level_notify = SUPPORTS_PERF_LEVEL_NOTIFY(flags);
		memcpy(dom_info->name, attr->name, SCMI_MAX_STR_SIZE);
	}

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int scmi_perf_describe_levels_get(struct scmi_handle *handle, u32 domain,
					 struct perf_dom_info *perf_dom)
{
	int ret, cnt;
	u32 tot_opp_cnt = 0;
	u16 num_returned, num_remaining;
	struct scmi_xfer *t;
	struct scmi_opp *opp;
	struct scmi_msg_perf_describe_levels *dom_info;
	struct scmi_msg_resp_perf_describe_levels *level_info;

	ret = scmi_one_xfer_init(handle, PERF_DESCRIBE_LEVELS,
				 SCMI_PROTOCOL_PERF, sizeof(*dom_info), 0, &t);
	if (ret)
		return ret;

	dom_info = (struct scmi_msg_perf_describe_levels *)t->tx.buf;
	level_info = (struct scmi_msg_resp_perf_describe_levels *)t->rx.buf;

	do {
		dom_info->domain = cpu_to_le32(domain);
		/* Set the number of OPPs to be skipped/already read */
		dom_info->level_index = cpu_to_le32(tot_opp_cnt);

		ret = scmi_do_xfer(handle, t);
		if (ret)
			break;

		num_returned = le16_to_cpu(level_info->num_returned);
		num_remaining = le16_to_cpu(level_info->num_remaining);
		if (tot_opp_cnt + num_returned > MAX_OPPS) {
			dev_err(handle->dev, "No. of OPPs exceeded MAX_OPPS");
			break;
		}

		opp = &perf_dom->opp[tot_opp_cnt];
		for (cnt = 0; cnt < num_returned; cnt++, opp++) {
			opp->freq = le32_to_cpu(level_info->opp[cnt].perf_val);
			opp->volt = le32_to_cpu(level_info->opp[cnt].power);
			opp->trans_latency_us = le16_to_cpu(
				level_info->opp[cnt].transition_latency_us);

			dev_dbg(handle->dev, "Level %d Power %d Latency %dus\n",
				opp->freq, opp->volt, opp->trans_latency_us);
		}

		tot_opp_cnt += num_returned;
		/*
		 * check for both returned and remaining to avoid infinite
		 * loop due to buggy firmware
		 */
	} while (num_returned && num_remaining);

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int scmi_perf_limits_set(struct scmi_handle *handle, u32 domain,
				u32 max_perf, u32 min_perf)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_perf_set_limits *limits;

	ret = scmi_one_xfer_init(handle, PERF_LIMITS_SET, SCMI_PROTOCOL_PERF,
				 sizeof(*limits), 0, &t);
	if (ret)
		return ret;

	limits = (struct scmi_perf_set_limits *)t->tx.buf;
	limits->domain = cpu_to_le32(domain);
	limits->max_level = cpu_to_le32(max_perf);
	limits->min_level = cpu_to_le32(min_perf);

	ret = scmi_do_xfer(handle, t);

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int scmi_perf_limits_get(struct scmi_handle *handle, u32 domain,
				u32 *max_perf, u32 *min_perf)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_perf_get_limits *limits;

	ret = scmi_one_xfer_init(handle, PERF_LIMITS_GET, SCMI_PROTOCOL_PERF,
				 sizeof(__le32), 0, &t);
	if (ret)
		return ret;

	*(__le32 *)t->tx.buf = cpu_to_le32(domain);

	ret = scmi_do_xfer(handle, t);
	if (!ret) {
		limits = (struct scmi_perf_get_limits *)t->rx.buf;

		*max_perf = le32_to_cpu(limits->max_level);
		*min_perf = le32_to_cpu(limits->min_level);
	}

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int
scmi_perf_level_set(struct scmi_handle *handle, u32 domain, u32 level)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_perf_set_level *lvl;

	ret = scmi_one_xfer_init(handle, PERF_LEVEL_SET, SCMI_PROTOCOL_PERF,
				 sizeof(*lvl), 0, &t);
	if (ret)
		return ret;

	lvl = (struct scmi_perf_set_level *)t->tx.buf;
	lvl->domain = cpu_to_le32(domain);
	lvl->level = cpu_to_le32(level);

	ret = scmi_do_xfer(handle, t);

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int
scmi_perf_level_get(struct scmi_handle *handle, u32 domain, u32 *level)
{
	int ret;
	struct scmi_xfer *t;

	ret = scmi_one_xfer_init(handle, PERF_LEVEL_GET, SCMI_PROTOCOL_PERF,
				 sizeof(u32), sizeof(u32), &t);
	if (ret)
		return ret;

	*(__le32 *)t->tx.buf = cpu_to_le32(domain);

	ret = scmi_do_xfer(handle, t);
	if (!ret)
		*level = le32_to_cpu(*(__le32 *)t->rx.buf);

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int __scmi_perf_notify_enable(struct scmi_handle *handle, u32 cmd,
				     u32 domain, bool enable)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_perf_notify_level_or_limits *notify;

	ret = scmi_one_xfer_init(handle, cmd, SCMI_PROTOCOL_PERF,
				 sizeof(*notify), 0, &t);
	if (ret)
		return ret;

	notify = (struct scmi_perf_notify_level_or_limits *)t->tx.buf;
	notify->domain = cpu_to_le32(domain);
	notify->notify_enable = cpu_to_le32(enable & BIT(0));

	ret = scmi_do_xfer(handle, t);

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int
scmi_perf_limits_notify_enable(struct scmi_handle *handle, u32 dom, bool en)
{
	return __scmi_perf_notify_enable(handle, PERF_NOTIFY_LIMITS, dom, en);
}

static int
scmi_perf_level_notify_enable(struct scmi_handle *handle, u32 dom, bool en)
{
	return __scmi_perf_notify_enable(handle, PERF_NOTIFY_LEVEL, dom, en);
}

static struct scmi_perf_ops perf_ops = {
	.limits_set = scmi_perf_limits_set,
	.limits_get = scmi_perf_limits_get,
	.level_set = scmi_perf_level_set,
	.level_get = scmi_perf_level_get,
	.limits_notify_enable = scmi_perf_limits_notify_enable,
	.level_notify_enable = scmi_perf_level_notify_enable,
};

int scmi_perf_protocol_init(struct scmi_handle *handle)
{
	int domain;
	u32 version;

	if (!scmi_is_protocol_implemented(handle, SCMI_PROTOCOL_PERF)) {
		dev_err(handle->dev, "SCMI Perf protocol not implemented\n");
		return -EPROTONOSUPPORT;
	}

	scmi_version_get(handle, SCMI_PROTOCOL_PERF, &version);

	dev_dbg(handle->dev, "Performance Version %d.%d\n",
		PROTOCOL_REV_MAJOR(version), PROTOCOL_REV_MINOR(version));

	scmi_perf_attributes_get(handle, &perf_info);

	perf_info.dom_info = devm_kcalloc(handle->dev, perf_info.num_domains,
					     sizeof(struct perf_dom_info),
					     GFP_KERNEL);
	if (!perf_info.dom_info)
		return -ENOMEM;

	for (domain = 0; domain < perf_info.num_domains; domain++) {
		struct perf_dom_info *dom = perf_info.dom_info + domain;

		scmi_perf_domain_attributes_get(handle, domain, dom);
		scmi_perf_describe_levels_get(handle, domain, dom);
	}

	handle->perf_ops = &perf_ops;

	return 0;
}
