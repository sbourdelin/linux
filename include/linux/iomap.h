#ifndef _IOMAP_H
#define _IOMAP_H

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

#endif /* _IOMAP_H */
