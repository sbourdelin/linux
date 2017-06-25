#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fs.h>

enum compression_advice {
    COMPRESS_NONE,
    COMPRESS_COST_EASY,
    COMPRESS_COST_MEDIUM,
    COMPRESS_COST_HARD
};

enum compression_advice btrfs_compress_heuristic(struct inode *inode,
	u64 start, u64 end);
