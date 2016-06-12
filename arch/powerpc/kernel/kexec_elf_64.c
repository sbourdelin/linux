/*
 * Load ELF vmlinux file for the kexec_file_load syscall.
 *
 * Copyright (C) 2004  Adam Litke (agl@us.ibm.com)
 * Copyright (C) 2004  IBM Corp.
 * Copyright (C) 2005  R Sharada (sharada@in.ibm.com)
 * Copyright (C) 2006  Mohan Kumar M (mohan@in.ibm.com)
 * Copyright (C) 2016  IBM Corporation
 *
 * Based on kexec-tools' kexec-elf-exec.c and kexec-elf-ppc64.c.
 * Heavily modified for the kernel by
 * Thiago Jung Bauermann <bauerman@linux.vnet.ibm.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation (version 2 of the License).
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define pr_fmt(fmt)	"kexec_elf: " fmt

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/kexec.h>
#include <linux/elf.h>
#include <linux/kexec.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <asm/elf_util.h>

extern size_t kexec_purgatory_size;

#define PURGATORY_STACK_SIZE	(16 * 1024)
#define SLAVE_CODE_SIZE		256

/**
 * build_elf_exec_info - read ELF executable and check that we can use it
 */
static int build_elf_exec_info(const char *buf, size_t len, struct elfhdr *ehdr,
			       struct elf_info *elf_info)
{
	int i;
	int ret;

	ret = elf_read_from_buffer(buf, len, ehdr, elf_info);
	if (ret)
		return ret;

	if (ehdr->e_type != ET_EXEC) {
		pr_err("Not an ELF executable.\n");
		goto error;
	} else if (!elf_info->proghdrs) {
		pr_err("No ELF program header.\n");
		goto error;
	}

	for (i = 0; i < ehdr->e_phnum; i++) {
		/*
		 * Kexec does not support loading interpreters.
		 * In addition this check keeps us from attempting
		 * to kexec ordinay executables.
		 */
		if (elf_info->proghdrs[i].p_type == PT_INTERP) {
			pr_err("Requires an ELF interpreter.\n");
			goto error;
		}
	}

	return 0;
error:
	elf_free_info(elf_info);
	return -ENOEXEC;
}

static int elf64_probe(const char *buf, unsigned long len)
{
	struct elfhdr ehdr;
	struct elf_info elf_info;
	int ret;

	ret = build_elf_exec_info(buf, len, &ehdr, &elf_info);
	if (ret)
		return ret;

	elf_free_info(&elf_info);

	return elf_check_arch(&ehdr)? 0 : -ENOEXEC;
}

static bool find_debug_console(void *fdt, int chosen_node)
{
	int len;
	int console_node;
	const void *prop, *colon;

	prop = fdt_getprop(fdt, chosen_node, "stdout-path", &len);
	if (prop == NULL) {
		if (len == -FDT_ERR_NOTFOUND) {
			prop = fdt_getprop(fdt, chosen_node, "linux,stdout-path",
					   &len);
			if (prop == NULL) {
				pr_debug("Unable to find [linux,]stdout-path.\n");
				return false;
			}
		} else {
			pr_debug("Error finding console: %s\n",
				 fdt_strerror(len));
			return false;
		}
	}

	/*
	 * stdout-path can have a ':' separating the path from device-specific
	 * information, so we should only consider what's before it.
	 */
	colon = strchr(prop, ':');
	if (colon != NULL)
		len = colon - prop;
	else
		len -= 1;	/* Ignore the terminating NUL. */

	console_node = fdt_path_offset_namelen(fdt, prop, len);
	if (console_node < 0) {
		pr_debug("Error finding console: %s\n",
			 fdt_strerror(console_node));
		return false;
	}

	if (fdt_node_check_compatible(fdt, console_node, "hvterm1") == 0)
		return true;
	else if (fdt_node_check_compatible(fdt, console_node,
					   "hvterm-protocol") == 0)
		return true;

	return false;
}

static int setup_purgatory(struct kimage *image, struct elf_info *kernel_info,
			   void *fdt, unsigned long kernel_load_addr,
			   unsigned long fdt_load_addr, unsigned long stack_top,
			   int debug)
{
	int ret, tree_node;
	const void *prop;
	unsigned long opal_base, opal_entry;
	uint64_t toc;
	unsigned int *slave_code, master_entry;
	struct elf_info purg_info;

	/* Get the slave code from the new kernel and put it in purgatory. */
	slave_code = kmalloc(SLAVE_CODE_SIZE, GFP_KERNEL);
	if (!slave_code)
		return -ENOMEM;
	ret = kexec_purgatory_get_set_symbol(image, "purgatory_start",
					     slave_code, SLAVE_CODE_SIZE, true);
	if (ret) {
		kfree(slave_code);
		return ret;
	}
	master_entry = slave_code[0];
	memcpy(slave_code,
	       kernel_info->buffer + kernel_info->proghdrs[0].p_offset,
	       SLAVE_CODE_SIZE);
	slave_code[0] = master_entry;
	ret = kexec_purgatory_get_set_symbol(image, "purgatory_start",
					     slave_code, SLAVE_CODE_SIZE,
					     false);
	kfree(slave_code);

	ret = kexec_purgatory_get_set_symbol(image, "kernel", &kernel_load_addr,
					     sizeof(kernel_load_addr), false);
	if (ret)
		return ret;
	ret = kexec_purgatory_get_set_symbol(image, "dt_offset", &fdt_load_addr,
					     sizeof(fdt_load_addr), false);
	if (ret)
		return ret;

	tree_node = fdt_path_offset(fdt, "/ibm,opal");
	if (tree_node >= 0) {
		prop = fdt_getprop(fdt, tree_node, "opal-base-address", NULL);
		if (!prop) {
			pr_err("OPAL address not found in the device tree.\n");
			return -EINVAL;
		}
		opal_base = fdt64_to_cpu((const fdt64_t *) prop);

		prop = fdt_getprop(fdt, tree_node, "opal-entry-address", NULL);
		if (!prop) {
			pr_err("OPAL address not found in the device tree.\n");
			return -EINVAL;
		}
		opal_entry = fdt64_to_cpu((const fdt64_t *) prop);

		ret = kexec_purgatory_get_set_symbol(image, "opal_base",
						     &opal_base,
						     sizeof(opal_base), false);
		if (ret)
			return ret;
		ret = kexec_purgatory_get_set_symbol(image, "opal_entry",
						     &opal_entry,
						     sizeof(opal_entry), false);
		if (ret)
			return ret;
	}

	ret = kexec_purgatory_get_set_symbol(image, "stack", &stack_top,
					     sizeof(stack_top), false);
	if (ret)
		return ret;

	elf_init_elf_info(image->purgatory_info.ehdr,
			  image->purgatory_info.sechdrs, &purg_info);
	toc = my_r2(&purg_info);
	ret = kexec_purgatory_get_set_symbol(image, "my_toc", &toc, sizeof(toc),
					     false);
	if (ret)
		return ret;
	pr_debug("Purgatory TOC is at 0x%llx\n", toc);

	ret = kexec_purgatory_get_set_symbol(image, "debug", &debug,
					     sizeof(debug), false);
	if (ret)
		return ret;
	if (!debug)
		pr_debug("Disabling purgatory output.\n");

	return 0;
}

/**
 * elf_exec_load - load ELF executable image
 * @lowest_load_addr:	On return, will be the address where the first PT_LOAD
 *			section will be loaded in memory.
 *
 * Return:
 * 0 on success, negative value on failure.
 */
static int elf_exec_load(struct kimage *image, struct elfhdr *ehdr,
			 struct elf_info *elf_info,
			 unsigned long *lowest_load_addr)
{
	unsigned long base = 0, lowest_addr = UINT_MAX;
	int ret;
	size_t i;

	/* Read in the PT_LOAD segments. */
	for(i = 0; i < ehdr->e_phnum; i++) {
		unsigned long load_addr;
		size_t size;
		const struct elf_phdr *phdr;

		phdr = &elf_info->proghdrs[i];
		if (phdr->p_type != PT_LOAD)
			continue;

		size = phdr->p_filesz;
		if (size > phdr->p_memsz)
			size = phdr->p_memsz;

		ret = kexec_add_buffer(image,
				       (char *) elf_info->buffer + phdr->p_offset,
				       size, phdr->p_memsz, phdr->p_align,
				       phdr->p_paddr + base, ppc64_rma_size,
				       false, &load_addr);
		if (ret)
			goto out;

		if (load_addr < lowest_addr)
			lowest_addr = load_addr;
	}

	/* Update entry point to reflect new load address. */
	ehdr->e_entry += base;

	*lowest_load_addr = lowest_addr;
	ret = 0;
 out:
	return ret;
}

void *elf64_load(struct kimage *image, char *kernel_buf,
		 unsigned long kernel_len, char *initrd,
		 unsigned long initrd_len, char *cmdline,
		 unsigned long cmdline_len)
{
	int i;
	int ret = 0, chosen_node;
	unsigned int fdt_size;
	unsigned long kernel_load_addr, purgatory_load_addr;
	unsigned long initrd_load_addr, fdt_load_addr, stack_top;
	uint64_t oldfdt_addr;
	void *fdt;
	const void *prop;
	struct elfhdr ehdr;
	struct elf_info elf_info;
	struct fdt_reserve_entry *rsvmap;

	ret = build_elf_exec_info(kernel_buf, kernel_len, &ehdr, &elf_info);
	if (ret)
		goto out;

	ret = elf_exec_load(image, &ehdr, &elf_info, &kernel_load_addr);
	if (ret)
		goto out;

	pr_debug("Loaded the kernel at 0x%lx\n", kernel_load_addr);

	ret = kexec_load_purgatory(image, 0, ppc64_rma_size, true,
				   &purgatory_load_addr);
	if (ret) {
		pr_err("Loading purgatory failed.\n");
		goto out;
	}

	pr_debug("Loaded purgatory at 0x%lx\n", purgatory_load_addr);

	fdt_size = fdt_totalsize(initial_boot_params) * 2;
	fdt = kmalloc(fdt_size, GFP_KERNEL);
	if (!fdt) {
		pr_err("Not enough memory for the device tree.\n");
		ret = -ENOMEM;
		goto out;
	}
	ret = fdt_open_into(initial_boot_params, fdt, fdt_size);
	if (ret < 0) {
		pr_err("Error setting up the new device tree.\n");
		ret = -EINVAL;
		goto out;
	}

	/* Remove memory reservation for the current device tree. */
	oldfdt_addr = __pa(initial_boot_params);
	for (i = 0; i < fdt_num_mem_rsv(fdt); i++) {
		uint64_t rsv_start, rsv_size;

		ret = fdt_get_mem_rsv(fdt, i, &rsv_start, &rsv_size);
		if (ret) {
			pr_err("Malformed device tree.\n");
			ret = -EINVAL;
			goto out;
		}

		if (rsv_start == oldfdt_addr &&
		    rsv_size == fdt_totalsize(initial_boot_params)) {
			ret = fdt_del_mem_rsv(fdt, i);
			if (ret) {
				pr_err("Error deleting fdt reservation.\n");
				ret = -EINVAL;
				goto out;
			}
			pr_debug("Removed old device tree reservation.\n");

			break;
		}
	}

	chosen_node = fdt_path_offset(fdt, "/chosen");
	if (chosen_node < 0) {
		pr_err("Malformed device tree: /chosen not found.\n");
		ret = -EINVAL;
		goto out;
	}

	/* Did we boot using an initrd? */
	prop = fdt_getprop(fdt, chosen_node, "linux,initrd-start", NULL);
	if (prop) {
		uint64_t tmp_start, tmp_end, tmp_size, tmp_sizepg;

		tmp_start = fdt64_to_cpu(*((const fdt64_t *) prop));

		prop = fdt_getprop(fdt, chosen_node, "linux,initrd-end", NULL);
		if (!prop) {
			pr_err("Malformed device tree.\n");
			ret = -EINVAL;
			goto out;
		}
		tmp_end = fdt64_to_cpu(*((const fdt64_t *) prop));

		/*
		 * kexec reserves exact initrd size, while firmware may
		 * reserve a multiple of PAGE_SIZE, so check for both.
		 */
		tmp_size = tmp_end - tmp_start;
		tmp_sizepg = round_up(tmp_size, PAGE_SIZE);

		/* Remove memory reservation for the current initrd. */
		for (i = 0; i < fdt_num_mem_rsv(fdt); i++) {
			uint64_t rsv_start, rsv_size;

			ret = fdt_get_mem_rsv(fdt, i, &rsv_start, &rsv_size);
			if (ret) {
				pr_err("Malformed device tree.\n");
				ret = -EINVAL;
				goto out;
			}

			if (rsv_start == tmp_start &&
			    (rsv_size == tmp_size || rsv_size == tmp_sizepg)) {
				ret = fdt_del_mem_rsv(fdt, i);
				if (ret) {
					pr_err("Error deleting fdt reservation.\n");
					ret = -EINVAL;
					goto out;
				}
				pr_debug("Removed old initrd reservation.\n");

				/* fdt was modified, offsets may have changed. */
				chosen_node = fdt_path_offset(fdt, "/chosen");
				if (chosen_node < 0) {
					pr_err("Malformed device tree.\n");
					ret = -EINVAL;
					goto out;
				}

				break;
			}
		}

		/* If there's no new initrd, delete the old initrd's info. */
		if (initrd == NULL) {
			ret = fdt_delprop(fdt, chosen_node, "linux,initrd-start");
			if (ret) {
				pr_err("Error deleting linux,initrd-start.\n");
				ret = -EINVAL;
				goto out;
			}

			ret = fdt_delprop(fdt, chosen_node, "linux,initrd-end");
			if (ret) {
				pr_err("Error deleting linux,initrd-end.\n");
				ret = -EINVAL;
				goto out;
			}
		}
	}

	if (initrd != NULL) {
		ret = kexec_add_buffer(image, initrd, initrd_len, initrd_len,
				       PAGE_SIZE, 0, ppc64_rma_size, false,
				       &initrd_load_addr);
		if (ret)
			goto out;

		pr_debug("Loaded initrd at 0x%lx\n", initrd_load_addr);

		ret = fdt_setprop_u64(fdt, chosen_node, "linux,initrd-start",
				      initrd_load_addr);
		if (ret < 0) {
			pr_err("Error setting up the new device tree.\n");
			ret = -EINVAL;
			goto out;
		}
		/* initrd-end is the first address after the initrd image. */
		ret = fdt_setprop_u64(fdt, chosen_node, "linux,initrd-end",
				      initrd_load_addr + initrd_len);
		if (ret < 0) {
			pr_err("Error setting up the new device tree.\n");
			ret = -EINVAL;
			goto out;
		}

		ret = fdt_add_mem_rsv(fdt, initrd_load_addr, initrd_len);
		if (ret) {
			pr_err("Error reserving initrd memory: %s\n",
			       fdt_strerror(ret));
			ret = -EINVAL;
			goto out;
		}
	}

	if (cmdline_len) {
		ret = fdt_setprop_string(fdt, chosen_node, "bootargs", cmdline);
		if (ret < 0) {
			pr_err("Error setting up the new device tree.\n");
			ret = -EINVAL;
			goto out;
		}
	} else {
		ret = fdt_delprop(fdt, chosen_node, "bootargs");
		if (ret && ret != -FDT_ERR_NOTFOUND) {
			pr_err("Error deleting bootargs.\n");
			ret = -EINVAL;
			goto out;
		}
	}

	ret = fdt_setprop(fdt, chosen_node, "linux,booted-from-kexec", NULL, 0);
	if (ret) {
		pr_err("Error setting up the new device tree.\n");
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Documentation/devicetree/booting-without-of.txt says we need to
	 * add a reservation entry for the device tree block, but
	 * early_init_fdt_reserve_self reserves the memory even if there's no
	 * such entry. We'll add a reservation entry anyway, to be safe and
	 * compliant.
	 *
	 * Use dummy values, we will correct them in a moment.
	 */
	ret = fdt_add_mem_rsv(fdt, 1, 1);
	if (ret) {
		pr_err("Error reserving device tree memory: %s\n",
		       fdt_strerror(ret));
		ret = -EINVAL;
		goto out;
	}
	fdt_pack(fdt);

	ret = kexec_add_buffer(image, fdt, fdt_size, fdt_size, PAGE_SIZE, 0,
			       ppc64_rma_size, true, &fdt_load_addr);
	if (ret)
		goto out;

	/*
	 * Fix fdt reservation, now that we now where it will be loaded
	 * and how big it is.
	 */
	rsvmap = fdt + fdt_off_mem_rsvmap(fdt);
	i = fdt_num_mem_rsv(fdt) - 1;
	rsvmap[i].address = cpu_to_fdt64(fdt_load_addr);
	rsvmap[i].size = cpu_to_fdt64(fdt_totalsize(fdt));

	pr_debug("Loaded device tree at 0x%lx\n", fdt_load_addr);

	ret = kexec_locate_mem_hole(image, PURGATORY_STACK_SIZE, PAGE_SIZE, 0,
				    ppc64_rma_size, true, &stack_top);
	if (ret) {
		pr_err("Couldn't find free memory for the purgatory stack.\n");
		ret = -ENOMEM;
		goto out;
	}
	stack_top = stack_top + PURGATORY_STACK_SIZE - 1;
	pr_debug("Purgatory stack is at 0x%lx\n", stack_top);

	ret = setup_purgatory(image, &elf_info, fdt, kernel_load_addr,
			      fdt_load_addr, stack_top,
			      find_debug_console(fdt, chosen_node));
	if (ret)
		pr_err("Error setting up the purgatory.\n");

out:
	elf_free_info(&elf_info);

	/* Make kimage_file_post_load_cleanup free the fdt buffer for us. */
	return ret? ERR_PTR(ret) : fdt;
}

struct kexec_file_ops kexec_elf64_ops = {
	.probe = elf64_probe,
	.load = elf64_load,
};
