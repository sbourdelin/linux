#ifndef _NF_CONNTRACK_COUNT_H
#define _NF_CONNTRACK_COUNT_H

struct nf_conncount_data;

struct nf_conncount_data *nf_conncount_init(struct net *net, unsigned int family,
					    unsigned int keylen);
void nf_conncount_destroy(struct net *net, unsigned int family,
			  struct nf_conncount_data *data);

unsigned int nf_conncount_count(struct net *net,
				struct nf_conncount_data *data,
				const u32 *key,
				const struct nf_conntrack_tuple *tuple,
				const struct nf_conntrack_zone *zone);

struct kmem_cache;

struct kmem_cache *nf_conncount_cache_alloc(const char *cache_name);
void nf_conncount_cache_free(struct kmem_cache *cache, struct hlist_head *hhead);

unsigned int nf_conncount_lookup(struct net *net, struct kmem_cache *cache,
				 struct hlist_head *head,
				 const struct nf_conntrack_tuple *tuple,
				 const struct nf_conntrack_zone *zone,
				 bool *addit);

bool nf_conncount_add(struct kmem_cache *cache, struct hlist_head *head,
		      const struct nf_conntrack_tuple *tuple);

#endif
