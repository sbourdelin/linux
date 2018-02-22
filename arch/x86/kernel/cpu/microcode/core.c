/*
 * CPU Microcode Update Driver for Linux
 *
 * Copyright (C) 2000-2006 Tigran Aivazian <aivazian.tigran@gmail.com>
 *	      2006	Shaohua Li <shaohua.li@intel.com>
 *	      2013-2016	Borislav Petkov <bp@alien8.de>
 *
 * X86 CPU microcode early update for Linux:
 *
 *	Copyright (C) 2012 Fenghua Yu <fenghua.yu@intel.com>
 *			   H Peter Anvin" <hpa@zytor.com>
 *		  (C) 2015 Borislav Petkov <bp@alien8.de>
 *
 * This driver allows to upgrade microcode on x86 processors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt) "microcode: " fmt

#include <linux/platform_device.h>
#include <linux/syscore_ops.h>
#include <linux/miscdevice.h>
#include <linux/capability.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/stop_machine.h>
#include <linux/delay.h>
#include <linux/topology.h>

#include <asm/microcode_intel.h>
#include <asm/cpu_device_id.h>
#include <asm/microcode_amd.h>
#include <asm/perf_event.h>
#include <asm/microcode.h>
#include <asm/processor.h>
#include <asm/cmdline.h>
#include <asm/setup.h>

#define DRIVER_VERSION	"2.2"

static struct microcode_ops	*microcode_ops;
static bool dis_ucode_ldr = true;

bool initrd_gone;

LIST_HEAD(microcode_cache);

/*
 * Synchronization.
 *
 * All non cpu-hotplug-callback call sites use:
 *
 * - microcode_mutex to synchronize with each other;
 * - get/put_online_cpus() to synchronize with
 *   the cpu-hotplug-callback call sites.
 *
 * We guarantee that only a single cpu is being
 * updated at any particular moment of time.
 */
static DEFINE_MUTEX(microcode_mutex);

struct ucode_cpu_info		ucode_cpu_info[NR_CPUS];

struct cpu_info_ctx {
	struct cpu_signature	*cpu_sig;
	int			err;
};

/*
 * Those patch levels cannot be updated to newer ones and thus should be final.
 */
static u32 final_levels[] = {
	0x01000098,
	0x0100009f,
	0x010000af,
	0, /* T-101 terminator */
};

/*
 * Check the current patch level on this CPU.
 *
 * Returns:
 *  - true: if update should stop
 *  - false: otherwise
 */
static bool amd_check_current_patch_level(void)
{
	u32 lvl, dummy, i;
	u32 *levels;

	native_rdmsr(MSR_AMD64_PATCH_LEVEL, lvl, dummy);

	if (IS_ENABLED(CONFIG_X86_32))
		levels = (u32 *)__pa_nodebug(&final_levels);
	else
		levels = final_levels;

	for (i = 0; levels[i]; i++) {
		if (lvl == levels[i])
			return true;
	}
	return false;
}

static bool __init check_loader_disabled_bsp(void)
{
	static const char *__dis_opt_str = "dis_ucode_ldr";

#ifdef CONFIG_X86_32
	const char *cmdline = (const char *)__pa_nodebug(boot_command_line);
	const char *option  = (const char *)__pa_nodebug(__dis_opt_str);
	bool *res = (bool *)__pa_nodebug(&dis_ucode_ldr);

#else /* CONFIG_X86_64 */
	const char *cmdline = boot_command_line;
	const char *option  = __dis_opt_str;
	bool *res = &dis_ucode_ldr;
#endif

	/*
	 * CPUID(1).ECX[31]: reserved for hypervisor use. This is still not
	 * completely accurate as xen pv guests don't see that CPUID bit set but
	 * that's good enough as they don't land on the BSP path anyway.
	 */
	if (native_cpuid_ecx(1) & BIT(31))
		return *res;

	if (x86_cpuid_vendor() == X86_VENDOR_AMD) {
		if (amd_check_current_patch_level())
			return *res;
	}

	if (cmdline_find_option_bool(cmdline, option) <= 0)
		*res = false;

	return *res;
}

extern struct builtin_fw __start_builtin_fw[];
extern struct builtin_fw __end_builtin_fw[];

bool get_builtin_firmware(struct cpio_data *cd, const char *name)
{
#ifdef CONFIG_FW_LOADER
	struct builtin_fw *b_fw;

	for (b_fw = __start_builtin_fw; b_fw != __end_builtin_fw; b_fw++) {
		if (!strcmp(name, b_fw->name)) {
			cd->size = b_fw->size;
			cd->data = b_fw->data;
			return true;
		}
	}
#endif
	return false;
}

void __init load_ucode_bsp(void)
{
	unsigned int cpuid_1_eax;
	bool intel = true;

	if (!have_cpuid_p())
		return;

	cpuid_1_eax = native_cpuid_eax(1);

	switch (x86_cpuid_vendor()) {
	case X86_VENDOR_INTEL:
		if (x86_family(cpuid_1_eax) < 6)
			return;
		break;

	case X86_VENDOR_AMD:
		if (x86_family(cpuid_1_eax) < 0x10)
			return;
		intel = false;
		break;

	default:
		return;
	}

	if (check_loader_disabled_bsp())
		return;

	if (intel)
		load_ucode_intel_bsp();
	else
		load_ucode_amd_bsp(cpuid_1_eax);
}

static bool check_loader_disabled_ap(void)
{
#ifdef CONFIG_X86_32
	return *((bool *)__pa_nodebug(&dis_ucode_ldr));
#else
	return dis_ucode_ldr;
#endif
}

void load_ucode_ap(void)
{
	unsigned int cpuid_1_eax;

	if (check_loader_disabled_ap())
		return;

	cpuid_1_eax = native_cpuid_eax(1);

	switch (x86_cpuid_vendor()) {
	case X86_VENDOR_INTEL:
		if (x86_family(cpuid_1_eax) >= 6)
			load_ucode_intel_ap();
		break;
	case X86_VENDOR_AMD:
		if (x86_family(cpuid_1_eax) >= 0x10)
			load_ucode_amd_ap(cpuid_1_eax);
		break;
	default:
		break;
	}
}

static int __init save_microcode_in_initrd(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	int ret = -EINVAL;

	switch (c->x86_vendor) {
	case X86_VENDOR_INTEL:
		if (c->x86 >= 6)
			ret = save_microcode_in_initrd_intel();
		break;
	case X86_VENDOR_AMD:
		if (c->x86 >= 0x10)
			ret = save_microcode_in_initrd_amd(cpuid_eax(1));
		break;
	default:
		break;
	}

	initrd_gone = true;

	return ret;
}

struct cpio_data find_microcode_in_initrd(const char *path, bool use_pa)
{
#ifdef CONFIG_BLK_DEV_INITRD
	unsigned long start = 0;
	size_t size;

#ifdef CONFIG_X86_32
	struct boot_params *params;

	if (use_pa)
		params = (struct boot_params *)__pa_nodebug(&boot_params);
	else
		params = &boot_params;

	size = params->hdr.ramdisk_size;

	/*
	 * Set start only if we have an initrd image. We cannot use initrd_start
	 * because it is not set that early yet.
	 */
	if (size)
		start = params->hdr.ramdisk_image;

# else /* CONFIG_X86_64 */
	size  = (unsigned long)boot_params.ext_ramdisk_size << 32;
	size |= boot_params.hdr.ramdisk_size;

	if (size) {
		start  = (unsigned long)boot_params.ext_ramdisk_image << 32;
		start |= boot_params.hdr.ramdisk_image;

		start += PAGE_OFFSET;
	}
# endif

	/*
	 * Fixup the start address: after reserve_initrd() runs, initrd_start
	 * has the virtual address of the beginning of the initrd. It also
	 * possibly relocates the ramdisk. In either case, initrd_start contains
	 * the updated address so use that instead.
	 *
	 * initrd_gone is for the hotplug case where we've thrown out initrd
	 * already.
	 */
	if (!use_pa) {
		if (initrd_gone)
			return (struct cpio_data){ NULL, 0, "" };
		if (initrd_start)
			start = initrd_start;
	} else {
		/*
		 * The picture with physical addresses is a bit different: we
		 * need to get the *physical* address to which the ramdisk was
		 * relocated, i.e., relocated_ramdisk (not initrd_start) and
		 * since we're running from physical addresses, we need to access
		 * relocated_ramdisk through its *physical* address too.
		 */
		u64 *rr = (u64 *)__pa_nodebug(&relocated_ramdisk);
		if (*rr)
			start = *rr;
	}

	return find_cpio_data(path, (void *)start, size, NULL);
#else /* !CONFIG_BLK_DEV_INITRD */
	return (struct cpio_data){ NULL, 0, "" };
#endif
}

void reload_early_microcode(void)
{
	int vendor, family;

	vendor = x86_cpuid_vendor();
	family = x86_cpuid_family();

	switch (vendor) {
	case X86_VENDOR_INTEL:
		if (family >= 6)
			reload_ucode_intel();
		break;
	case X86_VENDOR_AMD:
		if (family >= 0x10)
			reload_ucode_amd();
		break;
	default:
		break;
	}
}

static void collect_cpu_info_local(void *arg)
{
	struct cpu_info_ctx *ctx = arg;

	ctx->err = microcode_ops->collect_cpu_info(smp_processor_id(),
						   ctx->cpu_sig);
}

static int collect_cpu_info_on_target(int cpu, struct cpu_signature *cpu_sig)
{
	struct cpu_info_ctx ctx = { .cpu_sig = cpu_sig, .err = 0 };
	int ret;

	ret = smp_call_function_single(cpu, collect_cpu_info_local, &ctx, 1);
	if (!ret)
		ret = ctx.err;

	return ret;
}

static int collect_cpu_info(int cpu)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;
	int ret;

	memset(uci, 0, sizeof(*uci));

	ret = collect_cpu_info_on_target(cpu, &uci->cpu_sig);
	if (!ret)
		uci->valid = 1;

	return ret;
}

struct apply_microcode_ctx {
	enum ucode_state err;
};

static void apply_microcode_local(void *arg)
{
	struct apply_microcode_ctx *ctx = arg;

	ctx->err = microcode_ops->apply_microcode(smp_processor_id());
}

static int apply_microcode_on_target(int cpu)
{
	struct apply_microcode_ctx ctx = { .err = 0 };
	int ret;

	ret = smp_call_function_single(cpu, apply_microcode_local, &ctx, 1);
	if (!ret)
		ret = ctx.err;

	return ret;
}

#ifdef CONFIG_MICROCODE_OLD_INTERFACE
static int do_microcode_update(const void __user *buf, size_t size)
{
	int error = 0;
	int cpu;

	for_each_online_cpu(cpu) {
		struct ucode_cpu_info *uci = ucode_cpu_info + cpu;
		enum ucode_state ustate;

		if (!uci->valid)
			continue;

		ustate = microcode_ops->request_microcode_user(cpu, buf, size);
		if (ustate == UCODE_ERROR) {
			error = -1;
			break;
		} else if (ustate == UCODE_OK)
			apply_microcode_on_target(cpu);
	}

	return error;
}

static int microcode_open(struct inode *inode, struct file *file)
{
	return capable(CAP_SYS_RAWIO) ? nonseekable_open(inode, file) : -EPERM;
}

static ssize_t microcode_write(struct file *file, const char __user *buf,
			       size_t len, loff_t *ppos)
{
	ssize_t ret = -EINVAL;

	if ((len >> PAGE_SHIFT) > totalram_pages) {
		pr_err("too much data (max %ld pages)\n", totalram_pages);
		return ret;
	}

	get_online_cpus();
	mutex_lock(&microcode_mutex);

	if (do_microcode_update(buf, len) == 0)
		ret = (ssize_t)len;

	if (ret > 0)
		perf_check_microcode();

	mutex_unlock(&microcode_mutex);
	put_online_cpus();

	return ret;
}

static const struct file_operations microcode_fops = {
	.owner			= THIS_MODULE,
	.write			= microcode_write,
	.open			= microcode_open,
	.llseek		= no_llseek,
};

static struct miscdevice microcode_dev = {
	.minor			= MICROCODE_MINOR,
	.name			= "microcode",
	.nodename		= "cpu/microcode",
	.fops			= &microcode_fops,
};

static int __init microcode_dev_init(void)
{
	int error;

	error = misc_register(&microcode_dev);
	if (error) {
		pr_err("can't misc_register on minor=%d\n", MICROCODE_MINOR);
		return error;
	}

	return 0;
}

static void __exit microcode_dev_exit(void)
{
	misc_deregister(&microcode_dev);
}
#else
#define microcode_dev_init()	0
#define microcode_dev_exit()	do { } while (0)
#endif

/* fake device for request_firmware */
static struct platform_device	*microcode_pdev;

static struct ucode_update_param {
	spinlock_t ucode_lock;	/* Serialize microcode updates */
	atomic_t   count;  	/* # of CPUs that attempt to load ucode */
	atomic_t   errors; 	/* # of CPUs ucode load failed */
	atomic_t   enter;  	/* ucode rendezvous count */
	int	   timeout;	/* Timeout to wait for all cpus to enter */
} uc_data;

static int do_ucode_update(int cpu, struct ucode_update_param *ucd)
{
	enum ucode_state retval = 0;

	spin_lock(&ucd->ucode_lock);
	retval = microcode_ops->apply_microcode(cpu);
	spin_unlock(&ucd->ucode_lock);
	if (retval > UCODE_NFOUND) {
		atomic_inc(&ucd->errors);
		pr_warn("microcode update to CPU %d failed\n", cpu);
	}
	atomic_inc(&ucd->count);

	return retval;
}

/*
 * Wait for upto 1sec for all CPUs
 * to show up in the rendezvous function
 */
#define MAX_UCODE_RENDEZVOUS	1000000000 /* nanosec */
#define SPINUNIT		100	   /* 100ns */

/*
 * Each cpu waits for 1sec max.
 */
static int microcode_wait_timedout(int *time_out, void *data)
{
	struct ucode_update_param *ucd = data;
	if (*time_out < SPINUNIT) {
		pr_err("Not all CPUs entered ucode update handler %d CPUs missed rendezvous\n",
			(num_online_cpus() - atomic_read(&ucd->enter)));
		return 1;
	}
	*time_out -= SPINUNIT;
	touch_nmi_watchdog();
	return 0;
}

/*
 * All cpus enter here before a ucode load upto 1 sec.
 * If not all cpus showed up, we abort the ucode update
 * and return. ucode update is serialized with the spinlock
 */
static int microcode_load_rendezvous(void *data)
{
	int cpu = smp_processor_id();
	struct ucode_update_param *ucd = data;
	int timeout = MAX_UCODE_RENDEZVOUS;
	int total_cpus = num_online_cpus();

	/*
	 * Wait for all cpu's to arrive
	 */
	atomic_dec(&ucd->enter);
	while(atomic_read(&ucd->enter)) {
		if (microcode_wait_timedout(&timeout, ucd))
			return 1;
		ndelay(SPINUNIT);
	}

	do_ucode_update(cpu, ucd);

	/*
	 * Wait for all cpu's to complete
	 * ucode update
	 */
	while (atomic_read(&ucd->count) != total_cpus)
		cpu_relax();
	return 0;
}

/*
 * If any of the cpu's present are offline, we avoid loading microcode
 * to the rest of the system. This is simply to avoid having some CPUs with older
 * microcode. In theory we would update for the upcoming CPU during early_load.
 * but we want to be *PARANOID* and avoid such situations.
 * What if some CPUs are offlined and with older microcode. There are two 
 * senarios: 
 *    1: Both CPUs are of a core are offline: We skip the update now, but
 *       If they are onlined later, we will do an update then. Online operations
 *       are serialized, so we will update one thread when the other is still
 *       offline. When the second CPU is onlined, it already has the updated
 *       microcode, since its sibling was updated earlier.
 *    2: One CPU of the core is offline when we did the update. This is not
 *       a problem, since when we online the sibling it already has a copy 
 *       since its sibling was already updated.
 */
static int check_online_cpus(void)
{
	if (cpumask_equal(cpu_online_mask, cpu_present_mask))
		return 0;
	pr_err("Not all CPUs online, please online all CPUs before reloading microcode\n");
	return 1;
}

/*
 * when loading microcode, its important for the HT sibling to be
 * idle, otherwise there can be some bad interaction between the sibling
 * executing code and the microcode update process on its thread sibling.
 * To make this less * complicated we simply park all the cpus with a
 * stop_machine(). This * gaurantees that all cpus are spinning with
 * interrupts disabled.
 * - Wait for all cpus to arrive.
 * - serialize ucode updates, to avoid 2 cpus from updating ucode at the
 *   same time. This is required since microcode/intel.c also has some static
 *   variables that need protection otherwise. This serves both purposes.
 * - After updates are completed, wait for all CPUs to complete before
 *   returing back to reload_store.
 */
static int perform_microcode_reload(struct ucode_update_param *ucd)
{
	int ret;

	memset(ucd, 0, sizeof(struct ucode_update_param));
	spin_lock_init(&ucd->ucode_lock);
	atomic_set(&ucd->enter, num_online_cpus());
	/*
	 * Wait for a 1 sec
	 */
	ucd->timeout = USEC_PER_SEC;
	ret = stop_machine(microcode_load_rendezvous, ucd, cpu_online_mask);

	pr_debug("Total CPUS = %d unable to load on %d CPUs\n",
		atomic_read(&ucd->count), atomic_read(&ucd->errors));

	if (!(ret || atomic_read(&ucd->errors)))
		return 0;

	pr_warn("Update failed for %d CPUs\n", atomic_read(&ucd->errors));
	return 1;
}

/*
 * loads microcode files for all cpus.
 * TBD: We load for each cpu which is useful
 * if we support hetero cores. We really don't yet
 * support hetero, so we could optimize this in future to load
 * just for 1 cpu and reuse the same image for other cpus.
 */
static int reload_microcode_files(void)
{
	int cpu, ret = 0;

	/*
	 * First load the microcode file for all cpu's
	 */
	for_each_online_cpu(cpu) {
		ret = microcode_ops->request_microcode_fw(cpu,
				&microcode_pdev->dev, true);
		if (ret > UCODE_NFOUND) {
			pr_warn("Error reloading microcode file for CPU %d\n", cpu);

			/*
			 * set retval for the first encountered reload error
			 * stop further processing of ucode loads.
			 */
			if (!ret)
				ret = -EINVAL;
			break;
		}
	}
	return ret;
}

static ssize_t reload_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t size)
{
	unsigned long val;
	ssize_t ret = 0;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	if (val != 1)
		return size;

	get_online_cpus();
	mutex_lock(&microcode_mutex);

	ret = check_online_cpus();
	if (ret)
		goto fail;

	ret = reload_microcode_files();
	if (ret)
		goto fail;

	pr_debug("Done loading microcode file for all CPUs\n");

	ret = perform_microcode_reload(&uc_data);
	if (!ret)
		microcode_check();

fail:
	mutex_unlock(&microcode_mutex);
	put_online_cpus();
	if (!ret)
		ret = size;

	return ret;
}

static ssize_t version_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + dev->id;

	return sprintf(buf, "0x%x\n", uci->cpu_sig.rev);
}

static ssize_t pf_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + dev->id;

	return sprintf(buf, "0x%x\n", uci->cpu_sig.pf);
}

static DEVICE_ATTR_WO(reload);
static DEVICE_ATTR(version, 0400, version_show, NULL);
static DEVICE_ATTR(processor_flags, 0400, pf_show, NULL);

static struct attribute *mc_default_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_processor_flags.attr,
	NULL
};

static const struct attribute_group mc_attr_group = {
	.attrs			= mc_default_attrs,
	.name			= "microcode",
};

static void microcode_fini_cpu(int cpu)
{
	if (microcode_ops->microcode_fini_cpu)
		microcode_ops->microcode_fini_cpu(cpu);
}

static enum ucode_state microcode_resume_cpu(int cpu)
{
	if (apply_microcode_on_target(cpu))
		return UCODE_ERROR;

	pr_debug("CPU%d updated upon resume\n", cpu);

	return UCODE_OK;
}

static enum ucode_state microcode_init_cpu(int cpu, bool refresh_fw)
{
	enum ucode_state ustate;
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;

	if (uci->valid)
		return UCODE_OK;

	if (collect_cpu_info(cpu))
		return UCODE_ERROR;

	/* --dimm. Trigger a delayed update? */
	if (system_state != SYSTEM_RUNNING)
		return UCODE_NFOUND;

	ustate = microcode_ops->request_microcode_fw(cpu, &microcode_pdev->dev,
						     refresh_fw);

	if (ustate == UCODE_OK) {
		pr_debug("CPU%d updated upon init\n", cpu);
		apply_microcode_on_target(cpu);
	}

	return ustate;
}

static enum ucode_state microcode_update_cpu(int cpu)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;

	/* Refresh CPU microcode revision after resume. */
	collect_cpu_info(cpu);

	if (uci->valid)
		return microcode_resume_cpu(cpu);

	return microcode_init_cpu(cpu, false);
}

static int mc_device_add(struct device *dev, struct subsys_interface *sif)
{
	int err, cpu = dev->id;

	if (!cpu_online(cpu))
		return 0;

	pr_debug("CPU%d added\n", cpu);

	err = sysfs_create_group(&dev->kobj, &mc_attr_group);
	if (err)
		return err;

	if (microcode_init_cpu(cpu, true) == UCODE_ERROR)
		return -EINVAL;

	return err;
}

static void mc_device_remove(struct device *dev, struct subsys_interface *sif)
{
	int cpu = dev->id;

	if (!cpu_online(cpu))
		return;

	pr_debug("CPU%d removed\n", cpu);
	microcode_fini_cpu(cpu);
	sysfs_remove_group(&dev->kobj, &mc_attr_group);
}

static struct subsys_interface mc_cpu_interface = {
	.name			= "microcode",
	.subsys			= &cpu_subsys,
	.add_dev		= mc_device_add,
	.remove_dev		= mc_device_remove,
};

/**
 * mc_bp_resume - Update boot CPU microcode during resume.
 */
static void mc_bp_resume(void)
{
	int cpu = smp_processor_id();
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;

	if (uci->valid && uci->mc)
		microcode_ops->apply_microcode(cpu);
	else if (!uci->mc)
		reload_early_microcode();
}

static struct syscore_ops mc_syscore_ops = {
	.resume			= mc_bp_resume,
};

static int mc_cpu_online(unsigned int cpu)
{
	struct device *dev;

	dev = get_cpu_device(cpu);
	microcode_update_cpu(cpu);
	pr_debug("CPU%d added\n", cpu);

	if (sysfs_create_group(&dev->kobj, &mc_attr_group))
		pr_err("Failed to create group for CPU%d\n", cpu);
	return 0;
}

static int mc_cpu_down_prep(unsigned int cpu)
{
	struct device *dev;

	dev = get_cpu_device(cpu);
	/* Suspend is in progress, only remove the interface */
	sysfs_remove_group(&dev->kobj, &mc_attr_group);
	pr_debug("CPU%d removed\n", cpu);

	return 0;
}

static struct attribute *cpu_root_microcode_attrs[] = {
	&dev_attr_reload.attr,
	NULL
};

static const struct attribute_group cpu_root_microcode_group = {
	.name  = "microcode",
	.attrs = cpu_root_microcode_attrs,
};

int __init microcode_init(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	int error;

	if (dis_ucode_ldr)
		return -EINVAL;

	if (c->x86_vendor == X86_VENDOR_INTEL)
		microcode_ops = init_intel_microcode();
	else if (c->x86_vendor == X86_VENDOR_AMD)
		microcode_ops = init_amd_microcode();
	else
		pr_err("no support for this CPU vendor\n");

	if (!microcode_ops)
		return -ENODEV;

	microcode_pdev = platform_device_register_simple("microcode", -1,
							 NULL, 0);
	if (IS_ERR(microcode_pdev))
		return PTR_ERR(microcode_pdev);

	get_online_cpus();
	mutex_lock(&microcode_mutex);

	error = subsys_interface_register(&mc_cpu_interface);
	if (!error)
		perf_check_microcode();
	mutex_unlock(&microcode_mutex);
	put_online_cpus();

	if (error)
		goto out_pdev;

	error = sysfs_create_group(&cpu_subsys.dev_root->kobj,
				   &cpu_root_microcode_group);

	if (error) {
		pr_err("Error creating microcode group!\n");
		goto out_driver;
	}

	error = microcode_dev_init();
	if (error)
		goto out_ucode_group;

	register_syscore_ops(&mc_syscore_ops);
	cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "x86/microcode:online",
				  mc_cpu_online, mc_cpu_down_prep);

	pr_info("Microcode Update Driver: v%s.", DRIVER_VERSION);

	return 0;

 out_ucode_group:
	sysfs_remove_group(&cpu_subsys.dev_root->kobj,
			   &cpu_root_microcode_group);

 out_driver:
	get_online_cpus();
	mutex_lock(&microcode_mutex);

	subsys_interface_unregister(&mc_cpu_interface);

	mutex_unlock(&microcode_mutex);
	put_online_cpus();

 out_pdev:
	platform_device_unregister(microcode_pdev);
	return error;

}
fs_initcall(save_microcode_in_initrd);
late_initcall(microcode_init);
