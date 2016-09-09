#ifndef __NET_RESOLVER_H
#define __NET_RESOLVER_H

#include <linux/rhashtable.h>

struct net_rslv;
struct net_rslv_ent;

typedef int (*net_rslv_cmpfn)(struct net_rslv *nrslv, const void *key,
			      const void *object);
typedef void (*net_rslv_initfn)(struct net_rslv *nrslv, void *object);
typedef void (*net_rslv_destroyfn)(struct net_rslv_ent *nrent);

struct net_rslv {
	struct rhashtable rhash_table;
	struct rhashtable_params params;
	net_rslv_cmpfn rslv_cmp;
	net_rslv_initfn rslv_init;
	net_rslv_destroyfn rslv_destroy;
	size_t obj_size;
	spinlock_t *locks;
	unsigned int locks_mask;
	unsigned int hash_rnd;
	long timeout;
};

struct net_rslv_ent {
	struct rcu_head rcu;
	union {
		/* Fields set when entry is in hash table */
		struct {
			struct rhash_head node;
			struct delayed_work timeout_work;
			struct net_rslv *nrslv;
		};

		/* Fields set when rcu freeing structure */
		struct {
			net_rslv_destroyfn destroy;
		};
	};
	char object[];
};

struct net_rslv *net_rslv_create(size_t size, size_t key_len,
				 size_t max_size, long timeout,
				 net_rslv_cmpfn cmp_fn,
				 net_rslv_initfn init_fn,
				 net_rslv_destroyfn destroy_fn);

struct net_rslv_ent *net_rslv_lookup_and_create(struct net_rslv *nrslv,
						void *key, bool *created);

void net_rslv_resolved(struct net_rslv *nrslv, void *key);

void net_rslv_destroy(struct net_rslv *nrslv);

#endif
