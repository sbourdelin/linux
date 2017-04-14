#ifndef _RMNET_DATA_H_
#define _RMNET_DATA_H_

/* Constants */
#define RMNET_EGRESS_FORMAT__RESERVED__         (1<<0)
#define RMNET_EGRESS_FORMAT_MAP                 (1<<1)
#define RMNET_EGRESS_FORMAT_AGGREGATION         (1<<2)
#define RMNET_EGRESS_FORMAT_MUXING              (1<<3)
#define RMNET_EGRESS_FORMAT_MAP_CKSUMV3         (1<<4)
#define RMNET_EGRESS_FORMAT_MAP_CKSUMV4         (1<<5)

#define RMNET_INGRESS_FIX_ETHERNET              (1<<0)
#define RMNET_INGRESS_FORMAT_MAP                (1<<1)
#define RMNET_INGRESS_FORMAT_DEAGGREGATION      (1<<2)
#define RMNET_INGRESS_FORMAT_DEMUXING           (1<<3)
#define RMNET_INGRESS_FORMAT_MAP_COMMANDS       (1<<4)
#define RMNET_INGRESS_FORMAT_MAP_CKSUMV3        (1<<5)
#define RMNET_INGRESS_FORMAT_MAP_CKSUMV4        (1<<6)

/* Pass the frame up the stack with no modifications to skb->dev */
#define RMNET_EPMODE_NONE (0)
/* Replace skb->dev to a virtual rmnet device and pass up the stack */
#define	RMNET_EPMODE_VND (1)
/* Pass the frame directly to another device with dev_queue_xmit() */
#define	RMNET_EPMODE_BRIDGE (2)

enum {
	IFLA_RMNET_UNSPEC,
	IFLA_RMNET_MUX_ID,
	__IFLA_RMNET_MAX,
};
#define __IFLA_RMNET_MAX	(__IFLA_RMNET_MAX - 1)

#endif /* _RMNET_DATA_H_ */
