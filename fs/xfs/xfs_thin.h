#ifndef __XFS_THIN_H__
#define __XFS_THIN_H__

/*
 * Convert an fsb count to a sector reservation.
 */
static inline sector_t
xfs_fsb_res(
	struct xfs_mount	*mp,
	xfs_fsblock_t		fsb,
	bool			contig)
{
	sector_t		bb;

       if (contig) {
		bb = XFS_FSB_TO_BB(mp, fsb);
		bb += (2 * mp->m_thin_sectpb);
		bb = round_up(bb, mp->m_thin_sectpb);
	} else
		bb = fsb * mp->m_thin_sectpb;

	return bb;
}

int xfs_thin_init(struct xfs_mount *);
int xfs_thin_reserve(struct xfs_mount *, sector_t);
int xfs_thin_unreserve(struct xfs_mount *, sector_t);
int xfs_thin_provision(struct xfs_mount *, xfs_fsblock_t, xfs_fsblock_t,
		       sector_t *);

#endif	/* __XFS_THIN_H__ */
