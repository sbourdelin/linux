#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/memory.h>
#include <linux/module.h>
#include <asm/insn.h>
#include <asm/text-patching.h>

struct const_u64_table {
	const char *name;
	u64 orig;
	u64 *new;
};

#define PATCHABLE_CONST_U64(id_str)						\
extern unsigned long *__start_const_u64_ ## id_str[];				\
extern unsigned long *__stop_const_u64_ ## id_str[];				\
static __init_or_module u64 id_str ## _CURRENT = id_str ## _DEFAULT;		\
__init __nostackprotector int id_str ## _SET(u64 new)				\
{										\
	int ret;								\
	ret = patch_const_u64(__start_const_u64_ ## id_str,			\
			__stop_const_u64_ ## id_str, id_str ## _CURRENT, new);	\
	if (!ret)								\
		id_str ## _CURRENT = new;					\
	return ret;								\
}

static __init_or_module __nostackprotector
int patch_const_u64(unsigned long **start, unsigned long **stop,
		u64 orig, u64 new)
{
	char buf[MAX_INSN_SIZE];
	struct insn insn;
	unsigned long **iter;

	pr_debug("Patch const: %#llx -> %#llx\n", orig, new);

	mutex_lock(&text_mutex);
	for (iter = start; iter < stop; iter++) {
		memcpy(buf, *iter, MAX_INSN_SIZE);

		kernel_insn_init(&insn, buf, MAX_INSN_SIZE);
		insn_get_length(&insn);

		/*
		 * We expect to see 10-byte MOV instruction here:
		 *  - 1 byte REX prefix;
		 *  - 1 byte opcode;
		 *  - 8 byte immediate value;
		 *
		 * Back off, if something else is found.
		 */
		if (insn.length != 10)
			break;

		insn_get_opcode(&insn);

		/* MOV r64, imm64: REX.W + B8 + rd io */
		if (!X86_REX_W(insn.rex_prefix.bytes[0]))
			break;
		if ((insn.opcode.bytes[0] & ~7) != 0xb8)
			break;

		/* Check that the original value is correct */
		if (memcmp(buf + 2, &orig, sizeof(orig)))
			break;

		memcpy(buf + 2, &new, 8);
		text_poke(*iter, buf, 10);
	}

	if (iter == stop) {
		/* Everything if fine: DONE */
		mutex_unlock(&text_mutex);
		return 0;
	}

	/* Something went wrong. */
	pr_err("Unexpected instruction found at %px: %10ph\n", iter, buf);

	/* Undo */
	while (--iter != start) {
		memcpy(&buf, *iter, MAX_INSN_SIZE);
		memcpy(buf + 2, &orig, 8);
		text_poke(*iter, buf, 10);
	}

	mutex_unlock(&text_mutex);
	return -EFAULT;
}

PATCHABLE_CONST_U64(__PHYSICAL_MASK);
PATCHABLE_CONST_U64(sme_me_mask);

#ifdef CONFIG_MODULES
/* Add an entry for a constant here if it expected to be seen in the modules */
static const struct const_u64_table const_u64_table[] = {
	{"__PHYSICAL_MASK", __PHYSICAL_MASK_DEFAULT, &__PHYSICAL_MASK_CURRENT},
	{"sme_me_mask", sme_me_mask_DEFAULT, &sme_me_mask_CURRENT},
};

__init_or_module __nostackprotector
void module_patch_const_u64(const char *name,
		unsigned long **start, unsigned long **stop)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(const_u64_table); i++) {
		if (strcmp(name, const_u64_table[i].name))
			continue;

		patch_const_u64(start, stop, const_u64_table[i].orig,
				*const_u64_table[i].new);
		return;
	}

	pr_err("Unknown patchable constant: '%s'\n", name);
}
#endif
