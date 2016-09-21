/*
 * NAND flash simulator.
 *
 * Author: Artem B. Bityuckiy <dedekind@oktetlabs.ru>, <dedekind@infradead.org>
 *
 * Copyright (C) 2004 Nokia Corporation
 *
 * Note: NS means "NAND Simulator".
 * Note: Input means input TO flash chip, output means output FROM chip.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/nand_bch.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/nandsim.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/compat.h>
#include <linux/miscdevice.h>
#include <linux/major.h>
#include <linux/mutex.h>
#include <linux/file.h>

/* Default simulator parameters values */
#if !defined(CONFIG_NANDSIM_FIRST_ID_BYTE)  || \
    !defined(CONFIG_NANDSIM_SECOND_ID_BYTE) || \
    !defined(CONFIG_NANDSIM_THIRD_ID_BYTE)  || \
    !defined(CONFIG_NANDSIM_FOURTH_ID_BYTE)
#define CONFIG_NANDSIM_FIRST_ID_BYTE  0x98
#define CONFIG_NANDSIM_SECOND_ID_BYTE 0x39
#define CONFIG_NANDSIM_THIRD_ID_BYTE  0xFF /* No byte */
#define CONFIG_NANDSIM_FOURTH_ID_BYTE 0xFF /* No byte */
#endif

#ifndef CONFIG_NANDSIM_ACCESS_DELAY
#define CONFIG_NANDSIM_ACCESS_DELAY 25
#endif
#ifndef CONFIG_NANDSIM_PROGRAMM_DELAY
#define CONFIG_NANDSIM_PROGRAMM_DELAY 200
#endif
#ifndef CONFIG_NANDSIM_ERASE_DELAY
#define CONFIG_NANDSIM_ERASE_DELAY 2
#endif
#ifndef CONFIG_NANDSIM_OUTPUT_CYCLE
#define CONFIG_NANDSIM_OUTPUT_CYCLE 40
#endif
#ifndef CONFIG_NANDSIM_INPUT_CYCLE
#define CONFIG_NANDSIM_INPUT_CYCLE  50
#endif
#ifndef CONFIG_NANDSIM_BUS_WIDTH
#define CONFIG_NANDSIM_BUS_WIDTH  8
#endif
#ifndef CONFIG_NANDSIM_DO_DELAYS
#define CONFIG_NANDSIM_DO_DELAYS  0
#endif
#ifndef CONFIG_NANDSIM_MAX_PARTS
#define CONFIG_NANDSIM_MAX_PARTS  32
#endif

static uint access_delay   = CONFIG_NANDSIM_ACCESS_DELAY;
static uint programm_delay = CONFIG_NANDSIM_PROGRAMM_DELAY;
static uint erase_delay    = CONFIG_NANDSIM_ERASE_DELAY;
static uint output_cycle   = CONFIG_NANDSIM_OUTPUT_CYCLE;
static uint input_cycle    = CONFIG_NANDSIM_INPUT_CYCLE;
static uint bus_width      = CONFIG_NANDSIM_BUS_WIDTH;
static uint do_delays      = CONFIG_NANDSIM_DO_DELAYS;
static unsigned long parts[CONFIG_NANDSIM_MAX_PARTS];
static unsigned int parts_num;
static char *badblocks = NULL;
static char *weakblocks = NULL;
static char *weakpages = NULL;
static unsigned int bitflips = 0;
static char *gravepages = NULL;
static unsigned int overridesize = 0;
static char *cache_file = NULL;
static unsigned int bbt;
static unsigned int bch;
static u_char id_bytes[8] = {
	[0] = CONFIG_NANDSIM_FIRST_ID_BYTE,
	[1] = CONFIG_NANDSIM_SECOND_ID_BYTE,
	[2] = CONFIG_NANDSIM_THIRD_ID_BYTE,
	[3] = CONFIG_NANDSIM_FOURTH_ID_BYTE,
	[4 ... 7] = 0xFF,
};
static bool defaults = true;

module_param_array(id_bytes, byte, NULL, 0400);
module_param_named(first_id_byte, id_bytes[0], byte, 0400);
module_param_named(second_id_byte, id_bytes[1], byte, 0400);
module_param_named(third_id_byte, id_bytes[2], byte, 0400);
module_param_named(fourth_id_byte, id_bytes[3], byte, 0400);
module_param(access_delay,   uint, 0400);
module_param(programm_delay, uint, 0400);
module_param(erase_delay,    uint, 0400);
module_param(output_cycle,   uint, 0400);
module_param(input_cycle,    uint, 0400);
module_param(bus_width,      uint, 0400);
module_param(do_delays,      uint, 0400);
module_param_array(parts, ulong, &parts_num, 0400);
module_param(badblocks,      charp, 0400);
module_param(weakblocks,     charp, 0400);
module_param(weakpages,      charp, 0400);
module_param(bitflips,       uint, 0400);
module_param(gravepages,     charp, 0400);
module_param(overridesize,   uint, 0400);
module_param(cache_file,     charp, 0400);
module_param(bbt,	     uint, 0400);
module_param(bch,	     uint, 0400);
module_param(defaults,	     bool, 0400);

MODULE_PARM_DESC(id_bytes,       "The ID bytes returned by NAND Flash 'read ID' command");
MODULE_PARM_DESC(first_id_byte,  "The first byte returned by NAND Flash 'read ID' command (manufacturer ID) (obsolete)");
MODULE_PARM_DESC(second_id_byte, "The second byte returned by NAND Flash 'read ID' command (chip ID) (obsolete)");
MODULE_PARM_DESC(third_id_byte,  "The third byte returned by NAND Flash 'read ID' command (obsolete)");
MODULE_PARM_DESC(fourth_id_byte, "The fourth byte returned by NAND Flash 'read ID' command (obsolete)");
MODULE_PARM_DESC(access_delay,   "Initial page access delay (microseconds)");
MODULE_PARM_DESC(programm_delay, "Page program delay (microseconds");
MODULE_PARM_DESC(erase_delay,    "Sector erase delay (milliseconds)");
MODULE_PARM_DESC(output_cycle,   "Word output (from flash) time (nanoseconds)");
MODULE_PARM_DESC(input_cycle,    "Word input (to flash) time (nanoseconds)");
MODULE_PARM_DESC(bus_width,      "Chip's bus width (8- or 16-bit)");
MODULE_PARM_DESC(do_delays,      "Simulate NAND delays using busy-waits if not zero");
MODULE_PARM_DESC(parts,          "Partition sizes (in erase blocks) separated by commas");
/* Page and erase block positions for the following parameters are independent of any partitions */
MODULE_PARM_DESC(badblocks,      "Erase blocks that are initially marked bad, separated by commas");
MODULE_PARM_DESC(weakblocks,     "Weak erase blocks [: remaining erase cycles (defaults to 3)]"
				 " separated by commas e.g. 113:2 means eb 113"
				 " can be erased only twice before failing");
MODULE_PARM_DESC(weakpages,      "Weak pages [: maximum writes (defaults to 3)]"
				 " separated by commas e.g. 1401:2 means page 1401"
				 " can be written only twice before failing");
MODULE_PARM_DESC(bitflips,       "Maximum number of random bit flips per page (zero by default)");
MODULE_PARM_DESC(gravepages,     "Pages that lose data [: maximum reads (defaults to 3)]"
				 " separated by commas e.g. 1401:2 means page 1401"
				 " can be read only twice before failing");
MODULE_PARM_DESC(overridesize,   "Specifies the NAND Flash size overriding the ID bytes. "
				 "The size is specified in erase blocks and as the exponent of a power of two"
				 " e.g. 5 means a size of 32 erase blocks");
MODULE_PARM_DESC(cache_file,     "File to use to cache nand pages instead of memory");
MODULE_PARM_DESC(bbt,		 "0 OOB, 1 BBT with marker in OOB, 2 BBT with marker in data area");
MODULE_PARM_DESC(bch,		 "Enable BCH ecc and set how many bits should "
				 "be correctable in 512-byte blocks");
MODULE_PARM_DESC(defaults,	 "Register a MTD during module load using default values and module parametes. "
				 "Set to N if you want to use the nandsimctl user space tool to setup nandsim.");

/* Busy-wait delay macros (microseconds, milliseconds) */
#define NS_UDELAY(ns, us) \
	do { if (ns->do_delays) udelay(us); } while (0)
#define NS_MDELAY(ns, us) \
	do { if (ns->do_delays) mdelay(us); } while (0)

/* Good operation completion status */
#define NS_STATUS_OK(ns) (NAND_STATUS_READY | (NAND_STATUS_WP * ((ns)->lines.wp == 0)))

/* Operation failed completion status */
#define NS_STATUS_FAILED(ns) (NAND_STATUS_FAIL | NS_STATUS_OK(ns))

/* Calculate the page offset in flash RAM image by (row, column) address */
#define NS_RAW_OFFSET(ns) \
	(((ns)->regs.row * ((ns)->no_oob ? (ns)->geom.pgsz : (ns)->geom.pgszoob)) + (ns)->regs.column)

/* After a command is input, the simulator goes to one of the following states */
#define STATE_CMD_READ0        0x00000001 /* read data from the beginning of page */
#define STATE_CMD_READ1        0x00000002 /* read data from the second half of page */
#define STATE_CMD_READSTART    0x00000003 /* read data second command (large page devices) */
#define STATE_CMD_PAGEPROG     0x00000004 /* start page program */
#define STATE_CMD_READOOB      0x00000005 /* read OOB area */
#define STATE_CMD_ERASE1       0x00000006 /* sector erase first command */
#define STATE_CMD_STATUS       0x00000007 /* read status */
#define STATE_CMD_SEQIN        0x00000009 /* sequential data input */
#define STATE_CMD_READID       0x0000000A /* read ID */
#define STATE_CMD_ERASE2       0x0000000B /* sector erase second command */
#define STATE_CMD_RESET        0x0000000C /* reset */
#define STATE_CMD_RNDOUT       0x0000000D /* random output command */
#define STATE_CMD_RNDOUTSTART  0x0000000E /* random output start command */
#define STATE_CMD_MASK         0x0000000F /* command states mask */

/* After an address is input, the simulator goes to one of these states */
#define STATE_ADDR_PAGE        0x00000010 /* full (row, column) address is accepted */
#define STATE_ADDR_SEC         0x00000020 /* sector address was accepted */
#define STATE_ADDR_COLUMN      0x00000030 /* column address was accepted */
#define STATE_ADDR_ZERO        0x00000040 /* one byte zero address was accepted */
#define STATE_ADDR_MASK        0x00000070 /* address states mask */

/* During data input/output the simulator is in these states */
#define STATE_DATAIN           0x00000100 /* waiting for data input */
#define STATE_DATAIN_MASK      0x00000100 /* data input states mask */

#define STATE_DATAOUT          0x00001000 /* waiting for page data output */
#define STATE_DATAOUT_ID       0x00002000 /* waiting for ID bytes output */
#define STATE_DATAOUT_STATUS   0x00003000 /* waiting for status output */
#define STATE_DATAOUT_MASK     0x00007000 /* data output states mask */

/* Previous operation is done, ready to accept new requests */
#define STATE_READY            0x00000000

/* This state is used to mark that the next state isn't known yet */
#define STATE_UNKNOWN          0x10000000

/* Simulator's actions bit masks */
#define ACTION_CPY       0x00100000 /* copy page/OOB to the internal buffer */
#define ACTION_PRGPAGE   0x00200000 /* program the internal buffer to flash */
#define ACTION_SECERASE  0x00300000 /* erase sector */
#define ACTION_ZEROOFF   0x00400000 /* don't add any offset to address */
#define ACTION_HALFOFF   0x00500000 /* add to address half of page */
#define ACTION_OOBOFF    0x00600000 /* add to address OOB offset */
#define ACTION_MASK      0x00700000 /* action mask */

#define NS_OPER_NUM      13 /* Number of operations supported by the simulator */
#define NS_OPER_STATES   6  /* Maximum number of states in operation */

#define OPT_ANY          0xFFFFFFFF /* any chip supports this operation */
#define OPT_PAGE512      0x00000002 /* 512-byte  page chips */
#define OPT_LARGEPAGE    0x00000008 /* >= 2048-byte page chips */
#define OPT_PAGE512_8BIT 0x00000040 /* 512-byte page chips with 8-bit bus width */
#define OPT_SMALLPAGE    (OPT_PAGE512) /* 512-byte page chips */

/* Remove action bits from state */
#define NS_STATE(x) ((x) & ~ACTION_MASK)

/*
 * Maximum previous states which need to be saved. Currently saving is
 * only needed for page program operation with preceded read command
 * (which is only valid for 512-byte pages).
 */
#define NS_MAX_PREVSTATES 1

/* Maximum page cache pages needed to read or write a NAND page to the cache_file */
#define NS_MAX_HELD_PAGES 16

struct nandsim_debug_info {
	struct dentry *dfs_root;
	struct dentry *dfs_wear_report;
};

/*
 * A union to represent flash memory contents and flash buffer.
 */
union ns_mem {
	u_char *byte;    /* for byte access */
	uint16_t *word;  /* for 16-bit word access */
};

struct ns_ram_data {
	/* The simulated NAND flash pages array */
	union ns_mem *pages;

	/* Slab allocator for nand pages */
	struct kmem_cache *nand_pages_slab;
};

struct ns_cachefile_data {
	struct file *cfile; /* Open file */
	bool file_opened; /* False when we operate on an already opened file */
	unsigned long *pages_written; /* Which pages have been written */
	void *file_buf;
	struct page *held_pages[NS_MAX_HELD_PAGES];
	int held_cnt;
};

struct ns_file_data {
	struct file *file;
	void *file_buf;
	bool ro;
};

/*
 * The structure which describes all the internal simulator data.
 */
struct nandsim {
	unsigned int index;
	unsigned int refcnt;
	spinlock_t refcnt_lock;
	struct mtd_partition partitions[CONFIG_NANDSIM_MAX_PARTS];
	bool destroying;
	unsigned int nbparts;

	uint busw;              /* flash chip bus width (8 or 16) */
	u_char ids[8];          /* chip's ID bytes */
	uint32_t options;       /* chip's characteristic bits */
	uint32_t state;         /* current chip state */
	uint32_t nxstate;       /* next expected state */

	uint32_t *op;           /* current operation, NULL operations isn't known yet  */
	uint32_t pstates[NS_MAX_PREVSTATES]; /* previous states */
	uint16_t npstates;      /* number of previous states saved */
	uint16_t stateidx;      /* current state index */

	/* Internal buffer of page + OOB size bytes */
	union ns_mem buf;
	struct nandsim_geom geom;

	/* NAND flash internal registers */
	struct {
		unsigned int command; /* the command register */
		u_char   status;  /* the status register */
		uint     row;     /* the page number */
		uint     column;  /* the offset within page */
		uint     count;   /* internal counter */
		uint     num;     /* number of bytes which must be processed */
		uint     off;     /* fixed page offset */
	} regs;

	/* NAND flash lines state */
        struct {
                int ce;  /* chip Enable */
                int cle; /* command Latch Enable */
                int ale; /* address Latch Enable */
                int wp;  /* write Protect */
        } lines;

	bool no_oob;
	struct ns_backend_ops *bops;
	void *backend_data;

	struct list_head weak_blocks;
	struct list_head weak_pages;
	struct list_head grave_pages;

	unsigned long *erase_block_wear;
	unsigned int wear_eb_count;
	unsigned long total_wear;

	bool do_delays;
	unsigned int access_delay;
	unsigned int program_delay;
	unsigned int erase_delay;
	unsigned int output_cycle;
	unsigned int input_cycle;
	unsigned int bitflips;

	struct nandsim_debug_info dbg;
};

static struct ns_backend_ops ns_ram_bops;
static struct ns_backend_ops ns_cachefile_bops;

/*
 * Operations array. To perform any operation the simulator must pass
 * through the correspondent states chain.
 */
static struct nandsim_operations {
	uint32_t reqopts;  /* options which are required to perform the operation */
	uint32_t states[NS_OPER_STATES]; /* operation's states */
} ops[NS_OPER_NUM] = {
	/* Read page + OOB from the beginning */
	{OPT_SMALLPAGE, {STATE_CMD_READ0 | ACTION_ZEROOFF, STATE_ADDR_PAGE | ACTION_CPY,
			STATE_DATAOUT, STATE_READY}},
	/* Read page + OOB from the second half */
	{OPT_PAGE512_8BIT, {STATE_CMD_READ1 | ACTION_HALFOFF, STATE_ADDR_PAGE | ACTION_CPY,
			STATE_DATAOUT, STATE_READY}},
	/* Read OOB */
	{OPT_SMALLPAGE, {STATE_CMD_READOOB | ACTION_OOBOFF, STATE_ADDR_PAGE | ACTION_CPY,
			STATE_DATAOUT, STATE_READY}},
	/* Program page starting from the beginning */
	{OPT_ANY, {STATE_CMD_SEQIN, STATE_ADDR_PAGE, STATE_DATAIN,
			STATE_CMD_PAGEPROG | ACTION_PRGPAGE, STATE_READY}},
	/* Program page starting from the beginning */
	{OPT_SMALLPAGE, {STATE_CMD_READ0, STATE_CMD_SEQIN | ACTION_ZEROOFF, STATE_ADDR_PAGE,
			      STATE_DATAIN, STATE_CMD_PAGEPROG | ACTION_PRGPAGE, STATE_READY}},
	/* Program page starting from the second half */
	{OPT_PAGE512, {STATE_CMD_READ1, STATE_CMD_SEQIN | ACTION_HALFOFF, STATE_ADDR_PAGE,
			      STATE_DATAIN, STATE_CMD_PAGEPROG | ACTION_PRGPAGE, STATE_READY}},
	/* Program OOB */
	{OPT_SMALLPAGE, {STATE_CMD_READOOB, STATE_CMD_SEQIN | ACTION_OOBOFF, STATE_ADDR_PAGE,
			      STATE_DATAIN, STATE_CMD_PAGEPROG | ACTION_PRGPAGE, STATE_READY}},
	/* Erase sector */
	{OPT_ANY, {STATE_CMD_ERASE1, STATE_ADDR_SEC, STATE_CMD_ERASE2 | ACTION_SECERASE, STATE_READY}},
	/* Read status */
	{OPT_ANY, {STATE_CMD_STATUS, STATE_DATAOUT_STATUS, STATE_READY}},
	/* Read ID */
	{OPT_ANY, {STATE_CMD_READID, STATE_ADDR_ZERO, STATE_DATAOUT_ID, STATE_READY}},
	/* Large page devices read page */
	{OPT_LARGEPAGE, {STATE_CMD_READ0, STATE_ADDR_PAGE, STATE_CMD_READSTART | ACTION_CPY,
			       STATE_DATAOUT, STATE_READY}},
	/* Large page devices random page read */
	{OPT_LARGEPAGE, {STATE_CMD_RNDOUT, STATE_ADDR_COLUMN, STATE_CMD_RNDOUTSTART | ACTION_CPY,
			       STATE_DATAOUT, STATE_READY}},
};

struct weak_block {
	struct list_head list;
	unsigned int erase_block_no;
	unsigned int max_erases;
	unsigned int erases_done;
};

struct weak_page {
	struct list_head list;
	unsigned int page_no;
	unsigned int max_writes;
	unsigned int writes_done;
};

struct grave_page {
	struct list_head list;
	unsigned int page_no;
	unsigned int max_reads;
	unsigned int reads_done;
};

#define NS_MAX_DEVICES 32

/* MTD structure for NAND controller */
static struct mtd_info *ns_mtds[NS_MAX_DEVICES];
static DEFINE_MUTEX(ns_mtd_mutex);

static struct dentry *dfs_root;

static inline struct nandsim *chip_to_ns(struct nand_chip *chip)
{
	return (struct nandsim *)(chip + 1);
}

static inline struct mtd_info *ns_to_mtd(struct nandsim *ns)
{
	struct nand_chip *chip = ((struct nand_chip *)ns - 1);

	return nand_to_mtd(chip);
}

static int nandsim_debugfs_show(struct seq_file *m, void *private)
{
	struct nandsim *ns = (struct nandsim *)m->private;
	unsigned long wmin = -1, wmax = 0, avg;
	unsigned long deciles[10], decile_max[10], tot = 0;
	unsigned int i;

	/* Calc wear stats */
	for (i = 0; i < ns->wear_eb_count; ++i) {
		unsigned long wear = ns->erase_block_wear[i];
		if (wear < wmin)
			wmin = wear;
		if (wear > wmax)
			wmax = wear;
		tot += wear;
	}

	for (i = 0; i < 9; ++i) {
		deciles[i] = 0;
		decile_max[i] = (wmax * (i + 1) + 5) / 10;
	}
	deciles[9] = 0;
	decile_max[9] = wmax;
	for (i = 0; i < ns->wear_eb_count; ++i) {
		int d;
		unsigned long wear = ns->erase_block_wear[i];
		for (d = 0; d < 10; ++d)
			if (wear <= decile_max[d]) {
				deciles[d] += 1;
				break;
			}
	}
	avg = tot / ns->wear_eb_count;

	/* Output wear report */
	seq_printf(m, "Total numbers of erases:  %lu\n", tot);
	seq_printf(m, "Number of erase blocks:   %u\n", ns->wear_eb_count);
	seq_printf(m, "Average number of erases: %lu\n", avg);
	seq_printf(m, "Maximum number of erases: %lu\n", wmax);
	seq_printf(m, "Minimum number of erases: %lu\n", wmin);
	for (i = 0; i < 10; ++i) {
		unsigned long from = (i ? decile_max[i - 1] + 1 : 0);
		if (from > decile_max[i])
			continue;
		seq_printf(m, "Number of ebs with erase counts from %lu to %lu : %lu\n",
			from,
			decile_max[i],
			deciles[i]);
	}

	return 0;
}

static int nandsim_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, nandsim_debugfs_show, inode->i_private);
}

static const struct file_operations dfs_fops = {
	.open		= nandsim_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int nandsim_debugfs_init(void)
{
	if (!IS_ENABLED(CONFIG_DEBUG_FS))
		return 0;

	dfs_root = debugfs_create_dir("nandsim", NULL);
	if (IS_ERR_OR_NULL(dfs_root)) {
		int err = dfs_root ? -ENODEV : PTR_ERR(dfs_root);

		pr_err("cannot create \"nandsim\" debugfs directory, err %d\n",
			err);
		return err;
	}

	return 0;
}

/**
 * nandsim_debugfs_create - initialize debugfs
 * @dev: nandsim device description object
 *
 * This function creates all debugfs files for UBI device @ubi. Returns zero in
 * case of success and a negative error code in case of failure.
 */
static int nandsim_debugfs_create(struct nandsim *dev)
{
	struct nandsim_debug_info *dbg = &dev->dbg;
	struct dentry *dent;
	int err;
	char *dirname;

	if (!IS_ENABLED(CONFIG_DEBUG_FS))
		return 0;

	dirname = kasprintf(GFP_KERNEL, "nandsim%i", dev->index);
	if (!dirname)
		return -ENOMEM;

	dent = debugfs_create_dir(dirname, dfs_root);
	kfree(dirname);
	if (IS_ERR_OR_NULL(dent)) {
		int err = dent ? -ENODEV : PTR_ERR(dent);

		pr_err("cannot create nandsim debugfs sub-directory, err %d\n",
			err);
		return err;
	}
	dbg->dfs_root = dent;

	dent = debugfs_create_file("wear_report", S_IRUSR,
				   dbg->dfs_root, dev, &dfs_fops);
	if (IS_ERR_OR_NULL(dent))
		goto out_remove;
	dbg->dfs_wear_report = dent;

	return 0;

out_remove:
	debugfs_remove_recursive(dbg->dfs_root);
	err = dent ? PTR_ERR(dent) : -ENODEV;
	return err;
}

/**
 * nandsim_debugfs_remove - destroy all debugfs files
 */
static void nandsim_debugfs_remove(struct nandsim *ns)
{
	if (IS_ENABLED(CONFIG_DEBUG_FS))
		debugfs_remove_recursive(ns->dbg.dfs_root);
}

static int ns_ram_init(struct nandsim *ns, struct nandsim_params *nsparam)
{
	int i;
	struct ns_ram_data *data = kzalloc(sizeof(*data), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	data->pages = vmalloc(ns->geom.pgnum * sizeof(union ns_mem));
	if (!data->pages) {
		kfree(data);
		pr_err("unable to allocate page array\n");
		return -ENOMEM;
	}
	for (i = 0; i < ns->geom.pgnum; i++) {
		data->pages[i].byte = NULL;
	}

	data->nand_pages_slab = kmem_cache_create("nandsim",
						ns->geom.pgszoob, 0, 0, NULL);
	if (!data->nand_pages_slab) {
		vfree(data->pages);
		kfree(data);
		pr_err("unable to create kmem_cache\n");
		return -ENOMEM;
	}

	ns->backend_data = data;

	return 0;
}

static struct file *get_file_from_nsparam(struct nandsim_params *nsparam, bool *opened)
{
	int err;
	struct file *file;
	struct inode *inode;

	if (nsparam->cache_file) {
		file = filp_open(nsparam->cache_file, O_CREAT | O_RDWR | O_LARGEFILE, 0600);
		if (IS_ERR(file))
			return file;

		*opened = true;
	} else {
		file = fget(nsparam->file_fd);
		if (!file)
			return ERR_PTR(-EBADF);

		*opened = false;
	}

	inode = file->f_mapping->host;
	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode)) {
		pr_err("backing file is not a regular file nor a block device\n");
		err = -EINVAL;
		goto out;
	}

	if (!(file->f_mode & FMODE_CAN_READ)) {
		pr_err("cache file not readable\n");
		err = -EINVAL;
		goto out;
	}

	if (!(file->f_mode & FMODE_CAN_WRITE)) {
		pr_err("cache file not writeable\n");
		err = -EINVAL;
		goto out;
	}

	return file;

out:
	if (*opened)
		filp_close(file, NULL);
	else
		fput(file);

	return ERR_PTR(err);
}

static int ns_cachefile_init(struct nandsim *ns, struct nandsim_params *nsparam)
{
	int err;
	struct ns_cachefile_data *data = kzalloc(sizeof(*data), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	data->cfile = get_file_from_nsparam(nsparam, &data->file_opened);
	if (IS_ERR(data->cfile)) {
		err = PTR_ERR(data->cfile);
		goto out;
	}

	data->pages_written = vzalloc(BITS_TO_LONGS(ns->geom.pgnum) *
				    sizeof(unsigned long));
	if (!data->pages_written) {
		pr_err("unable to allocate pages written array\n");
		err = -ENOMEM;
		goto err_close;
	}
	data->file_buf = kmalloc(ns->geom.pgszoob, GFP_KERNEL);
	if (!data->file_buf) {
		pr_err("unable to allocate file buf\n");
		err = -ENOMEM;
		goto err_free;
	}

	ns->backend_data = data;

	return 0;

err_free:
	vfree(data->pages_written);
err_close:
	filp_close(data->cfile, NULL);
out:
	kfree(data);
	return err;
}

static int ns_file_init(struct nandsim *ns, struct nandsim_params *nsparam)
{
	int ret;
	bool dummy;
	struct ns_file_data *data = kzalloc(sizeof(*data), GFP_KERNEL);

	if (!data) {
		ret = -ENOMEM;
		goto out;
	}

	data->file = get_file_from_nsparam(nsparam, &dummy);
	if (IS_ERR(data->file)) {
		ret = PTR_ERR(data->file);
		goto out_free;
	}

	data->file_buf = kmalloc(ns->geom.pgszoob, GFP_KERNEL);
	if (!data->file_buf) {
		pr_err("unable to allocate file buf\n");
		ret = -ENOMEM;
		goto out_put;
	}

	ns->backend_data = data;
	ret = 0;

	return ret;

out_put:
	fput(data->file);
out_free:
	kfree(data);
out:
	return ret;
}

struct nandsim_geom *nandsim_get_geom(struct nandsim *ns)
{
	return &ns->geom;
}
EXPORT_SYMBOL_GPL(nandsim_get_geom);

void nandsim_set_backend_data(struct nandsim *ns, void *data)
{
	ns->backend_data = data;
}
EXPORT_SYMBOL_GPL(nandsim_set_backend_data);

void *nandsim_get_backend_data(struct nandsim *ns)
{
	return ns->backend_data;
}
EXPORT_SYMBOL_GPL(nandsim_get_backend_data);

static void ns_ram_destroy(struct nandsim *ns)
{
	struct ns_ram_data *data = ns->backend_data;
	int i;

	if (!data)
		return;

	for (i = 0; i < ns->geom.pgnum; i++) {
		if (data->pages[i].byte)
			kmem_cache_free(data->nand_pages_slab,
					data->pages[i].byte);
	}
	kmem_cache_destroy(data->nand_pages_slab);
	vfree(data->pages);
	kfree(data);
}

static void ns_cachefile_destroy(struct nandsim *ns)
{
	struct ns_cachefile_data *data = ns->backend_data;

	if (!data)
		return;

	kfree(data->file_buf);
	vfree(data->pages_written);
	if (data->file_opened)
		filp_close(data->cfile, NULL);
	else
		fput(data->cfile);
	kfree(data);
}

static void ns_file_destroy(struct nandsim *ns)
{
	struct ns_file_data *data = ns->backend_data;

	if (!data)
		return;

	kfree(data->file_buf);
	fput(data->file);
	kfree(data);
}

static char *get_partition_name(struct nandsim *ns, int i)
{
	return kasprintf(GFP_KERNEL, "nandsim%d_%d", ns->index, i);
}

/*
 * Initialize the nandsim structure.
 *
 * RETURNS: 0 if success, -ERRNO if failure.
 */
static int init_nandsim(struct mtd_info *mtd, struct nandsim_params *nsparam)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim   *ns   = nand_get_controller_data(chip);
	int i, ret = 0;
	uint64_t remains;
	uint64_t next_offset;

	/* Force mtd to not do delays */
	chip->chip_delay = 0;

	/* Initialize the NAND flash parameters */
	ns->busw = chip->options & NAND_BUSWIDTH_16 ? 16 : 8;
	ns->geom.totsz    = mtd->size;
	ns->geom.pgsz     = mtd->writesize;
	ns->geom.oobsz    = mtd->oobsize;
	ns->geom.secsz    = mtd->erasesize;
	ns->geom.pgszoob  = ns->geom.pgsz + ns->geom.oobsz;
	ns->geom.pgnum    = div_u64(ns->geom.totsz, ns->geom.pgsz);
	ns->geom.totszoob = ns->geom.totsz + (uint64_t)ns->geom.pgnum * ns->geom.oobsz;
	ns->geom.secshift = ffs(ns->geom.secsz) - 1;
	ns->geom.pgshift  = chip->page_shift;
	ns->geom.pgsec    = ns->geom.secsz / ns->geom.pgsz;
	ns->geom.secszoob = ns->geom.secsz + ns->geom.oobsz * ns->geom.pgsec;
	ns->options = 0;

	if (ns->geom.pgsz == 512) {
		ns->options |= OPT_PAGE512;
		if (ns->busw == 8)
			ns->options |= OPT_PAGE512_8BIT;
	} else if (ns->geom.pgsz >= 2048) {
		ns->options |= OPT_LARGEPAGE;
	}

	if (!is_power_of_2(ns->geom.pgsz)) {
		pr_err("page size is not a power of two.\n");
		return -EINVAL;
	}

	if (ns->geom.pgszoob > KMALLOC_MAX_SIZE) {
		pr_err("page size plus oob too large: %u.\n", ns->geom.pgszoob);
		return -EINVAL;
	}

	if (ns->options & OPT_SMALLPAGE) {
		if (ns->geom.totsz <= (32 << 20)) {
			ns->geom.pgaddrbytes  = 3;
			ns->geom.secaddrbytes = 2;
		} else {
			ns->geom.pgaddrbytes  = 4;
			ns->geom.secaddrbytes = 3;
		}
	} else {
		if (ns->geom.totsz <= (128 << 20)) {
			ns->geom.pgaddrbytes  = 4;
			ns->geom.secaddrbytes = 2;
		} else {
			ns->geom.pgaddrbytes  = 5;
			ns->geom.secaddrbytes = 3;
		}
	}

	/* Fill the partition_info structure */
	if (nsparam->parts_num > ARRAY_SIZE(ns->partitions)) {
		pr_err("too many partitions.\n");
		return -EINVAL;
	}
	remains = ns->geom.totsz;
	next_offset = 0;
	for (i = 0; i < nsparam->parts_num; ++i) {
		uint64_t part_sz = (uint64_t)nsparam->parts[i] * ns->geom.secsz;

		if (!part_sz || part_sz > remains) {
			pr_err("bad partition size.\n");
			return -EINVAL;
		}
		ns->partitions[i].name = get_partition_name(ns, i);
		if (!ns->partitions[i].name) {
			pr_err("unable to allocate memory.\n");
			return -ENOMEM;
		}
		ns->partitions[i].offset = next_offset;
		ns->partitions[i].size   = part_sz;
		next_offset += ns->partitions[i].size;
		remains -= ns->partitions[i].size;
	}
	ns->nbparts = nsparam->parts_num;
	if (remains) {
		if (nsparam->parts_num + 1 > ARRAY_SIZE(ns->partitions)) {
			pr_err("too many partitions.\n");
			return -EINVAL;
		}
		ns->partitions[i].name = get_partition_name(ns, i);
		if (!ns->partitions[i].name) {
			pr_err("unable to allocate memory.\n");
			return -ENOMEM;
		}
		ns->partitions[i].offset = next_offset;
		ns->partitions[i].size   = remains;
		ns->nbparts += 1;
	}

	if (ns->busw == 16)
		pr_warn("16-bit flashes support wasn't tested\n");

	pr_info("flash size: %llu MiB\n",
			(unsigned long long)ns->geom.totsz >> 20);
	pr_info("page size: %u bytes\n",         ns->geom.pgsz);
	pr_info("OOB area size: %u bytes\n",     ns->geom.oobsz);
	pr_info("sector size: %u KiB\n",         ns->geom.secsz >> 10);
	pr_info("pages number: %u\n",            ns->geom.pgnum);
	pr_info("pages per sector: %u\n",        ns->geom.pgsec);
	pr_info("bus width: %u\n",               ns->busw);
	pr_info("bits in sector size: %u\n",     ns->geom.secshift);
	pr_info("bits in page size: %u\n",       ns->geom.pgshift);
	pr_info("bits in OOB size: %u\n",	ffs(ns->geom.oobsz) - 1);
	pr_info("flash size with OOB: %llu KiB\n",
			(unsigned long long)ns->geom.totszoob >> 10);
	pr_info("page address bytes: %u\n",      ns->geom.pgaddrbytes);
	pr_info("sector address bytes: %u\n",    ns->geom.secaddrbytes);
	pr_info("options: %#x\n",                ns->options);

	ns->bops = nsparam->bops;

	pr_info("Using backend: %s\n", ns->bops->name);
	if ((ret = ns->bops->init(ns, nsparam)) != 0) {
		pr_err("Unable to initialize simulator backend: %i\n", ret);
		return ret;
	}

	/* Allocate / initialize the internal buffer */
	ns->buf.byte = kmalloc(ns->geom.pgszoob, GFP_KERNEL);
	if (!ns->buf.byte) {
		pr_err("unable to allocate %u bytes for the internal buffer\n",
			ns->geom.pgszoob);
		return -ENOMEM;
	}
	memset(ns->buf.byte, 0xFF, ns->geom.pgszoob);

	return 0;
}

/*
 * Free the nandsim structure.
 */
static void free_nandsim(struct nandsim *ns)
{
	kfree(ns->buf.byte);
	ns->bops->destroy(ns);
}

static int parse_badblocks(struct nandsim *ns, struct mtd_info *mtd,
			   unsigned char *badblocks)
{
	char *w;
	int zero_ok;
	unsigned int erase_block_no;
	loff_t offset;

	if (!badblocks)
		return 0;
	w = badblocks;
	do {
		zero_ok = (*w == '0' ? 1 : 0);
		erase_block_no = simple_strtoul(w, &w, 0);
		if (!zero_ok && !erase_block_no) {
			pr_err("invalid badblocks.\n");
			return -EINVAL;
		}
		offset = (loff_t)erase_block_no * ns->geom.secsz;
		if (mtd_block_markbad(mtd, offset)) {
			pr_err("invalid badblocks.\n");
			return -EINVAL;
		}
		if (*w == ',')
			w += 1;
	} while (*w);
	return 0;
}

static int parse_weakblocks(struct nandsim *ns, unsigned char *weakblocks)
{
	char *w;
	int zero_ok;
	unsigned int erase_block_no;
	unsigned int max_erases;
	struct weak_block *wb;

	if (!weakblocks)
		return 0;
	w = weakblocks;
	do {
		zero_ok = (*w == '0' ? 1 : 0);
		erase_block_no = simple_strtoul(w, &w, 0);
		if (!zero_ok && !erase_block_no) {
			pr_err("invalid weakblocks.\n");
			return -EINVAL;
		}
		max_erases = 3;
		if (*w == ':') {
			w += 1;
			max_erases = simple_strtoul(w, &w, 0);
		}
		if (*w == ',')
			w += 1;
		wb = kzalloc(sizeof(*wb), GFP_KERNEL);
		if (!wb) {
			pr_err("unable to allocate memory.\n");
			return -ENOMEM;
		}
		wb->erase_block_no = erase_block_no;
		wb->max_erases = max_erases;
		list_add(&wb->list, &ns->weak_blocks);
	} while (*w);
	return 0;
}

static int erase_error(struct nandsim *ns, unsigned int erase_block_no)
{
	struct weak_block *wb;

	list_for_each_entry(wb, &ns->weak_blocks, list)
		if (wb->erase_block_no == erase_block_no) {
			if (wb->erases_done >= wb->max_erases)
				return 1;
			wb->erases_done += 1;
			return 0;
		}
	return 0;
}

static int parse_weakpages(struct nandsim *ns, unsigned char *weakpages)
{
	char *w;
	int zero_ok;
	unsigned int page_no;
	unsigned int max_writes;
	struct weak_page *wp;

	if (!weakpages)
		return 0;
	w = weakpages;
	do {
		zero_ok = (*w == '0' ? 1 : 0);
		page_no = simple_strtoul(w, &w, 0);
		if (!zero_ok && !page_no) {
			pr_err("invalid weakpagess.\n");
			return -EINVAL;
		}
		max_writes = 3;
		if (*w == ':') {
			w += 1;
			max_writes = simple_strtoul(w, &w, 0);
		}
		if (*w == ',')
			w += 1;
		wp = kzalloc(sizeof(*wp), GFP_KERNEL);
		if (!wp) {
			pr_err("unable to allocate memory.\n");
			return -ENOMEM;
		}
		wp->page_no = page_no;
		wp->max_writes = max_writes;
		list_add(&wp->list, &ns->weak_pages);
	} while (*w);
	return 0;
}

static int write_error(struct nandsim *ns, unsigned int page_no)
{
	struct weak_page *wp;

	list_for_each_entry(wp, &ns->weak_pages, list)
		if (wp->page_no == page_no) {
			if (wp->writes_done >= wp->max_writes)
				return 1;
			wp->writes_done += 1;
			return 0;
		}
	return 0;
}

static int parse_gravepages(struct nandsim *ns, unsigned char *gravepages)
{
	char *g;
	int zero_ok;
	unsigned int page_no;
	unsigned int max_reads;
	struct grave_page *gp;

	if (!gravepages)
		return 0;
	g = gravepages;
	do {
		zero_ok = (*g == '0' ? 1 : 0);
		page_no = simple_strtoul(g, &g, 0);
		if (!zero_ok && !page_no) {
			pr_err("invalid gravepagess.\n");
			return -EINVAL;
		}
		max_reads = 3;
		if (*g == ':') {
			g += 1;
			max_reads = simple_strtoul(g, &g, 0);
		}
		if (*g == ',')
			g += 1;
		gp = kzalloc(sizeof(*gp), GFP_KERNEL);
		if (!gp) {
			pr_err("unable to allocate memory.\n");
			return -ENOMEM;
		}
		gp->page_no = page_no;
		gp->max_reads = max_reads;
		list_add(&gp->list, &ns->grave_pages);
	} while (*g);
	return 0;
}

static int read_error(struct nandsim *ns, unsigned int page_no)
{
	struct grave_page *gp;

	list_for_each_entry(gp, &ns->grave_pages, list)
		if (gp->page_no == page_no) {
			if (gp->reads_done >= gp->max_reads)
				return 1;
			gp->reads_done += 1;
			return 0;
		}
	return 0;
}

static void free_lists(struct nandsim *ns)
{
	struct list_head *pos, *n;
	list_for_each_safe(pos, n, &ns->weak_blocks) {
		list_del(pos);
		kfree(list_entry(pos, struct weak_block, list));
	}
	list_for_each_safe(pos, n, &ns->weak_pages) {
		list_del(pos);
		kfree(list_entry(pos, struct weak_page, list));
	}
	list_for_each_safe(pos, n, &ns->grave_pages) {
		list_del(pos);
		kfree(list_entry(pos, struct grave_page, list));
	}
	kfree(ns->erase_block_wear);
}

static int setup_wear_reporting(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim *ns = nand_get_controller_data(chip);
	size_t mem;

	ns->wear_eb_count = div_u64(mtd->size, mtd->erasesize);
	mem = ns->wear_eb_count * sizeof(unsigned long);
	if (mem / sizeof(unsigned long) != ns->wear_eb_count) {
		pr_err("Too many erase blocks for wear reporting\n");
		return -ENOMEM;
	}
	ns->erase_block_wear = kzalloc(mem, GFP_KERNEL);
	if (!ns->erase_block_wear) {
		pr_err("Too many erase blocks for wear reporting\n");
		return -ENOMEM;
	}
	return 0;
}

static void update_wear(struct nandsim *ns, unsigned int erase_block_no)
{
	if (!ns->erase_block_wear)
		return;
	ns->total_wear += 1;
	/*
	 * TODO: Notify this through a debugfs entry,
	 * instead of showing an error message.
	 */
	if (ns->total_wear == 0)
		pr_err("Erase counter total overflow\n");
	ns->erase_block_wear[erase_block_no] += 1;
	if (ns->erase_block_wear[erase_block_no] == 0)
		pr_err("Erase counter overflow for erase block %u\n", erase_block_no);
}

/*
 * Returns the string representation of 'state' state.
 */
static char *get_state_name(uint32_t state)
{
	switch (NS_STATE(state)) {
		case STATE_CMD_READ0:
			return "STATE_CMD_READ0";
		case STATE_CMD_READ1:
			return "STATE_CMD_READ1";
		case STATE_CMD_PAGEPROG:
			return "STATE_CMD_PAGEPROG";
		case STATE_CMD_READOOB:
			return "STATE_CMD_READOOB";
		case STATE_CMD_READSTART:
			return "STATE_CMD_READSTART";
		case STATE_CMD_ERASE1:
			return "STATE_CMD_ERASE1";
		case STATE_CMD_STATUS:
			return "STATE_CMD_STATUS";
		case STATE_CMD_SEQIN:
			return "STATE_CMD_SEQIN";
		case STATE_CMD_READID:
			return "STATE_CMD_READID";
		case STATE_CMD_ERASE2:
			return "STATE_CMD_ERASE2";
		case STATE_CMD_RESET:
			return "STATE_CMD_RESET";
		case STATE_CMD_RNDOUT:
			return "STATE_CMD_RNDOUT";
		case STATE_CMD_RNDOUTSTART:
			return "STATE_CMD_RNDOUTSTART";
		case STATE_ADDR_PAGE:
			return "STATE_ADDR_PAGE";
		case STATE_ADDR_SEC:
			return "STATE_ADDR_SEC";
		case STATE_ADDR_ZERO:
			return "STATE_ADDR_ZERO";
		case STATE_ADDR_COLUMN:
			return "STATE_ADDR_COLUMN";
		case STATE_DATAIN:
			return "STATE_DATAIN";
		case STATE_DATAOUT:
			return "STATE_DATAOUT";
		case STATE_DATAOUT_ID:
			return "STATE_DATAOUT_ID";
		case STATE_DATAOUT_STATUS:
			return "STATE_DATAOUT_STATUS";
		case STATE_READY:
			return "STATE_READY";
		case STATE_UNKNOWN:
			return "STATE_UNKNOWN";
	}

	pr_err("unknown state, BUG\n");
	return NULL;
}

/*
 * Check if command is valid.
 *
 * RETURNS: 1 if wrong command, 0 if right.
 */
static int check_command(int cmd)
{
	switch (cmd) {

	case NAND_CMD_READ0:
	case NAND_CMD_READ1:
	case NAND_CMD_READSTART:
	case NAND_CMD_PAGEPROG:
	case NAND_CMD_READOOB:
	case NAND_CMD_ERASE1:
	case NAND_CMD_STATUS:
	case NAND_CMD_SEQIN:
	case NAND_CMD_READID:
	case NAND_CMD_ERASE2:
	case NAND_CMD_RESET:
	case NAND_CMD_RNDOUT:
	case NAND_CMD_RNDOUTSTART:
		return 0;

	default:
		return 1;
	}
}

/*
 * Returns state after command is accepted by command number.
 */
static uint32_t get_state_by_command(unsigned command)
{
	switch (command) {
		case NAND_CMD_READ0:
			return STATE_CMD_READ0;
		case NAND_CMD_READ1:
			return STATE_CMD_READ1;
		case NAND_CMD_PAGEPROG:
			return STATE_CMD_PAGEPROG;
		case NAND_CMD_READSTART:
			return STATE_CMD_READSTART;
		case NAND_CMD_READOOB:
			return STATE_CMD_READOOB;
		case NAND_CMD_ERASE1:
			return STATE_CMD_ERASE1;
		case NAND_CMD_STATUS:
			return STATE_CMD_STATUS;
		case NAND_CMD_SEQIN:
			return STATE_CMD_SEQIN;
		case NAND_CMD_READID:
			return STATE_CMD_READID;
		case NAND_CMD_ERASE2:
			return STATE_CMD_ERASE2;
		case NAND_CMD_RESET:
			return STATE_CMD_RESET;
		case NAND_CMD_RNDOUT:
			return STATE_CMD_RNDOUT;
		case NAND_CMD_RNDOUTSTART:
			return STATE_CMD_RNDOUTSTART;
	}

	pr_err("unknown command, BUG\n");
	return 0;
}

/*
 * Move an address byte to the correspondent internal register.
 */
static inline void accept_addr_byte(struct nandsim *ns, u_char bt)
{
	uint byte = (uint)bt;

	if (ns->regs.count < (ns->geom.pgaddrbytes - ns->geom.secaddrbytes))
		ns->regs.column |= (byte << 8 * ns->regs.count);
	else {
		ns->regs.row |= (byte << 8 * (ns->regs.count -
						ns->geom.pgaddrbytes +
						ns->geom.secaddrbytes));
	}

	return;
}

/*
 * Switch to STATE_READY state.
 */
static inline void switch_to_ready_state(struct nandsim *ns, u_char status)
{
	pr_debug("switch to %s state\n", get_state_name(STATE_READY));

	ns->state       = STATE_READY;
	ns->nxstate     = STATE_UNKNOWN;
	ns->op          = NULL;
	ns->npstates    = 0;
	ns->stateidx    = 0;
	ns->regs.num    = 0;
	ns->regs.count  = 0;
	ns->regs.off    = 0;
	ns->regs.row    = 0;
	ns->regs.column = 0;
	ns->regs.status = status;
}

/*
 * If the operation isn't known yet, try to find it in the global array
 * of supported operations.
 *
 * Operation can be unknown because of the following.
 *   1. New command was accepted and this is the first call to find the
 *      correspondent states chain. In this case ns->npstates = 0;
 *   2. There are several operations which begin with the same command(s)
 *      (for example program from the second half and read from the
 *      second half operations both begin with the READ1 command). In this
 *      case the ns->pstates[] array contains previous states.
 *
 * Thus, the function tries to find operation containing the following
 * states (if the 'flag' parameter is 0):
 *    ns->pstates[0], ... ns->pstates[ns->npstates], ns->state
 *
 * If (one and only one) matching operation is found, it is accepted (
 * ns->ops, ns->state, ns->nxstate are initialized, ns->npstate is
 * zeroed).
 *
 * If there are several matches, the current state is pushed to the
 * ns->pstates.
 *
 * The operation can be unknown only while commands are input to the chip.
 * As soon as address command is accepted, the operation must be known.
 * In such situation the function is called with 'flag' != 0, and the
 * operation is searched using the following pattern:
 *     ns->pstates[0], ... ns->pstates[ns->npstates], <address input>
 *
 * It is supposed that this pattern must either match one operation or
 * none. There can't be ambiguity in that case.
 *
 * If no matches found, the function does the following:
 *   1. if there are saved states present, try to ignore them and search
 *      again only using the last command. If nothing was found, switch
 *      to the STATE_READY state.
 *   2. if there are no saved states, switch to the STATE_READY state.
 *
 * RETURNS: -2 - no matched operations found.
 *          -1 - several matches.
 *           0 - operation is found.
 */
static int find_operation(struct nandsim *ns, uint32_t flag)
{
	int opsfound = 0;
	int i, j, idx = 0;

	for (i = 0; i < NS_OPER_NUM; i++) {

		int found = 1;

		if (!(ns->options & ops[i].reqopts))
			/* Ignore operations we can't perform */
			continue;

		if (flag) {
			if (!(ops[i].states[ns->npstates] & STATE_ADDR_MASK))
				continue;
		} else {
			if (NS_STATE(ns->state) != NS_STATE(ops[i].states[ns->npstates]))
				continue;
		}

		for (j = 0; j < ns->npstates; j++)
			if (NS_STATE(ops[i].states[j]) != NS_STATE(ns->pstates[j])
				&& (ns->options & ops[idx].reqopts)) {
				found = 0;
				break;
			}

		if (found) {
			idx = i;
			opsfound += 1;
		}
	}

	if (opsfound == 1) {
		/* Exact match */
		ns->op = &ops[idx].states[0];
		if (flag) {
			/*
			 * In this case the find_operation function was
			 * called when address has just began input. But it isn't
			 * yet fully input and the current state must
			 * not be one of STATE_ADDR_*, but the STATE_ADDR_*
			 * state must be the next state (ns->nxstate).
			 */
			ns->stateidx = ns->npstates - 1;
		} else {
			ns->stateidx = ns->npstates;
		}
		ns->npstates = 0;
		ns->state = ns->op[ns->stateidx];
		ns->nxstate = ns->op[ns->stateidx + 1];
		pr_debug("operation found, index: %d, state: %s, nxstate %s\n",
				idx, get_state_name(ns->state), get_state_name(ns->nxstate));
		return 0;
	}

	if (opsfound == 0) {
		/* Nothing was found. Try to ignore previous commands (if any) and search again */
		if (ns->npstates != 0) {
			pr_debug("no operation found, try again with state %s\n",
					get_state_name(ns->state));
			ns->npstates = 0;
			return find_operation(ns, 0);

		}
		pr_debug("no operations found\n");
		switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
		return -2;
	}

	if (flag) {
		/* This shouldn't happen */
		pr_err("BUG, operation must be known if address is input\n");
		return -2;
	}

	pr_debug("there is still ambiguity\n");

	ns->pstates[ns->npstates++] = ns->state;

	return -1;
}

static void put_pages(struct nandsim *ns)
{
	int i;
	struct ns_cachefile_data *data = ns->backend_data;

	for (i = 0; i < data->held_cnt; i++)
		put_page(data->held_pages[i]);
}

/* Get page cache pages in advance to provide NOFS memory allocation */
static int get_pages(struct nandsim *ns, struct file *file, size_t count, loff_t pos)
{
	pgoff_t index, start_index, end_index;
	struct page *page;
	struct address_space *mapping = file->f_mapping;
	struct ns_cachefile_data *data = ns->backend_data;

	start_index = pos >> PAGE_SHIFT;
	end_index = (pos + count - 1) >> PAGE_SHIFT;
	if (end_index - start_index + 1 > NS_MAX_HELD_PAGES)
		return -EINVAL;
	data->held_cnt = 0;
	for (index = start_index; index <= end_index; index++) {
		page = find_get_page(mapping, index);
		if (page == NULL) {
			page = find_or_create_page(mapping, index, GFP_NOFS);
			if (page == NULL) {
				write_inode_now(mapping->host, 1);
				page = find_or_create_page(mapping, index, GFP_NOFS);
			}
			if (page == NULL) {
				put_pages(ns);
				return -ENOMEM;
			}
			unlock_page(page);
		}
		data->held_pages[data->held_cnt++] = page;
	}
	return 0;
}

static int set_memalloc(void)
{
	if (current->flags & PF_MEMALLOC)
		return 0;
	current->flags |= PF_MEMALLOC;
	return 1;
}

static void clear_memalloc(int memalloc)
{
	if (memalloc)
		current->flags &= ~PF_MEMALLOC;
}

static ssize_t read_file(struct nandsim *ns, struct file *file, void *buf, size_t count, loff_t pos)
{
	ssize_t tx;
	int err, memalloc;

	err = get_pages(ns, file, count, pos);
	if (err)
		return err;
	memalloc = set_memalloc();
	tx = kernel_read(file, pos, buf, count);
	clear_memalloc(memalloc);
	put_pages(ns);
	return tx;
}

static ssize_t write_file(struct nandsim *ns, struct file *file, void *buf, size_t count, loff_t pos)
{
	ssize_t tx;
	int err, memalloc;

	err = get_pages(ns, file, count, pos);
	if (err)
		return err;
	memalloc = set_memalloc();
	tx = kernel_write(file, buf, count, pos);
	clear_memalloc(memalloc);
	put_pages(ns);
	return tx;
}

/*
 * Returns a pointer to the current page.
 */
static inline union ns_mem *NS_GET_PAGE(struct nandsim *ns)
{
	struct ns_ram_data *data = ns->backend_data;

	return &(data->pages[ns->regs.row]);
}

/*
 * Retuns a pointer to the current byte, within the current page.
 */
static inline u_char *NS_PAGE_BYTE_OFF(struct nandsim *ns)
{
	return NS_GET_PAGE(ns)->byte + ns->regs.column + ns->regs.off;
}

static bool buffer_is_ff(struct nandsim *ns, int num)
{
	int i;

	for (i = 0; i < num; i++) {
		if (ns->buf.byte[i] != 0xff)
			return false;
	}

	return true;
}

static int do_read_error(struct nandsim *ns, int num, bool check_ff)
{
	unsigned int page_no = ns->regs.row;

	if (read_error(ns, page_no)) {
		if (check_ff && buffer_is_ff(ns, num))
			return 0;

		prandom_bytes(ns->buf.byte, num);
		pr_warn("simulating read error in page %u\n", page_no);
		return 1;
	}
	return 0;
}

static void do_bit_flips(struct nandsim *ns, int num, bool check_ff)
{
	struct mtd_info *nsmtd = ns_to_mtd(ns);

	if (ns->bitflips && prandom_u32() < (1 << 22)) {
		int flips = 1;

		if (check_ff && buffer_is_ff(ns, num))
			return;

		if (ns->bitflips > 1)
			flips = (prandom_u32() % (int)ns->bitflips) + 1;
		while (flips--) {
			int pos = prandom_u32() % (num * 8);
			ns->buf.byte[pos / 8] ^= (1 << (pos % 8));
			pr_warn("flipping bit %d in page %d "
				"reading from %d ecc: corrected=%u failed=%u\n",
				pos, ns->regs.row, ns->regs.column + ns->regs.off,
				nsmtd->ecc_stats.corrected, nsmtd->ecc_stats.failed);
		}
	}
}

static void ns_ram_read_page(struct nandsim *ns, int num)
{
	union ns_mem *mypage;

	mypage = NS_GET_PAGE(ns);
	if (mypage->byte == NULL) {
		pr_debug("page %d not allocated\n", ns->regs.row);
		memset(ns->buf.byte, 0xFF, num);
	} else {
		pr_debug("page %d allocated, reading from %d\n",
			ns->regs.row, ns->regs.column + ns->regs.off);
		if (do_read_error(ns, num, false))
			return;
		memcpy(ns->buf.byte, NS_PAGE_BYTE_OFF(ns), num);
		do_bit_flips(ns, num, false);
	}
}

static void ns_cachefile_read_page(struct nandsim *ns, int num)
{
	struct ns_cachefile_data *data = ns->backend_data;

	if (!test_bit(ns->regs.row, data->pages_written)) {
		pr_debug("page %d not written\n", ns->regs.row);
		memset(ns->buf.byte, 0xFF, num);
	} else {
		loff_t pos;
		ssize_t tx;

		pr_debug("page %d written, reading from %d\n",
			ns->regs.row, ns->regs.column + ns->regs.off);
		if (do_read_error(ns, num, false))
			return;
		pos = (loff_t)NS_RAW_OFFSET(ns) + ns->regs.off;
		tx = read_file(ns, data->cfile, ns->buf.byte, num, pos);
		if (tx != num) {
			pr_err("read error for page %d ret %ld\n", ns->regs.row, (long)tx);
			return;
		}
		do_bit_flips(ns, num, false);
	}
}

void __ns_file_read_page(struct nandsim *ns, int num,
			 int (*read_fn)(struct nandsim *ns, char *addr,
					unsigned long count, loff_t offset))
{
	loff_t pos;
	ssize_t tx;

	pr_debug("page %d written, reading from %d\n",
		ns->regs.row, ns->regs.column + ns->regs.off);

	if (ns->no_oob) {
		loff_t pg_read_end = ns->regs.column + ns->regs.off + num;

		/* direct read from OOB */
		if (ns->regs.column + ns->regs.off >= ns->geom.pgsz) {
			memset(ns->buf.byte, 0xff, num);
			return;
		}

		/* read overlapps into OOB, needs fixup */
		if (pg_read_end > ns->geom.pgsz) {
			loff_t oob_part = pg_read_end - ns->geom.pgsz;

			memset(ns->buf.byte + pg_read_end - oob_part, 0xff, oob_part);
			num -= oob_part;
		}
	}

	pos = (loff_t)NS_RAW_OFFSET(ns) + ns->regs.off;
	tx = read_fn(ns, ns->buf.byte, num, pos);
	if (tx == 0)
		memset(ns->buf.byte, 0xff, num);
	else if (tx != num)
		pr_err("read error for page %d ret %ld\n", ns->regs.row, (long)tx);

	do_read_error(ns, num, true);
	do_bit_flips(ns, num, true);
}
EXPORT_SYMBOL_GPL(__ns_file_read_page);

static inline int do_kernel_read(struct nandsim *ns, char *addr,
				 unsigned long count, loff_t offset)
{
	struct ns_file_data *data = ns->backend_data;

	return kernel_read(data->file, offset, addr, count);
}

static void ns_file_read_page(struct nandsim *ns, int num)
{
	__ns_file_read_page(ns, num, do_kernel_read);
}

static void ns_ram_erase_sector(struct nandsim *ns)
{
	union ns_mem *mypage;
	int i;
	struct ns_ram_data *data = ns->backend_data;

	mypage = NS_GET_PAGE(ns);
	for (i = 0; i < ns->geom.pgsec; i++) {
		if (mypage->byte != NULL) {
			pr_debug("freeing page %d\n", ns->regs.row+i);
			kmem_cache_free(data->nand_pages_slab, mypage->byte);
			mypage->byte = NULL;
		}
		mypage++;
	}
}

static void ns_cachefile_erase_sector(struct nandsim *ns)
{
	int i;
	struct ns_cachefile_data *data = ns->backend_data;

	for (i = 0; i < ns->geom.pgsec; i++) {
		if (__test_and_clear_bit(ns->regs.row + i,
					 data->pages_written)) {
			pr_debug("freeing page %d\n", ns->regs.row + i);
		}
	}
}

static inline ssize_t do_kernel_write(struct nandsim *ns, const char *addr,
				      size_t count, loff_t offset)
{
	struct ns_file_data *data = ns->backend_data;

	return kernel_write(data->file, addr, count, offset);
}


void __ns_file_erase_sector(struct nandsim *ns, char *file_buf,
			    ssize_t (*write_fn)(struct nandsim *ns, const char *buf,
			    size_t count, loff_t pos))
{
	int i;
	loff_t pos;
	ssize_t tx;
	unsigned int pagesz = ns->no_oob ? ns->geom.pgsz : ns->geom.pgszoob;

	memset(file_buf, 0xff, pagesz);

	for (i = 0; i < ns->geom.pgsec; i++) {
		pos = (loff_t)(ns->regs.row + i) * pagesz;
		tx = write_fn(ns, file_buf, pagesz, pos);
		if (tx != pagesz) {
			pr_err("write error for page %d ret %ld\n", ns->regs.row, (long)tx);
		}
	}
}
EXPORT_SYMBOL_GPL(__ns_file_erase_sector);

static void ns_file_erase_sector(struct nandsim *ns)
{
	struct ns_file_data *data = ns->backend_data;

	__ns_file_erase_sector(ns, data->file_buf, do_kernel_write);
}

static int ns_ram_prog_page(struct nandsim *ns, int num)
{
	int i;
	union ns_mem *mypage;
	u_char *pg_off;
	struct ns_ram_data *data = ns->backend_data;

	mypage = NS_GET_PAGE(ns);
	if (mypage->byte == NULL) {
		pr_debug("allocating page %d\n", ns->regs.row);
		/*
		 * We allocate memory with GFP_NOFS because a flash FS may
		 * utilize this. If it is holding an FS lock, then gets here,
		 * then kernel memory alloc runs writeback which goes to the FS
		 * again and deadlocks. This was seen in practice.
		 */
		mypage->byte = kmem_cache_alloc(data->nand_pages_slab, GFP_NOFS);
		if (mypage->byte == NULL) {
			pr_err("error allocating memory for page %d\n", ns->regs.row);
			return -1;
		}
		memset(mypage->byte, 0xFF, ns->geom.pgszoob);
	}

	pg_off = NS_PAGE_BYTE_OFF(ns);
	for (i = 0; i < num; i++)
		pg_off[i] &= ns->buf.byte[i];

	return 0;
}

static int ns_cachefile_prog_page(struct nandsim *ns, int num)
{
	int i, all;
	loff_t off;
	ssize_t tx;
	u_char *pg_off;
	struct ns_cachefile_data *data = ns->backend_data;

	pr_debug("writing page %d\n", ns->regs.row);
	pg_off = data->file_buf + ns->regs.column + ns->regs.off;
	off = (loff_t)NS_RAW_OFFSET(ns) + ns->regs.off;
	if (!test_bit(ns->regs.row, data->pages_written)) {
		all = 1;
		memset(data->file_buf, 0xff, ns->geom.pgszoob);
	} else {
		all = 0;
		tx = read_file(ns, data->cfile, pg_off, num, off);
		if (tx != num) {
			pr_err("read error for page %d ret %ld\n", ns->regs.row, (long)tx);
			return -1;
		}
	}
	for (i = 0; i < num; i++)
		pg_off[i] &= ns->buf.byte[i];
	if (all) {
		loff_t pos = (loff_t)ns->regs.row * ns->geom.pgszoob;
		tx = write_file(ns, data->cfile, data->file_buf, ns->geom.pgszoob, pos);
		if (tx != ns->geom.pgszoob) {
			pr_err("write error for page %d ret %ld\n", ns->regs.row, (long)tx);
			return -1;
		}
		__set_bit(ns->regs.row, data->pages_written);
	} else {
		tx = write_file(ns, data->cfile, pg_off, num, off);
		if (tx != num) {
			pr_err("write error for page %d ret %ld\n", ns->regs.row, (long)tx);
			return -1;
		}
	}
	return 0;
}

int __ns_file_prog_page(struct nandsim *ns, int num, char *file_buf,
			int (*read_fn)(struct nandsim *ns, char *addr,
				       unsigned long count, loff_t offset),
			ssize_t (*write_fn)(struct nandsim *ns, const char *buf,
					    size_t count, loff_t pos))
{
	int i;
	loff_t off;
	ssize_t tx;
	u_char *pg_off;

	pr_debug("writing page %d\n", ns->regs.row);

	if (ns->no_oob) {
		loff_t pg_write_end = ns->regs.column + ns->regs.off + num;

		/* direct write into oob */
		if (ns->regs.column + ns->regs.off >= ns->geom.pgsz)
			return 0;

		/* write overlapps into OOB, needs fixup */
		if (pg_write_end > ns->geom.pgsz)
			num -= pg_write_end - ns->geom.pgsz;
	}

	pg_off = file_buf + ns->regs.column + ns->regs.off;
	off = (loff_t)NS_RAW_OFFSET(ns) + ns->regs.off;

	tx = read_fn(ns, pg_off, num, off);
	if (tx == 0)
		memset(pg_off, 0xff, num);
	else if (tx != num) {
		pr_err("read error for page %d ret %ld\n", ns->regs.row, (long)tx);
		return -1;
	}

	for (i = 0; i < num; i++)
		pg_off[i] &= ns->buf.byte[i];

	tx = write_fn(ns, pg_off, num, off);
	if (tx != num) {
		pr_err("write error for page %d ret %ld\n", ns->regs.row, (long)tx);
		return -1;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(__ns_file_prog_page);

static int ns_file_prog_page(struct nandsim *ns, int num)
{
	struct ns_file_data *data = ns->backend_data;

	return __ns_file_prog_page(ns, num, data->file_buf, do_kernel_read,
				   do_kernel_write);
}

static struct ns_backend_ops ns_ram_bops = {
	.erase_sector = ns_ram_erase_sector,
	.prog_page = ns_ram_prog_page,
	.read_page = ns_ram_read_page,
	.init = ns_ram_init,
	.destroy = ns_ram_destroy,
	.name = "ram",
};

static struct ns_backend_ops ns_cachefile_bops = {
	.erase_sector = ns_cachefile_erase_sector,
	.prog_page = ns_cachefile_prog_page,
	.read_page = ns_cachefile_read_page,
	.init = ns_cachefile_init,
	.destroy = ns_cachefile_destroy,
	.name = "cache_file",
};

static struct ns_backend_ops ns_file_bops = {
	.erase_sector = ns_file_erase_sector,
	.prog_page = ns_file_prog_page,
	.read_page = ns_file_read_page,
	.init = ns_file_init,
	.destroy = ns_file_destroy,
	.name = "file",
};

/*
 * If state has any action bit, perform this action.
 *
 * RETURNS: 0 if success, -1 if error.
 */
static int do_state_action(struct nandsim *ns, uint32_t action)
{
	int num;
	int busdiv = ns->busw == 8 ? 1 : 2;
	unsigned int erase_block_no, page_no;

	action &= ACTION_MASK;

	/* Check that page address input is correct */
	if (action != ACTION_SECERASE && ns->regs.row >= ns->geom.pgnum) {
		pr_warn("wrong page number (%#x)\n", ns->regs.row);
		return -1;
	}

	switch (action) {

	case ACTION_CPY:
		/*
		 * Copy page data to the internal buffer.
		 */

		/* Column shouldn't be very large */
		if (ns->regs.column >= (ns->geom.pgszoob - ns->regs.off)) {
			pr_err("column number is too large\n");
			break;
		}
		num = ns->geom.pgszoob - ns->regs.off - ns->regs.column;
		ns->bops->read_page(ns, num);

		pr_debug("ACTION_CPY: copy %d bytes to int buf, raw offset %d\n",
			num, NS_RAW_OFFSET(ns) + ns->regs.off);

		if (ns->regs.off == 0)
			pr_debug("read page %d\n", ns->regs.row);
		else if (ns->regs.off < ns->geom.pgsz)
			pr_debug("read page %d (second half)\n", ns->regs.row);
		else
			pr_debug("read OOB of page %d\n", ns->regs.row);

		NS_UDELAY(ns, ns->access_delay);
		NS_UDELAY(ns, ns->input_cycle * ns->geom.pgsz / 1000 / busdiv);

		break;

	case ACTION_SECERASE:
		/*
		 * Erase sector.
		 */

		if (ns->lines.wp) {
			pr_err("device is write-protected, ignore sector erase\n");
			return -1;
		}

		if (ns->regs.row >= ns->geom.pgnum - ns->geom.pgsec
			|| (ns->regs.row & ~(ns->geom.secsz - 1))) {
			pr_err("wrong sector address (%#x)\n", ns->regs.row);
			return -1;
		}

		ns->regs.row = (ns->regs.row <<
				8 * (ns->geom.pgaddrbytes - ns->geom.secaddrbytes)) | ns->regs.column;
		ns->regs.column = 0;

		erase_block_no = ns->regs.row >> (ns->geom.secshift - ns->geom.pgshift);

		pr_debug("erase sector at address %#x, off = %d\n",
				ns->regs.row, NS_RAW_OFFSET(ns));
		pr_debug("erase sector %u\n", erase_block_no);

		ns->bops->erase_sector(ns);

		NS_MDELAY(ns, ns->erase_delay);

		if (ns->erase_block_wear)
			update_wear(ns, erase_block_no);

		if (erase_error(ns, erase_block_no)) {
			pr_warn("simulating erase failure in erase block %u\n", erase_block_no);
			return -1;
		}

		break;

	case ACTION_PRGPAGE:
		/*
		 * Program page - move internal buffer data to the page.
		 */

		if (ns->lines.wp) {
			pr_warn("device is write-protected, program\n");
			return -1;
		}

		if (ns->no_oob)
			num = ns->geom.pgsz - ns->regs.off - ns->regs.column;
		else
			num = ns->geom.pgszoob - ns->regs.off - ns->regs.column;

		if (num != ns->regs.count) {
			pr_err("too few bytes were input (%d instead of %d)\n",
					ns->regs.count, num);
			return -1;
		}

		if (ns->bops->prog_page(ns, num) == -1)
			return -1;

		page_no = ns->regs.row;

		pr_debug("copy %d bytes from int buf to (%#x, %#x), raw off = %d\n",
			num, ns->regs.row, ns->regs.column, NS_RAW_OFFSET(ns) + ns->regs.off);
		pr_debug("program page %d\n", ns->regs.row);

		NS_UDELAY(ns, ns->program_delay);
		NS_UDELAY(ns, ns->output_cycle * ns->geom.pgsz / 1000 / busdiv);

		if (write_error(ns, page_no)) {
			pr_warn("simulating write failure in page %u\n", page_no);
			return -1;
		}

		break;

	case ACTION_ZEROOFF:
		pr_debug("set internal offset to 0\n");
		ns->regs.off = 0;
		break;

	case ACTION_HALFOFF:
		if (!(ns->options & OPT_PAGE512_8BIT)) {
			pr_err("BUG! can't skip half of page for non-512"
				"byte page size 8x chips\n");
			return -1;
		}
		pr_debug("set internal offset to %d\n", ns->geom.pgsz/2);
		ns->regs.off = ns->geom.pgsz/2;
		break;

	case ACTION_OOBOFF:
		pr_debug("set internal offset to %d\n", ns->geom.pgsz);
		ns->regs.off = ns->geom.pgsz;
		break;

	default:
		pr_err("BUG! unknown action\n");
	}

	return 0;
}

/*
 * Switch simulator's state.
 */
static void switch_state(struct nandsim *ns)
{
	if (ns->op) {
		/*
		 * The current operation have already been identified.
		 * Just follow the states chain.
		 */

		ns->stateidx += 1;
		ns->state = ns->nxstate;
		ns->nxstate = ns->op[ns->stateidx + 1];

		pr_debug("operation is known, switch to the next state, "
			"state: %s, nxstate: %s\n",
			get_state_name(ns->state), get_state_name(ns->nxstate));

		/* See, whether we need to do some action */
		if ((ns->state & ACTION_MASK) && do_state_action(ns, ns->state) < 0) {
			switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
			return;
		}

	} else {
		/*
		 * We don't yet know which operation we perform.
		 * Try to identify it.
		 */

		/*
		 *  The only event causing the switch_state function to
		 *  be called with yet unknown operation is new command.
		 */
		ns->state = get_state_by_command(ns->regs.command);

		pr_debug("operation is unknown, try to find it\n");

		if (find_operation(ns, 0) != 0)
			return;

		if ((ns->state & ACTION_MASK) && do_state_action(ns, ns->state) < 0) {
			switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
			return;
		}
	}

	/* For 16x devices column means the page offset in words */
	if ((ns->nxstate & STATE_ADDR_MASK) && ns->busw == 16) {
		pr_debug("double the column number for 16x device\n");
		ns->regs.column <<= 1;
	}

	if (NS_STATE(ns->nxstate) == STATE_READY) {
		/*
		 * The current state is the last. Return to STATE_READY
		 */

		u_char status = NS_STATUS_OK(ns);

		/* In case of data states, see if all bytes were input/output */
		if ((ns->state & (STATE_DATAIN_MASK | STATE_DATAOUT_MASK))
			&& ns->regs.count != ns->regs.num) {
			pr_warn("not all bytes were processed, %d left\n",
					ns->regs.num - ns->regs.count);
			status = NS_STATUS_FAILED(ns);
		}

		pr_debug("operation complete, switch to STATE_READY state\n");

		switch_to_ready_state(ns, status);

		return;
	} else if (ns->nxstate & (STATE_DATAIN_MASK | STATE_DATAOUT_MASK)) {
		/*
		 * If the next state is data input/output, switch to it now
		 */

		ns->state      = ns->nxstate;
		ns->nxstate    = ns->op[++ns->stateidx + 1];
		ns->regs.num   = ns->regs.count = 0;

		pr_debug("the next state is data I/O, switch, "
			"state: %s, nxstate: %s\n",
			get_state_name(ns->state), get_state_name(ns->nxstate));

		/*
		 * Set the internal register to the count of bytes which
		 * are expected to be input or output
		 */
		switch (NS_STATE(ns->state)) {
			case STATE_DATAIN:
			case STATE_DATAOUT:
				ns->regs.num = ns->geom.pgszoob - ns->regs.off - ns->regs.column;
				break;

			case STATE_DATAOUT_ID:
				ns->regs.num = ns->geom.idbytes;
				break;

			case STATE_DATAOUT_STATUS:
				ns->regs.count = ns->regs.num = 0;
				break;

			default:
				pr_err("BUG! unknown data state\n");
		}

	} else if (ns->nxstate & STATE_ADDR_MASK) {
		/*
		 * If the next state is address input, set the internal
		 * register to the number of expected address bytes
		 */

		ns->regs.count = 0;

		switch (NS_STATE(ns->nxstate)) {
			case STATE_ADDR_PAGE:
				ns->regs.num = ns->geom.pgaddrbytes;

				break;
			case STATE_ADDR_SEC:
				ns->regs.num = ns->geom.secaddrbytes;
				break;

			case STATE_ADDR_ZERO:
				ns->regs.num = 1;
				break;

			case STATE_ADDR_COLUMN:
				/* Column address is always 2 bytes */
				ns->regs.num = ns->geom.pgaddrbytes - ns->geom.secaddrbytes;
				break;

			default:
				pr_err("BUG! unknown address state\n");
		}
	} else {
		/*
		 * Just reset internal counters.
		 */

		ns->regs.num = 0;
		ns->regs.count = 0;
	}
}

static u_char ns_nand_read_byte(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim *ns = nand_get_controller_data(chip);
	u_char outb = 0x00;

	/* Sanity and correctness checks */
	if (!ns->lines.ce) {
		pr_err("chip is disabled, return %#x\n", (uint)outb);
		return outb;
	}
	if (ns->lines.ale || ns->lines.cle) {
		pr_err("ALE or CLE pin is high, return %#x\n", (uint)outb);
		return outb;
	}
	if (!(ns->state & STATE_DATAOUT_MASK)) {
		pr_warn("unexpected data output cycle, state is %s "
			"return %#x\n", get_state_name(ns->state), (uint)outb);
		return outb;
	}

	/* Status register may be read as many times as it is wanted */
	if (NS_STATE(ns->state) == STATE_DATAOUT_STATUS) {
		pr_debug("return %#x status\n", ns->regs.status);
		return ns->regs.status;
	}

	/* Check if there is any data in the internal buffer which may be read */
	if (ns->regs.count == ns->regs.num) {
		pr_warn("no more data to output, return %#x\n", (uint)outb);
		return outb;
	}

	switch (NS_STATE(ns->state)) {
		case STATE_DATAOUT:
			if (ns->busw == 8) {
				outb = ns->buf.byte[ns->regs.count];
				ns->regs.count += 1;
			} else {
				outb = (u_char)cpu_to_le16(ns->buf.word[ns->regs.count >> 1]);
				ns->regs.count += 2;
			}
			break;
		case STATE_DATAOUT_ID:
			pr_debug("read ID byte %d, total = %d\n", ns->regs.count, ns->regs.num);
			outb = ns->ids[ns->regs.count];
			ns->regs.count += 1;
			break;
		default:
			BUG();
	}

	if (ns->regs.count == ns->regs.num) {
		pr_debug("all bytes were read\n");

		if (NS_STATE(ns->nxstate) == STATE_READY)
			switch_state(ns);
	}

	return outb;
}

static void ns_nand_write_byte(struct mtd_info *mtd, u_char byte)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim *ns = nand_get_controller_data(chip);

	/* Sanity and correctness checks */
	if (!ns->lines.ce) {
		pr_err("chip is disabled, ignore write\n");
		return;
	}
	if (ns->lines.ale && ns->lines.cle) {
		pr_err("ALE and CLE pins are high simultaneously, ignore write\n");
		return;
	}

	if (ns->lines.cle == 1) {
		/*
		 * The byte written is a command.
		 */

		if (byte == NAND_CMD_RESET) {
			pr_debug("reset chip\n");
			switch_to_ready_state(ns, NS_STATUS_OK(ns));
			return;
		}

		/* Check that the command byte is correct */
		if (check_command(byte)) {
			pr_err("unknown command %#x\n", (uint)byte);
			return;
		}

		if (NS_STATE(ns->state) == STATE_DATAOUT_STATUS
			|| NS_STATE(ns->state) == STATE_DATAOUT) {
			int row = ns->regs.row;

			switch_state(ns);
			if (byte == NAND_CMD_RNDOUT)
				ns->regs.row = row;
		}

		/* Check if chip is expecting command */
		if (NS_STATE(ns->nxstate) != STATE_UNKNOWN && !(ns->nxstate & STATE_CMD_MASK)) {
			/* Do not warn if only 2 id bytes are read */
			if (!(ns->regs.command == NAND_CMD_READID &&
			    NS_STATE(ns->state) == STATE_DATAOUT_ID && ns->regs.count == 2)) {
				/*
				 * We are in situation when something else (not command)
				 * was expected but command was input. In this case ignore
				 * previous command(s)/state(s) and accept the last one.
				 */
				pr_warn("command (%#x) wasn't expected, expected state is %s, "
					"ignore previous states\n", (uint)byte, get_state_name(ns->nxstate));
			}
			switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
		}

		pr_debug("command byte corresponding to %s state accepted\n",
			get_state_name(get_state_by_command(byte)));
		ns->regs.command = byte;
		switch_state(ns);

	} else if (ns->lines.ale == 1) {
		/*
		 * The byte written is an address.
		 */

		if (NS_STATE(ns->nxstate) == STATE_UNKNOWN) {

			pr_debug("operation isn't known yet, identify it\n");

			if (find_operation(ns, 1) < 0)
				return;

			if ((ns->state & ACTION_MASK) && do_state_action(ns, ns->state) < 0) {
				switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
				return;
			}

			ns->regs.count = 0;
			switch (NS_STATE(ns->nxstate)) {
				case STATE_ADDR_PAGE:
					ns->regs.num = ns->geom.pgaddrbytes;
					break;
				case STATE_ADDR_SEC:
					ns->regs.num = ns->geom.secaddrbytes;
					break;
				case STATE_ADDR_ZERO:
					ns->regs.num = 1;
					break;
				default:
					BUG();
			}
		}

		/* Check that chip is expecting address */
		if (!(ns->nxstate & STATE_ADDR_MASK)) {
			pr_err("address (%#x) isn't expected, expected state is %s, "
				"switch to STATE_READY\n", (uint)byte, get_state_name(ns->nxstate));
			switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
			return;
		}

		/* Check if this is expected byte */
		if (ns->regs.count == ns->regs.num) {
			pr_err("no more address bytes expected\n");
			switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
			return;
		}

		accept_addr_byte(ns, byte);

		ns->regs.count += 1;

		pr_debug("address byte %#x was accepted (%d bytes input, %d expected)\n",
				(uint)byte, ns->regs.count, ns->regs.num);

		if (ns->regs.count == ns->regs.num) {
			pr_debug("address (%#x, %#x) is accepted\n", ns->regs.row, ns->regs.column);
			switch_state(ns);
		}

	} else {
		/*
		 * The byte written is an input data.
		 */

		/* Check that chip is expecting data input */
		if (!(ns->state & STATE_DATAIN_MASK)) {
			pr_err("data input (%#x) isn't expected, state is %s, "
				"switch to %s\n", (uint)byte,
				get_state_name(ns->state), get_state_name(STATE_READY));
			switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
			return;
		}

		/* Check if this is expected byte */
		if (ns->regs.count == ns->regs.num) {
			pr_warn("%u input bytes has already been accepted, ignore write\n",
					ns->regs.num);
			return;
		}

		if (ns->busw == 8) {
			ns->buf.byte[ns->regs.count] = byte;
			ns->regs.count += 1;
		} else {
			ns->buf.word[ns->regs.count >> 1] = cpu_to_le16((uint16_t)byte);
			ns->regs.count += 2;
		}
	}

	return;
}

static void ns_hwcontrol(struct mtd_info *mtd, int cmd, unsigned int bitmask)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim *ns = nand_get_controller_data(chip);

	ns->lines.cle = bitmask & NAND_CLE ? 1 : 0;
	ns->lines.ale = bitmask & NAND_ALE ? 1 : 0;
	ns->lines.ce = bitmask & NAND_NCE ? 1 : 0;

	if (cmd != NAND_CMD_NONE)
		ns_nand_write_byte(mtd, cmd);
}

static int ns_device_ready(struct mtd_info *mtd)
{
	pr_debug("device_ready\n");
	return 1;
}

static uint16_t ns_nand_read_word(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	pr_debug("read_word\n");

	return chip->read_byte(mtd) | (chip->read_byte(mtd) << 8);
}

static void ns_nand_write_buf(struct mtd_info *mtd, const u_char *buf, int len)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim *ns = nand_get_controller_data(chip);

	/* Check that chip is expecting data input */
	if (!(ns->state & STATE_DATAIN_MASK)) {
		pr_err("data input isn't expected, state is %s, "
			"switch to STATE_READY\n", get_state_name(ns->state));
		switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
		return;
	}

	/* Check if these are expected bytes */
	if (ns->regs.count + len > ns->regs.num) {
		pr_err("too many input bytes\n");
		switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
		return;
	}

	memcpy(ns->buf.byte + ns->regs.count, buf, len);
	ns->regs.count += len;

	if (ns->regs.count == ns->regs.num) {
		pr_debug("%d bytes were written\n", ns->regs.count);
	}
}

static void ns_nand_read_buf(struct mtd_info *mtd, u_char *buf, int len)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim *ns = nand_get_controller_data(chip);

	/* Sanity and correctness checks */
	if (!ns->lines.ce) {
		pr_err("chip is disabled\n");
		return;
	}
	if (ns->lines.ale || ns->lines.cle) {
		pr_err("ALE or CLE pin is high\n");
		return;
	}
	if (!(ns->state & STATE_DATAOUT_MASK)) {
		pr_warn("unexpected data output cycle, current state is %s\n",
			get_state_name(ns->state));
		return;
	}

	if (NS_STATE(ns->state) != STATE_DATAOUT) {
		int i;

		for (i = 0; i < len; i++)
			buf[i] = mtd_to_nand(mtd)->read_byte(mtd);

		return;
	}

	/* Check if these are expected bytes */
	if (ns->regs.count + len > ns->regs.num) {
		pr_err("too many bytes to read\n");
		switch_to_ready_state(ns, NS_STATUS_FAILED(ns));
		return;
	}

	memcpy(buf, ns->buf.byte + ns->regs.count, len);
	ns->regs.count += len;

	if (ns->regs.count == ns->regs.num) {
		if (NS_STATE(ns->nxstate) == STATE_READY)
			switch_state(ns);
	}

	return;
}

static int ns_ctrl_new_instance(struct ns_new_instance_req *req)
{
	struct mtd_info *nsmtd;
	struct nand_chip *chip;
	struct nandsim *ns;
	struct nandsim_params *nsparam = kzalloc(sizeof(*nsparam), GFP_KERNEL);

	if (!nsparam)
		return -ENOMEM;

	memcpy(nsparam->id_bytes, req->id_bytes, sizeof(nsparam->id_bytes));
	nsparam->bus_width = req->bus_width;
	nsparam->file_fd = req->file_fd;
	nsparam->no_oob = !!req->no_oob;

	switch (req->backend) {
		case NANDSIM_BACKEND_RAM:
			nsparam->bops = &ns_ram_bops;
		break;

		case NANDSIM_BACKEND_FILE:
			nsparam->bops = &ns_file_bops;
		break;

		case NANDSIM_BACKEND_CACHEFILE:
			nsparam->bops = &ns_cachefile_bops;
		break;

		default:
			kfree(nsparam);
			return -EINVAL;
	}

	nsmtd = ns_new_instance(nsparam);
	kfree(nsparam);

	if (IS_ERR(nsmtd))
		return PTR_ERR(nsmtd);

	chip = mtd_to_nand(nsmtd);
	ns = nand_get_controller_data(chip);

	return ns->index;
}

static int ns_ctrl_destroy_instance(struct ns_destroy_instance_req *req)
{
	struct mtd_info *nsmtd;
	int id = req->id, ret = 0;
	struct nand_chip *chip;
	struct nandsim *ns;

	if (id < 0 || id >= NS_MAX_DEVICES)
		return -EINVAL;

	mutex_lock(&ns_mtd_mutex);
	nsmtd = ns_mtds[id];
	if (nsmtd) {
		chip = mtd_to_nand(nsmtd);
		ns = nand_get_controller_data(chip);
		spin_lock(&ns->refcnt_lock);
		if (ns->refcnt > 0) {
			ret = -EBUSY;
			spin_unlock(&ns->refcnt_lock);
			goto out;
		}
		ns->destroying = true;
		spin_unlock(&ns->refcnt_lock);
		ret = ns_destroy_instance(nsmtd);
		if (ret)
			goto out;
		ns_mtds[id] = NULL;
	}

out:
	mutex_unlock(&ns_mtd_mutex);

	return ret;
}

static long ns_ctrl_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	if (!capable(CAP_SYS_RESOURCE))
		return -EPERM;

	return -ENOTTY;
}

#ifdef CONFIG_COMPAT
static long ns_ctrl_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	unsigned long translated_arg = (unsigned long)compat_ptr(arg);

	return ns_ctrl_ioctl(file, cmd, translated_arg);
}
#else
#define ns_ctrl_compat_ioctl NULL
#endif

static const struct file_operations nansim_ctrl_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = ns_ctrl_ioctl,
	.compat_ioctl   = ns_ctrl_compat_ioctl,
	.llseek         = no_llseek,
};

static struct miscdevice nandsim_ctrl_cdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "nandsim_ctrl",
	.fops = &nansim_ctrl_fops,
};

static void ns_put_device(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim *ns = nand_get_controller_data(chip);

	spin_lock(&ns->refcnt_lock);
	ns->refcnt -= 1;
	spin_unlock(&ns->refcnt_lock);
}

static int ns_get_device(struct mtd_info *mtd)
{
	int ret = 0;
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nandsim *ns = nand_get_controller_data(chip);

	spin_lock(&ns->refcnt_lock);
	if (ns->destroying)
		ret = -EBUSY;
	else
		ns->refcnt += 1;
	spin_unlock(&ns->refcnt_lock);

	return ret;
}

struct mtd_info *ns_new_instance(struct nandsim_params *nsparam)
{
	struct nand_chip *chip;
	struct nandsim *nand;
	struct mtd_info *nsmtd;
	int retval = -ENOMEM, i;
	unsigned char *id_bytes = nsparam->id_bytes;

	if (nsparam->bus_width != 8 && nsparam->bus_width != 16) {
		pr_err("wrong bus width (%d), use only 8 or 16\n", nsparam->bus_width);
		return ERR_PTR(-EINVAL);
	}

	/* Allocate and initialize mtd_info, nand_chip and nandsim structures */
	chip = kzalloc(sizeof(struct nand_chip) + sizeof(struct nandsim),
		       GFP_KERNEL);
	if (!chip) {
		pr_err("unable to allocate core structures.\n");
		return ERR_PTR(-ENOMEM);
	}

	mutex_lock(&ns_mtd_mutex);
	for (i = 0; i < NS_MAX_DEVICES; i++) {
		if (!ns_mtds[i])
			break;
	}

	if (i == NS_MAX_DEVICES) {
		pr_err("Cannot allocate more than %i instances!\n", NS_MAX_DEVICES);
		retval = -ENFILE;
		mutex_unlock(&ns_mtd_mutex);
		goto error;
	}

	nsmtd = ns_mtds[i] = nand_to_mtd(chip);
	nand = chip_to_ns(chip);
	nand_set_controller_data(chip, (void *)nand);
	nand->index = i;
	mutex_unlock(&ns_mtd_mutex);

	INIT_LIST_HEAD(&nand->weak_blocks);
	INIT_LIST_HEAD(&nand->grave_pages);
	INIT_LIST_HEAD(&nand->weak_pages);
	INIT_LIST_HEAD(&nand->weak_blocks);
	spin_lock_init(&nand->refcnt_lock);

	/*
	 * Register simulator's callbacks.
	 */
	chip->cmd_ctrl	 = ns_hwcontrol;
	chip->read_byte  = ns_nand_read_byte;
	chip->dev_ready  = ns_device_ready;
	chip->write_buf  = ns_nand_write_buf;
	chip->read_buf   = ns_nand_read_buf;
	chip->read_word  = ns_nand_read_word;
	/* The NAND_SKIP_BBTSCAN option is necessary for 'overridesize' */
	/* and 'badblocks' parameters to work */
	chip->options   |= NAND_SKIP_BBTSCAN;

	switch (nsparam->bbt) {
	case 2:
		 chip->bbt_options |= NAND_BBT_NO_OOB;
	case 1:
		 chip->bbt_options |= NAND_BBT_USE_FLASH;
	case 0:
		break;
	default:
		pr_err("bbt has to be 0..2\n");
		retval = -EINVAL;
		goto error;
	}
	/*
	 * Perform minimum nandsim structure initialization to handle
	 * the initial ID read command correctly
	 */
	if (id_bytes[6] != 0xFF || id_bytes[7] != 0xFF)
		nand->geom.idbytes = 8;
	else if (id_bytes[4] != 0xFF || id_bytes[5] != 0xFF)
		nand->geom.idbytes = 6;
	else if (id_bytes[2] != 0xFF || id_bytes[3] != 0xFF)
		nand->geom.idbytes = 4;
	else
		nand->geom.idbytes = 2;
	nand->regs.status = NS_STATUS_OK(nand);
	nand->nxstate = STATE_UNKNOWN;
	nand->options |= OPT_PAGE512; /* temporary value */
	memcpy(nand->ids, id_bytes, sizeof(nand->ids));
	if (nsparam->bus_width == 16) {
		nand->busw = 16;
		chip->options |= NAND_BUSWIDTH_16;
	}

	nsmtd->owner = THIS_MODULE;
	nsmtd->_get_device = ns_get_device;
	nsmtd->_put_device = ns_put_device;

	if ((retval = parse_weakblocks(nand, nsparam->weakblocks)) != 0)
		goto error;

	if ((retval = parse_weakpages(nand, nsparam->weakpages)) != 0)
		goto error;

	if ((retval = parse_gravepages(nand, nsparam->gravepages)) != 0)
		goto error;

	nand->do_delays = nsparam->do_delays;
	nand->access_delay = nsparam->access_delay;
	nand->program_delay = nsparam->program_delay;
	nand->erase_delay = nsparam->erase_delay;
	nand->output_cycle = nsparam->output_cycle;
	nand->input_cycle = nsparam->input_cycle;
	nand->bitflips = nsparam->bitflips;

	retval = nand_scan_ident(nsmtd, 1, NULL);
	if (retval) {
		pr_err("cannot scan NAND Simulator device\n");
		if (retval > 0)
			retval = -ENXIO;
		goto error;
	}

	if (nsparam->no_oob) {
		if (nsparam->bch) {
			pr_err("Cannot use ECC without OOB\n");
			retval = -EINVAL;
			goto error;
		}

		chip->ecc.mode = NAND_ECC_NONE;
		nand->no_oob = true;
	} else if (nsparam->bch) {
		unsigned int eccsteps, eccbytes;
		if (!mtd_nand_has_bch()) {
			pr_err("BCH ECC support is disabled\n");
			retval = -EINVAL;
			goto error;
		}
		/* use 512-byte ecc blocks */
		eccsteps = nsmtd->writesize/512;
		eccbytes = (nsparam->bch * 13 + 7) / 8;
		/* do not bother supporting small page devices */
		if ((nsmtd->oobsize < 64) || !eccsteps) {
			pr_err("bch not available on small page devices\n");
			retval = -EINVAL;
			goto error;
		}
		if ((eccbytes*eccsteps+2) > nsmtd->oobsize) {
			pr_err("invalid bch value %u\n", nsparam->bch);
			retval = -EINVAL;
			goto error;
		}
		chip->ecc.mode = NAND_ECC_SOFT;
		chip->ecc.algo = NAND_ECC_BCH;
		chip->ecc.size = 512;
		chip->ecc.strength = nsparam->bch;
		chip->ecc.bytes = eccbytes;
		pr_info("using %u-bit/%u bytes BCH ECC\n", nsparam->bch, chip->ecc.size);
	} else {
		chip->ecc.mode = NAND_ECC_SOFT;
		chip->ecc.algo = NAND_ECC_HAMMING;
	}

	retval = nand_scan_tail(nsmtd);
	if (retval) {
		pr_err("can't register NAND Simulator\n");
		if (retval > 0)
			retval = -ENXIO;
		goto error;
	}

	if (nsparam->overridesize) {
		uint64_t new_size = (uint64_t)nsmtd->erasesize << nsparam->overridesize;
		if (new_size >> nsparam->overridesize != nsmtd->erasesize) {
			pr_err("overridesize is too big\n");
			retval = -EINVAL;
			goto err_exit;
		}
		/* N.B. This relies on nand_scan not doing anything with the size before we change it */
		nsmtd->size = new_size;
		chip->chipsize = new_size;
		chip->chip_shift = ffs(nsmtd->erasesize) + nsparam->overridesize - 1;
		chip->pagemask = (chip->chipsize >> chip->page_shift) - 1;
	}

	if ((retval = setup_wear_reporting(nsmtd)) != 0)
		goto err_exit;

	if ((retval = nandsim_debugfs_create(nand)) != 0)
		goto err_exit;

	if ((retval = init_nandsim(nsmtd, nsparam)) != 0)
		goto err_exit;

	if ((retval = chip->scan_bbt(nsmtd)) != 0)
		goto err_exit;

	if ((retval = parse_badblocks(nand, nsmtd, nsparam->badblocks)) != 0)
		goto err_exit;

	/* Register NAND partitions */
	retval = mtd_device_register(nsmtd, &nand->partitions[0],
				     nand->nbparts);
	if (retval != 0)
		goto err_exit;

	return nsmtd;

err_exit:
	free_nandsim(nand);
	nand_release(nsmtd);
	for (i = 0;i < ARRAY_SIZE(nand->partitions); ++i)
		kfree(nand->partitions[i].name);
error:
	free_lists(nand);
	kfree(chip);

	return ERR_PTR(retval);
}
EXPORT_SYMBOL_GPL(ns_new_instance);

int ns_destroy_instance(struct mtd_info *nsmtd)
{
	int i, ret;
	struct nand_chip *chip = mtd_to_nand(nsmtd);
	struct nandsim *ns = nand_get_controller_data(chip);

	ret = mtd_device_unregister(nsmtd);
	if (ret)
		return ret;
	nand_cleanup(nsmtd);

	nandsim_debugfs_remove(ns);
	free_lists(ns);
	free_nandsim(ns);    /* Free nandsim private resources */
	nand_release(nsmtd); /* Unregister driver */
	for (i = 0;i < ARRAY_SIZE(ns->partitions); ++i)
		kfree(ns->partitions[i].name);
	kfree(mtd_to_nand(nsmtd));        /* Free other structures */

	return 0;
}
EXPORT_SYMBOL_GPL(ns_destroy_instance);

static void ns_destroy_all(void)
{
	int i;

	mutex_lock(&ns_mtd_mutex);
	for (i = 0; i < NS_MAX_DEVICES; i++)
		if (ns_mtds[i])
			WARN_ON(ns_destroy_instance(ns_mtds[i]) != 0);
	mutex_unlock(&ns_mtd_mutex);
}

static int __init ns_init_default(void)
{
	struct mtd_info *nsmtd;
	struct nandsim_params *nsparam = kzalloc(sizeof(*nsparam), GFP_KERNEL);

	if (!nsparam)
		return -ENOMEM;

	nsparam->access_delay = access_delay;
	nsparam->program_delay = programm_delay;
	nsparam->erase_delay = erase_delay;
	nsparam->output_cycle = output_cycle;
	nsparam->input_cycle = input_cycle;
	nsparam->bus_width = bus_width;
	nsparam->do_delays = do_delays;
	nsparam->parts = parts;
	nsparam->parts_num = parts_num;
	nsparam->badblocks = badblocks;
	nsparam->weakblocks = weakblocks;
	nsparam->weakpages = weakpages;
	nsparam->bitflips = bitflips;
	nsparam->gravepages = gravepages;
	nsparam->overridesize = overridesize;
	nsparam->cache_file = cache_file;
	nsparam->bbt = bbt;
	nsparam->bch = bch;
	memcpy(nsparam->id_bytes, id_bytes, sizeof(nsparam->id_bytes));

	if (!nsparam->cache_file)
		nsparam->bops = &ns_ram_bops;
	else
		nsparam->bops = &ns_cachefile_bops;

	nsmtd = ns_new_instance(nsparam);
	kfree(nsparam);

	if (IS_ERR(nsmtd))
		return PTR_ERR(nsmtd);

	return 0;
}

static int __init ns_init_module(void)
{
	int ret;

	ret = nandsim_debugfs_init();
	if (ret)
		return ret;

	if (defaults) {
		ret = ns_init_default();
		if (ret) {
			debugfs_remove_recursive(dfs_root);
			return ret;
		}
	}

	ret = misc_register(&nandsim_ctrl_cdev);
	if (ret) {
		ns_destroy_all();
		debugfs_remove_recursive(dfs_root);
	}

	return ret;
}
module_init(ns_init_module);

static void __exit ns_cleanup_module(void)
{
	misc_deregister(&nandsim_ctrl_cdev);
	ns_destroy_all();
	debugfs_remove_recursive(dfs_root);
}
module_exit(ns_cleanup_module);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Artem B. Bityuckiy");
MODULE_DESCRIPTION ("The NAND flash simulator");
