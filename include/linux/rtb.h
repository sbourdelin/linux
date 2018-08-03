/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RTB_H
#define _RTB_H

struct rtb_layout {
	const char *log_type;
	u32 idx;
	u64 caller;
	u64 data;
	u64 timestamp;
} __attribute__ ((__packed__));

#if defined(CONFIG_RTB)
void uncached_logk(const char *log_type, void *data);
int rtb_init(void);
void rtb_exit(void);
#else
static inline void uncached_logk(const char *log_type,
				void *data) { }
static inline int rtb_init(void) { return 0; }
static inline void rtb_exit(void) { }
#endif

#if defined(CONFIG_PSTORE_RTB)
extern void pstore_rtb_call(struct rtb_layout *start);
#else
static inline void pstore_rtb_call(struct rtb_layout *start)
{ }
#endif

#endif /* _RTB_H */
