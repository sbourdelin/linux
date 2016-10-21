/*
 * security/caitsith/permission.c
 *
 * Copyright (C) 2005-2012  NTT DATA CORPORATION
 */

#include "caitsith.h"

/* Type of condition argument. */
enum cs_arg_type {
	CS_ARG_TYPE_NONE,
	CS_ARG_TYPE_NAME,
} __packed;

/* Structure for holding single condition component. */
struct cs_cond_arg {
	enum cs_arg_type type;
	const struct cs_path_info *name;
};

static bool cs_alphabet_char(const char c);
static bool cs_byte_range(const char *str);
static bool cs_check_entry(struct cs_request_info *r,
			   const struct cs_acl_info *ptr);
static bool cs_condition(struct cs_request_info *r,
			 const struct cs_condition *cond);
static bool cs_file_matches_pattern(const char *filename,
				    const char *filename_end,
				    const char *pattern,
				    const char *pattern_end);
static bool cs_file_matches_pattern2(const char *filename,
				     const char *filename_end,
				     const char *pattern,
				     const char *pattern_end);
static bool cs_path_matches_pattern(const struct cs_path_info *filename,
				    const struct cs_path_info *pattern);
static bool cs_path_matches_pattern2(const char *f, const char *p);
static int cs_execute_path(struct linux_binprm *bprm, struct path *path);
static int cs_execute(struct cs_request_info *r);
static void cs_clear_request_info(struct cs_request_info *r);

/* The list for ACL policy. */
struct list_head cs_acl_list[CS_MAX_MAC_INDEX];

/* NULL value. */
struct cs_path_info cs_null_name;

/**
 * cs_check_entry - Do permission check.
 *
 * @r:   Pointer to "struct cs_request_info".
 * @ptr: Pointer to "struct cs_acl_info".
 *
 * Returns true on match, false otherwise.
 *
 * Caller holds cs_read_lock().
 */
static bool cs_check_entry(struct cs_request_info *r,
			   const struct cs_acl_info *ptr)
{
	return !ptr->is_deleted && cs_condition(r, ptr->cond);
}

/**
 * cs_check_acl_list - Do permission check.
 *
 * @r: Pointer to "struct cs_request_info".
 *
 * Returns 0 on success, negative value otherwise.
 *
 * Caller holds cs_read_lock().
 */
static int cs_check_acl_list(struct cs_request_info *r)
{
	struct cs_acl_info *ptr;
	int error = 0;
	struct list_head * const list = &cs_acl_list[r->type];

	r->matched_acl = NULL;
	list_for_each_entry_rcu(ptr, list, list) {
		struct cs_acl_info *ptr2;

		if (!cs_check_entry(r, ptr)) {
			if (unlikely(r->failed_by_oom))
				goto oom;
			continue;
		}
		r->matched_acl = ptr;
		r->result = CS_MATCHING_UNMATCHED;
		list_for_each_entry_rcu(ptr2, &ptr->acl_info_list, list) {
			if (!cs_check_entry(r, ptr2)) {
				if (unlikely(r->failed_by_oom))
					goto oom;
				continue;
			}
			r->result = ptr2->is_deny ?
				CS_MATCHING_DENIED : CS_MATCHING_ALLOWED;
			break;
		}
		error = cs_audit_log(r);
		/* Ignore out of memory during audit. */
		r->failed_by_oom = false;
		if (error)
			break;
	}
	return error;
oom:
	/*
	 * If conditions could not be checked due to out of memory,
	 * reject the request with -ENOMEM, for we don't know whether
	 * there was a possibility of matching "deny" lines or not.
	 */
	{
		static unsigned long cs_last_oom;
		unsigned long oom = get_seconds();

		if (oom != cs_last_oom) {
			cs_last_oom = oom;
			printk(KERN_INFO "CaitSith: Rejecting access request due to out of memory.\n");
		}
	}
	return -ENOMEM;
}

/**
 * cs_check_acl - Do permission check.
 *
 * @r:     Pointer to "struct cs_request_info".
 * @clear: True to cleanup @r before return, false otherwise.
 *
 * Returns 0 on success, negative value otherwise.
 */
int cs_check_acl(struct cs_request_info *r, const bool clear)
{
	int error;
	const int idx = cs_read_lock();

	error = cs_check_acl_list(r);
	cs_read_unlock(idx);
	if (clear)
		cs_clear_request_info(r);
	return error;
}

/**
 * cs_execute - Check permission for "execute".
 *
 * @r: Pointer to "struct cs_request_info".
 *
 * Returns 0 on success, negative value otherwise.
 *
 * Caller holds cs_read_lock().
 */
static int cs_execute(struct cs_request_info *r)
{
	int retval;

	/* Get symlink's dentry/vfsmount. */
	retval = cs_execute_path(r->bprm, &r->obj.path[1]);
	if (retval < 0)
		return retval;
	cs_populate_patharg(r, false);
	if (!r->param.s[1])
		return -ENOMEM;

	/* Check execute permission. */
	r->type = CS_MAC_EXECUTE;
	return cs_check_acl(r, false);
}

/**
 * cs_start_execve - Prepare for execve() operation.
 *
 * @bprm: Pointer to "struct linux_binprm".
 *
 * Returns 0 on success, negative value otherwise.
 */
int cs_start_execve(struct linux_binprm *bprm)
{
	int retval;
	struct cs_request_info r = { };
	int idx;

	r.tmp = kzalloc(CS_EXEC_TMPSIZE, GFP_NOFS);
	if (!r.tmp)
		return -ENOMEM;
	idx = cs_read_lock();
	r.bprm = bprm;
	r.obj.path[0] = bprm->file->f_path;
	retval = cs_execute(&r);
	cs_clear_request_info(&r);
	/* Drop refcount obtained by cs_execute_path(). */
	if (r.obj.path[1].dentry) {
		path_put(&r.obj.path[1]);
		r.obj.path[1].dentry = NULL;
	}
	cs_read_unlock(idx);
	kfree(r.tmp);
	return retval;
}

/**
 * cs_execute_path - Get dentry/vfsmount of a program.
 *
 * @bprm: Pointer to "struct linux_binprm".
 * @path: Pointer to "struct path".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int cs_execute_path(struct linux_binprm *bprm, struct path *path)
{
	/*
	 * Follow symlinks if the requested pathname is on procfs, for
	 * /proc/\$/exe is meaningless.
	 */
	const unsigned int follow =
		(bprm->file->f_path.dentry->d_sb->s_magic == PROC_SUPER_MAGIC)
		? LOOKUP_FOLLOW : 0;

	if (kern_path(bprm->filename, follow, path))
		return -ENOENT;
	return 0;
}

/**
 * cs_manager - Check whether the current process is a policy manager.
 *
 * Returns true if the current process is permitted to modify policy
 * via /sys/kernel/security/caitsith/ interface.
 *
 * Caller holds cs_read_lock().
 */
bool cs_manager(void)
{
	if (!cs_policy_loaded)
		return true;
	{
		struct cs_request_info r = { };

		r.type = CS_MAC_MODIFY_POLICY;
		if (cs_check_acl(&r, true) == 0)
			return true;
	}
	{ /* Reduce error messages. */
		static pid_t cs_last_pid;
		const pid_t pid = current->pid;

		if (cs_last_pid != pid) {
			const char *exe = cs_get_exe();

			printk(KERN_WARNING "'%s' (pid=%u) is not permitted to update policies.\n",
			       exe, pid);
			cs_last_pid = pid;
			kfree(exe);
		}
	}
	return false;
}

/**
 * cs_populate_patharg - Calculate pathname for permission check and audit logs.
 *
 * @r:     Pointer to "struct cs_request_info".
 * @first: True for first pathname, false for second pathname.
 *
 * Returns nothing.
 */
void cs_populate_patharg(struct cs_request_info *r, const bool first)
{
	struct cs_path_info *buf = &r->obj.pathname[!first];
	struct path *path = &r->obj.path[!first];

	if (!buf->name && path->dentry) {
		buf->name = cs_realpath(path);
		/* Set OOM flag if failed. */
		if (!buf->name) {
			r->failed_by_oom = true;
			return;
		}
		cs_fill_path_info(buf);
	}
	if (!r->param.s[!first] && buf->name)
		r->param.s[!first] = buf;
}

/**
 * cs_cond2arg - Assign values to condition variables.
 *
 * @arg:   Pointer to "struct cs_cond_arg".
 * @cmd:   One of values in "enum cs_conditions_index".
 * @condp: Pointer to "union cs_condition_element *".
 * @r:     Pointer to "struct cs_request_info".
 *
 * Returns true on success, false othwerwise.
 *
 * This function should not fail. But it can fail if (for example) out of
 * memory has occurred while calculating cs_populate_patharg() or
 * cs_get_exename().
 */
static bool cs_cond2arg(struct cs_cond_arg *arg,
			const enum cs_conditions_index cmd,
			const union cs_condition_element **condp,
			struct cs_request_info *r)
{
	switch (cmd) {
	case CS_COND_SARG0:
		if (!r->param.s[0])
			cs_populate_patharg(r, true);
		arg->name = r->param.s[0];
		break;
	case CS_COND_SARG1:
		if (!r->param.s[1])
			cs_populate_patharg(r, false);
		arg->name = r->param.s[1];
		break;
	case CS_IMM_NAME_ENTRY:
		arg->name = (*condp)->path;
		(*condp)++;
		break;
	case CS_SELF_EXE:
		if (!r->exename.name) {
			cs_get_exename(&r->exename);
			/* Set OOM flag if failed. */
			if (!r->exename.name)
				r->failed_by_oom = true;
		}
		arg->name = &r->exename;
		break;
	default:
		arg->name = NULL;
		break;
	}
	if (!arg->name)
		return false;
	arg->type = CS_ARG_TYPE_NAME;
	return true;
}

/**
 * cs_condition - Check condition part.
 *
 * @r:    Pointer to "struct cs_request_info".
 * @cond: Pointer to "struct cs_condition". Maybe NULL.
 *
 * Returns true on success, false otherwise.
 *
 * Caller holds cs_read_lock().
 */
static bool cs_condition(struct cs_request_info *r,
			 const struct cs_condition *cond)
{
	const union cs_condition_element *condp;

	if (!cond)
		return true;
	condp = (typeof(condp)) (cond + 1);
	while ((void *) condp < (void *) ((u8 *) cond) + cond->size) {
		struct cs_cond_arg left;
		struct cs_cond_arg right;
		const enum cs_conditions_index left_op = condp->left;
		const enum cs_conditions_index right_op = condp->right;
		const bool match = !condp->is_not;

		condp++;
		if (!cs_cond2arg(&left, left_op, &condp, r) ||
		    !cs_cond2arg(&right, right_op, &condp, r))
			/*
			 * Something wrong (e.g. out of memory or invalid
			 * argument) occurred. We can't check permission.
			 */
			return false;
		if (left.type == CS_ARG_TYPE_NAME) {
			if (right.type != CS_ARG_TYPE_NAME)
				return false;
			if (cs_path_matches_pattern
			    (left.name, right.name) == match)
				continue;
		}
		return false;
	}
	return true;
}

/**
 * cs_byte_range - Check whether the string is a \ooo style octal value.
 *
 * @str: Pointer to the string.
 *
 * Returns true if @str is a \ooo style octal value, false otherwise.
 */
static bool cs_byte_range(const char *str)
{
	return *str >= '0' && *str++ <= '3' &&
		*str >= '0' && *str++ <= '7' &&
		*str >= '0' && *str <= '7';
}

/**
 * cs_alphabet_char - Check whether the character is an alphabet.
 *
 * @c: The character to check.
 *
 * Returns true if @c is an alphabet character, false otherwise.
 */
static bool cs_alphabet_char(const char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/**
 * cs_file_matches_pattern2 - Pattern matching without '/' character and "\-" pattern.
 *
 * @filename:     The start of string to check.
 * @filename_end: The end of string to check.
 * @pattern:      The start of pattern to compare.
 * @pattern_end:  The end of pattern to compare.
 *
 * Returns true if @filename matches @pattern, false otherwise.
 */
static bool cs_file_matches_pattern2(const char *filename,
				     const char *filename_end,
				     const char *pattern,
				     const char *pattern_end)
{
	while (filename < filename_end && pattern < pattern_end) {
		char c;

		if (*pattern != '\\') {
			if (*filename++ != *pattern++)
				return false;
			continue;
		}
		c = *filename;
		pattern++;
		switch (*pattern) {
			int i;
			int j;
		case '?':
			if (c == '/') {
				return false;
			} else if (c == '\\') {
				if (cs_byte_range(filename + 1))
					filename += 3;
				else
					return false;
			}
			break;
		case '+':
			if (!isdigit(c))
				return false;
			break;
		case 'x':
			if (!isxdigit(c))
				return false;
			break;
		case 'a':
			if (!cs_alphabet_char(c))
				return false;
			break;
		case '0':
		case '1':
		case '2':
		case '3':
			if (c == '\\' && cs_byte_range(filename + 1)
			    && !strncmp(filename + 1, pattern, 3)) {
				filename += 3;
				pattern += 2;
				break;
			}
			return false; /* Not matched. */
		case '*':
		case '@':
			for (i = 0; i <= filename_end - filename; i++) {
				if (cs_file_matches_pattern2(filename + i,
							     filename_end,
							     pattern + 1,
							     pattern_end))
					return true;
				c = filename[i];
				if (c == '.' && *pattern == '@')
					break;
				if (c != '\\')
					continue;
				if (cs_byte_range(filename + i + 1))
					i += 3;
				else
					break; /* Bad pattern. */
			}
			return false; /* Not matched. */
		default:
			j = 0;
			c = *pattern;
			if (c == '$') {
				while (isdigit(filename[j]))
					j++;
			} else if (c == 'X') {
				while (isxdigit(filename[j]))
					j++;
			} else if (c == 'A') {
				while (cs_alphabet_char(filename[j]))
					j++;
			}
			for (i = 1; i <= j; i++) {
				if (cs_file_matches_pattern2(filename + i,
							     filename_end,
							     pattern + 1,
							     pattern_end))
					return true;
			}
			return false; /* Not matched or bad pattern. */
		}
		filename++;
		pattern++;
	}
	/* Ignore trailing "\*" and "\@" in @pattern. */
	while (*pattern == '\\' &&
	       (*(pattern + 1) == '*' || *(pattern + 1) == '@'))
		pattern += 2;
	return filename == filename_end && pattern == pattern_end;
}

/**
 * cs_file_matches_pattern - Pattern matching without '/' character.
 *
 * @filename:     The start of string to check.
 * @filename_end: The end of string to check.
 * @pattern:      The start of pattern to compare.
 * @pattern_end:  The end of pattern to compare.
 *
 * Returns true if @filename matches @pattern, false otherwise.
 */
static bool cs_file_matches_pattern(const char *filename,
				    const char *filename_end,
				    const char *pattern,
				    const char *pattern_end)
{
	const char *pattern_start = pattern;
	bool first = true;
	bool result;

	while (pattern < pattern_end - 1) {
		/* Split at "\-" pattern. */
		if (*pattern++ != '\\' || *pattern++ != '-')
			continue;
		result = cs_file_matches_pattern2(filename, filename_end,
						  pattern_start, pattern - 2);
		if (first)
			result = !result;
		if (result)
			return false;
		first = false;
		pattern_start = pattern;
	}
	result = cs_file_matches_pattern2(filename, filename_end,
					  pattern_start, pattern_end);
	return first ? result : !result;
}

/**
 * cs_path_matches_pattern2 - Do pathname pattern matching.
 *
 * @f: The start of string to check.
 * @p: The start of pattern to compare.
 *
 * Returns true if @f matches @p, false otherwise.
 */
static bool cs_path_matches_pattern2(const char *f, const char *p)
{
	const char *f_delimiter;
	const char *p_delimiter;

	while (*f && *p) {
		f_delimiter = strchr(f + 1, '/');
		if (!f_delimiter)
			f_delimiter = f + strlen(f);
		p_delimiter = strchr(p + 1, '/');
		if (!p_delimiter)
			p_delimiter = p + strlen(p);
		if (*p == '/' && *(p + 1) == '\\') {
			if (*(p + 2) == '(') {
				/* Check zero repetition. */
				if (cs_path_matches_pattern2(f, p_delimiter))
					return true;
				/* Check one or more repetition. */
				goto repetition;
			}
			if (*(p + 2) == '{')
				goto repetition;
		}
		if ((*f == '/' || *p == '/') && *f++ != *p++)
			return false;
		if (!cs_file_matches_pattern(f, f_delimiter, p, p_delimiter))
			return false;
		f = f_delimiter;
		p = p_delimiter;
	}
	/* Ignore trailing "\*" and "\@" in @pattern. */
	while (*p == '\\' && (*(p + 1) == '*' || *(p + 1) == '@'))
		p += 2;
	return !*f && !*p;
repetition:
	do {
		/* Compare current component with pattern. */
		if (!cs_file_matches_pattern(f + 1, f_delimiter, p + 3,
					     p_delimiter - 2))
			break;
		/* Proceed to next component. */
		f = f_delimiter;
		if (!*f)
			break;
		/* Continue comparison. */
		if (cs_path_matches_pattern2(f, p_delimiter))
			return true;
		f_delimiter = strchr(f + 1, '/');
	} while (f_delimiter);
	return false; /* Not matched. */
}

/**
 * cs_path_matches_pattern - Check whether the given filename matches the given pattern.
 *
 * @filename: The filename to check.
 * @pattern:  The pattern to compare.
 *
 * Returns true if matches, false otherwise.
 *
 * The following patterns are available.
 *   \ooo   Octal representation of a byte.
 *   \*     Zero or more repetitions of characters other than '/'.
 *   \@     Zero or more repetitions of characters other than '/' or '.'.
 *   \?     1 byte character other than '/'.
 *   \$     One or more repetitions of decimal digits.
 *   \+     1 decimal digit.
 *   \X     One or more repetitions of hexadecimal digits.
 *   \x     1 hexadecimal digit.
 *   \A     One or more repetitions of alphabet characters.
 *   \a     1 alphabet character.
 *
 *   \-     Subtraction operator.
 *
 *   /\{dir\}/   '/' + 'One or more repetitions of dir/' (e.g. /dir/ /dir/dir/
 *               /dir/dir/dir/ ).
 *
 *   /\(dir\)/   '/' + 'Zero or more repetitions of dir/' (e.g. / /dir/
 *               /dir/dir/ ).
 */
static bool cs_path_matches_pattern(const struct cs_path_info *filename,
				    const struct cs_path_info *pattern)
{
	const char *f = filename->name;
	const char *p = pattern->name;
	const int len = pattern->const_len;

	/* If @pattern doesn't contain pattern, I can use strcmp(). */
	if (len == pattern->total_len)
		return !cs_pathcmp(filename, pattern);
	/* Compare the initial length without patterns. */
	if (len) {
		if (strncmp(f, p, len))
			return false;
		f += len - 1;
		p += len - 1;
	}
	return cs_path_matches_pattern2(f, p);
}

/**
 * cs_clear_request_info - Release memory allocated during permission check.
 *
 * @r: Pointer to "struct cs_request_info".
 *
 * Returns nothing.
 */
static void cs_clear_request_info(struct cs_request_info *r)
{
	u8 i;

	/*
	 * r->obj.pathname[0] (which is referenced by r->obj.s[0]) and
	 * r->obj.pathname[1] (which is referenced by r->obj.s[1]) may contain
	 * pathnames allocated using cs_populate_patharg().
	 * Their callers do not allocate memory until pathnames becomes needed
	 * for checking condition.
	 */
	for (i = 0; i < 2; i++) {
		kfree(r->obj.pathname[i].name);
		r->obj.pathname[i].name = NULL;
	}
	kfree(r->exename.name);
	r->exename.name = NULL;
}
