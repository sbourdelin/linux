#ifndef __XFS_THIN_H__
#define __XFS_THIN_H__

int xfs_thin_init(struct xfs_mount *);
int xfs_thin_reserve(struct xfs_mount *, xfs_fsblock_t);
int xfs_thin_unreserve(struct xfs_mount *, xfs_fsblock_t);
int xfs_thin_provision(struct xfs_mount *, xfs_fsblock_t, xfs_fsblock_t);

#endif	/* __XFS_THIN_H__ */
