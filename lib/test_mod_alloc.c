// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/moduleloader.h>
#include <linux/random.h>
#include <linux/uaccess.h>

#include <linux/vmalloc.h>

struct mod { int filesize; int coresize; int initsize; };

/* ==== Begin optional logging ==== */

/*
 * Note: for more accurate test results add this to mm/vmalloc.c:
 * void debug_purge_vmap_area_lazy(void)
 * {
 *	purge_vmap_area_lazy();
 * }
 * and replace the below with:
 * extern void debug_purge_vmap_area_lazy(void);
 */
static void debug_purge_vmap_area_lazy(void)
{
}


/*
 * Note: In order to get an accurate count for the tlb flushes triggered in
 * vmalloc, create a counter in vmalloc.c: with this method signature and export
 * it. Then replace the below with: __purge_vmap_area_lazy
 * extern unsigned long get_tlb_flushes_vmalloc(void);
 */
static unsigned long get_tlb_flushes_vmalloc(void)
{
	return 0;
}

/* ==== End optional logging ==== */


#define MAX_ALLOC_CNT 20000
#define ITERS 1000

struct vm_alloc {
	void *core;
	unsigned long core_size;
	void *init;
};

struct check_alloc {
	unsigned long start;
	unsigned long vm_end;
	unsigned long real_end;
};

static struct vm_alloc *allocs_vm;

static struct check_alloc *check_allocs;
static int check_alloc_cnt;

static long mod_cnt = 100;

/* This may be different for non-x86 */
static unsigned long calc_end(void *start, unsigned long size)
{
	unsigned long startl = (unsigned long) start;

	return startl + PAGE_ALIGN(size) + PAGE_SIZE;
}

static void reset_cur_allocs(void)
{
	check_alloc_cnt = 0;
}

static void add_check_alloc(void *start, unsigned long size)
{
	check_allocs[check_alloc_cnt].start = (unsigned long) start;
	check_allocs[check_alloc_cnt].vm_end = calc_end(start, size);
	check_allocs[check_alloc_cnt].real_end = size;
	check_alloc_cnt++;
}

static int do_check(void *ptr, unsigned long size)
{
	int i;
	unsigned long start = (unsigned long) ptr;
	unsigned long end = calc_end(ptr, size);
	unsigned long sum = 0;
	unsigned long addr;

	if (!start)
		return 1;

	for (i = 0; i < check_alloc_cnt; i++) {
		struct check_alloc *cur_alloc = &(check_allocs[i]);

		/* overlap end */
		if (start >= cur_alloc->start && start < cur_alloc->vm_end) {
			pr_info("overlap end\n");
			return 1;
		}

		/* overlap start */
		if (end >= cur_alloc->start && end < cur_alloc->start) {
			pr_info("overlap start\n");
			return 1;
		}

		/* overlap whole thing */
		if (start <= cur_alloc->start && end > cur_alloc->vm_end) {
			pr_info("overlap whole thing\n");
			return 1;
		}

		/* inside */
		if (start >= cur_alloc->start && end < cur_alloc->vm_end) {
			pr_info("inside\n");
			return 1;
		}

		/* bounds */
		if (start < MODULES_VADDR ||
			end > MODULES_VADDR + MODULES_LEN) {
			pr_info("out of bounds\n");
			return 1;
		}
		for (addr = cur_alloc->start;
			addr < cur_alloc->real_end;
			addr += PAGE_SIZE) {
			sum += *((unsigned long *) addr);
		}
		if (sum != 0)
			pr_info("Memory was not zeroed\n");

		kasan_check_read((void *)cur_alloc->start,
			cur_alloc->vm_end - cur_alloc->start - PAGE_SIZE);
	}
	return 0;
}


const static int core_hist[10] = {1, 5, 21, 46, 141, 245, 597, 2224, 1875, 0};
const static int init_hist[10] = {0, 0, 0, 0, 10, 19, 70, 914, 3906, 236};
const static int file_hist[10] = {6, 20, 55, 86, 286, 551, 918, 2024, 1028,
					181};

const static int bins[10] = {5000000, 2000000, 1000000, 500000, 200000, 100000,
		50000, 20000, 10000, 5000};
/*
 * Rough approximation of the X86_64 module size distribution.
 */
static int get_mod_rand_size(const int *hist)
{
	int area_under = get_random_long() % 5155;
	int i;
	int last_bin = bins[0] + 1;
	int sum = 0;

	for (i = 0; i <= 9; i++) {
		sum += hist[i];
		if (area_under <= sum)
			return bins[i]
				+ (get_random_long() % (last_bin - bins[i]));
		last_bin = bins[i];
	}
	return 4096;
}

static struct mod get_rand_module(void)
{
	struct mod ret;

	ret.coresize = get_mod_rand_size(core_hist);
	ret.initsize = get_mod_rand_size(init_hist);
	ret.filesize = get_mod_rand_size(file_hist);
	return ret;
}

static void do_test_alloc_fail(void)
{
	struct vm_alloc *cur_alloc;
	struct mod cur_mod;
	void *file;
	int mod_n, free_mod_n;
	unsigned long fail = 0;
	int iter;

	for (iter = 0; iter < ITERS; iter++) {
		pr_info("Running iteration: %d\n", iter);
		memset(allocs_vm, 0, mod_cnt * sizeof(struct vm_alloc));
		reset_cur_allocs();
		debug_purge_vmap_area_lazy();
		for (mod_n = 0; mod_n < mod_cnt; mod_n++) {
			cur_mod = get_rand_module();
			cur_alloc = &allocs_vm[mod_n];

			/* Allocate */
			file = vmalloc(cur_mod.filesize);
			cur_alloc->core = module_alloc(cur_mod.coresize);

			/* Check core allocation postion is good */
			if (do_check(cur_alloc->core, cur_mod.coresize)) {
				pr_info("Check failed core:%d\n", mod_n);
				break;
			}
			/* Add core position for future checking */
			add_check_alloc(cur_alloc->core, cur_mod.coresize);

			cur_alloc->init = module_alloc(cur_mod.initsize);

			/* Check init position */
			if (do_check(cur_alloc->init, cur_mod.initsize)) {
				pr_info("Check failed init:%d\n", mod_n);
				break;
			}

			/* Clean up everything except core */
			if (!cur_alloc->core || !cur_alloc->init) {
				fail++;
				vfree(file);
				if (cur_alloc->init)
					vfree(cur_alloc->init);
				break;
			}
			vfree(cur_alloc->init);
			vfree(file);
		}

		/* Clean up core sizes */
		for (free_mod_n = 0; free_mod_n < mod_n; free_mod_n++) {
			cur_alloc = &allocs_vm[free_mod_n];
			if (cur_alloc->core)
				vfree(cur_alloc->core);
		}
	}
	pr_info("Failures(%ld modules):%lu\n", mod_cnt, fail);
}

#if defined(CONFIG_X86_64) && defined(CONFIG_RANDOMIZE_BASE)
static int is_in_backup(void *addr)
{
	return (unsigned long)addr >= MODULES_VADDR + MODULES_RAND_LEN;
}
#else
static int is_in_backup(void *addr)
{
	return 0;
}
#endif

static void do_test_last_perf(void)
{
	struct vm_alloc *cur_alloc;
	struct mod cur_mod;
	void *file;
	int mod_n, mon_n_free;
	unsigned long fail = 0;
	int iter;
	ktime_t start, diff;
	ktime_t total_last = 0;
	ktime_t total_all = 0;

	/*
	 * The number of last core allocations for each iteration that were
	 * allocated in the backup area.
	 */
	int last_in_bk = 0;

	/*
	 * The total number of core allocations that were in the backup area for
	 * all iterations.
	 */
	int total_in_bk = 0;

	/* The number of iterations where the count was more than 1 */
	int cnt_more_than_1 = 0;

	/*
	 * The number of core allocations that were in the backup area for the
	 * current iteration.
	 */
	int cur_in_bk = 0;

	unsigned long before_tlbs;
	unsigned long tlb_cnt_total;
	unsigned long tlb_cur;
	unsigned long total_tlbs = 0;

	pr_info("Starting %d iterations of %ld modules\n", ITERS, mod_cnt);

	for (iter = 0; iter < ITERS; iter++) {
		debug_purge_vmap_area_lazy();
		before_tlbs = get_tlb_flushes_vmalloc();
		memset(allocs_vm, 0, mod_cnt * sizeof(struct vm_alloc));
		tlb_cnt_total = 0;
		cur_in_bk = 0;
		for (mod_n = 0; mod_n < mod_cnt; mod_n++) {
			/* allocate how the module allocator allocates */

			cur_mod = get_rand_module();
			cur_alloc = &allocs_vm[mod_n];
			file = vmalloc(cur_mod.filesize);

			tlb_cur = get_tlb_flushes_vmalloc();

			start = ktime_get();
			cur_alloc->core = module_alloc(cur_mod.coresize);
			diff = ktime_get() - start;

			cur_alloc->init = module_alloc(cur_mod.initsize);

			/* Collect metrics */
			if (is_in_backup(cur_alloc->core)) {
				cur_in_bk++;
				if (mod_n == mod_cnt - 1)
					last_in_bk++;
			}
			total_all += diff;

			if (mod_n == mod_cnt - 1)
				total_last += diff;

			tlb_cnt_total += get_tlb_flushes_vmalloc() - tlb_cur;

			/* If there is a failure, quit. init/core freed later */
			if (!cur_alloc->core || !cur_alloc->init) {
				fail++;
				vfree(file);
				break;
			}
			/* Init sections do not last long so free here */
			vfree(cur_alloc->init);
			cur_alloc->init = NULL;
			vfree(file);
		}

		/* Collect per iteration metrics */
		total_in_bk += cur_in_bk;
		if (cur_in_bk > 1)
			cnt_more_than_1++;
		total_tlbs += get_tlb_flushes_vmalloc() - before_tlbs;

		/* Collect per iteration metrics */
		for (mon_n_free = 0; mon_n_free < mod_cnt; mon_n_free++) {
			cur_alloc = &allocs_vm[mon_n_free];
			vfree(cur_alloc->init);
			vfree(cur_alloc->core);
		}
	}

	if (fail)
		pr_info("There was an alloc failure, results invalid!\n");

	pr_info("num\t\tall(ns)\t\tlast(ns)");
	pr_info("%ld\t\t%llu\t\t%llu\n", mod_cnt,
					total_all / (ITERS * mod_cnt),
					total_last / ITERS);

	if (IS_ENABLED(CONFIG_X86_64) && IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		pr_info("Last module in backup count = %d\n", last_in_bk);
		pr_info("Total modules in backup     = %d\n", total_in_bk);
		pr_info(">1 module in backup count   = %d\n", cnt_more_than_1);
	}
	/*
	 * This will usually hide info when the instrumentation is not in place.
	 */
	if (tlb_cnt_total)
		pr_info("TLB Flushes: %lu\n", tlb_cnt_total);
}

static void do_test(int test)
{
	switch (test) {
	case 1:
		do_test_alloc_fail();
		break;
	case 2:
		do_test_last_perf();
		break;
	default:
		pr_info("Unknown test\n");
	}
}

static ssize_t device_file_write(struct file *filp, const char *user_buf,
				size_t count, loff_t *offp)
{
	char buf[100];
	long iter;
	long new_mod_cnt;

	if (count >= sizeof(buf) - 1) {
		pr_info("Command too long\n");
		return count;
	}

	copy_from_user(buf, user_buf, count);
	buf[count] = 0;
	if (buf[0] == 'm') {
		kstrtol(buf+1, 10, &new_mod_cnt);
		if (new_mod_cnt > 0 && new_mod_cnt <= MAX_ALLOC_CNT) {
			pr_info("New module count: %ld\n", new_mod_cnt);
			mod_cnt = new_mod_cnt;
			if (allocs_vm)
				vfree(allocs_vm);
			allocs_vm = vmalloc(sizeof(struct vm_alloc) * mod_cnt);

			if (check_allocs)
				vfree(check_allocs);
			check_allocs = vmalloc(sizeof(struct check_alloc)
						* mod_cnt);
		} else
			pr_info("more than %d not supported\n", MAX_ALLOC_CNT);
	} else if (buf[0] == 't') {
		kstrtol(buf + 1, 10, &iter);
		do_test(iter);
	} else {
		pr_info("Unknown command\n");
	}

	return count;
}

static const char *dv_name = "mod_alloc_test";
const static struct file_operations test_mod_alloc_fops = {
	.owner	= THIS_MODULE,
	.write	= device_file_write,
};

static int __init mod_alloc_test_init(void)
{
	debugfs_create_file(dv_name, 0400, NULL, NULL, &test_mod_alloc_fops);

	return 0;
}

MODULE_LICENSE("GPL");

module_init(mod_alloc_test_init);
