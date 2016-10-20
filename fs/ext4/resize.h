/*
 * linux/fs/ext4/resize.h
 *
 */

#define EXT4_RESIZING	0

#if IS_ENABLED(CONFIG_EXT4_RESIZE)
extern int ext4_resize_fs(struct super_block *, ext4_fsblk_t);
extern int ext4_resize_begin(struct super_block *);
extern void ext4_resize_end(struct super_block *);
extern int ext4_group_add(struct super_block *, struct ext4_new_group_data *);
extern int ext4_group_extend(struct super_block *, struct ext4_super_block *,
			     ext4_fsblk_t);
#else
static int ext4_resize_begin(struct super_block *sb)
{
	return -EOPNOTSUPP;
}

static void ext4_resize_end(struct super_block *sb)
{
}

static inline int ext4_group_add(struct super_block *sb,
				 struct ext4_new_group_data *input)
{
	return -EOPNOTSUPP;
}

static inline int ext4_group_extend(struct super_block *sb,
		struct ext4_super_block *es, ext4_fsblk_t n_blocks_count)
{
	return -EOPNOTSUPP;
}

static inline int ext4_resize_fs(struct super_block *sb,
				 ext4_fsblk_t n_blocks_count)
{
	return -EOPNOTSUPP;
}
#endif
