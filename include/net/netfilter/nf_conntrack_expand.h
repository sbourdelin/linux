#ifndef _NF_CONNTRACK_EXPAND_H
#define _NF_CONNTRACK_EXPAND_H

#include <linux/types.h>
#include <net/netfilter/nf_conntrack.h>

#define NF_EXPAND_NAMSIZ 16

/* expansion type */
struct nf_ct_expand_type {
	struct hlist_node node;	/* private */
	/* Destroys relationships (can be NULL) */
	void (*destroy)(void *data);
	/* unique name, not more than NF_EXPAND_NAMSIZ */
	const char *name;
	int len;
	int align;
};


int nf_ct_expand_type_register(struct nf_ct_expand_type *type);
int nf_ct_expand_type_unregister(struct nf_ct_expand_type *type);
void *nf_ct_expand_area_find(struct nf_conn *ct, const char *name);
void *nf_ct_expand_area_add(struct nf_conn *ct, const char *name, gfp_t gfp);

#endif /* _NF_CONNTRACK_EXPAND_H */
