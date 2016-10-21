/*
 * security/caitsith/policy_io.c
 *
 * Copyright (C) 2005-2012  NTT DATA CORPORATION
 */

#include <linux/lsm_hooks.h>
#include "caitsith.h"

/* Define this to enable debug mode. */
/* #define DEBUG_CONDITION */

#ifdef DEBUG_CONDITION
#define dprintk printk
#else
#define dprintk(...) do { } while (0)
#endif

/* String table for operation. */
static const char * const cs_mac_keywords[CS_MAX_MAC_INDEX] = {
	[CS_MAC_EXECUTE] = "execute",
	[CS_MAC_MODIFY_POLICY] = "modify_policy",
};

/* String table for stat info. */
static const char * const cs_memory_headers[CS_MAX_MEMORY_STAT] = {
	[CS_MEMORY_POLICY] = "policy",
};

#define F(bit) (1U << bit)

enum cs_var_type {
	CS_TYPE_INVALID,
	CS_TYPE_STRING,
} __packed;

/* String table for conditions. */
static const struct {
	const char * const keyword;
	const enum cs_var_type left_type;
	const enum cs_var_type right_type;
	const enum cs_conditions_index cmd;
	const u8 available;
} cs_conditions[] = {
	{ "exec", CS_TYPE_STRING, CS_TYPE_STRING, CS_COND_SARG1,
	  F(CS_MAC_EXECUTE) },
	{ "path", CS_TYPE_STRING, CS_TYPE_STRING, CS_COND_SARG0,
	  F(CS_MAC_EXECUTE) },
	{ "task.exe", CS_TYPE_STRING, CS_TYPE_STRING, CS_SELF_EXE,
	  F(CS_MAC_EXECUTE) | F(CS_MAC_MODIFY_POLICY) },
};

/* Structure for holding single condition component. */
struct cs_cond_tmp {
	enum cs_conditions_index left;
	enum cs_conditions_index right;
	bool is_not;
	enum cs_var_type type;
	const struct cs_path_info *path;
};

static bool cs_correct_word(const char *string);
static bool cs_flush(struct cs_io_buffer *head);
static bool cs_print_condition(struct cs_io_buffer *head,
			       const struct cs_condition *cond);
static bool cs_memory_ok(const void *ptr);
static bool cs_read_acl(struct cs_io_buffer *head,
			const struct cs_acl_info *acl);
static bool cs_set_lf(struct cs_io_buffer *head);
static char *cs_read_token(struct cs_io_buffer *head);
static const struct cs_path_info *cs_get_dqword(char *start);
static const struct cs_path_info *cs_get_name(const char *name);
static int cs_open(struct inode *inode, struct file *file);
static int cs_parse_policy(struct cs_io_buffer *head, char *line);
static int cs_release(struct inode *inode, struct file *file);
static int cs_write_policy(struct cs_io_buffer *head);
static ssize_t cs_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos);
static ssize_t cs_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos);
static struct cs_condition *cs_get_condition(struct cs_io_buffer *head);
static void __init cs_create_entry(const char *name, const umode_t mode,
				   struct dentry *parent, const u8 key);
static void __init cs_load_builtin_policy(void);
static int __init cs_securityfs_init(void);
static void cs_convert_time(time_t time, struct cs_time *stamp);
static void cs_io_printf(struct cs_io_buffer *head, const char *fmt, ...)
	__printf(2, 3);
static void cs_normalize_line(unsigned char *buffer);
static void cs_read_policy(struct cs_io_buffer *head);
static void *cs_commit_ok(void *data, const unsigned int size);
static void cs_read_stat(struct cs_io_buffer *head);
static void cs_read_version(struct cs_io_buffer *head);
static void cs_set_space(struct cs_io_buffer *head);
static void cs_set_string(struct cs_io_buffer *head, const char *string);
static void cs_update_stat(const u8 index);

/**
 * cs_convert_time - Convert time_t to YYYY/MM/DD hh/mm/ss.
 *
 * @time:  Seconds since 1970/01/01 00:00:00.
 * @stamp: Pointer to "struct cs_time".
 *
 * Returns nothing.
 *
 * This function does not handle Y2038 problem.
 */
static void cs_convert_time(time_t time, struct cs_time *stamp)
{
	static const u16 cs_eom[2][12] = {
		{ 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
		{ 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
	};
	u16 y;
	u8 m;
	bool r;

	stamp->sec = time % 60;
	time /= 60;
	stamp->min = time % 60;
	time /= 60;
	stamp->hour = time % 24;
	time /= 24;
	for (y = 1970; ; y++) {
		const unsigned short days = (y & 3) ? 365 : 366;

		if (time < days)
			break;
		time -= days;
	}
	r = (y & 3) == 0;
	for (m = 0; m < 11 && time >= cs_eom[r][m]; m++)
		;
	if (m)
		time -= cs_eom[r][m - 1];
	stamp->year = y;
	stamp->month = ++m;
	stamp->day = ++time;
}

/* Lock for protecting policy. */
DEFINE_MUTEX(cs_policy_lock);

/* Has /sbin/init started? */
bool cs_policy_loaded;

/* Policy version. Currently only 20120401 is defined. */
static unsigned int cs_policy_version;

/* List of "struct cs_condition". */
LIST_HEAD(cs_condition_list);

/* Memoy currently used by policy. */
unsigned int cs_memory_used[CS_MAX_MEMORY_STAT];

/* The list for "struct cs_name". */
struct list_head cs_name_list[CS_MAX_HASH];

/* Timestamp counter for last updated. */
static unsigned int cs_stat_updated[CS_MAX_POLICY_STAT];

/* Counter for number of updates. */
static unsigned int cs_stat_modified[CS_MAX_POLICY_STAT];

/* Operations for /sys/kernel/security/caitsith/ interface. */
static const struct file_operations cs_operations = {
	.open    = cs_open,
	.release = cs_release,
	.read    = cs_read,
	.write   = cs_write,
};

/**
 * list_for_each_cookie - iterate over a list with cookie.
 *
 * @pos:  Pointer to "struct list_head".
 * @head: Pointer to "struct list_head".
 */
#define list_for_each_cookie(pos, head)					\
	for (pos = pos ? pos : srcu_dereference((head)->next, &cs_ss);	\
	     pos != (head); pos = srcu_dereference(pos->next, &cs_ss))

/**
 * cs_warn_oom - Print out of memory warning message.
 *
 * @function: Function's name.
 *
 * Returns nothing.
 */
void cs_warn_oom(const char *function)
{
	/* Reduce error messages. */
	static pid_t cs_last_pid;
	const pid_t pid = current->pid;

	if (cs_last_pid != pid) {
		printk(KERN_WARNING "ERROR: Out of memory at %s.\n",
		       function);
		cs_last_pid = pid;
	}
	if (!cs_policy_loaded)
		panic("MAC Initialization failed.\n");
}

/**
 * cs_memory_ok - Check memory quota.
 *
 * @ptr:  Pointer to allocated memory. Maybe NULL.
 *
 * Returns true if @ptr is not NULL and quota not exceeded, false otherwise.
 *
 * Caller holds cs_policy_lock mutex.
 */
static bool cs_memory_ok(const void *ptr)
{
	if (ptr) {
		cs_memory_used[CS_MEMORY_POLICY] += ksize(ptr);
		return true;
	}
	cs_warn_oom(__func__);
	return false;
}

/**
 * cs_get_name - Allocate memory for string data.
 *
 * @name: The string to store into the permernent memory. Maybe NULL.
 *
 * Returns pointer to "struct cs_path_info" on success, NULL otherwise.
 */
static const struct cs_path_info *cs_get_name(const char *name)
{
	struct cs_name *ptr;
	unsigned int hash;
	int len;
	int allocated_len;
	struct list_head *head;

	if (!name)
		return NULL;
	len = strlen(name) + 1;
	hash = full_name_hash(NULL, name, len - 1);
	head = &cs_name_list[hash_long(hash, CS_HASH_BITS)];
	if (mutex_lock_interruptible(&cs_policy_lock))
		return NULL;
	list_for_each_entry(ptr, head, head.list) {
		if (hash != ptr->entry.hash || strcmp(name, ptr->entry.name) ||
		    atomic_read(&ptr->head.users) == CS_GC_IN_PROGRESS)
			continue;
		atomic_inc(&ptr->head.users);
		goto out;
	}
	allocated_len = sizeof(*ptr) + len;
	ptr = kzalloc(allocated_len, GFP_NOFS);
	if (cs_memory_ok(ptr)) {
		ptr->entry.name = ((char *) ptr) + sizeof(*ptr);
		memmove((char *) ptr->entry.name, name, len);
		atomic_set(&ptr->head.users, 1);
		cs_fill_path_info(&ptr->entry);
		ptr->size = allocated_len;
		list_add_tail(&ptr->head.list, head);
	} else {
		kfree(ptr);
		ptr = NULL;
	}
out:
	mutex_unlock(&cs_policy_lock);
	return ptr ? &ptr->entry : NULL;
}

/**
 * cs_read_token - Read a word from a line.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns a word on success, "" otherwise.
 *
 * To allow the caller to skip NULL check, this function returns "" rather than
 * NULL if there is no more words to read.
 */
static char *cs_read_token(struct cs_io_buffer *head)
{
	char *pos = head->w.data;
	char *del = strchr(pos, ' ');

	if (del)
		*del++ = '\0';
	else
		del = pos + strlen(pos);
	head->w.data = del;
	return pos;
}

/**
 * cs_correct_word - Check whether the given string follows the naming rules.
 *
 * @string: The string to check.
 *
 * Returns true if @string follows the naming rules, false otherwise.
 */
static bool cs_correct_word(const char *string)
{
	u8 recursion = 20;
	const char *const start = string;
	u8 in_repetition = 0;

	if (!*string)
		goto out;
	while (*string) {
		unsigned char c = *string++;

		if (in_repetition && c == '/')
			goto out;
		if (c <= ' ' || c >= 127)
			goto out;
		if (c != '\\')
			continue;
		c = *string++;
		if (c >= '0' && c <= '3') {
			unsigned char d;
			unsigned char e;

			d = *string++;
			if (d < '0' || d > '7')
				goto out;
			e = *string++;
			if (e < '0' || e > '7')
				goto out;
			c = ((c - '0') << 6) + ((d - '0') << 3) + (e - '0');
			if (c <= ' ' || c >= 127 || c == '\\')
				continue;
			goto out;
		}
		switch (c) {
		case '+':   /* "\+" */
		case '?':   /* "\?" */
		case 'x':   /* "\x" */
		case 'a':   /* "\a" */
		case '-':   /* "\-" */
			continue;
		}
		/* Reject too deep wildcard that consumes too much stack. */
		if (!recursion--)
			goto out;
		switch (c) {
		case '*':   /* "\*" */
		case '@':   /* "\@" */
		case '$':   /* "\$" */
		case 'X':   /* "\X" */
		case 'A':   /* "\A" */
			continue;
		case '{':   /* "/\{" */
			if (string - 3 < start || *(string - 3) != '/')
				goto out;
			in_repetition = 1;
			continue;
		case '}':   /* "\}/" */
			if (in_repetition != 1 || *string++ != '/')
				goto out;
			in_repetition = 0;
			continue;
		case '(':   /* "/\(" */
			if (string - 3 < start || *(string - 3) != '/')
				goto out;
			in_repetition = 2;
			continue;
		case ')':   /* "\)/" */
			if (in_repetition != 2 || *string++ != '/')
				goto out;
			in_repetition = 0;
			continue;
		}
		goto out;
	}
	if (in_repetition)
		goto out;
	return true;
out:
	return false;
}

/**
 * cs_commit_ok - Allocate memory and check memory quota.
 *
 * @data: Data to copy from.
 * @size: Size in byte.
 *
 * Returns pointer to allocated memory on success, NULL otherwise.
 * @data is zero-cleared on success.
 *
 * Caller holds cs_policy_lock mutex.
 */
static void *cs_commit_ok(void *data, const unsigned int size)
{
	void *ptr = kmalloc(size, GFP_NOFS);

	if (cs_memory_ok(ptr)) {
		memmove(ptr, data, size);
		memset(data, 0, size);
		return ptr;
	}
	kfree(ptr);
	return NULL;
}

/**
 * cs_get_dqword - cs_get_name() for a quoted string.
 *
 * @start: String to parse.
 *
 * Returns pointer to "struct cs_path_info" on success, NULL otherwise.
 */
static const struct cs_path_info *cs_get_dqword(char *start)
{
	char *cp = start + strlen(start) - 1;

	if (cp == start || *start++ != '"' || *cp != '"')
		return NULL;
	*cp = '\0';
	if (*start && !cs_correct_word(start))
		return NULL;
	return cs_get_name(start);
}

/**
 * cs_same_condition - Check for duplicated "struct cs_condition" entry.
 *
 * @a: Pointer to "struct cs_condition".
 * @b: Pointer to "struct cs_condition".
 *
 * Returns true if @a == @b, false otherwise.
 */
static inline bool cs_same_condition(const struct cs_condition *a,
				     const struct cs_condition *b)
{
	return a->size == b->size &&
		!memcmp(a + 1, b + 1, a->size - sizeof(*a));
}

/**
 * cs_commit_condition - Commit "struct cs_condition".
 *
 * @entry: Pointer to "struct cs_condition".
 *
 * Returns pointer to "struct cs_condition" on success, NULL otherwise.
 *
 * This function merges duplicated entries. This function returns NULL if
 * @entry is not duplicated but memory quota for policy has exceeded.
 */
static struct cs_condition *cs_commit_condition(struct cs_condition *entry)
{
	struct cs_condition *ptr = kmemdup(entry, entry->size, GFP_NOFS);
	bool found = false;

	if (ptr) {
		kfree(entry);
		entry = ptr;
	}
	if (mutex_lock_interruptible(&cs_policy_lock)) {
		dprintk(KERN_WARNING "%u: %s failed\n", __LINE__, __func__);
		ptr = NULL;
		found = true;
		goto out;
	}
	list_for_each_entry(ptr, &cs_condition_list, head.list) {
		if (!cs_same_condition(ptr, entry) ||
		    atomic_read(&ptr->head.users) == CS_GC_IN_PROGRESS)
			continue;
		/* Same entry found. Share this entry. */
		atomic_inc(&ptr->head.users);
		found = true;
		break;
	}
	if (!found) {
		if (cs_memory_ok(entry)) {
			atomic_set(&entry->head.users, 1);
			list_add(&entry->head.list, &cs_condition_list);
		} else {
			found = true;
			ptr = NULL;
		}
	}
	mutex_unlock(&cs_policy_lock);
out:
	if (found) {
		cs_del_condition(&entry->head.list);
		kfree(entry);
		entry = ptr;
	}
	return entry;
}

/**
 * cs_normalize_line - Format string.
 *
 * @buffer: The line to normalize.
 *
 * Returns nothing.
 *
 * Leading and trailing whitespaces are removed.
 * Multiple whitespaces are packed into single space.
 */
static void cs_normalize_line(unsigned char *buffer)
{
	unsigned char *sp = buffer;
	unsigned char *dp = buffer;
	bool first = true;

	while (*sp && (*sp <= ' ' || *sp >= 127))
		sp++;
	while (*sp) {
		if (!first)
			*dp++ = ' ';
		first = false;
		while (*sp > ' ' && *sp < 127)
			*dp++ = *sp++;
		while (*sp && (*sp <= ' ' || *sp >= 127))
			sp++;
	}
	*dp = '\0';
}

/**
 * cs_parse_righthand - Parse special righthand conditions.
 *
 * @word: Keyword to search.
 * @head: Pointer to "struct cs_io_buffer".
 * @tmp:  Pointer to "struct cs_cond_tmp".
 *
 * Returns one of values in "enum cs_conditions_index".
 */
static enum cs_conditions_index cs_parse_righthand
(char *word, struct cs_io_buffer *head, struct cs_cond_tmp *tmp)
{
	const enum cs_var_type type = tmp->type;

	dprintk(KERN_WARNING "%u: tmp->left=%u type=%u\n",
		__LINE__, tmp->left, type);
	if (type == CS_TYPE_STRING) {
		dprintk(KERN_WARNING "%u: word='%s'\n", __LINE__, word);
		if (!strcmp(word, "NULL"))
			goto null_word;
		tmp->path = cs_get_dqword(word);
		dprintk(KERN_WARNING "%u: tmp->path=%p\n", __LINE__,
			tmp->path);
		if (tmp->path)
			return CS_IMM_NAME_ENTRY;
		goto out;
	}
out:
	dprintk(KERN_WARNING "%u: righthand failed\n", __LINE__);
	return CS_INVALID_CONDITION;
null_word:
	tmp->path = &cs_null_name;
	return CS_IMM_NAME_ENTRY;
}

/**
 * cs_condindex - Get condition's index.
 *
 * @word: Name of condition.
 * @mac:  One of values in "enum cs_mac_index".
 * @tmp:  Pointer to "struct cs_cond_tmp".
 * @left: True if lefthand part, false otherwise.
 *
 * Returns one of values in "enum cs_condition_index".
 */
static enum cs_conditions_index cs_condindex(const char *word,
					     const enum cs_mac_index mac,
					     struct cs_cond_tmp *tmp,
					     const bool lefthand)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs_conditions); i++) {
		if (!(cs_conditions[i].available & F(mac)) ||
		    strcmp(cs_conditions[i].keyword, word))
			continue;
		tmp->type = lefthand ? cs_conditions[i].left_type :
			cs_conditions[i].right_type;
		if (tmp->type != CS_TYPE_INVALID)
			return cs_conditions[i].cmd;
		break;
	}
	return CS_INVALID_CONDITION;
}

/**
 * cs_parse_cond - Parse single condition.
 *
 * @tmp:  Pointer to "struct cs_cond_tmp".
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns true on success, false otherwise.
 */
static bool cs_parse_cond(struct cs_cond_tmp *tmp,
			  struct cs_io_buffer *head)
{
	char *left = head->w.data;
	char *right;
	const enum cs_mac_index mac = head->w.acl_index;
	enum cs_var_type type = CS_TYPE_STRING;

	dprintk(KERN_WARNING "%u: type=%u word='%s'\n",
		__LINE__, mac, left);
	right = strchr(left, '=');
	if (!right || right == left)
		return false;
	*right++ = '\0';
	tmp->is_not = (*(right - 2) == '!');
	if (tmp->is_not)
		*(right - 2) = '\0';
	if (!*left || !*right)
		return false;
	tmp->left = cs_condindex(left, mac, tmp, true);
	dprintk(KERN_WARNING "%u: tmp->left=%u\n", __LINE__, tmp->left);
	if (tmp->left == CS_INVALID_CONDITION)
		return false;
	type = tmp->type;
	dprintk(KERN_WARNING "%u: tmp->type=%u\n", __LINE__, tmp->type);
	tmp->right = cs_condindex(right, mac, tmp, false);
	dprintk(KERN_WARNING "%u: tmp->right=%u tmp->type=%u\n",
		__LINE__, tmp->right, tmp->type);
	if (tmp->right != CS_INVALID_CONDITION && type != tmp->type)
		return false;
	if (tmp->right == CS_INVALID_CONDITION)
		tmp->right = cs_parse_righthand(right, head, tmp);
	dprintk(KERN_WARNING "%u: tmp->right=%u tmp->type=%u\n",
		__LINE__, tmp->right, tmp->type);
	return tmp->right != CS_INVALID_CONDITION;
}

/**
 * cs_get_condition - Parse condition part.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns pointer to "struct cs_condition" on success, NULL otherwise.
 */
struct cs_condition *cs_get_condition(struct cs_io_buffer *head)
{
	struct cs_condition *entry = kzalloc(PAGE_SIZE, GFP_NOFS);
	union cs_condition_element *condp;
	struct cs_cond_tmp tmp;
#ifdef DEBUG_CONDITION
	const enum cs_mac_index type = head->w.acl_index;
#endif
	char *pos = head->w.data;

	if (!entry)
		return NULL;
	condp = (union cs_condition_element *) (entry + 1);
	while (1) {
		memset(&tmp, 0, sizeof(tmp));
		while (*pos == ' ')
			pos++;
		if (!*pos)
			break;
		if ((u8 *) condp >= ((u8 *) entry) + PAGE_SIZE
		    - (sizeof(*condp) * 2))
			goto out;
		{
			char *next = strchr(pos, ' ');

			if (next)
				*next++ = '\0';
			else
				next = "";
			head->w.data = pos;
			pos = next;
		}
		if (!cs_parse_cond(&tmp, head))
			goto out;
		condp->is_not = tmp.is_not;
		condp->left = tmp.left;
		condp->right = tmp.right;
		condp++;
		if (tmp.right == CS_IMM_NAME_ENTRY) {
			condp->path = tmp.path;
			condp++;
		}
	}
	entry->size = (void *) condp - (void *) entry;
	return cs_commit_condition(entry);
out:
	dprintk(KERN_WARNING "%u: bad condition: type=%u path='%s'\n",
		__LINE__, type, tmp.path ? tmp.path->name : "");
	if (tmp.path != &cs_null_name)
		cs_put_name(tmp.path);
	entry->size = (void *) condp - (void *) entry;
	cs_del_condition(&entry->head.list);
	kfree(entry);
	return NULL;
}

/**
 * cs_flush - Flush queued string to userspace's buffer.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns true if all data was flushed, false otherwise.
 */
static bool cs_flush(struct cs_io_buffer *head)
{
	while (head->r.w_pos) {
		const char *w = head->r.w[0];
		size_t len = strlen(w);

		if (len) {
			if (len > head->read_user_buf_avail)
				len = head->read_user_buf_avail;
			if (!len)
				return false;
			if (copy_to_user(head->read_user_buf, w, len))
				return false;
			head->read_user_buf_avail -= len;
			head->read_user_buf += len;
			w += len;
		}
		head->r.w[0] = w;
		if (*w)
			return false;
		head->r.w_pos--;
		for (len = 0; len < head->r.w_pos; len++)
			head->r.w[len] = head->r.w[len + 1];
	}
	head->r.avail = 0;
	return true;
}

/**
 * cs_set_string - Queue string to "struct cs_io_buffer" structure.
 *
 * @head:   Pointer to "struct cs_io_buffer".
 * @string: String to print.
 *
 * Returns nothing.
 *
 * Note that @string has to be kept valid until @head is kfree()d.
 * This means that char[] allocated on stack memory cannot be passed to
 * this function. Use cs_io_printf() for char[] allocated on stack memory.
 */
static void cs_set_string(struct cs_io_buffer *head, const char *string)
{
	if (head->r.w_pos < CS_MAX_IO_READ_QUEUE) {
		head->r.w[head->r.w_pos++] = string;
		cs_flush(head);
	} else
		printk(KERN_WARNING "Too many words in a line.\n");
}

/**
 * cs_io_printf - printf() to "struct cs_io_buffer" structure.
 *
 * @head: Pointer to "struct cs_io_buffer".
 * @fmt:  The printf()'s format string, followed by parameters.
 *
 * Returns nothing.
 */
static void cs_io_printf(struct cs_io_buffer *head, const char *fmt, ...)
{
	va_list args;
	size_t len;
	size_t pos = head->r.avail;
	int size = head->readbuf_size - pos;

	if (size <= 0)
		return;
	va_start(args, fmt);
	len = vsnprintf(head->read_buf + pos, size, fmt, args) + 1;
	va_end(args);
	if (pos + len >= head->readbuf_size) {
		printk(KERN_WARNING "Too many words in a line.\n");
		return;
	}
	head->r.avail += len;
	cs_set_string(head, head->read_buf + pos);
}

/**
 * cs_set_space - Put a space to "struct cs_io_buffer" structure.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns nothing.
 */
static void cs_set_space(struct cs_io_buffer *head)
{
	cs_set_string(head, " ");
}

/**
 * cs_set_lf - Put a line feed to "struct cs_io_buffer" structure.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns true if all data was flushed, false otherwise.
 */
static bool cs_set_lf(struct cs_io_buffer *head)
{
	cs_set_string(head, "\n");
	return !head->r.w_pos;
}

/**
 * cs_check_profile - Check policy is loaded.
 *
 * Returns nothing.
 */
void cs_check_profile(void)
{
	cs_policy_loaded = true;
	printk(KERN_INFO "CaitSith 2016/09/25\n");
	if (cs_policy_version == 20120401) {
		printk(KERN_INFO "CaitSith module activated.\n");
		return;
	}
	printk(KERN_ERR "Policy version %u is not supported.\n",
	       cs_policy_version);
	printk(KERN_ERR "Userland tools for CaitSith must be installed and policy must be initialized.\n");
	printk(KERN_ERR "Please see https://caitsith.osdn.jp/ for more information.\n");
	panic("STOP!");
}

/**
 * cs_update_acl - Update "struct cs_acl_info" entry.
 *
 * @list:   Pointer to "struct list_head".
 * @head:   Pointer to "struct cs_io_buffer".
 * @update: True to store matching entry, false otherwise.
 *
 * Returns 0 on success, negative value otherwise.
 *
 * Caller holds cs_read_lock().
 */
static int cs_update_acl(struct list_head * const list,
			 struct cs_io_buffer *head, const bool update)
{
	struct cs_acl_info *ptr;
	struct cs_acl_info new_entry = { };
	const bool is_delete = head->w.is_delete;
	int error = is_delete ? -ENOENT : -ENOMEM;

	new_entry.priority = head->w.priority;
	new_entry.is_deny = head->w.is_deny;
	if (head->w.data[0]) {
		new_entry.cond = cs_get_condition(head);
		if (!new_entry.cond)
			return -EINVAL;
	}
	if (mutex_lock_interruptible(&cs_policy_lock))
		goto out;
	list_for_each_entry_rcu(ptr, list, list) {
		if (ptr->priority > new_entry.priority)
			break;
		/*
		 * We cannot reuse deleted "struct cs_acl_info" entry because
		 * somebody might be referencing children of this deleted entry
		 * from srcu section. We cannot delete children of this deleted
		 * entry until all children are no longer referenced. Thus, let
		 * the garbage collector wait and delete rather than trying to
		 * reuse this deleted entry.
		 */
		if (ptr->is_deleted || ptr->cond != new_entry.cond ||
		    ptr->priority != new_entry.priority ||
		    ptr->is_deny != new_entry.is_deny)
			continue;
		ptr->is_deleted = is_delete;
		if (!is_delete && update)
			head->w.acl = ptr;
		error = 0;
		break;
	}
	if (error && !is_delete) {
		struct cs_acl_info *entry =
			cs_commit_ok(&new_entry, sizeof(new_entry));

		if (entry) {
			INIT_LIST_HEAD(&entry->acl_info_list);
			list_add_tail_rcu(&entry->list, &ptr->list);
			if (update)
				head->w.acl = entry;
		}
	}
	mutex_unlock(&cs_policy_lock);
out:
	cs_put_condition(new_entry.cond);
	return error;
}

/**
 * cs_parse_entry - Update ACL entry.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns 0 on success, negative value otherwise.
 *
 * Caller holds cs_read_lock().
 */
static int cs_parse_entry(struct cs_io_buffer *head)
{
	enum cs_mac_index type;
	const char *operation = cs_read_token(head);

	for (type = 0; type < CS_MAX_MAC_INDEX; type++) {
		if (strcmp(operation, cs_mac_keywords[type]))
			continue;
		head->w.acl_index = type;
		return cs_update_acl(&cs_acl_list[type], head, true);
	}
	return -EINVAL;
}

/**
 * cs_condword - Get condition's name.
 *
 * @type: One of values in "enum cs_mac_index".
 * @cond: One of values in "enum cs_condition_index".
 *
 * Returns condition's name.
 */
static const char *cs_condword(const enum cs_mac_index type,
			       const enum cs_conditions_index cond)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs_conditions); i++) {
		if (!(cs_conditions[i].available & F(type)) ||
		    cs_conditions[i].cmd != cond)
			continue;
		return cs_conditions[i].keyword;
	}
	return "unknown"; /* This should not happen. */
}

/**
 * cs_print_condition_loop - Print condition part.
 *
 * @head: Pointer to "struct cs_io_buffer".
 * @cond: Pointer to "struct cs_condition".
 *
 * Returns true on success, false otherwise.
 */
static bool cs_print_condition_loop(struct cs_io_buffer *head,
				    const struct cs_condition *cond)
{
	const enum cs_mac_index type = head->r.acl_index;
	const union cs_condition_element *condp = head->r.cond;

	while ((void *) condp < (void *) ((u8 *) cond) + cond->size) {
		const bool is_not = condp->is_not;
		const enum cs_conditions_index left = condp->left;
		const enum cs_conditions_index right = condp->right;

		if (!cs_flush(head)) {
			head->r.cond = condp;
			return false;
		}
		condp++;
		cs_set_space(head);
		cs_set_string(head, cs_condword(type, left));
		cs_set_string(head, is_not ? "!=" : "=");
		switch (right) {
		case CS_IMM_NAME_ENTRY:
			if (condp->path != &cs_null_name) {
				cs_set_string(head, "\"");
				cs_set_string(head, condp->path->name);
				cs_set_string(head, "\"");
			} else {
				cs_set_string(head, "NULL");
			}
			condp++;
			break;
		default:
			cs_set_string(head, cs_condword(type, right));
		}
	}
	head->r.cond = NULL;
	return true;
}

/**
 * cs_print_condition - Print condition part.
 *
 * @head: Pointer to "struct cs_io_buffer".
 * @cond: Pointer to "struct cs_condition".
 *
 * Returns true on success, false otherwise.
 */
static bool cs_print_condition(struct cs_io_buffer *head,
			       const struct cs_condition *cond)
{
	switch (head->r.cond_step) {
	case 0:
		head->r.cond = (const union cs_condition_element *)
			(cond + 1);
		head->r.cond_step++;
		/* fall through */
	case 1:
		if (!cs_print_condition_loop(head, cond))
			return false;
		head->r.cond_step++;
		/* fall through */
	case 2:
		head->r.cond = NULL;
		return true;
	}
	return false;
}

/**
 * cs_read_acl - Print an ACL entry.
 *
 * @head: Pointer to "struct cs_io_buffer".
 * @acl:  Pointer to an ACL entry.
 *
 * Returns true on success, false otherwise.
 */
static bool cs_read_acl(struct cs_io_buffer *head,
			const struct cs_acl_info *acl)
{
	const enum cs_mac_index type = head->r.acl_index;

	if (head->r.cond)
		goto print_cond_part;
	if (acl->is_deleted)
		return true;
	if (!cs_flush(head))
		return false;
	cs_io_printf(head, "%u ", acl->priority);
	cs_set_string(head, "acl ");
	cs_set_string(head, cs_mac_keywords[type]);
	if (acl->cond) {
		head->r.cond_step = 0;
print_cond_part:
		if (!cs_print_condition(head, acl->cond))
			return false;
	}
	cs_set_lf(head);
	return true;
}

/**
 * cs_write_policy - Write policy.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int cs_write_policy(struct cs_io_buffer *head)
{
	unsigned int priority;
	char *word = cs_read_token(head);

	if (sscanf(word, "%u", &priority) == 1)
		word = cs_read_token(head);
	else
		priority = 1000;
	if (priority >= 65536 || !*word)
		return -EINVAL;
	head->w.priority = priority;
	if (!head->w.acl)
		goto no_acl_selected;
	head->w.is_deny = !strcmp(word, "deny");
	if (head->w.is_deny || !strcmp(word, "allow"))
		return cs_update_acl(&head->w.acl->acl_info_list, head,
				     false);
	head->w.acl = NULL;
no_acl_selected:
	if (!strcmp(word, "acl"))
		return cs_parse_entry(head);
	if (sscanf(word, "POLICY_VERSION=%u", &cs_policy_version) == 1)
		return 0;
	return -EINVAL;
}

/**
 * cs_audit_log - Audit permission check log.
 *
 * @r: Pointer to "struct cs_request_info".
 *
 * Returns 0 to grant the request, CS_RETRY_REQUEST to retry the permission
 * check, -EPERM otherwise.
 */
int cs_audit_log(struct cs_request_info *r)
{
	/* Do not reject if not yet activated. */
	if (!cs_policy_loaded)
		return 0;
	/* Nothing more to do unless denied. */
	if (r->result != CS_MATCHING_DENIED)
		return 0;
	/* Update policy violation counter if denied. */
	cs_update_stat(CS_STAT_REQUEST_DENIED);
	return -EPERM;
}

/**
 * cs_read_version - Get version.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns nothing.
 */
static void cs_read_version(struct cs_io_buffer *head)
{
	if (head->r.eof)
		return;
	cs_set_string(head, "2016/09/25");
	head->r.eof = true;
}

/**
 * cs_update_stat - Update statistic counters.
 *
 * @index: Index for policy type.
 *
 * Returns nothing.
 */
static void cs_update_stat(const u8 index)
{
	/*
	 * I don't use atomic operations because race condition is not fatal.
	 */
	cs_stat_updated[index]++;
	cs_stat_modified[index] = get_seconds();
}

/**
 * cs_read_stat - Read statistic data.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Returns nothing.
 */
static void cs_read_stat(struct cs_io_buffer *head)
{
	u8 i;

	for (i = 0; i < CS_MAX_POLICY_STAT; i++) {
		static const char * const k[CS_MAX_POLICY_STAT] = {
			[CS_STAT_POLICY_UPDATES] = "Policy updated:",
			[CS_STAT_REQUEST_DENIED] = "Requests denied:",
		};

		cs_io_printf(head, "stat %s %u", k[i], cs_stat_updated[i]);
		if (cs_stat_modified[i]) {
			struct cs_time stamp;

			cs_convert_time(cs_stat_modified[i], &stamp);
			cs_io_printf(head,
				     " (Last: %04u/%02u/%02u %02u:%02u:%02u)",
				     stamp.year, stamp.month, stamp.day,
				     stamp.hour, stamp.min, stamp.sec);
		}
		cs_set_lf(head);
	}
	for (i = 0; i < CS_MAX_MEMORY_STAT; i++)
		cs_io_printf(head, "stat Memory used by %s: %u\n",
			     cs_memory_headers[i], cs_memory_used[i]);
}

/**
 * cs_parse_policy - Parse a policy line.
 *
 * @head: Pointer to "struct cs_io_buffer".
 * @line: Line to parse.
 *
 * Returns 0 on success, negative value otherwise.
 *
 * Caller holds cs_read_lock().
 */
static int cs_parse_policy(struct cs_io_buffer *head, char *line)
{
	/* Set current line's content. */
	head->w.data = line;
	head->w.is_deny = false;
	head->w.priority = 0;
	/* Delete request? */
	head->w.is_delete = !strncmp(line, "delete ", 7);
	if (head->w.is_delete)
		memmove(line, line + 7, strlen(line + 7) + 1);
	/* Do the update. */
	return cs_write_policy(head);
}

/**
 * cs_load_builtin_policy - Load built-in policy.
 *
 * Returns nothing.
 */
static void __init cs_load_builtin_policy(void)
{
	/*
	 * This include file is manually created and contains built-in policy.
	 *
	 * static char [] __initdata cs_builtin_policy = { ... };
	 */
#include "builtin-policy.h"
	const int idx = cs_read_lock();
	struct cs_io_buffer head = { };
	char *start = cs_builtin_policy;

	head.type = CS_POLICY;
	while (1) {
		char *end = strchr(start, '\n');

		if (!end)
			break;
		*end = '\0';
		cs_normalize_line(start);
		head.write_buf = start;
		cs_parse_policy(&head, start);
		start = end + 1;
	}
	cs_read_unlock(idx);
#ifdef CONFIG_SECURITY_CAITSITH_OMIT_USERSPACE_LOADER
	cs_check_profile();
#endif
}

/**
 * cs_read_subacl - Read sub ACL in ACL entry.
 *
 * @head: Pointer to "struct cs_io_buffer".
 * @list: Pointer to "struct list_head".
 *
 * Returns true on success, false otherwise.
 *
 * Caller holds cs_read_lock().
 */
static bool cs_read_subacl(struct cs_io_buffer *head,
			   const struct list_head *list)
{
	list_for_each_cookie(head->r.subacl, list) {
		struct cs_acl_info *acl =
			list_entry(head->r.subacl, typeof(*acl), list);

		switch (head->r.step) {
		case 3:
			if (acl->is_deleted)
				continue;
			if (!cs_flush(head))
				return false;
			cs_io_printf(head, "    %u ", acl->priority);
			if (acl->is_deny)
				cs_set_string(head, "deny");
			else
				cs_set_string(head, "allow");
			head->r.cond_step = 0;
			head->r.step++;
			/* fall through */
		case 4:
			if (!cs_flush(head))
				return false;
			if (acl->cond &&
			    !cs_print_condition(head, acl->cond))
				return false;
			cs_set_lf(head);
			head->r.step--;
		}
	}
	head->r.subacl = NULL;
	return true;
}

/**
 * cs_read_policy - Read policy.
 *
 * @head: Pointer to "struct cs_io_buffer".
 *
 * Caller holds cs_read_lock().
 */
static void cs_read_policy(struct cs_io_buffer *head)
{
	if (head->r.eof)
		return;
	if (!head->r.version_done) {
		cs_io_printf(head, "POLICY_VERSION=%u\n", cs_policy_version);
		head->r.version_done = true;
	}
	if (!head->r.stat_done) {
		cs_read_stat(head);
		head->r.stat_done = true;
		cs_set_lf(head);
	}
	while (head->r.acl_index < CS_MAX_MAC_INDEX) {
		list_for_each_cookie(head->r.acl,
				     &cs_acl_list[head->r.acl_index]) {
			struct cs_acl_info *ptr;

			ptr = list_entry(head->r.acl, typeof(*ptr), list);
			switch (head->r.step) {
			case 0:
				if (ptr->is_deleted)
					continue;
				head->r.step++;
				/* fall through */
			case 1:
				if (!cs_read_acl(head, ptr))
					return;
				head->r.step++;
				/* fall through */
			case 2:
				if (!cs_flush(head))
					return;
				head->r.step++;
				/* fall through */
			case 3:
			case 4:
				if (!cs_read_subacl(head, &ptr->acl_info_list))
					return;
				head->r.step = 5;
				/* fall through */
			case 5:
				if (!cs_flush(head))
					return;
				cs_set_lf(head);
				head->r.step = 0;
			}
		}
		head->r.acl = NULL;
		head->r.acl_index++;
	}
	head->r.eof = true;
}

/**
 * cs_open - open() for /sys/kernel/security/caitsith/ interface.
 *
 * @inode: Pointer to "struct inode".
 * @file:  Pointer to "struct file".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int cs_open(struct inode *inode, struct file *file)
{
	const u8 type = (unsigned long) inode->i_private;
	struct cs_io_buffer *head = kzalloc(sizeof(*head), GFP_NOFS);

	if (!head)
		return -ENOMEM;
	mutex_init(&head->io_sem);
	head->type = type;
	if (file->f_mode & FMODE_READ) {
		head->readbuf_size = 4096;
		head->read_buf = kzalloc(head->readbuf_size, GFP_NOFS);
		if (!head->read_buf) {
			kfree(head);
			return -ENOMEM;
		}
	}
	if (file->f_mode & FMODE_WRITE) {
		head->writebuf_size = 4096;
		head->write_buf = kzalloc(head->writebuf_size, GFP_NOFS);
		if (!head->write_buf) {
			kfree(head->read_buf);
			kfree(head);
			return -ENOMEM;
		}
	}
	file->private_data = head;
	cs_notify_gc(head, true);
	return 0;
}

/**
 * cs_release - close() for /sys/kernel/security/caitsith/ interface.
 *
 * @inode: Pointer to "struct inode".
 * @file:  Pointer to "struct file".
 *
 * Returns 0.
 */
static int cs_release(struct inode *inode, struct file *file)
{
	struct cs_io_buffer *head = file->private_data;

	cs_notify_gc(head, false);
	return 0;
}

/**
 * cs_read - read() for /sys/kernel/security/caitsith/ interface.
 *
 * @file:  Pointer to "struct file".
 * @buf:   Pointer to buffer.
 * @count: Size of @buf.
 * @ppos:  Unused.
 *
 * Returns bytes read on success, negative value otherwise.
 */
static ssize_t cs_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	struct cs_io_buffer *head = file->private_data;
	int len;
	int idx;

	if (mutex_lock_interruptible(&head->io_sem))
		return -EINTR;
	head->read_user_buf = buf;
	head->read_user_buf_avail = count;
	idx = cs_read_lock();
	if (cs_flush(head)) {
		/* Call the policy handler. */
		switch (head->type) {
		case CS_VERSION:
			cs_read_version(head);
			break;
		case CS_POLICY:
			cs_read_policy(head);
			break;
		}
		cs_flush(head);
	}
	cs_read_unlock(idx);
	len = head->read_user_buf - buf;
	mutex_unlock(&head->io_sem);
	return len;
}

/**
 * cs_write - write() for /sys/kernel/security/caitsith/ interface.
 *
 * @file:  Pointer to "struct file".
 * @buf:   Pointer to buffer.
 * @count: Size of @buf.
 * @ppos:  Unused.
 *
 * Returns @count on success, negative value otherwise.
 */
static ssize_t cs_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	struct cs_io_buffer *head = file->private_data;
	int error = count;
	char *cp0 = head->write_buf;
	int idx;

	if (head->type != CS_POLICY)
		return -EIO;
	if (mutex_lock_interruptible(&head->io_sem))
		return -EINTR;
	head->read_user_buf_avail = 0;
	idx = cs_read_lock();
	/* Read a line and dispatch it to the policy handler. */
	while (count) {
		char c;

		if (head->w.avail >= head->writebuf_size - 1) {
			const int len = head->writebuf_size * 2;
			char *cp = kzalloc(len, GFP_NOFS);

			if (!cp) {
				error = -ENOMEM;
				break;
			}
			memmove(cp, cp0, head->w.avail);
			kfree(cp0);
			head->write_buf = cp;
			cp0 = cp;
			head->writebuf_size = len;
		}
		if (get_user(c, buf)) {
			error = -EFAULT;
			break;
		}
		buf++;
		count--;
		cp0[head->w.avail++] = c;
		if (c != '\n')
			continue;
		cp0[head->w.avail - 1] = '\0';
		head->w.avail = 0;
		cs_normalize_line(cp0);
		/* Don't allow updating policies by non manager programs. */
		if (!cs_manager()) {
			error = -EPERM;
			goto out;
		}
		if (cs_parse_policy(head, cp0) == 0)
			/* Update statistics. */
			cs_update_stat(CS_STAT_POLICY_UPDATES);
	}
out:
	cs_read_unlock(idx);
	mutex_unlock(&head->io_sem);
	return error;
}

/**
 * cs_create_entry - Create interface files under /sys/kernel/security/caitsith/ directory.
 *
 * @name:   The name of the interface file.
 * @mode:   The permission of the interface file.
 * @parent: The parent directory.
 * @key:    Type of interface.
 *
 * Returns nothing.
 */
static void __init cs_create_entry(const char *name, const umode_t mode,
				   struct dentry *parent, const u8 key)
{
	securityfs_create_file(name, S_IFREG | mode, parent,
			       (void *) (unsigned long) key, &cs_operations);
}

/**
 * cs_securityfs_init - Initialize /sys/kernel/security/caitsith/ interface.
 *
 * Returns 0.
 */
static int __init cs_securityfs_init(void)
{
	struct dentry *cs_dir;

	if (!security_module_enable("caitsith"))
		return 0;
	cs_dir = securityfs_create_dir("caitsith", NULL);
	cs_create_entry("version", 0400, cs_dir, CS_VERSION);
	cs_create_entry("policy", 0600, cs_dir, CS_POLICY);
	cs_load_builtin_policy();
	return 0;
}

fs_initcall(cs_securityfs_init);

/**
 * cs_init_module - Initialize this module.
 *
 * Returns nothing.
 */
void __init cs_init_module(void)
{
	u16 idx;

#ifdef DEBUG_CONDITION
	for (idx = 0; idx < CS_MAX_MAC_INDEX; idx++) {
		if (cs_mac_keywords[idx])
			continue;
		panic("cs_mac_keywords[%u]==NULL\n", idx);
	}
#endif
	if (init_srcu_struct(&cs_ss))
		panic("Out of memory.");
	for (idx = 0; idx < CS_MAX_MAC_INDEX; idx++)
		INIT_LIST_HEAD(&cs_acl_list[idx]);
	for (idx = 0; idx < CS_MAX_HASH; idx++)
		INIT_LIST_HEAD(&cs_name_list[idx]);
	cs_null_name.name = "NULL";
	cs_fill_path_info(&cs_null_name);
}
