/*
 * livepatch.c - powerpc-specific Kernel Live Patching Core
 *
 * Copyright (C) 2015 SUSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/module.h>
#include <asm/livepatch.h>

/**
 * klp_write_module_reloc() - write a relocation in a module
 * @mod:       module in which the section to be modified is found
 * @type:      ELF relocation type (see asm/elf.h)
 * @loc:       address that the relocation should be written to
 * @value:     relocation value (sym address + addend)
 *
 * This function writes a relocation to the specified location for
 * a particular module.
 */
int klp_write_module_reloc(struct module *mod, unsigned long type,
			    unsigned long loc, unsigned long value)
{
	/* This requires infrastructure changes; we need the loadinfos. */
	pr_err("lpc_write_module_reloc not yet supported\n");
	return -ENOSYS;
}
