// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/file.h>
#include "initramfs.h"

/* externally loaded initrd or initramfs location */
unsigned long initrd_start, initrd_end;
int initrd_below_start_ok;

static int __initdata do_retain_initrd;

static int __init retain_initrd_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initrd = 1;
	return 1;
}
__setup("retain_initrd", retain_initrd_param);

extern char __initramfs_start[];
extern unsigned long __initramfs_size;
#include <linux/initrd.h>
#include <linux/kexec.h>

static void __init free_initrd(void)
{
#ifdef CONFIG_KEXEC_CORE
	unsigned long crashk_start = (unsigned long)__va(crashk_res.start);
	unsigned long crashk_end   = (unsigned long)__va(crashk_res.end);
#endif
	if (do_retain_initrd)
		goto skip;

#ifdef CONFIG_KEXEC_CORE
	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (initrd_start < crashk_end && initrd_end > crashk_start) {
		/*
		 * Initialize initrd memory region since the kexec boot does
		 * not do.
		 */
		memset((void *)initrd_start, 0, initrd_end - initrd_start);
		if (initrd_start < crashk_start)
			free_initrd_mem(initrd_start, crashk_start);
		if (initrd_end > crashk_end)
			free_initrd_mem(crashk_end, initrd_end);
	} else
#endif
		free_initrd_mem(initrd_start, initrd_end);
skip:
	initrd_start = 0;
	initrd_end = 0;
}

#ifdef CONFIG_BLK_DEV_RAM
#define BUF_SIZE 1024
static void __init clean_rootfs(void)
{
	int fd;
	void *buf;
	struct linux_dirent64 *dirp;
	int num;

	fd = ksys_open("/", O_RDONLY, 0);
	WARN_ON(fd < 0);
	if (fd < 0)
		return;
	buf = kzalloc(BUF_SIZE, GFP_KERNEL);
	WARN_ON(!buf);
	if (!buf) {
		ksys_close(fd);
		return;
	}

	dirp = buf;
	num = ksys_getdents64(fd, dirp, BUF_SIZE);
	while (num > 0) {
		while (num > 0) {
			struct kstat st;
			int ret;

			ret = vfs_lstat(dirp->d_name, &st);
			WARN_ON_ONCE(ret);
			if (!ret) {
				if (S_ISDIR(st.mode))
					ksys_rmdir(dirp->d_name);
				else
					ksys_unlink(dirp->d_name);
			}

			num -= dirp->d_reclen;
			dirp = (void *)dirp + dirp->d_reclen;
		}
		dirp = buf;
		memset(buf, 0, BUF_SIZE);
		num = ksys_getdents64(fd, dirp, BUF_SIZE);
	}

	ksys_close(fd);
	kfree(buf);
}
#endif

static int __init populate_rootfs(void)
{
	/* Load the built in initramfs */
	char *err = initramfs_unpack_to_rootfs(__initramfs_start, __initramfs_size);
	if (err)
		panic("%s", err); /* Failed to decompress INTERNAL initramfs */
	/* If available load the bootloader supplied initrd */
	if (initrd_start && !IS_ENABLED(CONFIG_INITRAMFS_FORCE)) {
#ifdef CONFIG_BLK_DEV_RAM
		int fd;
		printk(KERN_INFO "Trying to unpack rootfs image as initramfs...\n");
		err = initramfs_unpack_to_rootfs((char *)initrd_start,
			initrd_end - initrd_start);
		if (!err) {
			free_initrd();
			goto done;
		} else {
			clean_rootfs();
			initramfs_unpack_to_rootfs(__initramfs_start, __initramfs_size);
		}
		printk(KERN_INFO "rootfs image is not initramfs (%s)"
				"; looks like an initrd\n", err);
		fd = ksys_open("/initrd.image",
			      O_WRONLY|O_CREAT, 0700);
		if (fd >= 0) {
			ssize_t written = xwrite(fd, (char *)initrd_start,
						initrd_end - initrd_start);

			if (written != initrd_end - initrd_start)
				pr_err("/initrd.image: incomplete write (%zd != %ld)\n",
				       written, initrd_end - initrd_start);

			ksys_close(fd);
			free_initrd();
		}
	done:
		/* empty statement */;
#else
		printk(KERN_INFO "Unpacking initramfs...\n");
		err = initramfs_unpack_to_rootfs((char *)initrd_start,
			initrd_end - initrd_start);
		if (err)
			printk(KERN_EMERG "Initramfs unpacking failed: %s\n", err);
		free_initrd();
#endif
	}
	flush_delayed_fput();
	/*
	 * Try loading default modules from initramfs.  This gives
	 * us a chance to load before device_initcalls.
	 */
	load_default_modules();

	return 0;
}
rootfs_initcall(populate_rootfs);
