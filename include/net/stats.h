#ifndef _NET_STATS_H_
#define _NET_STATS_H_

typedef int (*net_stats_cb_t)(int cpu);

extern int register_net_stats_cb(net_stats_cb_t func);
extern int unregister_net_stats_cb(net_stats_cb_t func);

#endif /* _NET_STATS_H_ */
