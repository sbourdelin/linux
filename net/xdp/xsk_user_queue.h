#ifndef XSK_USER_QUEUE_H_
#define XSK_USER_QUEUE_H_

#define XDP_KERNEL_HEADROOM 256 /* Headrom for XDP */

#define XSK_FRAME_COMPLETED XDP_DESC_KERNEL

enum xsk_validation {
	XSK_VALIDATION_NONE,	  /* No validation is performed */
	XSK_VALIDATION_RX,	  /* Only address to packet buffer validated */
	XSK_VALIDATION_TX	  /* Full descriptor is validated */
};

struct xsk_packet_array;

struct xsk_user_queue {
	int (*enqueue)(struct xsk_packet_array *pa, u32 cnt);
	int (*enqueue_completed)(struct xsk_packet_array *pa, u32 cnt);
	int (*dequeue)(struct xsk_packet_array *pa, u32 cnt);
	u32 (*get_ring_size)(struct xsk_user_queue *q);
	char *(*get_ring_address)(struct xsk_user_queue *q);
};

#endif /* XSK_USER_QUEUE_H_ */
