/*
 * bpf map common stuff
 *
 *	Copyright (c) 2016 Ming Lei
 *
 * Authors:
 *	Ming Lei <tom.leiming@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/bpf.h>

void *map_lookup_elem_nop(struct bpf_map *map, void *key)
{
	return NULL;
}

int map_delete_elem_nop(struct bpf_map *map, void *key)
{
	return -EINVAL;
}

void *map_lookup_elem_percpu_nop(struct bpf_map *map, void *key, u32 cpu)
{
	return NULL;
}

int map_update_elem_percpu_nop(struct bpf_map *map, void *key, void *value,
		u64 flags, u32 cpu)
{
	return -EINVAL;
}

