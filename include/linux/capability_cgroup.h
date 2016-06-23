#ifdef CONFIG_CGROUP_CAPABILITY
void capability_cgroup_update_used(int cap);
#else
static inline void capability_cgroup_update_used(int cap)
{
}
#endif
