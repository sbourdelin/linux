#ifndef _XFS_LAYOUT_H
#define _XFS_LAYOUT_H 1

#ifdef CONFIG_XFS_LAYOUT
int xfs_break_layouts(struct inode *inode, uint *iolock);
#else
static inline int
xfs_break_layouts(struct inode *inode, uint *iolock)
{
	return 0;
}
#endif /* CONFIG_XFS_LAYOUT */
#endif /* _XFS_LAYOUT_H */
