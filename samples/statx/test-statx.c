/* Test the statx() system call
 *
 * Copyright (C) 2015 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#define _GNU_SOURCE
#define _ATFILE_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <sys/stat.h>

#define AT_FORCE_ATTR_SYNC	0x2000
#define AT_NO_ATTR_SYNC		0x4000

#define __NR_statx 325

static __attribute__((unused))
ssize_t statx(int dfd, const char *filename, unsigned flags,
	      unsigned int mask, struct statx *buffer)
{
	return syscall(__NR_statx, dfd, filename, flags, mask, buffer);
}

static void print_time(const char *field, __s64 tv_sec, __s32 tv_nsec)
{
	struct tm tm;
	time_t tim;
	char buffer[100];
	int len;

	tim = tv_sec;
	if (!localtime_r(&tim, &tm)) {
		perror("localtime_r");
		exit(1);
	}
	len = strftime(buffer, 100, "%F %T", &tm);
	if (len == 0) {
		perror("strftime");
		exit(1);
	}
	printf("%s", field);
	fwrite(buffer, 1, len, stdout);
	printf(".%09u", tv_nsec);
	len = strftime(buffer, 100, "%z", &tm);
	if (len == 0) {
		perror("strftime2");
		exit(1);
	}
	fwrite(buffer, 1, len, stdout);
	printf("\n");
}

static void dump_statx(struct statx *stx)
{
	char buffer[256], ft = '?';

	printf("results=%x\n", stx->st_mask);

	printf(" ");
	if (stx->st_mask & STATX_SIZE)
		printf(" Size: %-15llu", (unsigned long long)stx->st_size);
	if (stx->st_mask & STATX_BLOCKS)
		printf(" Blocks: %-10llu", (unsigned long long)stx->st_blocks);
	printf(" IO Block: %-6llu ", (unsigned long long)stx->st_blksize);
	if (stx->st_mask & STATX_MODE) {
		switch (stx->st_mode & S_IFMT) {
		case S_IFIFO:	printf(" FIFO\n");			ft = 'p'; break;
		case S_IFCHR:	printf(" character special file\n");	ft = 'c'; break;
		case S_IFDIR:	printf(" directory\n");			ft = 'd'; break;
		case S_IFBLK:	printf(" block special file\n");	ft = 'b'; break;
		case S_IFREG:	printf(" regular file\n");		ft = '-'; break;
		case S_IFLNK:	printf(" symbolic link\n");		ft = 'l'; break;
		case S_IFSOCK:	printf(" socket\n");			ft = 's'; break;
		default:
			printf("unknown type (%o)\n", stx->st_mode & S_IFMT);
			break;
		}
	}

	sprintf(buffer, "%02x:%02x", stx->st_dev_major, stx->st_dev_minor);
	printf("Device: %-15s", buffer);
	if (stx->st_mask & STATX_INO)
		printf(" Inode: %-11llu", (unsigned long long) stx->st_ino);
	if (stx->st_mask & STATX_SIZE)
		printf(" Links: %-5u", stx->st_nlink);
	if (stx->st_mask & STATX_RDEV)
		printf(" Device type: %u,%u", stx->st_rdev_major, stx->st_rdev_minor);
	printf("\n");

	if (stx->st_mask & STATX_MODE)
		printf("Access: (%04o/%c%c%c%c%c%c%c%c%c%c)  ",
		       stx->st_mode & 07777,
		       ft,
		       stx->st_mode & S_IRUSR ? 'r' : '-',
		       stx->st_mode & S_IWUSR ? 'w' : '-',
		       stx->st_mode & S_IXUSR ? 'x' : '-',
		       stx->st_mode & S_IRGRP ? 'r' : '-',
		       stx->st_mode & S_IWGRP ? 'w' : '-',
		       stx->st_mode & S_IXGRP ? 'x' : '-',
		       stx->st_mode & S_IROTH ? 'r' : '-',
		       stx->st_mode & S_IWOTH ? 'w' : '-',
		       stx->st_mode & S_IXOTH ? 'x' : '-');
	if (stx->st_mask & STATX_UID)
		printf("Uid: %5d   ", stx->st_uid);
	if (stx->st_mask & STATX_GID)
		printf("Gid: %5d\n", stx->st_gid);

	if (stx->st_mask & STATX_ATIME)
		print_time("Access: ", stx->st_atime_s, stx->st_atime_ns);
	if (stx->st_mask & STATX_MTIME)
		print_time("Modify: ", stx->st_mtime_s, stx->st_mtime_ns);
	if (stx->st_mask & STATX_CTIME)
		print_time("Change: ", stx->st_ctime_s, stx->st_ctime_ns);
	if (stx->st_mask & STATX_BTIME)
		print_time(" Birth: ", stx->st_btime_s, stx->st_btime_ns);

	if (stx->st_mask & STATX_VERSION)
		printf("Data version: %llxh\n",
		       (unsigned long long)stx->st_version);

	if (stx->st_mask & STATX_IOC_FLAGS) {
		unsigned char bits;
		int loop, byte;

		static char flag_representation[32 + 1] =
			/* FS_IOC_GETFLAGS flags: */
			"?????ASH"	/* 31-24	0x00000000-ff000000 */
			"????ehTD"	/* 23-16	0x00000000-00ff0000 */
			"tj?IE?XZ"	/* 15- 8	0x00000000-0000ff00 */
			"AdaiScus"	/*  7- 0	0x00000000-000000ff */
			;

		printf("Inode flags: %08llx (",
		       (unsigned long long)stx->st_ioc_flags);
		for (byte = 32 - 8; byte >= 0; byte -= 8) {
			bits = stx->st_ioc_flags >> byte;
			for (loop = 7; loop >= 0; loop--) {
				int bit = byte + loop;

				if (bits & 0x80)
					putchar(flag_representation[31 - bit]);
				else
					putchar('-');
				bits <<= 1;
			}
			if (byte)
				putchar(' ');
		}
		printf(")\n");
	}

	if (stx->st_information) {
		unsigned char bits;
		int loop, byte;

		static char info_representation[32 + 1] =
			/* STATX_INFO_ flags: */
			"????????"	/* 31-24	0x00000000-ff000000 */
			"????????"	/* 23-16	0x00000000-00ff0000 */
			"??????Rn"	/* 15- 8	0x00000000-0000ff00 */
			"dmorkfte"	/*  7- 0	0x00000000-000000ff */
			;

		printf("Information: %08x (", stx->st_information);
		for (byte = 32 - 8; byte >= 0; byte -= 8) {
			bits = stx->st_information >> byte;
			for (loop = 7; loop >= 0; loop--) {
				int bit = byte + loop;

				if (bits & 0x80)
					putchar(info_representation[31 - bit]);
				else
					putchar('-');
				bits <<= 1;
			}
			if (byte)
				putchar(' ');
		}
		printf(")\n");
	}

	printf("IO-blocksize: blksize=%u\n", stx->st_blksize);
}

static void dump_hex(unsigned long long *data, int from, int to)
{
	unsigned offset, print_offset = 1, col = 0;

	from /= 8;
	to = (to + 7) / 8;

	for (offset = from; offset < to; offset++) {
		if (print_offset) {
			printf("%04x: ", offset * 8);
			print_offset = 0;
		}
		printf("%016llx", data[offset]);
		col++;
		if ((col & 3) == 0) {
			printf("\n");
			print_offset = 1;
		} else {
			printf(" ");
		}
	}

	if (!print_offset)
		printf("\n");
}

int main(int argc, char **argv)
{
	struct statx stx;
	int ret, raw = 0, atflag = AT_SYMLINK_NOFOLLOW;

	unsigned int mask = STATX_ALL_STATS;

	for (argv++; *argv; argv++) {
		if (strcmp(*argv, "-F") == 0) {
			atflag |= AT_FORCE_ATTR_SYNC;
			continue;
		}
		if (strcmp(*argv, "-N") == 0) {
			atflag |= AT_NO_ATTR_SYNC;
			continue;
		}
		if (strcmp(*argv, "-L") == 0) {
			atflag &= ~AT_SYMLINK_NOFOLLOW;
			continue;
		}
		if (strcmp(*argv, "-O") == 0) {
			mask &= ~STATX_BASIC_STATS;
			continue;
		}
		if (strcmp(*argv, "-A") == 0) {
			atflag |= AT_NO_AUTOMOUNT;
			continue;
		}
		if (strcmp(*argv, "-R") == 0) {
			raw = 1;
			continue;
		}

		memset(&stx, 0xbf, sizeof(stx));
		ret = statx(AT_FDCWD, *argv, atflag, mask, &stx);
		printf("statx(%s) = %d\n", *argv, ret);
		if (ret < 0) {
			perror(*argv);
			exit(1);
		}

		if (raw)
			dump_hex((unsigned long long *)&stx, 0, sizeof(stx));

		dump_statx(&stx);
	}
	return 0;
}
