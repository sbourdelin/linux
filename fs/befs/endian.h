/*
 * linux/fs/befs/endian.h
 *
 * Copyright (C) 2001 Will Dyson <will_dyson@pobox.com>
 *
 * Partially based on similar funtions in the sysv driver.
 */

#ifndef LINUX_BEFS_ENDIAN
#define LINUX_BEFS_ENDIAN

#include <asm/byteorder.h>

static inline u64
fs64_to_cpu(const struct super_block *sb, fs64 n)
{
	if (BEFS_SB(sb)->byte_order == BEFS_BYTESEX_LE)
		return le64_to_cpu((__force __le64)n);
	else
		return be64_to_cpu((__force __be64)n);
}

static inline u32
fs32_to_cpu(const struct super_block *sb, fs32 n)
{
	if (BEFS_SB(sb)->byte_order == BEFS_BYTESEX_LE)
		return le32_to_cpu((__force __le32)n);
	else
		return be32_to_cpu((__force __be32)n);
}

static inline u16
fs16_to_cpu(const struct super_block *sb, fs16 n)
{
	if (BEFS_SB(sb)->byte_order == BEFS_BYTESEX_LE)
		return le16_to_cpu((__force __le16)n);
	else
		return be16_to_cpu((__force __be16)n);
}

/* Composite types below here */

static inline befs_block_run
fsrun_to_cpu(const struct super_block *sb, befs_disk_block_run n)
{
	befs_block_run run;

	if (BEFS_SB(sb)->byte_order == BEFS_BYTESEX_LE) {
		run.allocation_group = le32_to_cpu((__force __le32)n.allocation_group);
		run.start = le16_to_cpu((__force __le16)n.start);
		run.len = le16_to_cpu((__force __le16)n.len);
	} else {
		run.allocation_group = be32_to_cpu((__force __be32)n.allocation_group);
		run.start = be16_to_cpu((__force __be16)n.start);
		run.len = be16_to_cpu((__force __be16)n.len);
	}
	return run;
}

static inline befs_data_stream
fsds_to_cpu(const struct super_block *sb, const befs_disk_data_stream *n)
{
	befs_data_stream data;
	int i;

	for (i = 0; i < BEFS_NUM_DIRECT_BLOCKS; ++i)
		data.direct[i] = fsrun_to_cpu(sb, n->direct[i]);

	data.max_direct_range = fs64_to_cpu(sb, n->max_direct_range);
	data.indirect = fsrun_to_cpu(sb, n->indirect);
	data.max_indirect_range = fs64_to_cpu(sb, n->max_indirect_range);
	data.double_indirect = fsrun_to_cpu(sb, n->double_indirect);
	data.max_double_indirect_range = fs64_to_cpu(sb,
						     n->
						     max_double_indirect_range);
	data.size = fs64_to_cpu(sb, n->size);

	return data;
}

#endif				//LINUX_BEFS_ENDIAN
