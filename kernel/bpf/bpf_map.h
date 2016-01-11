#ifndef INT_BPF_MAP_COMMON_H
#define INT_BPF_MAP_COMMON_H

#include <linux/bpf.h>

extern void *map_lookup_elem_nop(struct bpf_map *map, void *key);
extern int map_delete_elem_nop(struct bpf_map *map, void *key);
extern int map_update_elem_nop(struct bpf_map *map, void *key,
		void *value, u64 flags);
extern void *map_lookup_elem_percpu_nop(struct bpf_map *map, void *key,
		u32 cpu);
extern int map_update_elem_percpu_nop(struct bpf_map *map, void *key,
		void *value, u64 flags, u32 cpu);

#endif
