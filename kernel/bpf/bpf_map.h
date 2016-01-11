#ifndef INT_BPF_MAP_COMMON_H
#define INT_BPF_MAP_COMMON_H

#include <linux/bpf.h>

extern void *map_lookup_elem_nop(struct bpf_map *map, void *key);
extern int map_delete_elem_nop(struct bpf_map *map, void *key);

#endif
