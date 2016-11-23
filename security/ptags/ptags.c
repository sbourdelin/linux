/*
 * Copyright (C) 2016 José Bollo <jobol@nonadev.net>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation, version 2.
 *
 * Author:
 *      José Bollo <jobol@nonadev.net>
 */

#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
#  define IF_NS(x)    x
#  define IF_NO_NS(x)
#else
#  define IF_NS(x)
#  define IF_NO_NS(x) x
#endif

/*
 * Definition of characters
 */
#define ADD_CHAR	'+'	/* add tag or keep flag */
#define SUB_CHAR	'-'	/* sub tag or keep flag */
#define SET_CHAR	'!'	/* set a value to a tag */
#define COMMENT_CHAR	'#'	/* comment */
#define QUERY_CHAR	'?'	/* query */

#define KEEP_CHAR	'@'	/* keep char */
#define ASSIGN_CHAR	'='	/* key/value separator */
#define SEPAR_CHAR	':'	/* field delimitors */
#define GLOB_CHAR	'*'	/* global pattern (at end) */

#define EOL_CHAR	'\n'	/* End Of Line */

/*
 * Maximum count of ptags
 */
#define MAXCOUNT	4000

/*
 * Maximum length of a tag
 */
#define MAXTAGLEN	4000

/*
 * Maximum length of a value
 */
#define MAXVALUELEN	32700

/*
 * Increment size
 */
#define CAPACITYINCR	100

/*
 * static strings
 */
static const char prefix_string[] = "ptags:";
static const char add_string[] = "add";
static const char sub_string[] = "sub";
static const char set_string[] = "set";
static const char others_string[] = "others";

/*
 * length of static strings
 */
#define prefix_string_length	((int)((sizeof prefix_string) - 1))
#define add_string_length	((int)((sizeof add_string) - 1))
#define sub_string_length	((int)((sizeof sub_string) - 1))
#define set_string_length	((int)((sizeof set_string) - 1))
#define others_string_length	((int)((sizeof others_string) - 1))

/*
 * slice of entry index
 */
struct slice {
	unsigned lower;		/* lower index of the slice */
	unsigned upper;		/* upper index of the slice plus one */
};

/*
 * items are reference counted strings
 */
struct item {
	atomic_t refcount;	/* reference count of the string */
	unsigned length;	/* length of the string (without terminal zero) */
	char value[1];		/* the zero terminated string */
};

/*
 * value records the value and the state of tags
 */
struct value {
	uintptr_t data;
	/* pointer to the value with used lower bits */
	/* bit 0: kept flag */
	/* bit 1: removed flag */
};

#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
/*
 * Specific data for user namespace
 */
#define HINT_COUNT	3	/* cache count for hints */
#define HINT_NONE	-1	/* not a parent */
#define NSCAPACITYINCR	10	/* capacity increment for nsrefs */

/*
 * hints record how namespaces are linked
 */
struct nshint {
	struct user_namespace *target;	/* target user namespace */
	int hint;			/* the hint: level of ancestry */
};
/*
 * weak reference to user namespace with cache
 */
struct nsref {
	struct user_namespace *userns;	/* the user namespace */
	struct nshint hints[HINT_COUNT]; /* cache of hints */
};

/*
 * value for multi user namespace
 */
struct nsval {
	struct nsval *next;	/* next nsval if any */
	struct nsref *nsref;	/* namespace reference */
	struct value value;	/* the value */
};
#endif

/*
 * entries record ptags, their value and kept flag
 */
struct entry {
	struct item *name;	/* the item for the name */
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsval *first;	/* first value of the entry */
#else
	struct value value;	/* the value of the entry */
#endif
};

/*
 * ptags internal data
 */
struct _ptags {
	struct entry *entries;	/* array of entries */
	unsigned count;		/* count of entries */
	unsigned capacity;	/* allocated count of entries */
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsref *nsrefs;		/* references to user namespaces */
	unsigned nsrefs_count;		/* count of user namespaces */
	unsigned nsrefs_capacity;	/* allocated count of user namespaces */
	int wantgc;			/* is garbage collection expected ? */
#endif
};

/*
 * ptags data attached to tasks
 */
struct ptags {
	struct mutex lock;	/* mutex access */
	struct _ptags data;	/* internal data */
};

/*
 * user namespace proxy
 */
struct uns {
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct user_namespace *userns;	/* the user namespace */
#endif
};

/*******************************************************************
 * section: validity
 ******************************************************************/

/**
 * is_valid_utf8 - Is buffer a valid utf8 string?
 *
 * @buffer: the start of the string
 * @length: length in bytes of the buffer
 *
 * Return 1 when valid or else returns 0
 */
static int is_valid_utf8(const char *buffer, unsigned length)
{
	unsigned x;
	while (length) {
		/* check the first character */
		x = (unsigned)(unsigned char)*buffer++;
		if (x <= 127)
			x = 1;
		else if ((x & 0xc0) == 0x80)
			return 0;
		else if ((x & 0xe0) == 0xc0) {
			if ((x & 0xfe) == 0xc0)
				return 0;
			x = 2;
		} else if ((x & 0xf0) == 0xe0)
			x = 3;
		else if ((x & 0xf8) == 0xf0)
			x = 4;
		else if ((x & 0xfc) == 0xf8)
			x = 5;
		else if ((x & 0xfe) == 0xfc)
			x = 6;
		else
			return 0;

		/* check the length */
		if (length < x)
			return 0;
		length -= x;

		/* check the remaining characters */
		while (--x) {
			if ((*buffer++ & '\xc0') != '\x80')
				return 0;
		}
	}
	return 1;
}

/**
 * is_valid_base - Is buffer a valid base for tag or prefix?
 *
 * @buffer: string for the tag or prefix name
 * @length: length in bytes of the buffer
 *
 * Return 1 when valid or else returns 0
 */
static int is_valid_base(const char *buffer, unsigned length)
{
	unsigned i;
	char c;

	/* should not start with KEEP_CHAR */
	if (length <= 0 || length > MAXTAGLEN || buffer[0] == KEEP_CHAR)
		return 0;

	/* should not contain ASSIGN_CHAR or GLOB_CHAR */
	for (i = 0; i < length; i++) {
		c = buffer[i];
		if (c < ' ' || c == '\x7f' || c == ASSIGN_CHAR
		    || c == GLOB_CHAR)
			return 0;
	}
	return 1;
}

/**
 * is_valid_tag - Is buffer a valid tag?
 *
 * @buffer: string for the tag name
 * @length: length in bytes of the buffer
 *
 * Return 1 when valid or else returns 0
 */
static int is_valid_tag(const char *buffer, unsigned length)
{
	unsigned i;

	/* exclude bad tags */
	if (!is_valid_base(buffer, length)
	    || buffer[length - 1] == SEPAR_CHAR)
		return 0;

	/* accept common tags */
	if (length <= prefix_string_length
	    || memcmp(buffer, prefix_string, prefix_string_length))
		return 1;

	/*
	 * The tag is "ptags:...."
	 * Get the last part
	 */
	i = length;
	while (buffer[i - 1] != SEPAR_CHAR)
		i = i - 1;
	length -= i;
	buffer += i;

	/*
	 * Check the last part
	 */
	return (length == add_string_length
		&& !memcmp(add_string, buffer, length))
	    || (length == sub_string_length
		&& !memcmp(sub_string, buffer, length))
	    || (length == set_string_length
		&& !memcmp(set_string, buffer, length))
	    || (length == others_string_length
		&& !memcmp(others_string, buffer, length));
}

/**
 * is_valid_prefix - Is buffer a valid prefix?
 *
 * @buffer: string for the tag name
 * @length: length in bytes of the buffer
 *
 * Return 1 when valid or else returns 0
 */
static inline int is_valid_prefix(const char *buffer, unsigned length)
{
	return is_valid_base(buffer, length)
	    && buffer[length - 1] == SEPAR_CHAR;
}

/**
 * is_valid_value - Is buffer a valid value?
 *
 * @buffer: string for the tag name
 * @length: length in bytes of the buffer
 *
 * Return 1 when valid or else returns 0
 */
static int is_valid_value(const char *buffer, unsigned length)
{
	unsigned i;
	char c;
	if (length > MAXVALUELEN)
		return 0;
	for (i = 0; i < length; i++) {
		c = buffer[i];
		if (c < ' ' || c == '\x7f')
			return 0;
	}
	return 1;
}

/*******************************************************************
 * section: item
 *
 * The structure item handles one string that is used either
 * for storing the tags or their values.
 * This structure is reference counted.
 ******************************************************************/

/*
 * item_create - Creates an item for the 'value' of 'length'
 *
 * @value: the value copied as value of the item
 * @length: length in bytes of the value
 *
 * Returns the create item or NULL on error.
 */
static struct item *item_create(const char *value, unsigned length)
{
	struct item *item;

	item = kmalloc(length + sizeof *item, GFP_KERNEL);
	if (item) {
		atomic_set(&item->refcount, 1);
		item->length = length;
		memcpy(item->value, value, length);
		item->value[length] = 0;
	}
	return item;
}

/*
 * item_addref - Adds a reference to 'item' and returns it
 *
 * @item: the item to use (must not be NULL)
 *
 * Returns the item
 */
static inline struct item *item_addref(struct item *item)
{
	atomic_inc(&item->refcount);
	return item;
}

/*
 * item_addref_safe - Adds a reference to 'item' and returns it
 *
 * @item: the item to use (can be NULL)
 *
 * Returns the item
 */
static inline struct item *item_addref_safe(struct item *item)
{
	return item ? item_addref(item) : item;
}

/*
 * item_unref - Removes a reference to 'item'
 *
 * @item: the item whose reference count is to be decremented
 *
 * 'item' must not be NULL. It is destroyed when no more used.
 */
static inline void item_unref(struct item *item)
{
	if (atomic_dec_and_test(&item->refcount))
		kfree(item);
}

/*
 * item_unref - Removes a reference to 'item'
 *
 * @item: the item whose reference count is to be decremented
 *
 * 'item' can be NULL. It is destroyed when no more used.
 */
static inline void item_unref_safe(struct item *item)
{
	if (item)
		item_unref(item);
}

/*
 * item_has_prefix - Has 'item' the 'prefix' of 'length'?
 *
 * @item: the item to test
 * @prefix: the prefix to search
 * @length: the length in byte of the prefix
 *
 * Returns 1 if 'item' has the 'prefix' or else returns 0
 */
static inline int item_has_prefix(const struct item *item, const char *prefix,
				  unsigned length)
{
	return item->length >= length && !memcmp(item->value, prefix, length);
}

/*******************************************************************
 * section: value
 *
 * A value records the following data:
 *  - value: an item being the value attached to the tag (can be NULL)
 *  - kept: a boolean flag indicating if the tag is kept accross execve
 *  - removed: a boolean flag indicating if the value was removed
 *
 * Note: for limiting memory usage, boolean flags are taken in the
 * lower bits of the pointer to the value
 ******************************************************************/

/*
 * value_get - Gets the value of 'value'
 *
 * @value: the value whose value is to get
 *
 * Returns the value that can be NULL of 'value'
 */
static inline struct item *value_get(struct value value)
{
	return (struct item *)(value.data & ~(uintptr_t) 3);
}

/*
 * value_set - Sets the value of 'value' to 'value'
 *
 * @value: the value whose value is to set
 * @item: the item to set (can be NULL)
 */
static inline void value_set(struct value *value, struct item *item)
{
	item_unref_safe(value_get(*value));
	value->data = (uintptr_t) item | (value->data & (uintptr_t) 1);
}

/*
 * value_is_removed - Is the 'value' removed?
 *
 * @value: the value to test
 *
 * Returns 1 if value is removed or 0 else
 */
static inline int value_is_removed(struct value value)
{
	return (int)(value.data & (uintptr_t) 2);
}

/*
 * value_set_removed - Sets the 'value' as removed
 *
 * @value: the value to set
 */
static inline void value_set_removed(struct value *value)
{
	item_unref_safe(value_get(*value));
	value->data = (value->data & (uintptr_t) 1) | (uintptr_t) 2;
}

/*
 * value_is_kept - Is the 'value' to be kept?
 *
 * @value: the value to test
 *
 * Returns 1 if value is kept or 0 else
 */
static inline int value_is_kept(struct value value)
{
	return (int)(value.data & (uintptr_t) 1);
}

/*
 * value_set_kept - Sets the kept flag of the 'value'
 *
 * @value: the value to set
 */
static inline void value_set_kept(struct value *value)
{
	value->data |= (uintptr_t) 1;
}

/*
 * value_clear_kept - Clears the kept flag of the 'value'
 *
 * @value: the value to clear
 */
static inline void value_clear_kept(struct value *value)
{
	value->data &= ~(uintptr_t) 1;
}

/*
 * value_make - Makes a new value
 *
 * Returns a just existing value
 */
static inline struct value value_make(void)
{
	struct value result;
	result.data = 0;
	return result;
}

/*
 * value_clone - Clones the 'value'
 *
 * @value: the value to clone
 *
 * Returns an value with same data than 'value' but whose reference
 * counts are incremented.
 */
static inline struct value value_clone(struct value value)
{
	struct value result;
	result.data = value.data;
	item_addref_safe(value_get(result));
	return result;
}

/*
 * value_erase - Erases the content of 'value'
 *
 * @value: the value to erase
 *
 * The name and value of the value are dereferenced
 */
static inline void value_erase(struct value value)
{
	item_unref_safe(value_get(value));
}

#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
/*******************************************************************
 * section: nsref
 ******************************************************************/

/*
 * nsref_init - Initialise 'nsref' to reference 'userns'
 *
 * @nsref: the nsref to initialize
 * @userns: the user namespace to reference
 */
static void nsref_init(struct nsref *nsref, struct user_namespace *userns)
{
	struct nshint *hcur, *hend;

	/* get a weak reference to the user namespace */
	nsref->userns = get_weak_user_ns(userns);

	/* clears hint cache */
	hcur = nsref->hints;
	hend = &hcur[HINT_COUNT];
	while (hcur != hend) {
		hcur->target = NULL;
		hcur++;
	}
}

/*
 * nsref_erase_hints - Erase hints data of 'nsref'
 *
 * @nsref: the reference to erase.
 */
static void nsref_erase_hints(struct nsref *nsref)
{
	struct nshint *hcur, *hend;

	/* unref hints */
	hcur = nsref->hints;
	hend = &hcur[HINT_COUNT];
	while (hcur != hend && hcur->target != NULL) {
		put_weak_user_ns(hcur->target);
		hcur++;
	}
}

/*
 * nsref_erase - Erase data of 'nsref'
 *
 * @nsref: the reference to erase.
 */
static inline void nsref_erase(struct nsref *nsref)
{
	nsref_erase_hints(nsref);
	put_weak_user_ns(nsref->userns);
}

/*
 * nsref_remove_ghost_hints - Removes hint to ghost namespaces
 *
 * @nsref: the reference to clean
 */
static void nsref_remove_ghost_hints(struct nsref *nsref)
{
	struct user_namespace *userns;
	struct nshint *hcur, *hend, *hto;

	hcur = nsref->hints;
	hto = hcur;
	hend = &hcur[HINT_COUNT];
	while (hcur != hend && (userns = hcur->target) != NULL) {
		if (!is_weak_user_ns_still_alive(userns))
			put_weak_user_ns(userns);
		else {
			if (hto != hcur)
				*hto = *hcur;
			hto++;
		}
		hcur++;
	}
	while (hto != hcur)
		hto++->target = NULL;
}

/*
 * nsref_userns_hint - Get the hint for 'userns'
 *
 * The hint is the level of ancestry of 'userns' within context
 * of 'nsref'. If 'nsref' references 'userns', the result if 0.
 * If 'userns' is a parent of the namespace referenced by 'nsref'
 * it returns a positive integer being the level of ancestry.
 * Otherwise a negative value is returned (HINT_NONE).
 *
 * @nsref: the reference to the usernamespace
 * @userns: the usernamespace queried
 *
 * Returns the hint.
 */
static int nsref_userns_hint(struct nsref *nsref, struct user_namespace *userns)
{
	struct nshint h0, h1, *hcur, *hend;
	struct user_namespace *it, *ref;

	/* 0 if equal to reference */
	ref = nsref->userns;
	if (userns == ref)
		return 0;

	/* search cached hint and reorder lru */
	hcur = nsref->hints;
	hend = &hcur[HINT_COUNT];
	h0 = *hcur;
	if (h0.target == userns)
		return h0.hint; /* no reorder needed */
	while (h0.target) {
		if (++hcur == hend) {
			put_weak_user_ns(h0.target);
			break;
		}
		h1 = *hcur;
		*hcur = h0;
		if (h1.target == userns) {
			nsref->hints[0] = h1;
			return h1.hint;
		}
		if (!h1.target)
			break;
		if (++hcur == hend) {
			put_weak_user_ns(h1.target);
			break;
		}
		h0 = *hcur;
		*hcur = h1;
		if (h0.target == userns) {
			nsref->hints[0] = h0;
			return h0.hint;
		}
	}

	/* compute the hint */
	h0.target = get_weak_user_ns(userns);
	h0.hint = 1;
	it = userns->parent;
	for (;;) {
		if (!it) {
			h0.hint = HINT_NONE;
			break;
		}
		if (it == ref)
			break;
		h0.hint++;
		it = it->parent;
	}

	/* record and end */
	nsref->hints[0] = h0;
	return h0.hint;
}
#endif
/*******************************************************************
 * section: entry
 *
 * An entry records the following data:
 *  - name: an item being the tag name
 *  - value: an item being the value attached to the tag (can be NULL)
 *  - kept: a boolean flag indicating if the tag is kept accross execve
 *  - removed: a boolean flag indicating if the entry was removed
 *
 * Note: for limiting memory usage, bollean flags are taken in the
 * lower bits of the pointer to the value
 ******************************************************************/

/*
 * entry_name - Gets the name of 'entry'
 *
 * @entry: the entry whose name is to get
 *
 * Returns the name of 'entry'
 */
static inline struct item *entry_name(struct entry entry)
{
	return entry.name;
}

#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
/*
 * entry_nsval - Returns the existing readable nsval of 'entry' for 'uns'
 *
 * @entry: the entry to search in
 * @uns: the user namespace proxy
 *
 * Returns the nsval of entry having the best hint or NULL if none exist.
 */
static struct nsval *entry_nsval(struct entry *entry, struct uns uns)
{
	struct nsval *val, *r;
	int h, rh;

	val = entry->first;
	while(val) {
		h = nsref_userns_hint(val->nsref, uns.userns);
		if (!h)
			return val;
		if (h > 0) {
			rh = h;
			r = val->next;
			while(r) {
				h = nsref_userns_hint(r->nsref, uns.userns);
				if (!h)
					return r;
				if (h > 0 && h < rh) {
					rh = h;
					val = r;
				}
				r = r->next;
			}
			return val;
		}
		val = val->next;
	}
	return NULL;
}
#endif

/*
 * entry_read - Get the read value of 'entry' for 'uns'
 *
 * @entry: the entry to search in
 * @uns: the user namespace proxy
 *
 * Returns the address of the value to read or NULL if the entry doean't
 * exist or is removed.
 */
static inline struct value *entry_read(struct entry *entry, struct uns uns)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsval *r;

	r = entry_nsval(entry, uns);
	return r && !value_is_removed(r->value) ? &r->value : NULL;
#else
	return value_is_removed(entry->value) ? NULL : &entry->value;
#endif
}

/*
 * entry_make - Makes a new entry
 *
 * @name: name of the entry
 *
 * Returns the entry initialized with the given values
 */
static inline struct entry entry_make(struct item *name)
{
	struct entry result;
	result.name = name;
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	result.first = NULL;
#else
	result.value = value_make();
#endif
	return result;
}

/*
 * entry_erase - Erases the content of 'entry'
 *
 * @entry: the entry to erase
 *
 * The name and value of the entry are dereferenced
 */
static inline void entry_erase(struct entry entry)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsval *val;

	while((val = entry.first) != NULL) {
		entry.first = val->next;
		value_erase(val->value);
		kfree(val);
	}
#else
	value_erase(entry.value);
#endif
	item_unref(entry_name(entry));
}

/*
 * entry_is_removed - Is the entry to be removed?
 *
 * @entry: the entry to test
 *
 * Returns 1 if the entry can be removed.
 */
static inline int entry_is_removed(struct entry entry)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsval *val;

	/* note: an other politic, here, would be to return 0 always */
	val = entry.first;
	while(val) {
		if (!value_is_removed(val->value))
			return 0;
		val = val->next;
	}
	return 1;
#else
	return value_is_removed(entry.value);
#endif
}

/*
 * entry_prune - Removes the values that are not to be kept
 *
 * @entry: the entry to prune
 *
 * Return 1 if the entry is to be kept or 0 if it is removed
 * and erased (erase is not to be called)
 */
static IF_NO_NS(inline) int entry_prune(struct entry *entry)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsval *val, **prv;
	int remove;

	/* effective prune */
	remove = 1;
	prv = &entry->first;
	val = entry->first;
	while(val != NULL) {
		if (value_is_kept(val->value)) {
			if (!value_is_removed(val->value))
				remove = 0;
			prv = &val->next;
			val = val->next;
		} else {
			*prv = val->next;
			value_erase(val->value);
			kfree(val);
			val = *prv;
		}
	}
	/* test if empty */
	prv = &entry->first;
	val = *prv;
	if (val && !remove)
		return 1;
	/* clean up */
	while(val != NULL) {
		*prv = val->next;
		value_erase(val->value);
		kfree(val);
		val = *prv;
	}	
#else
	if (value_is_kept(entry->value))
		return 1;
	value_erase(entry->value);
#endif
	item_unref(entry_name(*entry));
	return 0;
}

/*******************************************************************
 * section: entries
 ******************************************************************/

/*
 * entries_search - Searchs the 'name' of 'length' in the entries
 *
 * @entries: array of entries to be searched
 * @count: count of entries in 'entries'
 * @name: name to search
 * @length: length in bytes of the searched name
 * @glob: boolean indicating if search is global (on prefix)
 *
 * Returns the slice found. The entries found are at indexes from result.lower
 * to result.upper-1. When nothing is found, the insert index is returned
 * in result.lower and result.upper.
 *
 * This function asserts that entries is ordered.
 */
static struct slice entries_search(struct entry *entries, unsigned count,
				   const char *name, unsigned length, int glob)
{
	int cmp;
	unsigned idx;
	struct slice r;
	struct item *item;

	/* dichotomic search */
	r.lower = 0;
	r.upper = count;
	while (r.lower != r.upper) {
		idx = (r.lower + r.upper) >> 1;
		item = entry_name(entries[idx]);
		if (length > item->length) {
			cmp = memcmp(item->value, name, item->length);
			if (cmp <= 0)
				r.lower = idx + 1;
			else
				r.upper = idx;
		} else {
			cmp = memcmp(item->value, name, length);
			if (!cmp && (glob || length == item->length)) {
				r.lower = idx;
				r.upper = idx + 1;
				if (glob)
					goto extend;
				goto end;
			}
			if (cmp < 0)
				r.lower = idx + 1;
			else
				r.upper = idx;
		}
	}
	goto end;
 extend:
	/* extend selection (if glob) */
	while (r.lower > 0
	       && item_has_prefix(entry_name(entries[r.lower - 1]), name,
				  length))
		r.lower--;
	while (r.upper < count
	       && item_has_prefix(entry_name(entries[r.upper]), name, length))
		r.upper++;
 end:
	return r;
}

/*******************************************************************
 * section: _ptags
 ******************************************************************/

/*
 * _ptags_query_gc - Request a garbage collection for ptags
 *
 * @ptags: the ptags to be cleaned
 */
static inline void _ptags_query_gc(struct _ptags *ptags)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	ptags->wantgc = 1;
#endif
}

#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
/*
 * _ptags_erase_tagged_nsref - remove tagged namespace references
 *
 * @ptags: ptags to be cleaned
 */
static void _ptags_erase_tagged_nsref(struct _ptags *ptags)
{
	struct nsref *nscur, *nsend;
	struct entry *entcur, *entend, *entto;
	struct nsval *curval, **preval;

	/* clean ghost references references */
	nscur = ptags->nsrefs;
	nsend = &nscur[ptags->nsrefs_count];
	while (nscur != nsend) {
		if (nscur->userns) {
			/* still alive, skip */
			nscur++;
		} else {
			/* removes the current nsref */
			nsref_erase_hints(nscur);
			if (--nsend != nscur)
				*nscur = *nsend;

			/* update entries */
			entcur = ptags->entries;
			entto = entcur;
			entend = &entcur[ptags->count];
			while (entcur != entend) {
				preval = &entcur->first;
				curval = entcur->first;
				while (curval) {
					if (curval->nsref == nscur) {
						/*
						 * note: it is asserted that parent
						 * namespaces die after children.
						 */
						*preval = curval->next;
						value_erase(curval->value);
						kfree(curval);
						curval = *preval;
					} else {
						if (curval->nsref == nsend)
							curval->nsref = nscur;
						preval = &curval->next;
						curval = curval->next;
					}
				}
				if (entry_is_removed(*entcur))
					entry_erase(*entcur);
				else {
					if (entto != entcur)
						*entto = *entcur;
					entto++;
				}
				entcur++;
			}
			ptags->count = (unsigned)(entto - ptags->entries);
		}
	}
	/* clean ghost hints */
	nscur = ptags->nsrefs;
	ptags->nsrefs_count = (unsigned)(nsend - nscur);
	while (nscur != nsend) {
		nsref_remove_ghost_hints(nscur);
		nscur++;
	}
}

/*
 * _ptags_collect_garbage - remove unused namespace references
 *
 * @ptags: ptags to be cleaned
 */
static void _ptags_collect_garbage(struct _ptags *ptags)
{
	struct user_namespace *userns;
	struct nsref *nscur, *nsend;
	struct entry *entcur, *entend;
	struct nsval *val;
	int changed;

return;
	changed = 0;

	/* scan entries entries */
	entcur = ptags->entries;
	entend = &entcur[ptags->count];
	while (entcur != entend) {
		val = entcur->first;
		while (val) {
			nscur = val->nsref;
			userns = nscur->userns;
			nscur->userns = (void*)((uintptr_t) userns | (uintptr_t)1);
			val = val->next;
		}
		entcur++;
	}

	/* detect leaks */
	nscur = ptags->nsrefs;
	nsend = &nscur[ptags->nsrefs_count];
	while (nscur != nsend) {
		userns = nscur->userns;
		if (((uintptr_t)1) & ((uintptr_t)userns))
			nscur->userns = (void*)((uintptr_t) userns & ~(uintptr_t)1);
		else {
			nscur->userns = NULL;
			put_weak_user_ns(userns);
			changed = 1;
		}
		nscur++;
	}
	if (changed)
		_ptags_erase_tagged_nsref(ptags);

	ptags->wantgc = 0;
}

/*
 * _ptags_clean_nsrefs - Removes dependencies to ghost namespaces of 'ptags'
 *
 * @ptags: ptags to be cleaned
 */
static void _ptags_clean_nsrefs(struct _ptags *ptags)
{
	struct user_namespace *userns;
	struct nsref *nscur, *nsend;
	int changed;

	for(;;) {
		changed = 0;

		/* clean ghost references references */
		nscur = ptags->nsrefs;
		nsend = &nscur[ptags->nsrefs_count];
		while (nscur != nsend) {
			userns = nscur->userns;
			if (!is_weak_user_ns_still_alive(userns)) {
				nscur->userns = NULL;
				put_weak_user_ns(userns);
				changed = 1;
			}
			nscur++;
		}
		if (!changed)
			break;
		_ptags_erase_tagged_nsref(ptags);
	}
}

/*
 * _ptags_nsref - Get/create within 'ptags' the namespace reference for 'uns'
 *
 * @ptags: where is the reference
 * @uns: the user namespace proxy
 *
 * Return the reference to the user namespace proxied by 'uns' within 'ptags'
 */
static struct nsref *_ptags_nsref(struct _ptags *ptags, struct uns uns)
{
	struct nsref *nsorg, *nsref, *nsend;
	struct entry *ecur, *eend;
	struct nsval *nsval;
	unsigned count;

	/* search existing */
	nsorg = ptags->nsrefs;
	nsref = nsorg;
	count = ptags->nsrefs_count;
	nsend = &nsorg[count];
	while (nsref != nsend) {
		if (nsref->userns == uns.userns)
			return nsref; /* found */
		nsref++;
	}

	/* create one */
	if (count == ptags->nsrefs_capacity) {
		/* increase the size */
		nsref = krealloc(nsorg, (count + NSCAPACITYINCR) * sizeof *nsref, GFP_KERNEL);
		if (nsref == NULL)
			return NULL;

		/* rellocation if needed */
		if (nsref != nsorg) {
			ecur = ptags->entries;
			eend = &ecur[ptags->count];
			while (ecur != eend) {
				nsval = ecur++->first;
				while(nsval) {
					nsval->nsref = &nsref[nsval->nsref - nsorg];
					nsval = nsval->next;
				}
			}
		}

		/* records data */
		ptags->nsrefs = nsref;
		ptags->nsrefs_capacity = count + NSCAPACITYINCR;
		nsref += count;
	}
	ptags->nsrefs_count = count + 1;
	nsref_init(nsref, uns.userns);
	return nsref;
}
#endif

/*
 * _ptags_entry_write - Gets the write value of existing 'entry' for 'uns' within 'ptags'
 *
 * @ptags: the ptags where the value is to be written
 * @entry: the entry where the value will be written
 * @uns: proxy to user namespace
 *
 * Returns the pointer to the value to write or NULL on memory depletion.
 */
static IF_NO_NS(inline) int _ptags_entry_write(struct _ptags *ptags, struct entry *entry, struct uns uns, struct value **to)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsval *rval, *val;
	struct nsref *nsref;

	rval = entry_nsval(entry, uns);
	if (!rval || value_is_removed(rval->value))
		return 0;

	nsref = _ptags_nsref(ptags, uns);
	if (nsref == NULL)
		return -1;

	if (rval && rval->nsref == nsref)
		*to = &rval->value;
	else {
		val = kmalloc(sizeof *val, GFP_KERNEL);
		if (!val)
			return -1;

		val->value = rval ? value_clone(rval->value) : value_make();
		val->nsref = nsref;
		val->next = entry->first;
		entry->first = val;
		*to = &val->value;
	}
#else
	*to = &entry->value;
#endif
	return 1;
}

/*
 * _ptags_entry_create - Creates the write value of 'entry' for 'uns' within 'ptags'
 *
 * @ptags: the ptags where the value is to be written
 * @entry: the entry where the value will be written
 * @uns: proxy to user namespace
 *
 * Returns the pointer to the value to write or NULL on memory depletion.
 */
static IF_NO_NS(inline) struct value *_ptags_entry_create(struct _ptags *ptags, struct entry *entry, struct uns uns)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsval *rval, *val;
	struct nsref *nsref;

	/* get the namespace reference for uns */
	nsref = _ptags_nsref(ptags, uns);
	if (nsref == NULL)
		return NULL;

	/* search the read nsval */
	rval = entry_nsval(entry, uns);
	if (rval) {
		if (rval->nsref == nsref) {
			if (value_is_removed(rval->value))
				rval->value = value_make();
			return &rval->value;
		}
		if (value_is_removed(rval->value))
			rval = NULL;
	}

	/* creates the nsval */
	val = kmalloc(sizeof *val, GFP_KERNEL);
	if (!val)
		return NULL;

	/* init it */
	val->value = rval ? value_clone(rval->value) : value_make();
	val->nsref = nsref;
	val->next = entry->first;
	entry->first = val;
	return &val->value;
#else
	return &entry->value;
#endif
}

/*
 * _ptags_erase - Erases the content of 'ptags'
 *
 * @ptags: the ptags whose content is to erase (must not be NULL)
 */
static void _ptags_erase(struct _ptags *ptags)
{
	struct entry *entries;
	unsigned count;
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsref *nsrefs;
#endif

	count = ptags->count;
	entries = ptags->entries;
	while (count)
		entry_erase(entries[--count]);
	kfree(entries);
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	count = ptags->nsrefs_count;
	nsrefs = ptags->nsrefs;
	while (count)
		nsref_erase(&nsrefs[--count]);
	kfree(nsrefs);
#endif
}

/**
 * _ptags_prune - Prunes from 'ptags' the entries not kept
 *
 * @ptags: the ptags to be puned
 */
static void _ptags_prune(struct _ptags *ptags)
{
	unsigned i, j, count;
	struct entry *entries;

	entries = ptags->entries;
	count = ptags->count;
	for (i = j = 0; i < count; i++) {
		if (entry_prune(&entries[i]))
			entries[j++] = entries[i];
	}
	ptags->count = j;
}

/**
 * _ptags_copy - Copies entries from locked 'src' to locked 'dst'
 *
 * @dst: locked destination ptags
 * @src: locked source ptags
 *
 * Returns 0 on success or -ENOMEM on memory allocation failure.
 */
static int _ptags_copy(struct _ptags *dst, struct _ptags *src)
{
	struct _ptags tmp;
	unsigned i;
	struct entry *from, e, x;
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	struct nsref *fromrefs, *nscur, *nsend;
	struct nshint *hcur, *hend;
	struct nsval *val, **toval, *itval;
#endif

	/* allocates the entries */
	tmp.count = src->count;
	tmp.entries = kmalloc(tmp.count * sizeof *tmp.entries, GFP_KERNEL);
	if (!tmp.entries)
		return -ENOMEM;
	tmp.capacity = tmp.count;

#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	/* allocates the namespace references */
	tmp.nsrefs_count = src->nsrefs_count;
	tmp.nsrefs = kmalloc(tmp.nsrefs_count * sizeof *tmp.nsrefs, GFP_KERNEL);
	if (!tmp.nsrefs) {
		kfree(tmp.entries);
		return -ENOMEM;
	}
	tmp.nsrefs_capacity = tmp.nsrefs_count;

	/* copy the namespace references */
	fromrefs = src->nsrefs;
	memcpy(tmp.nsrefs, fromrefs, tmp.nsrefs_count * sizeof *tmp.nsrefs);
	nscur = tmp.nsrefs;
	nsend = &nscur[tmp.nsrefs_count];
	while (nscur != nsend) {
		get_weak_user_ns(nscur->userns);
		hcur = nscur++->hints;
		hend = &hcur[HINT_COUNT];
		while(hcur != hend && hcur->target)
			get_weak_user_ns(hcur++->target);
	}
#endif
	/* copy the entries */
	from = src->entries;
	for (i = 0; i < tmp.count; i++) {
		x = from[i];
		e.name = item_addref(entry_name(x));
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
		toval = &e.first;
		itval = x.first;
		while (itval) {
			val = kmalloc(sizeof *val, GFP_KERNEL);
			*toval = val;
			if (val == NULL) {
				/* out of memory: cleanup */
				entry_erase(e);
				tmp.count = i;
				_ptags_erase(&tmp);
				return -ENOMEM;
			}
			val->nsref = &tmp.nsrefs[itval->nsref - fromrefs];
			val->value = value_clone(itval->value);
			toval = &val->next;
			itval = itval->next;
		}
		*toval = NULL;
#else
		e.value = value_clone(x.value);
#endif
		tmp.entries[i] = e;
	}

	/* assign the copy */
	_ptags_erase(dst);
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	tmp.wantgc = 0;
#endif
	*dst = tmp;

	return 0;
}

/*
 * _ptags_init - Creates and initializes the ptags structure
 *
 * Returns the init ptags.
 */
static inline void _ptags_init(struct _ptags *ptags)
{
	ptags->entries = NULL;
	ptags->count = 0;
	ptags->capacity = 0;
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	ptags->nsrefs = NULL;
	ptags->nsrefs_count = 0;
	ptags->nsrefs_capacity = 0;
	ptags->wantgc = 0;
#endif
}

/**
 * _ptags_move - Transfers entries from 'src' to 'dst'
 *
 * @dst: destination ptags
 * @src: source ptags
 */
static inline void _ptags_move(struct _ptags *dst, struct _ptags *src)
{
	_ptags_erase(dst);
	*dst = *src;
	_ptags_init(src);
}

/*******************************************************************
 * section: uns
 ******************************************************************/
/*
 * uns_get - Get the current user namespace proxy
 */
static inline struct uns uns_get(void)
{
	struct uns uns;
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	uns.userns = get_user_ns(current_user_ns());
#endif
	return uns;
}

/*
 * uns_put - release the proxy
 *
 * @uns: the proxy to release
 */
static inline void uns_put(struct uns uns)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	put_user_ns(uns.userns);
#endif
}

/*******************************************************************
 * section: ptags check
 *
 * This checks the validity of requested actions
 ******************************************************************/

/*
 * check_action - Checks if the action 'astr' of length 'alen' is
 *                authorized for the tag 'tstr' of length 'tlen'
 *
 * @ptags: the ptags controling the action (ptags of current)
 * @tstr: the tags string to modify
 * @tlen: the length of the tag
 * @astr: the action sting to check
 * @alen: the length of the action
 * @uns: user namespace proxy
 *
 * Returns 1 if the action is authorized or 0 if not
 */
static int check_action(struct _ptags *ptags, const char *tstr, unsigned tlen,
			const char *astr, unsigned alen, struct uns uns)
{
	unsigned i, ilen;
	struct slice slice;
	struct entry *entries;
	struct item *item;
	char *istr;

	/* Searchs the entries "ptags:...." */
	entries = ptags->entries;
	slice = entries_search(entries, ptags->count, prefix_string,
			       prefix_string_length, 1);

	/* Loop on found entries */
	for (i = slice.lower; i < slice.upper; i++) {

		/* must be available for uns */
		if (!entry_read(&entries[i], uns))
			continue;

		/* get the unprefixed entry in istr and ilen */
		item = entry_name(entries[i]);
		ilen = item->length - prefix_string_length;
		istr = item->value + prefix_string_length;

		if (ilen == alen) {
			/*
			 * case of ptags:action
			 *
			 * Accept when action is the searched action 'astr'
			 * and when tstr hasn't prefix "ptags:"
			 */
			if (!memcmp(&istr[ilen - alen], astr, alen) &&
			    (tlen < prefix_string_length ||
			     memcmp(tstr, prefix_string, prefix_string_length)))
				return 1;
		} else if (ilen > alen) {
			/*
			 * case of ptags:prefix:action
			 *
			 * Accept when action is the searched action 'astr'
			 * and either 'tstr' has prefix "prefix:"
			 * or 'tstr' == prefix
			 */
			ilen = ilen - alen - 1;
			if (istr[ilen] == SEPAR_CHAR
			    && !memcmp(&istr[ilen + 1], astr, alen)) {
				/* searched action found */
				if (tlen > ilen) {
					if (!memcmp(istr, tstr, ilen + 1))
						return 1;
				} else if (tlen == ilen) {
					if (!memcmp(istr, tstr, tlen))
						return 1;
				}
			}
		}
	}

	/* not authorized */
	return 0;
}

/*
 * check_tag - Checks if the action 'action' of length 'alen' is
 *             authorized for the 'tag' of length 'length'
 *
 * @cptags: the ptags controling the action (ptags of current)
 * @mptags: the ptags modified
 * @tag: the tag name to modify
 * @length: the length of the tag
 * @action: the action sting to check
 * @alen: the length of the action
 * @uns: user namespace proxy
 *
 * Returns 1 if the action is authorized or 0 if not
 */
static inline int check_tag(struct _ptags *cptags, struct _ptags *mptags,
			    const char *tag, unsigned length,
			    const char *action, unsigned alen, struct uns uns)
{
	/* current ptags == NULL means "super capable" */
	if (!cptags)
		return 1;

	/* check if the action is forbidden */
	if (!check_action(cptags, tag, length, action, alen, uns))
		return 0;

	/* the action is authorized if current ptags == modified ptags */
	if (cptags == mptags)
		return 1;

	/* not the same process/thread, check "others" authorisation */
	return check_action(cptags, tag, length, others_string,
			    others_string_length, uns);
}

/*
 * check_tag - Checks if the action 'action' of length 'alen' is
 *             authorized for the 'entry'
 *
 * @cptags: the ptags controling the action (ptags of current)
 * @mptags: the ptags modified
 * @entry: the entry to modify
 * @action: the action sting to check
 * @alen: the length of the action
 * @uns: user namespace proxy
 *
 * Returns 1 if the action is authorized or 0 if not
 */
static inline int check_entry(struct _ptags *cptags, struct _ptags *mptags,
			      struct entry *entry, const char *action,
			      unsigned alen, struct uns uns)
{
	struct item *name = entry_name(*entry);
	return check_tag(cptags, mptags, name->value, name->length, action,
			 alen, uns);
}

/*******************************************************************
 * section: ptags operations
 ******************************************************************/

/**
 * _ptags_query - Queries existing of tags
 *
 * @ptags: queried ptags
 * @name: string for the name of the tag
 * @length: length in bytes of the tag's name
 * @uns: user namespace proxy
 *
 * Returns 0 in case of success tag present or -ENOENT if the tag is not found
 * or invalid.
 */
static int _ptags_query(struct _ptags *ptags, const char *line, unsigned length, struct uns uns)
{
	int qkept, glob;
	unsigned count;
	struct slice slice;
	struct entry *entries;
	struct value *value;

	/* is querying only @s? */
	qkept = length > 0 && line[0] == KEEP_CHAR;
	if (qkept) {
		line++;
		length--;
	}

	entries = ptags->entries;
	count = ptags->count;

	/* check length */
	if (length == 0) {
		/*
		 * global query
		 */
		glob = 1;
		slice.lower = 0;
		slice.upper = count;
	} else {
		/* is a global query? */
		glob = length > 1 && line[length - 2] == SEPAR_CHAR
		    && line[length - 1] == GLOB_CHAR;
		if (glob) {
			/* global */
			length -= 1;
			if (!is_valid_prefix(line, length))
				return -EINVAL;
		} else {
			/* not global */
			if (!is_valid_tag(line, length))
				return -EINVAL;
		}
		/* search entry slice */
		slice = entries_search(entries, count, line, length, glob);
	}

	/* iterate over found entries */
	while (slice.lower != slice.upper) {
		value = entry_read(&entries[slice.lower++], uns);
		if (value && (!qkept || value_is_kept(*value)))
			return 0;
	}
	return -ENOENT;
}

/**
 * _ptags_set - set the value of one tag
 *
 * @cptags: controling ptags
 * @mptags: modified ptags
 * @line: string for the line of setting the tag
 * @length: length in bytes of the tag's line
 * @uns: user namespace proxy
 *
 * Returns 0 in case of success tag present or
 *  o -ENOENT if the tag is not found
 *  o -EINVAL if the syntax is invalid
 *  o -EPERM if the operation is forbidden
 *  o -ENOMEM if not enough memory
 */
static int _ptags_set(struct _ptags *cptags, struct _ptags *mptags,
		     const char *line, unsigned length, struct uns uns)
{
	struct slice slice;
	unsigned taglen, idxval, vallen;
	struct entry *entry;
	struct item *item;
	struct value *value;
	int rc;

	/* compute the tag length */
	taglen = 0;
	while (taglen < length && line[taglen] != ASSIGN_CHAR)
		taglen++;
	if (!is_valid_tag(line, taglen))
		return -EINVAL;

	/* search the permission */
	if (!check_tag
	    (cptags, mptags, line, taglen, set_string, set_string_length, uns))
		return -EPERM;

	/* search the entry */
	slice = entries_search(mptags->entries, mptags->count, line, taglen, 0);
	if (slice.lower == slice.upper)
		return -ENOENT;

	/* instanciate the item */
	idxval = taglen + 1;
	if (length <= idxval)
		item = NULL;
	else {
		/* check validity of item */
		vallen = length - idxval;
		if (!is_valid_value(line + idxval, vallen))
			return -EINVAL;
		/* create the item */
		item = item_create(line + idxval, vallen);
		if (!item)
			return -ENOMEM;
	}

	/* create a value for writing */
	entry = &mptags->entries[slice.lower];
	rc = _ptags_entry_write(mptags, entry, uns, &value);
	if (rc <= 0) {
		item_unref_safe(item);
		/* test the case of deleted entry */
		return rc ? -ENOMEM : -ENOENT;
	}

	/* replace the previous value */
	value_set(value, item);
	return 0;
}

/**
 * _ptags_sub - Removes one or more tags
 *
 * @cptags: controling ptags
 * @mptags: modified ptags
 * @line: string for the line of the tag
 * @length: length in bytes of the tag's line
 * @uns: user namespace proxy
 *
 * Returns 0 in case of success or
 *  o -EINVAL if the syntax is invalid
 *  o -EPERM if the operation is forbiden
 */
static int _ptags_sub(struct _ptags *cptags, struct _ptags *mptags,
		     const char *line, unsigned length, struct uns uns)
{
	struct slice slice;
	int subkept, glob, rc;
	unsigned i, j, count;
	struct entry *entries, *entry, e;
	struct value *value;

	/* is for removing @s? */
	subkept = length > 0 && line[0] == KEEP_CHAR;
	if (subkept) {
		line++;
		length--;
	}

	/* init */
	entries = mptags->entries;
	count = mptags->count;

	/* check length */
	if (length == 0) {
		/*
		 *  global selection of all
		 */
		glob = 1;
		slice.lower = 0;
		slice.upper = count;
	} else {
		/* is a global sub? */
		glob = length > 1 && line[length - 2] == SEPAR_CHAR
		    && line[length - 1] == GLOB_CHAR;
		if (glob) {
			/* global */
			length -= 1;
			if (!is_valid_prefix(line, length))
				return -EINVAL;
		} else {
			/* not global */
			if (!is_valid_tag(line, length))
				return -EINVAL;
		}
		/* search entry slice */
		slice = entries_search(entries, count, line, length, glob);
	}

	/* check action */
	if (subkept) {
		/* remove kept flags */
		for (i = slice.lower; i < slice.upper; i++) {
			entry = &entries[i];
			if (check_entry (cptags, mptags, entry, sub_string, sub_string_length, uns)) {
				/* authorized */
				rc = _ptags_entry_write(mptags, entry, uns, &value);
				if (rc < 0)
					return -ENOMEM;
				if (rc)
					value_clear_kept(value);
			} else if (!glob) {
				/* not authorized and not global */
				value = entry_read(entry, uns);
				if (value && value_is_kept(*value))
					return -EPERM;
			}
		}
	} else {
		/* mark entries to remove */
		for (i = slice.lower; i < slice.upper; i++) {
			entry = &entries[i];
			if (check_entry (cptags, mptags, entry, sub_string, sub_string_length, uns)) {
				/* authorized */
				rc = _ptags_entry_write(mptags, entry, uns, &value);
				if (rc < 0)
					return -ENOMEM;
				if (rc)
					value_set_removed(value);
			} else if (!glob) {
				/* not authorized and not global */
				value = entry_read(entry, uns);
				if (value)
					return -EPERM;
			}
		}
		/* remove entries */
		for (i = j = slice.lower; i < slice.upper; i++) {
			e = entries[i];
			if (entry_is_removed(e))
				entry_erase(e);
			else {
				if (i != j)
					entries[j] = e;
				j++;
			}
		}
		if (i != j) {
			if (i != count)
				memmove(&entries[j], &entries[i], (count - i) * sizeof *entry);
			mptags->count -= i - j;
			_ptags_query_gc(mptags);
		}
	}
	return 0;
}

/**
 * _ptags_add - Adds one tag
 *
 * @cptags: controling ptags
 * @mptags: modified ptags
 * @line: string for the line of the tag
 * @length: length in bytes of the tag's line
 * @uns: user namespace proxy
 *
 * Returns 0 in case of success or one of the following error code if failed:
 *   o -EINVAL if the line is invalid
 *   o -EPERM if the addition is forbidden
 *   o -ENOMEM if the an allocation failed
 *   o -ECANCELED if the maximum count of tag is reached
 */
static int _ptags_add(struct _ptags *cptags, struct _ptags *mptags,
		     const char *line, unsigned length, struct uns uns)
{
	struct slice slice;
	int addkept, glob, rc;
	unsigned i, n, count;
	struct entry *entries, *entry;
	struct item *name;
	struct value *value;

	/* is for adding @s? */
	addkept = length > 0 && line[0] == KEEP_CHAR;
	if (addkept) {
		line++;
		length--;
	}

	entries = mptags->entries;
	count = mptags->count;

	/* check length */
	if (length == 0) {
		/*
		 *  global selection of all
		 */
		glob = 1;
		slice.lower = 0;
		slice.upper = count;
	} else {
		/* is a global sub? */
		glob = length > 1 && line[length - 2] == SEPAR_CHAR
		    && line[length - 1] == GLOB_CHAR;
		if (glob) {
			length -= 1;
			if (!is_valid_prefix(line, length))
				return -EINVAL;
		} else {
			/* not global */
			if (!is_valid_tag(line, length))
				return -EINVAL;
		}
	}
	if (glob && !addkept)
		return -EINVAL;

	/* search entry slice */
	slice = entries_search(entries, count, line, length, glob);

	/* check action */
	if (glob) {
		/* globally add kept flags to existing */
		for (i = slice.lower; i < slice.upper; i++) {
			entry = &entries[i];
			if (check_entry (cptags, mptags, entry, add_string, add_string_length, uns)) {
				/* authorized */
				rc = _ptags_entry_write(mptags, entry, uns, &value);
				if (rc < 0)
					return -ENOMEM;
				if (rc)
					value_set_kept(value);
			} 
		}
	} else if (slice.lower != slice.upper) {
		/* add kept if needed */
		entry = &entries[slice.lower];
		if (check_entry (cptags, mptags, entry, add_string, add_string_length, uns)) {
			/* authorized */
			value = _ptags_entry_create(mptags, entry, uns);
			if (!value)
				return -ENOMEM;
			if (addkept)
				value_set_kept(value);
		} else {
			/* not authorized */
			value = entry_read(entry, uns);
			if (!value || (addkept && !value_is_kept(*value)))
				return -EPERM;
		}
	} else {
		/* adds a new entry */
		if (count == MAXCOUNT)
			return -ECANCELED;
		if (!check_tag (cptags, mptags, line, length, add_string, add_string_length, uns))
			return -EPERM;
		if (count == mptags->capacity) {
			n = mptags->capacity + CAPACITYINCR;
			entries = krealloc(entries, n * sizeof *entries, GFP_KERNEL);
			if (!entries)
				return -ENOMEM;
			mptags->entries = entries;
			mptags->capacity = n;
		}
		name = item_create(line, length);
		if (!name)
			return -ENOMEM;
		entry = &entries[slice.lower];
		mptags->count = count + 1;
		n = count - slice.lower;
		if (n)
			memmove(&entry[1], entry, n * sizeof *entry);
		*entry = entry_make(name);
		value = _ptags_entry_create(mptags, entry, uns);
		if (!value)
			return -ENOMEM;
		if (addkept)
			value_set_kept(value);
	}
	return 0;
}

/**
 * _ptags_write - Implement the writing of the tags
 *
 * @cptags: controling ptags
 * @mptags: modified ptags
 * @buffer: a pointer to the written data
 * @size: the size of the written data
 * @uns: user namespace proxy
 *
 * Returns the positive count of byte written. It can be less than the
 * count given by size if an error appears after. This count indicates
 * the count of data treated without errors.
 * Returns one of the negative error code below if the data begins on error:
 *   o -EINVAL if the name or the syntax is invalid
 *   o -EPERM if the addition is forbidden
 *   o -ENOMEM if the an allocation failed
 *   o -ENOENT if the query failed
 *   o -ECANCELED if the maximum count of tag is reached
 */
static int _ptags_write(struct _ptags *cptags, struct _ptags *mptags,
			      const char *buffer, unsigned size, struct uns uns)
{
	unsigned start, stop, len;
	int err;

	/* begin the parsing */
	start = 0;
	while (start < size) {
		/* scan a line of 'len' */
		for (stop = start; stop < size && buffer[stop] != EOL_CHAR;
		     stop++) ;
		len = stop - start;

		/* ignore empty lines */
		if (len == 0)
			err = 0;

		/* check utf8 validity of the line */
		else if (!is_valid_utf8(buffer + start, len))
			err = -EINVAL;

		/* lines not terminated with EOL_CHAR */
		else if (stop == size)
			err = -EINVAL;

		/* skip comments (starting with COMMENT_CHAR) */
		else if (buffer[start] == COMMENT_CHAR)
			err = 0;

		/* line starting with ADD_CHAR */
		else if (buffer[start] == ADD_CHAR)
			err = _ptags_add(cptags, mptags,
					buffer + start + 1, len - 1, uns);

		/* lines starting with SUB_CHAR */
		else if (buffer[start] == SUB_CHAR)
			err = _ptags_sub(cptags, mptags,
					buffer + start + 1, len - 1, uns);

		/* lines starting with SET_CHAR */
		else if (buffer[start] == SET_CHAR)
			err = _ptags_set(cptags, mptags,
					buffer + start + 1, len - 1, uns);

		/* lines starting with QUERY_CHAR */
		else if (buffer[start] == QUERY_CHAR)
			err = _ptags_query(mptags, buffer + start + 1, len - 1, uns);

		/* other lines */
		else
			err = -EINVAL;

		/* treat the error case if any */
		if (err != 0) {
			return start ? (int)start : err;
		}

		/* parse next line */
		start = stop + 1;
	}
	return (int)start;
}

/**
 * _ptags_read - Implement the reading of the tags
 *
 * @ptags: tags structure of the readen task
 * @result: a pointer for storing the read result
 * @uns: user namespace proxy
 *
 * Returns the count of byte read or the negative code -ENOMEM
 * if an allocation failed.
 */
static int _ptags_read(struct _ptags *ptags, char **result, struct uns uns)
{
	unsigned idx, count;
	size_t size;
	struct entry *entries, *entry;
	char *buffer;
	struct value *value;
	struct item *item;

	/* init loops */
	count = ptags->count;
	entries = ptags->entries;

	/* compute printed size */
	size = 0;
	for (idx = 0; idx < count; idx++) {
		entry = &entries[idx];
		value = entry_read(entry, uns);
		if (value) {
			item = value_get(*value);
			size += entry_name(*entry)->length
				+ (unsigned)value_is_kept(*value)
				+ (item ? 2 + item->length : 1);
		}
	}

	if (size > INT_MAX)
		return -E2BIG;
	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	/* print in the buffer */
	*result = buffer;
	for (idx = 0; idx < count; idx++) {
		entry = &entries[idx];
		value = entry_read(entry, uns);
		if (value) {
			if (value_is_kept(*value))
				*buffer++ = KEEP_CHAR;
			item = entry_name(*entry);
			memcpy(buffer, item->value, item->length);
			buffer += item->length;
			item = value_get(*value);
			if (item) {
				*buffer++ = ASSIGN_CHAR;
				memcpy(buffer, item->value, item->length);
				buffer += item->length;
			}
			*buffer++ = EOL_CHAR;
		}
	}

	return (int)size;
}

/*******************************************************************
 * section: ptags
 ******************************************************************/

/*
 * ptags_lock - Locks one ptags
 *
 * @ptags: the ptags to lock
 */
static inline void ptags_lock(struct ptags *ptags)
{
	mutex_lock(&ptags->lock);
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	_ptags_clean_nsrefs(&ptags->data);
#endif
}

/*
 * ptags_unlock - Unlocks one ptags
 *
 * @ptags: the ptags to unlock
 */
static inline void ptags_unlock(struct ptags *ptags)
{
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	if (ptags->data.wantgc)
		_ptags_collect_garbage(&ptags->data);
#endif
	mutex_unlock(&ptags->lock);
}

/*
 * ptags_lock2 - Locks 2 ptags avoiding deadlocks
 *
 * @ptags1: one of the ptags to lock
 * @ptags2: the other of the ptags to lock
 */
static inline void ptags_lock2(struct ptags *ptags1, struct ptags *ptags2)
{
	if ((uintptr_t) ptags1 < (uintptr_t) ptags2) {
		mutex_lock(&ptags1->lock);
		mutex_lock(&ptags2->lock);
	} else {
		mutex_lock(&ptags2->lock);
		mutex_lock(&ptags1->lock);
	}
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
	_ptags_clean_nsrefs(&ptags1->data);
	_ptags_clean_nsrefs(&ptags2->data);
#endif
}

/*
 * ptags_unlock2 - Unlocks 2 ptags locked together
 *
 * @ptags1: one of the ptags to unlock
 * @ptags2: the other of the ptags to unlock
 */
static inline void ptags_unlock2(struct ptags *ptags1, struct ptags *ptags2)
{
	ptags_unlock(ptags1);
	ptags_unlock(ptags2);
}

/**
 * ptags_write - Implement the writing of the tags
 *
 * @cptags: controling ptags
 * @mptags: modified ptags
 * @buffer: a pointer to the written data
 * @size: the size of the written data
 *
 * Returns the positive count of byte written. It can be less than the
 * count given by size if an error appears after. This count indicates
 * the count of data treated without errors.
 * Returns one of the negative error code below if the data begins on error:
 *   o -EINVAL if the name or the syntax is invalid
 *   o -EPERM if the addition is forbidden
 *   o -ENOMEM if the an allocation failed
 *   o -ENOENT if the query failed
 *   o -ECANCELED if the maximum count of tag is reached
 */
static int ptags_write(struct ptags *cptags, struct ptags *mptags,
		       const char *buffer, size_t size)
{
	int result;
	unsigned length;
	struct uns uns;

	/* crop the length */
	length = (size > (size_t) INT_MAX) ? INT_MAX : (unsigned)size;

	/* lock the ptags */
	uns = uns_get();
	if (!cptags || cptags == mptags) {
		ptags_lock(mptags);
		result = _ptags_write(cptags ? &cptags->data : NULL, &mptags->data, buffer, length, uns);
		ptags_unlock(mptags);
	} else {
		ptags_lock2(cptags, mptags);
		result = _ptags_write(&cptags->data, &mptags->data, buffer, length, uns);
		ptags_unlock2(cptags, mptags);
	}
	uns_put(uns);

	return result;
}

/**
 * ptags_read - Implement the reading of the tags
 *
 * @ptags: tags structure of the readen task
 * @data: a pointer for storing the read data
 *
 * Returns the count of byte read or the negative code -ENOMEM
 * if an allocation failed.
 */
static int ptags_read(struct ptags *ptags, char **data)
{
	int result;
	struct uns uns;

	uns = uns_get();
	ptags_lock(ptags);
	result = _ptags_read(&ptags->data, data, uns);
	ptags_unlock(ptags);
	uns_put(uns);

	return result;
}

/*
 * ptags_free - Frees ptags
 *
 * @ptags: the ptags to free
 */
static void ptags_free(struct ptags *ptags)
{
	if (ptags) {
		_ptags_erase(&ptags->data);
		kfree(ptags);
	}
}

/**
 * ptags_copy - Copies entries from 'src' to 'dst'
 *
 * @dst: destination ptags
 * @src: source ptags
 *
 * Returns 0 on success or -ENOMEM on memory allocation failure.
 */
static int ptags_copy(struct ptags *dst, struct ptags *src)
{
	int rc;

	ptags_lock2(dst, src);
	rc = _ptags_copy(&dst->data, &src->data);
	ptags_unlock2(dst, src);
	return rc;
}

/**
 * ptags_move - Transfers entries from 'src' to 'dst'
 *
 * @dst: destination ptags
 * @src: source ptags
 */
static void ptags_move(struct ptags *dst, struct ptags *src)
{
	ptags_lock2(dst, src);
	_ptags_move(&dst->data, &src->data);
	ptags_unlock2(dst, src);
}

/**
 * ptags_prune - Prunes from 'ptags' the entries not kept
 *
 * @ptags: the ptags to be puned
 */
static void ptags_prune(struct ptags *ptags)
{
	ptags_lock(ptags);
	_ptags_prune(&ptags->data);
	ptags_unlock(ptags);
}

/*
 * ptags_create - Creates and initializes the ptags structure
 *
 * Returns the created ptags.
 */
static struct ptags *ptags_create(void)
{
	struct ptags *ptags;

	ptags = kmalloc(sizeof *ptags, GFP_KERNEL);
	if (ptags) {
		mutex_init(&ptags->lock);
		_ptags_init(&ptags->data);
	}
	return ptags;
}


