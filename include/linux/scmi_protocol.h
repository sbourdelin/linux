/*
 * SCMI Message Protocol driver header
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
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/types.h>

#define SCMI_MAX_STR_SIZE		16

/**
 * struct scmi_revision_info - version information structure
 *
 * @major_ver: Major ABI version. Change here implies risk of backward
 *	compatibility break.
 * @minor_ver: Minor ABI version. Change here implies new feature addition,
 *	or compatible change in ABI.
 * @num_protocols: Number of protocols that are implemented, excluding the
 *	base protocol.
 * @num_agents: Number of agents in the system.
 * @impl_ver: A vendor-specific implementation version.
 * @vendor_id: A vendor identifier(Null terminated ASCII string)
 * @sub_vendor_id: A sub-vendor identifier(Null terminated ASCII string)
 */
struct scmi_revision_info {
	u16 major_ver;
	u16 minor_ver;
	u8 num_protocols;
	u8 num_agents;
	u32 impl_ver;
	char vendor_id[SCMI_MAX_STR_SIZE];
	char sub_vendor_id[SCMI_MAX_STR_SIZE];
};

struct scmi_handle;

/**
 * struct scmi_clk_ops - represents the various operations provided
 *	by SCMI Clock Protocol
 *
 * @rate_get: request the current clock rate of a clock
 * @rate_set: set the clock rate of a clock
 * @enable: enables the specified clock
 * @disable: disables the specified clock
 */
struct scmi_clk_ops {
	int (*rate_get)(struct scmi_handle *, u32, u64*);
	int (*rate_set)(struct scmi_handle *, u32, u32, u64);
	int (*enable)(struct scmi_handle *, u32);
	int (*disable)(struct scmi_handle *, u32);
};

/**
 * struct scmi_perf_ops - represents the various operations provided
 *	by SCMI Performance Protocol
 *
 * @limits_set: sets limits on the performance level of a domain
 * @limits_get: gets limits on the performance level of a domain
 * @level_set: sets the performance level of a domain
 * @level_get: gets the performance level of a domain
 * @limits_notify_enable: requests notifications from the platform for changes
 *	in the allowed maximum and minimum performance levels
 * @level_notify_enable: requests notifications from the platform when the
 *	performance level for a domain changes in value
 */
struct scmi_perf_ops {
	int (*limits_set)(struct scmi_handle *, u32, u32, u32);
	int (*limits_get)(struct scmi_handle *, u32, u32 *, u32 *);
	int (*level_set)(struct scmi_handle *, u32, u32 level);
	int (*level_get)(struct scmi_handle *, u32, u32 *);
	int (*limits_notify_enable)(struct scmi_handle *, u32, bool);
	int (*level_notify_enable)(struct scmi_handle *, u32, bool);
};

/**
 * struct scmi_power_ops - represents the various operations provided
 *	by SCMI Power Protocol
 *
 * @state_set: sets the power state of a power domain
 * @state_get: gets the power state of a power domain
 * @state_notify_enable: request notifications from the platform for
 *	state changes in a specific power domain
 */
struct scmi_power_ops {
	int (*state_set)(struct scmi_handle *, u32, u32);
	int (*state_get)(struct scmi_handle *, u32, u32 *);
	int (*state_notify_enable)(struct scmi_handle *, u32, bool);
};

/**
 * struct scmi_handle - Handle returned to ARM SCMI clients for usage.
 *
 * @dev: pointer to the SCMI device
 * @version: pointer to the structure containing SCMI version information
 * @power_ops: pointer to set of power protocol operations
 * @perf_ops: pointer to set of performance protocol operations
 * @clk_ops: pointer to set of clock protocol operations
 */
struct scmi_handle {
	struct device *dev;
	struct scmi_revision_info *version;
	struct scmi_power_ops *power_ops;
	struct scmi_perf_ops *perf_ops;
	struct scmi_clk_ops *clk_ops;
};

struct scmi_opp {
	u32 freq;
	u32 volt;
	u32 trans_latency_us;
};

#if IS_REACHABLE(CONFIG_ARM_SCMI_PROTOCOL)
int scmi_put_handle(const struct scmi_handle *handle);
const struct scmi_handle *scmi_handle_get(struct device *dev);
const struct scmi_handle *devm_scmi_handle_get(struct device *dev);
#else
static inline int scmi_put_handle(const struct scmi_handle *handle)
{
	return 0;
}

static inline const struct scmi_handle *scmi_handle_get(struct device *dev)
{
	return NULL;
}

static inline const struct scmi_handle *devm_scmi_handle_get(struct device *dev)
{
	return NULL;
}
#endif /* CONFIG_ARM_SCMI_PROTOCOL */
