#ifndef _UAPI__LINUX_XDP_H__
#define _UAPI__LINUX_XDP_H__

/* User return codes for XDP prog type.
 * A valid XDP program must return one of these defined values. All other
 * return codes are reserved for future use. Unknown return codes will result
 * in packet drop.
 */
enum xdp_action {
	XDP_ABORTED = 0,
	XDP_DROP,
	XDP_PASS,
	XDP_TX,
};

/* user accessible metadata for XDP packet hook
 * new fields must be added to the end of this structure
 */
struct xdp_md {
	__u32 data;
	__u32 data_end;
};

#endif /* _UAPI__LINUX_XDP_H__ */
