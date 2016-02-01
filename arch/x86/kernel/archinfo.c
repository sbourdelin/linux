#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/cpu.h>

#include <asm/desc.h>

static const char * const system_desc_types[] = {
	[0] = "Reserved (illegal)",
	[1] = "Available 16-bit TSS",
	[2] = "LDT",
	[3] = "Busy 16-bit TSS",
	[4] = "16-bit Call Gate",
	[5] = "Task Gate",
	[6] = "16-bit Interrupt Gate",
	[7] = "16-bit Trap Gate",
	[8] = "Reserved (illegal)",
	[9] = "Available 32-bit TSS",
	[10] = "Reserved (illegal)",
	[11] = "Busy 32-bit TSS",
	[12] = "32-bit Call Gate",
	[13] = "Reserved (illegal)",
	[14] = "32-bit Interrupt Gate",
	[15] = "32-bit Trap Gate",
};

static const char * const user_desc_types[] = {
	[0] = "Read-Only",
	[1] = "Read-only - Accessed",
	[2] = "Read/Write",
	[3] = "Read/Write - Accessed",
	[4] = "Expand-down, Read-Only",
	[5] = "Expand-down, Read-Only - Accessed",
	[6] = "Expand-down, Read-Write",
	[7] = "Expand-down, Read-Write - Accessed",
	[8] = "Execute-Only",
	[9] = "Execute-Only - Accessed",
	[10] = "Execute/Readable",
	[11] = "Execute/Readable - Accessed",
	[12] = "Conforming, Execute-Only",
	[13] = "Conforming, Execute-Only - Accessed",
	[14] = "Conforming, Execute/Readable",
	[15] = "Conforming, Execute/Readable - Accessed",
};

static void print_seg_desc(struct seq_file *m, struct desc_struct *d, int num)
{
	seq_printf(m, "%02d:\n", num);
	seq_printf(m, "[ base[31:24]:%02x G:%x D:%x L:%x AVL:%x lim[19:16]:%x |",
		   d->base2, d->g, d->d, d->l, d->avl, d->limit);
	seq_printf(m, " P:%x DPL:%x S:%x C:%x base[23:16]:%02x ]\n",
		   d->p, d->dpl, d->s, !!(d->type & BIT(2)), d->base1);
	seq_printf(m, "[ base[15:00]:%04x | lim[15:00]:%04x ]: ",
		   d->base0, d->limit0);

	if (d->s)
		seq_printf(m, "User: (0x%x) %s, %s\n",
			    d->type,
			   (d->type > 7 ? "Code" : "Data"),
			   (user_desc_types[d->type]));
	else
		seq_printf(m, "System: (0x%x) %s\n", d->type, system_desc_types[d->type]);

	seq_printf(m, "\n");
}

static void dump_gdt(void *info)
{
	struct gdt_page *g = this_cpu_ptr(&gdt_page);
	struct seq_file *m = (struct seq_file *)info;
	int i;

	seq_printf(m, "CPU%d, GDT %p:\n", smp_processor_id(), &g->gdt);

	for (i = 0; i < GDT_ENTRIES; i++)
		print_seg_desc(m, &g->gdt[i], i);

	seq_printf(m, "----\n");

}

static int archinfo_show(struct seq_file *m, void *v)
{
	int c;

	/*
	 * Using on_each_cpu() here fudges the output and we want it nicely
	 * sorted by CPU.
	 */
	get_online_cpus();
		for_each_online_cpu(c)
			smp_call_function_single(c, dump_gdt, m, 1);
	put_online_cpus();

	seq_printf(m,
		   "\nInfo:\n"
		   "base,limit,A,G,R: ignored in 64-bit mode.\n"
		   "G: granularity bit (23):\n"
			"\t- 0b: segment limit is not scaled.\n"
			"\t- 1b: segment limit scaled by 4K.\n"
		   "D/B: CS default operand size bit (22):\n"
			"\t- 0b: 16-bit.\n"
			"\t- 1b: 32-bit.\n"
			"\tD=0b is the only allowed setting in long mode (L=1b).\n"
			"\tCalled B in stack segments.\n"
		   "L: long mode bit (21):\n"
			"\t- 0b: CPU in compat mode. Enables segmentation.\n"
			"\t- 1b: CPU in long mode.\n"
		   "AVL: bit available to software (20).\n"
		   "P: present bit (15):\n"
			"\t- 0b: seg. not present in mem => #NP.\n"
			"\t- 1b: seg is present in memory.\n"
		   "DPL: Descriptor Privilege Level [14:13]:\n"
			"\t- 0b: highest privilege level.\n"
			"    ...\n"
			"\t- 3b: lowest privilege level.\n"
		   "S+Type: decriptor types [12,11:8]:\n"
			"\t Specify descriptor type and access characteristics.\n"
		   " S:\n"
			"\t- 0b: System descriptor.\n"
			"\t- 1b: User descriptor.\n"
		   " R: readable bit (9):\n"
			"\t- 0b: code seg is executable, reads -> #GP\n"
			"\t- 1b: code seg is both read/exec\n"
		   " A: accessed bit (8): set by CPU when desc copied into %%cs; cleared only by sw.\n"
			);

	return 0;
}

static int archinfo_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, archinfo_show, NULL);
}

static const struct file_operations archinfo_fops = {
	.owner	 = THIS_MODULE,
	.open	 = archinfo_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

static struct dentry *dfs_entry;

static int __init archinfo_init(void)
{
	dfs_entry = debugfs_create_file("archinfo", S_IRUSR,
					arch_debugfs_dir, NULL, &archinfo_fops);
	if (!dfs_entry)
		return -ENOMEM;

	return 0;
}

static void __exit archinfo_exit(void)
{
	debugfs_remove_recursive(dfs_entry);
}

module_init(archinfo_init);
module_exit(archinfo_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Borislav Petkov <bp@alien8.de>");
MODULE_DESCRIPTION("x86 arch info dumper");
