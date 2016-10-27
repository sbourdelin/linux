/*
 * Copyright (C) 2016 Josh Poimboeuf <jpoimboe@redhat.com>
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

#include <stdlib.h>
#include <string.h>
#include "elf.h"
#include <linux/livepatch.h>

#define WARN(format, ...) \
	fprintf(stderr, "%s: " format "\n", elf->name, ##__VA_ARGS__)

#define MODULE_NAME_LEN (64 - sizeof(GElf_Addr))

#define SHN_LIVEPATCH		0xff20
#define SHF_RELA_LIVEPATCH	0x00100000

static const char usage_string[] =
	"klp-convert <input.ko> <output.ko>";

struct elf *elf;

static struct section *find_or_create_klp_rela_section(char *objname,
						       struct section *oldsec)
{
	char name[256];
	struct section *sec;

	if (snprintf(name, 256, KLP_RELA_PREFIX "%s.%s", objname,
		     oldsec->base->name) >= 256) {
		WARN("section name too long (%s)", oldsec->base->name);
		return NULL;
	}

	sec = find_section_by_name(elf, name);
	if (sec)
		return sec;

	sec = create_rela_section(elf, name, oldsec->base);
	if (!sec)
		return NULL;

	sec->sh.sh_flags |= SHF_RELA_LIVEPATCH;
	return sec;
}

static int rename_klp_symbols(struct section *sec, char *objname)
{
	struct section *relasec;
	struct rela *rela, *tmprela;
	struct klp_module_reloc *reloc;
	int nr_entries, i;
	char name[256];

	relasec = sec->rela;
	if (!relasec) {
		WARN("section %s doesn't have a corresponding rela section",
		     sec->name);
		return -1;
	}

	if (list_empty(&relasec->relas)) {
		WARN("section %s is empty", relasec->name);
		return -1;
	}

	reloc = sec->data;
	nr_entries = sec->size / sizeof(*reloc);

	i = 0;
	list_for_each_entry_safe(rela, tmprela, &relasec->relas, list) {

		if (snprintf(name, 256, KLP_SYM_PREFIX "%s.%s,%d", objname,
			     rela->sym->name, reloc[i].sympos) >= 256) {
			WARN("symbol name too long (%s)", rela->sym->name);
			return -1;
		}

		rela->sym->name = strdup(name);
		rela->sym->sym.st_name = -1;
		rela->sym->sec = NULL;
		rela->sym->sym.st_shndx = SHN_LIVEPATCH;

		list_del(&rela->list);
		i++;
	}

	if (i != nr_entries) {
		WARN("nr_entries mismatch (%d != %d) for %s\n",
		     i, nr_entries, relasec->name);
		return -1;
	}

	list_del(&relasec->list);
	list_del(&sec->list);
	list_del(&sec->sym->list);

	return 0;
}

static int migrate_klp_rela(struct section *oldsec, struct rela *rela)
{
	char objname[MODULE_NAME_LEN];
	struct section *newsec;

	if (sscanf(rela->sym->name, KLP_SYM_PREFIX "%55[^.]", objname) != 1) {
		WARN("bad format for klp rela %s", rela->sym->name);
		return -1;
	}

	newsec = find_or_create_klp_rela_section(objname, oldsec);
	if (!newsec)
		return -1;

	list_del(&rela->list);
	list_add_tail(&rela->list, &newsec->relas);

	return 0;
}

int main(int argc, const char **argv)
{
	const char *in_name, *out_name;
	struct section *sec, *tmpsec;
	char objname[MODULE_NAME_LEN];
	struct rela *rela, *tmprela;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s\n", usage_string);
		return 1;
	}

	in_name = argv[1];
	out_name = argv[2];

	elf = elf_open(in_name);
	if (!elf) {
		fprintf(stderr, "error reading elf file %s\b", in_name);
		return 1;
	}

	list_for_each_entry_safe(sec, tmpsec, &elf->sections, list) {
		if (sscanf(sec->name, ".klp.module_relocs.%55s", objname) != 1)
			continue;
		if (rename_klp_symbols(sec, objname))
			return 1;
	}

	list_for_each_entry(sec, &elf->sections, list) {
		if (!is_rela_section(sec))
			continue;
		if (!strncmp(sec->name, KLP_RELA_PREFIX,
			     strlen(KLP_RELA_PREFIX)))
			continue;
		list_for_each_entry_safe(rela, tmprela, &sec->relas, list) {
			if (strncmp(rela->sym->name, KLP_SYM_PREFIX,
				    strlen(KLP_SYM_PREFIX)))
				continue;
			if (migrate_klp_rela(sec, rela))
				return 1;
		}
	}

	if (elf_write_file(elf, out_name))
		return 1;

	return 0;
}
