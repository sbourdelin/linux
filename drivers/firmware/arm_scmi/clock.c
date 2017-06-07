/*
 * System Control and Management Interface (SCMI) Clock Protocol
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

enum scmi_clock_protocol_cmd {
	CLOCK_ATTRIBUTES = 0x3,
	CLOCK_DESCRIBE_RATES = 0x4,
	CLOCK_RATE_SET = 0x5,
	CLOCK_RATE_GET = 0x6,
	CLOCK_CONFIG_SET = 0x7,
};

struct scmi_msg_resp_clock_protocol_attributes {
	__le16 num_clocks;
	    u8 max_async_req;
	    u8 reserved;
} __packed;

struct scmi_msg_resp_clock_attributes {
	__le32 attributes;
#define	CLOCK_ENABLE	BIT(0)
	    u8 name[SCMI_MAX_STR_SIZE];
} __packed;

struct scmi_clock_set_config {
	__le32 id;
	__le32 attributes;
} __packed;

struct scmi_msg_clock_describe_rates {
	__le32 id;
	__le32 rate_index;
} __packed;

struct scmi_msg_resp_clock_describe_rates {
	__le16 num_returned;
#define NUM_RETURNED_MASK	(0xfff)
#define RATE_DISCRETE(x)	((x) & BIT(12))
	__le16 num_remaining;
	struct {
		__le32 value_low;
		__le32 value_high;
	} rate[0];
#define RATE_TO_U64(X)		\
({				\
	typeof(X) x = (X);	\
	le32_to_cpu((x).value_low) | (u64)le32_to_cpu((x).value_high) >> 32; \
})
} __packed;

struct scmi_clock_set_rate {
	__le32 flags;
#define CLOCK_SET_ASYSC		BIT(0)
#define CLOCK_SET_DELAYED	BIT(1)
#define CLOCK_ROUND_UP		BIT(2)
#define CLOCK_ROUND_AUTO	BIT(3)
	__le32 id;
	__le32 value_low;
	__le32 value_high;
} __packed;

struct clock_info {
	u32 attributes;
	char name[SCMI_MAX_STR_SIZE];
	union {
		int num_rates;
		u64 rates[MAX_NUM_RATES];
		struct {
			u64 min_rate;
			u64 max_rate;
			u64 step_size;
		} range;
	};
};

struct scmi_clock_info {
	int num_clocks;
	int max_async_req;
	struct clock_info *clk;
};

static struct scmi_clock_info clocks;

static int scmi_clock_protocol_attributes_get(struct scmi_handle *handle,
					      struct scmi_clock_info *clocks)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_clock_protocol_attributes *attr;

	ret = scmi_one_xfer_init(handle, PROTOCOL_ATTRIBUTES,
				 SCMI_PROTOCOL_CLOCK, 0, sizeof(*attr), &t);
	if (ret)
		return ret;

	attr = (struct scmi_msg_resp_clock_protocol_attributes *)t->rx.buf;

	ret = scmi_do_xfer(handle, t);
	if (!ret) {
		clocks->num_clocks = le16_to_cpu(attr->num_clocks);
		clocks->max_async_req = attr->max_async_req;
	}

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int scmi_clock_attributes_get(struct scmi_handle *handle, u32 clk_id,
				     struct clock_info *clk)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_clock_attributes *attr;

	ret = scmi_one_xfer_init(handle, CLOCK_ATTRIBUTES, SCMI_PROTOCOL_CLOCK,
				 sizeof(clk_id), sizeof(*attr), &t);
	if (ret)
		return ret;

	*(__le32 *)t->tx.buf = cpu_to_le32(clk_id);
	attr = (struct scmi_msg_resp_clock_attributes *)t->rx.buf;

	ret = scmi_do_xfer(handle, t);
	if (!ret) {
		clk->attributes = le32_to_cpu(attr->attributes);
		memcpy(clk->name, attr->name, SCMI_MAX_STR_SIZE);
	}

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int scmi_clock_describe_rates_get(struct scmi_handle *handle, u32 clk_id,
					 struct clock_info *clk)
{
	u64 *rate;
	int ret, cnt;
	bool rate_discrete;
	u32 tot_rate_cnt = 0;
	u16 num_returned, num_remaining;
	struct scmi_xfer *t;
	struct scmi_msg_clock_describe_rates *clk_desc;
	struct scmi_msg_resp_clock_describe_rates *rlist;

	ret = scmi_one_xfer_init(handle, CLOCK_DESCRIBE_RATES,
				 SCMI_PROTOCOL_CLOCK, sizeof(*clk_desc), 0, &t);
	if (ret)
		return ret;

	clk_desc = (struct scmi_msg_clock_describe_rates *)t->tx.buf;
	rlist = (struct scmi_msg_resp_clock_describe_rates *)t->rx.buf;

	do {
		clk_desc->id = cpu_to_le32(clk_id);
		/* Set the number of rates to be skipped/already read */
		clk_desc->rate_index = cpu_to_le32(tot_rate_cnt);

		ret = scmi_do_xfer(handle, t);
		if (ret)
			break;

		num_returned = le16_to_cpu(rlist->num_returned);
		num_remaining = le16_to_cpu(rlist->num_remaining);
		rate_discrete = RATE_DISCRETE(num_remaining);
		num_remaining &= NUM_RETURNED_MASK;

		if (tot_rate_cnt + num_returned > MAX_NUM_RATES) {
			dev_err(handle->dev, "No. of rates > MAX_NUM_RATES");
			break;
		}

		if (!rate_discrete) {
			clk->range.min_rate = RATE_TO_U64(rlist->rate[0]);
			clk->range.max_rate = RATE_TO_U64(rlist->rate[1]);
			clk->range.step_size = RATE_TO_U64(rlist->rate[2]);
			dev_dbg(handle->dev, "Min %llu Max %llu Step %llu Hz\n",
				clk->range.min_rate, clk->range.max_rate,
				clk->range.step_size);
			break;
		}

		rate = &clk->rates[tot_rate_cnt];
		for (cnt = 0; cnt < num_returned; cnt++, rate++) {
			*rate = RATE_TO_U64(rlist->rate[cnt]);
			dev_dbg(handle->dev, "Rate %llu Hz\n", *rate);
		}

		tot_rate_cnt += num_returned;
		/*
		 * check for both returned and remaining to avoid infinite
		 * loop due to buggy firmware
		 */
	} while (num_returned && num_remaining);

	clk->num_rates = tot_rate_cnt;

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int
scmi_clock_rate_get(struct scmi_handle *handle, u32 clk_id, u64 *value)
{
	int ret;
	struct scmi_xfer *t;

	ret = scmi_one_xfer_init(handle, CLOCK_RATE_GET, SCMI_PROTOCOL_CLOCK,
				 sizeof(__le32), sizeof(u64), &t);
	if (ret)
		return ret;

	*(__le32 *)t->tx.buf = cpu_to_le32(clk_id);

	ret = scmi_do_xfer(handle, t);
	if (!ret) {
		__le32 *pval = (__le32 *)t->rx.buf;

		*value = le32_to_cpu(*pval);
		*value  |= le32_to_cpu(*(pval + 1));
	}

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int scmi_clock_rate_set(struct scmi_handle *handle, u32 clk_id,
			       u32 config, u64 rate)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_clock_set_rate *cfg;

	ret = scmi_one_xfer_init(handle, CLOCK_RATE_SET, SCMI_PROTOCOL_CLOCK,
				 sizeof(*cfg), 0, &t);
	if (ret)
		return ret;

	cfg = (struct scmi_clock_set_rate *)t->tx.buf;
	cfg->flags = cpu_to_le32(config);
	cfg->id = cpu_to_le32(clk_id);
	cfg->value_low = cpu_to_le32(rate & 0xffffffff);
	cfg->value_high = cpu_to_le32(rate >> 32);

	ret = scmi_do_xfer(handle, t);

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int
scmi_clock_config_set(struct scmi_handle *handle, u32 clk_id, u32 config)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_clock_set_config *cfg;

	ret = scmi_one_xfer_init(handle, CLOCK_CONFIG_SET, SCMI_PROTOCOL_CLOCK,
				 sizeof(*cfg), 0, &t);
	if (ret)
		return ret;

	cfg = (struct scmi_clock_set_config *)t->tx.buf;
	cfg->id = cpu_to_le32(clk_id);
	cfg->attributes = cpu_to_le32(config);

	ret = scmi_do_xfer(handle, t);

	scmi_put_one_xfer(handle, t);
	return ret;
}

static int scmi_clock_enable(struct scmi_handle *handle, u32 clk_id)
{
	return scmi_clock_config_set(handle, clk_id, CLOCK_ENABLE);
}

static int scmi_clock_disable(struct scmi_handle *handle, u32 clk_id)
{
	return scmi_clock_config_set(handle, clk_id, 0);
}

static struct scmi_clk_ops clk_ops = {
	.rate_get = scmi_clock_rate_get,
	.rate_set = scmi_clock_rate_set,
	.enable = scmi_clock_enable,
	.disable = scmi_clock_disable,
};

int scmi_clock_protocol_init(struct scmi_handle *handle)
{
	int clk_id;
	u32 version;

	if (!scmi_is_protocol_implemented(handle, SCMI_PROTOCOL_CLOCK)) {
		dev_err(handle->dev, "SCMI Clock protocol not implemented\n");
		return -EPROTONOSUPPORT;
	}

	scmi_version_get(handle, SCMI_PROTOCOL_CLOCK, &version);

	dev_dbg(handle->dev, "Clock Version %d.%d\n",
		PROTOCOL_REV_MAJOR(version), PROTOCOL_REV_MINOR(version));

	scmi_clock_protocol_attributes_get(handle, &clocks);

	clocks.clk = devm_kcalloc(handle->dev, clocks.num_clocks,
				  sizeof(struct clock_info), GFP_KERNEL);
	if (!clocks.clk)
		return -ENOMEM;

	dev_info(handle->dev, "Num Clock %d Max Async Req %d\n",
		 clocks.num_clocks, clocks.max_async_req);

	for (clk_id = 0; clk_id < clocks.num_clocks; clk_id++) {
		struct clock_info *clk = clocks.clk + clk_id;

		scmi_clock_attributes_get(handle, clk_id, clk);
		scmi_clock_describe_rates_get(handle, clk_id, clk);
	}

	handle->clk_ops = &clk_ops;

	return 0;
}
