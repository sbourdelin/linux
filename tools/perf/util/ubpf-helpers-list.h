DEF_UBPF_HELPER(int, ubpf_memcmp, (void *s1, void *s2, unsigned int n))
DEF_UBPF_HELPER(void, ubpf_memcpy, (void *d, void *s, unsigned int size))
DEF_UBPF_HELPER(int, ubpf_strcmp, (char *s1, char *s2))
DEF_UBPF_HELPER(int, ubpf_printf, (char *fmt, ...))
DEF_UBPF_HELPER(int, ubpf_map_lookup_elem,
		(void *map_desc, void *key, void *value))
DEF_UBPF_HELPER(int, ubpf_map_update_elem,
		(void *map_desc, void *key, void *value,
		 unsigned long long flags))
DEF_UBPF_HELPER(int, ubpf_map_get_next_key,
		(void *map_desc, void *key, void *next_key))
