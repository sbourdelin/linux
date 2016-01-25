#ifndef _LINUX_JUMP_LABEL_BRANCH_H
#define _LINUX_JUMP_LABEL_BRANCH_H

/*
 * jump_label_branch.h: provides struct static_key and static_branch_[un]likely
 *
 * Users of the jump label interfaces usually just include jump_label.h.
 * However, some of the #includes in jump_label.h are problematic for inclusion
 * in basic headers such as kernel.h. So split off static_branch_[un]likely
 * since this is typically all we need in a .h file. For further info about
 * the jump_label infrastructure see jump_label.h
 */

#if defined(CC_HAVE_ASM_GOTO) && defined(CONFIG_JUMP_LABEL)
# define HAVE_JUMP_LABEL
#endif

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <linux/compiler.h>

#ifdef HAVE_JUMP_LABEL

struct static_key {
	atomic_t enabled;
/* Set lsb bit to 1 if branch is default true, 0 ot */
	struct jump_entry *entries;
#ifdef CONFIG_MODULES
	struct static_key_mod *next;
#endif
};

#else
struct static_key {
	atomic_t enabled;
};
#endif  /* HAVE_JUMP_LABEL */
#endif /* __ASSEMBLY__ */

#ifdef HAVE_JUMP_LABEL
#include <asm/jump_label.h>
#endif

#ifndef __ASSEMBLY__

#include <linux/atomic.h>

static inline int static_key_count(struct static_key *key)
{
	return atomic_read(&key->enabled);
}

#ifdef HAVE_JUMP_LABEL

#define JUMP_TYPE_FALSE 0UL
#define JUMP_TYPE_TRUE  1UL
#define JUMP_TYPE_MASK  1UL

#define STATIC_KEY_INIT_TRUE                                    \
	{ .enabled = ATOMIC_INIT(1),                            \
	  .entries = (void *)JUMP_TYPE_TRUE }
#define STATIC_KEY_INIT_FALSE                                   \
	{ .enabled = ATOMIC_INIT(0),                            \
	  .entries = (void *)JUMP_TYPE_FALSE }
#else

#define STATIC_KEY_INIT_TRUE    { .enabled = ATOMIC_INIT(1) }
#define STATIC_KEY_INIT_FALSE   { .enabled = ATOMIC_INIT(0) }

#endif

/*
 * Two type wrappers around static_key, such that we can use compile time
 * type differentiation to emit the right code.
 *
 * All the below code is macros in order to play type games.
 */

struct static_key_true {
	struct static_key key;
};

struct static_key_false {
	struct static_key key;
};

#define STATIC_KEY_TRUE_INIT  (struct static_key_true) { .key = STATIC_KEY_INIT_TRUE,  }
#define STATIC_KEY_FALSE_INIT (struct static_key_false){ .key = STATIC_KEY_INIT_FALSE, }

#define DEFINE_STATIC_KEY_TRUE(name)	\
	struct static_key_true name = STATIC_KEY_TRUE_INIT

#define DEFINE_STATIC_KEY_FALSE(name)	\
	struct static_key_false name = STATIC_KEY_FALSE_INIT

extern bool ____wrong_branch_error(void);

#define static_key_enabled(x)							\
({										\
	if (!__builtin_types_compatible_p(typeof(*x), struct static_key) &&	\
	    !__builtin_types_compatible_p(typeof(*x), struct static_key_true) &&\
	    !__builtin_types_compatible_p(typeof(*x), struct static_key_false))	\
		____wrong_branch_error();					\
	static_key_count((struct static_key *)x) > 0;				\
})

#ifdef HAVE_JUMP_LABEL

/*
 * Combine the right initial value (type) with the right branch order
 * to generate the desired result.
 *
 *
 * type\branch|	likely (1)	      |	unlikely (0)
 * -----------+-----------------------+------------------
 *            |                       |
 *  true (1)  |	   ...		      |	   ...
 *            |    NOP		      |	   JMP L
 *            |    <br-stmts>	      |	1: ...
 *            |	L: ...		      |
 *            |			      |
 *            |			      |	L: <br-stmts>
 *            |			      |	   jmp 1b
 *            |                       |
 * -----------+-----------------------+------------------
 *            |                       |
 *  false (0) |	   ...		      |	   ...
 *            |    JMP L	      |	   NOP
 *            |    <br-stmts>	      |	1: ...
 *            |	L: ...		      |
 *            |			      |
 *            |			      |	L: <br-stmts>
 *            |			      |	   jmp 1b
 *            |                       |
 * -----------+-----------------------+------------------
 *
 * The initial value is encoded in the LSB of static_key::entries,
 * type: 0 = false, 1 = true.
 *
 * The branch type is encoded in the LSB of jump_entry::key,
 * branch: 0 = unlikely, 1 = likely.
 *
 * This gives the following logic table:
 *
 *	enabled	type	branch	  instuction
 * -----------------------------+-----------
 *	0	0	0	| NOP
 *	0	0	1	| JMP
 *	0	1	0	| NOP
 *	0	1	1	| JMP
 *
 *	1	0	0	| JMP
 *	1	0	1	| NOP
 *	1	1	0	| JMP
 *	1	1	1	| NOP
 *
 * Which gives the following functions:
 *
 *   dynamic: instruction = enabled ^ branch
 *   static:  instruction = type ^ branch
 *
 * See jump_label_type() / jump_label_init_type().
 */

#define static_branch_likely(x)							\
({										\
	bool branch;								\
	if (__builtin_types_compatible_p(typeof(*x), struct static_key_true))	\
		branch = !arch_static_branch(&(x)->key, true);			\
	else if (__builtin_types_compatible_p(typeof(*x), struct static_key_false)) \
		branch = !arch_static_branch_jump(&(x)->key, true);		\
	else									\
		branch = ____wrong_branch_error();				\
	branch;									\
})

#define static_branch_unlikely(x)						\
({										\
	bool branch;								\
	if (__builtin_types_compatible_p(typeof(*x), struct static_key_true))	\
		branch = arch_static_branch_jump(&(x)->key, false);		\
	else if (__builtin_types_compatible_p(typeof(*x), struct static_key_false)) \
		branch = arch_static_branch(&(x)->key, false);			\
	else									\
		branch = ____wrong_branch_error();				\
	branch;									\
})

#else /* !HAVE_JUMP_LABEL */

#define static_branch_likely(x)		likely(static_key_enabled(&(x)->key))
#define static_branch_unlikely(x)	unlikely(static_key_enabled(&(x)->key))

#endif /* HAVE_JUMP_LABEL */

#endif /* __ASSEMBLY__ */

#endif
