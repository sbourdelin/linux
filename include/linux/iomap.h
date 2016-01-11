#ifndef _IOMAP_H
#define _IOMAP_H

/* ->iomap a_op command types */
#define IOMAP_READ	0x01	/* read the current mapping starting at the
				   given position, trimmed to a maximum length.
				   FS's should use this to obtain and lock
				   resources within this range */
#define	IOMAP_RESERVE	0x02	/* reserve space for an allocation that spans
				   the given iomap */
#define IOMAP_ALLOCATE	0x03	/* allocate space in a given iomap - must have
				   first been reserved */
#define	IOMAP_UNRESERVE	0x04	/* return unused reserved space for the given
				   iomap and used space. This will always be
				   called after a IOMAP_READ so as to allow the
				   FS to release held resources. */

/* types of block ranges for multipage write mappings. */
#define IOMAP_HOLE	0x01	/* no blocks allocated, need allocation */
#define IOMAP_DELALLOC	0x02	/* delayed allocation blocks */
#define IOMAP_MAPPED	0x03	/* blocks allocated @blkno */
#define IOMAP_UNWRITTEN	0x04	/* blocks allocated @blkno in unwritten state */

#define IOMAP_NULL_BLOCK -1LL	/* blkno is not valid */

struct iomap {
	sector_t	blkno;	/* first sector of mapping */
	loff_t		offset;	/* file offset of mapping, bytes */
	ssize_t		length;	/* length of mapping, bytes */
	int		type;	/* type of mapping */
	void		*priv;	/* fs private data associated with map */
};

static inline bool iomap_needs_allocation(struct iomap *iomap)
{
	return iomap->type == IOMAP_HOLE;
}

#endif /* _IOMAP_H */
