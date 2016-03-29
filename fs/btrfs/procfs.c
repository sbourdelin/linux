#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include "ctree.h"
#include "volumes.h"
#include "rcu-string.h"

#define BTRFS_PROC_PATH		"fs/btrfs"
#define BTRFS_PROC_DEVLIST	"devlist"

struct proc_dir_entry	*btrfs_proc_root;

void btrfs_print_devlist(struct seq_file *seq)
{

	/* Btrfs Procfs String Len */
#define BPSL	256
#define BTRFS_SEQ_PRINT(plist, arg)\
		snprintf(str, BPSL, plist, arg);\
		if (sprt)\
			seq_printf(seq, "\t");\
		seq_printf(seq, str)

	char str[BPSL];
	struct btrfs_device *device;
	struct btrfs_fs_devices *fs_devices;
	struct btrfs_fs_devices *cur_fs_devices;
	struct btrfs_fs_devices *sprt; //sprout fs devices
	struct list_head *fs_uuids = btrfs_get_fs_uuids();
	struct list_head *cur_uuid;

	seq_printf(seq, "\n#Its Experimental, parameters may change without notice.\n\n");

	mutex_lock(&uuid_mutex);
	/* Todo: there must be better way than nested locks */
	list_for_each(cur_uuid, fs_uuids) {
		cur_fs_devices  = list_entry(cur_uuid, struct btrfs_fs_devices, list);

		mutex_lock(&cur_fs_devices->device_list_mutex);

		fs_devices = cur_fs_devices;
		sprt = NULL;

again_fs_devs:
		if (sprt) {
			BTRFS_SEQ_PRINT("[[seed_fsid: %pU]]\n", fs_devices->fsid);
			BTRFS_SEQ_PRINT("\tsprout_fsid:\t\t%pU\n", sprt->fsid);
		} else {
			BTRFS_SEQ_PRINT("[fsid: %pU]\n", fs_devices->fsid);
		}
		if (fs_devices->seed) {
			BTRFS_SEQ_PRINT("\tseed_fsid:\t\t%pU\n", fs_devices->seed->fsid);
		}
		BTRFS_SEQ_PRINT("\tfs_devs_addr:\t\t%p\n", fs_devices);
		BTRFS_SEQ_PRINT("\tnum_devices:\t\t%llu\n", fs_devices->num_devices);
		BTRFS_SEQ_PRINT("\topen_devices:\t\t%llu\n", fs_devices->open_devices);
		BTRFS_SEQ_PRINT("\trw_devices:\t\t%llu\n", fs_devices->rw_devices);
		BTRFS_SEQ_PRINT("\tmissing_devices:\t%llu\n", fs_devices->missing_devices);
		BTRFS_SEQ_PRINT("\ttotal_rw_devices:\t%llu\n", fs_devices->total_rw_bytes);
		BTRFS_SEQ_PRINT("\ttotal_devices:\t\t%llu\n", fs_devices->total_devices);
		BTRFS_SEQ_PRINT("\topened:\t\t\t%d\n", fs_devices->opened);
		BTRFS_SEQ_PRINT("\tseeding:\t\t%d\n", fs_devices->seeding);
		BTRFS_SEQ_PRINT("\trotating:\t\t%d\n", fs_devices->rotating);
		BTRFS_SEQ_PRINT("\tspare:\t\t\t%d\n", fs_devices->spare);

		BTRFS_SEQ_PRINT("\tfsid_kobj_state:\t%d\n", fs_devices->fsid_kobj.state_initialized);
		BTRFS_SEQ_PRINT("\tfsid_kobj_insysfs:\t%d\n", fs_devices->fsid_kobj.state_in_sysfs);

		if (fs_devices->device_dir_kobj) {
		BTRFS_SEQ_PRINT("\tdevice_kobj_state:\t%d\n", fs_devices->device_dir_kobj->state_initialized);
		BTRFS_SEQ_PRINT("\tdevice_kobj_insysfs:\t%d\n", fs_devices->device_dir_kobj->state_in_sysfs);
		} else {
		BTRFS_SEQ_PRINT("\tdevice_kobj_state:\t%s\n", "null");
		BTRFS_SEQ_PRINT("\tdevice_kobj_insysfs:\t%s\n", "null");
		}

		list_for_each_entry(device, &fs_devices->devices, dev_list) {
			BTRFS_SEQ_PRINT("\t[[uuid: %pU]]\n", device->uuid);
			BTRFS_SEQ_PRINT("\t\tdev_addr:\t%p\n", device);
			rcu_read_lock();
			BTRFS_SEQ_PRINT("\t\tdevice:\t\t%s\n",
				device->name ? rcu_str_deref(device->name): "(null)");
			rcu_read_unlock();
			BTRFS_SEQ_PRINT("\t\tdevid:\t\t%llu\n", device->devid);
			if (device->dev_root) {
				BTRFS_SEQ_PRINT("\t\tdev_root_fsid:\t%pU\n",
						device->dev_root->fs_info->fsid);
			}
			BTRFS_SEQ_PRINT("\t\tgeneration:\t%llu\n", device->generation);
			BTRFS_SEQ_PRINT("\t\ttotal_bytes:\t%llu\n", device->total_bytes);
			BTRFS_SEQ_PRINT("\t\tdev_totalbytes:\t%llu\n", device->disk_total_bytes);
			BTRFS_SEQ_PRINT("\t\tbytes_used:\t%llu\n", device->bytes_used);
			BTRFS_SEQ_PRINT("\t\ttype:\t\t%llu\n", device->type);
			BTRFS_SEQ_PRINT("\t\tio_align:\t%u\n", device->io_align);
			BTRFS_SEQ_PRINT("\t\tio_width:\t%u\n", device->io_width);
			BTRFS_SEQ_PRINT("\t\tsector_size:\t%u\n", device->sector_size);
			BTRFS_SEQ_PRINT("\t\tmode:\t\t0x%llx\n", (u64)device->mode);
			BTRFS_SEQ_PRINT("\t\twriteable:\t%d\n", device->writeable);
			BTRFS_SEQ_PRINT("\t\tin_fs_metadata:\t%d\n", device->in_fs_metadata);
			BTRFS_SEQ_PRINT("\t\tmissing:\t%d\n", device->missing);
			BTRFS_SEQ_PRINT("\t\tfailed:\t\t%d\n", device->failed);
			BTRFS_SEQ_PRINT("\t\toffline:\t%d\n", device->offline);
			BTRFS_SEQ_PRINT("\t\tcan_discard:\t%d\n", device->can_discard);
			BTRFS_SEQ_PRINT("\t\treplace_tgtdev:\t%d\n",
								device->is_tgtdev_for_dev_replace);
			BTRFS_SEQ_PRINT("\t\tactive_pending:\t%d\n", device->running_pending);
			BTRFS_SEQ_PRINT("\t\tnobarriers:\t%d\n", device->nobarriers);
			BTRFS_SEQ_PRINT("\t\tdevstats_valid:\t%d\n", device->dev_stats_valid);
			BTRFS_SEQ_PRINT("\t\tbdev:\t\t%s\n", device->bdev ? "not_null":"null");
		}

		if (fs_devices->seed) {
			sprt = fs_devices;
			fs_devices = fs_devices->seed;
			goto again_fs_devs;
		}
		seq_printf(seq, "\n");

		mutex_unlock(&cur_fs_devices->device_list_mutex);
	}
	mutex_unlock(&uuid_mutex);
}
static int btrfs_devlist_show(struct seq_file *seq, void *offset)
{
	btrfs_print_devlist(seq);
	return 0;
}

static int btrfs_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, btrfs_devlist_show, PDE_DATA(inode));
}

static const struct file_operations btrfs_seq_fops = {
	.owner   = THIS_MODULE,
	.open    = btrfs_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

void btrfs_init_procfs(void)
{
	btrfs_proc_root = proc_mkdir(BTRFS_PROC_PATH, NULL);
	if (btrfs_proc_root)
		proc_create_data(BTRFS_PROC_DEVLIST, S_IRUGO, btrfs_proc_root,
					&btrfs_seq_fops, NULL);
	return;
}

void btrfs_exit_procfs(void)
{
	if (btrfs_proc_root)
		remove_proc_entry(BTRFS_PROC_DEVLIST, btrfs_proc_root);
	remove_proc_entry(BTRFS_PROC_PATH, NULL);
}

void btrfs_printk_fsdev(void)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *fs_devices;
	struct btrfs_fs_devices *cur_fs_devices;
	struct btrfs_fs_devices *sprt; //sprout fs devices
	struct list_head *fs_uuids = btrfs_get_fs_uuids();
	struct list_head *cur_uuid;

	list_for_each(cur_uuid, fs_uuids) {
		cur_fs_devices  = list_entry(cur_uuid, struct btrfs_fs_devices, list);

		fs_devices = cur_fs_devices;
		sprt = NULL;

again_fs_devs:
		if (sprt) {
			printk(KERN_INFO "[[seed_fsid: %pU]]\n", fs_devices->fsid);
			printk(KERN_INFO "\tsprout_fsid:\t\t%pU\n", sprt->fsid);
		} else {
			printk(KERN_INFO "[fsid: %pU]\n", fs_devices->fsid);
		}
		if (fs_devices->seed) {
			printk(KERN_INFO "\tseed_fsid:\t\t%pU\n", fs_devices->seed->fsid);
		}
		printk(KERN_INFO "\tfs_devs_addr:\t\t%p\n", fs_devices);
		printk(KERN_INFO "\tnum_devices:\t\t%llu\n", fs_devices->num_devices);
		printk(KERN_INFO "\topen_devices:\t\t%llu\n", fs_devices->open_devices);
		printk(KERN_INFO "\trw_devices:\t\t%llu\n", fs_devices->rw_devices);
		printk(KERN_INFO "\tmissing_devices:\t%llu\n", fs_devices->missing_devices);
		printk(KERN_INFO "\ttotal_rw_devices:\t%llu\n", fs_devices->total_rw_bytes);
		printk(KERN_INFO "\ttotal_devices:\t\t%llu\n", fs_devices->total_devices);
		printk(KERN_INFO "\topened:\t\t\t%d\n", fs_devices->opened);
		printk(KERN_INFO "\tseeding:\t\t%d\n", fs_devices->seeding);
		printk(KERN_INFO "\trotating:\t\t%d\n", fs_devices->rotating);
		printk(KERN_INFO "\tspare:\t\t\t%d\n", fs_devices->spare);

		printk(KERN_INFO "\tfsid_kobj_state:\t%d\n", fs_devices->fsid_kobj.state_initialized);
		printk(KERN_INFO "\tfsid_kobj_insysfs:\t%d\n", fs_devices->fsid_kobj.state_in_sysfs);

		if (fs_devices->device_dir_kobj) {
		printk(KERN_INFO "\tdevice_kobj_state:\t%d\n", fs_devices->device_dir_kobj->state_initialized);
		printk(KERN_INFO "\tdevice_kobj_insysfs:\t%d\n", fs_devices->device_dir_kobj->state_in_sysfs);
		} else {
		printk(KERN_INFO "\tdevice_kobj_state:\t%s\n", "null");
		printk(KERN_INFO "\tdevice_kobj_insysfs:\t%s\n", "null");
		}

		printk(KERN_INFO "\tfs_info:\t\t%pK\n", fs_devices->fs_info);

		list_for_each_entry(device, &fs_devices->devices, dev_list) {
			printk(KERN_INFO "\t[[uuid: %pU]]\n", device->uuid);
			printk(KERN_INFO "\t\tdev_addr:\t%p\n", device);
			rcu_read_lock();
			printk(KERN_INFO "\t\tdevice:\t\t%s\n",
				device->name ? rcu_str_deref(device->name): "(null)");
			rcu_read_unlock();
			printk(KERN_INFO "\t\tdevid:\t\t%llu\n", device->devid);
			if (device->dev_root) {
				printk(KERN_INFO "\t\tdev_root_fsid:\t%pU\n",
						device->dev_root->fs_info->fsid);
			}
			printk(KERN_INFO "\t\tgeneration:\t%llu\n", device->generation);
			printk(KERN_INFO "\t\ttotal_bytes:\t%llu\n", device->total_bytes);
			printk(KERN_INFO "\t\tdev_totalbytes:\t%llu\n", device->disk_total_bytes);
			printk(KERN_INFO "\t\tbytes_used:\t%llu\n", device->bytes_used);
			printk(KERN_INFO "\t\ttype:\t\t%llu\n", device->type);
			printk(KERN_INFO "\t\tio_align:\t%u\n", device->io_align);
			printk(KERN_INFO "\t\tio_width:\t%u\n", device->io_width);
			printk(KERN_INFO "\t\tsector_size:\t%u\n", device->sector_size);
			printk(KERN_INFO "\t\tmode:\t\t0x%llx\n", (u64)device->mode);
			printk(KERN_INFO "\t\twriteable:\t%d\n", device->writeable);
			printk(KERN_INFO "\t\tin_fs_metadata:\t%d\n", device->in_fs_metadata);
			printk(KERN_INFO "\t\tmissing:\t%d\n", device->missing);
			printk(KERN_INFO "\t\tfailed:\t\t%d\n", device->failed);
			printk(KERN_INFO "\t\toffline:\t%d\n", device->offline);
			printk(KERN_INFO "\t\tcan_discard:\t%d\n", device->can_discard);
			printk(KERN_INFO "\t\treplace_tgtdev:\t%d\n",
								device->is_tgtdev_for_dev_replace);
			printk(KERN_INFO "\t\tactive_pending:\t%d\n", device->running_pending);
			printk(KERN_INFO "\t\tnobarriers:\t%d\n", device->nobarriers);
			printk(KERN_INFO "\t\tdevstats_valid:\t%d\n", device->dev_stats_valid);
			printk(KERN_INFO "\t\tbdev:\t\t%s\n", device->bdev ? "not_null":"null");
		}

		if (fs_devices->seed) {
			sprt = fs_devices;
			fs_devices = fs_devices->seed;
			goto again_fs_devs;
		}
		printk(KERN_INFO "\n");
	}
}
