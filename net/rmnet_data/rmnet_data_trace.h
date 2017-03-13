/* Copyright (c) 2014-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM rmnet_data
#define TRACE_INCLUDE_FILE rmnet_data_trace

#if !defined(_RMNET_DATA_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _RMNET_DATA_TRACE_H_

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS
	(rmnet_handler_template,

	TP_PROTO(struct sk_buff *skb),

	TP_ARGS(skb),

	TP_STRUCT__entry(
		__field(void *, skbaddr)
		__field(unsigned int, len)
		__string(name, skb->dev->name)
	),

	TP_fast_assign(
		__entry->skbaddr = skb;
		__entry->len = skb->len;
		__assign_str(name, skb->dev->name);
	),

	TP_printk("dev=%s skbaddr=%pK len=%u",
		  __get_str(name), __entry->skbaddr, __entry->len)
)

DEFINE_EVENT
	(rmnet_handler_template, rmnet_egress_handler,

	TP_PROTO(struct sk_buff *skb),

	TP_ARGS(skb)
);

DEFINE_EVENT
	(rmnet_handler_template, rmnet_ingress_handler,

	TP_PROTO(struct sk_buff *skb),

	TP_ARGS(skb)
);

DEFINE_EVENT
	(rmnet_handler_template, rmnet_vnd_start_xmit,

	TP_PROTO(struct sk_buff *skb),

	TP_ARGS(skb)
);

DEFINE_EVENT
	(rmnet_handler_template, __rmnet_deliver_skb,

	TP_PROTO(struct sk_buff *skb),

	TP_ARGS(skb)
);

TRACE_EVENT
	(rmnet_start_deaggregation,

	TP_PROTO(struct sk_buff *skb),

	TP_ARGS(skb),

	TP_STRUCT__entry(
		__string(name, skb->dev->name)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
	),

	TP_printk("dev: %s, deaggregated first packet", __get_str(name))
)

TRACE_EVENT
	(rmnet_end_deaggregation,

	TP_PROTO(struct sk_buff *skb, int num_deagg_packets),

	TP_ARGS(skb, num_deagg_packets),

	TP_STRUCT__entry(
		__string(name, skb->dev->name)
		__field(int, num)
	),

	TP_fast_assign(
		__assign_str(name, skb->dev->name);
		__entry->num = num_deagg_packets;
	),

	TP_printk("dev: %s, deaggregate end count: %d",
		  __get_str(name), __entry->num)
)

DECLARE_EVENT_CLASS
	(rmnet_physdev_action_template,

	TP_PROTO(struct net_device *dev),

	TP_ARGS(dev),

	TP_STRUCT__entry(
		__string(name, dev->name)
	),

	TP_fast_assign(
		__assign_str(name, dev->name);
	),

	TP_printk("Physical dev=%s", __get_str(name))
)

DEFINE_EVENT
	(rmnet_physdev_action_template, rmnet_unregister_cb_unhandled,

	TP_PROTO(struct net_device *dev),

	TP_ARGS(dev)
);

DEFINE_EVENT
	(rmnet_physdev_action_template, rmnet_unregister_cb_entry,

	TP_PROTO(struct net_device *dev),

	TP_ARGS(dev)
);

DEFINE_EVENT
	(rmnet_physdev_action_template, rmnet_unregister_cb_exit,

	TP_PROTO(struct net_device *dev),

	TP_ARGS(dev)
);

DEFINE_EVENT
	(rmnet_physdev_action_template, rmnet_unregister_cb_clear_vnds,

	TP_PROTO(struct net_device *dev),

	TP_ARGS(dev)
);

DEFINE_EVENT
	(rmnet_physdev_action_template, rmnet_unregister_cb_clear_lepcs,

	TP_PROTO(struct net_device *dev),

	TP_ARGS(dev)
);

DEFINE_EVENT
	(rmnet_physdev_action_template, rmnet_associate,

	TP_PROTO(struct net_device *dev),

	TP_ARGS(dev)
);

DEFINE_EVENT
	(rmnet_physdev_action_template, rmnet_unassociate,

	TP_PROTO(struct net_device *dev),

	TP_ARGS(dev)
);

#endif /* _RMNET_DATA_TRACE_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#include <trace/define_trace.h>

