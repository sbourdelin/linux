/*
 * security/caitsith/caitsith.h
 *
 * Copyright (C) 2005-2012  NTT DATA CORPORATION
 */

#ifndef _SECURITY_CAITSITH_INTERNAL_H
#define _SECURITY_CAITSITH_INTERNAL_H

#include <linux/security.h>
#include <linux/binfmts.h>
#include <linux/namei.h>
#include <linux/kthread.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/ctype.h> /* isdigit()/isxdigit() */
#include <linux/kmod.h>

/* Enumeration definition for internal use. */

/* Index numbers for "struct cs_condition". */
enum cs_conditions_index {
	CS_INVALID_CONDITION,
	CS_SELF_EXE,
	CS_COND_SARG0,
	CS_COND_SARG1,
	CS_IMM_NAME_ENTRY,
} __packed;

/* Index numbers for functionality. */
enum cs_mac_index {
	CS_MAC_EXECUTE,
	CS_MAC_MODIFY_POLICY,
	CS_MAX_MAC_INDEX,
} __packed;

/* Index numbers for statistic information. */
enum cs_memory_stat_type {
	CS_MEMORY_POLICY,
	CS_MAX_MEMORY_STAT
} __packed;

enum cs_matching_result {
	CS_MATCHING_UNMATCHED,
	CS_MATCHING_ALLOWED,
	CS_MATCHING_DENIED,
	CS_MAX_MATCHING
} __packed;

/* Index numbers for entry type. */
enum cs_policy_id {
	CS_ID_CONDITION,
	CS_ID_NAME,
	CS_ID_ACL,
	CS_MAX_POLICY
} __packed;

/* Index numbers for statistic information. */
enum cs_policy_stat_type {
	CS_STAT_POLICY_UPDATES,
	CS_STAT_REQUEST_DENIED,
	CS_MAX_POLICY_STAT
} __packed;

/* Index numbers for /sys/kernel/security/caitsith/ interfaces. */
enum cs_securityfs_interface_index {
	CS_POLICY,
	CS_VERSION,
} __packed;

/* Constants definition for internal use. */

/*
 * CaitSith uses this hash only when appending a string into the string table.
 * Frequency of appending strings is very low. So we don't need large (e.g.
 * 64k) hash size. 256 will be sufficient.
 */
#define CS_HASH_BITS 8
#define CS_MAX_HASH (1u << CS_HASH_BITS)

/* Size of temporary buffer for execve() operation. */
#define CS_EXEC_TMPSIZE     4096

/* Garbage collector is trying to kfree() this element. */
#define CS_GC_IN_PROGRESS -1

/* Size of read buffer for /sys/kernel/security/caitsith/ interface. */
#define CS_MAX_IO_READ_QUEUE 64

/* Structure definition for internal use. */

/* Common header for holding ACL entries. */
struct cs_acl_head {
	struct list_head list;
	s8 is_deleted; /* true or false or CS_GC_IN_PROGRESS */
} __packed;

/* Common header for shared entries. */
struct cs_shared_acl_head {
	struct list_head list;
	atomic_t users;
} __packed;

/* Common header for individual entries. */
struct cs_acl_info {
	struct list_head list;
	struct list_head acl_info_list;
	struct cs_condition *cond; /* Maybe NULL. */
	bool is_deleted;
	bool is_deny;
	u16 priority;
};

/* Structure for entries which follows "struct cs_condition". */
union cs_condition_element {
	struct {
		enum cs_conditions_index left;
		enum cs_conditions_index right;
		bool is_not;
	};
	const struct cs_path_info *path;
};

/* Structure for optional arguments. */
struct cs_condition {
	struct cs_shared_acl_head head;
	u32 size; /* Memory size allocated for this entry. */
	/* union cs_condition_element condition[]; */
};

/* Structure for holding a token. */
struct cs_path_info {
	const char *name;
	u32 hash;          /* = full_name_hash(name, strlen(name)) */
	u32 total_len;     /* = strlen(name)                       */
	u32 const_len;     /* = cs_const_part_length(name)        */
};

/* Structure for request info. */
struct cs_request_info {
	/* For holding parameters. */
	struct cs_request_param {
		const struct cs_path_info *s[2];
	} param;
	/* For holding pathnames and attributes. */
	struct {
		/* Pointer to file objects. */
		struct path path[2];
		/*
		 * Name of @path[0] and @path[1].
		 * Cleared by cs_clear_request_info().
		 */
		struct cs_path_info pathname[2];
	} obj;
	struct {
		struct linux_binprm *bprm;
		/* For temporary use. Size is CS_EXEC_TMPSIZE bytes. */
		char *tmp;
	};
	/*
	 * Name of current thread's executable.
	 * Cleared by cs_clear_request_info().
	 */
	struct cs_path_info exename;
	/*
	 * Matching "struct cs_acl_info" is copied. Used for caitsith-queryd.
	 * Valid until cs_read_unlock().
	 */
	struct cs_acl_info *matched_acl;
	/*
	 * For holding operation index used for this request.
	 * One of values in "enum cs_mac_index".
	 */
	enum cs_mac_index type;
	/* For holding matching result. */
	enum cs_matching_result result;
	/*
	 * Set to true if condition could not be checked due to out of memory.
	 * This flag is used for returning out of memory flag back to
	 * cs_check_acl_list(). Thus, this flag will not be set if out of
	 * memory occurred before cs_check_acl_list() is called.
	 */
	bool failed_by_oom;
};

/* Structure for holding string data. */
struct cs_name {
	struct cs_shared_acl_head head;
	int size; /* Memory size allocated for this entry. */
	struct cs_path_info entry;
};

/*
 * Structure for reading/writing policy via /sys/kernel/security/caitsith/
 * interfaces.
 */
struct cs_io_buffer {
	/* Exclusive lock for this structure.   */
	struct mutex io_sem;
	char __user *read_user_buf;
	size_t read_user_buf_avail;
	struct {
		struct list_head *acl;
		struct list_head *subacl;
		const union cs_condition_element *cond;
		size_t avail;
		unsigned int step;
		u16 index;
		u8 cond_step;
		u8 w_pos;
		enum cs_mac_index acl_index;
		bool eof;
		bool version_done;
		bool stat_done;
		const char *w[CS_MAX_IO_READ_QUEUE];
	} r;
	struct {
		char *data;
		struct cs_acl_info *acl;
		size_t avail;
		enum cs_mac_index acl_index;
		bool is_delete;
		bool is_deny;
		u16 priority;
	} w;
	/* Buffer for reading.                  */
	char *read_buf;
	/* Size of read buffer.                 */
	size_t readbuf_size;
	/* Buffer for writing.                  */
	char *write_buf;
	/* Size of write buffer.                */
	size_t writebuf_size;
	/* Type of interface. */
	enum cs_securityfs_interface_index type;
	/* Users counter protected by cs_io_buffer_list_lock. */
	u8 users;
	/* List for telling GC not to kfree() elements. */
	struct list_head list;
};

/* Structure for representing YYYY/MM/DD hh/mm/ss. */
struct cs_time {
	u16 year;
	u8 month;
	u8 day;
	u8 hour;
	u8 min;
	u8 sec;
};

/* Prototype definition for internal use. */

void __init cs_init_module(void);
void cs_load_policy(const char *filename);
void cs_check_profile(void);
bool cs_get_exename(struct cs_path_info *buf);
bool cs_manager(void);
char *cs_encode(const char *str);
char *cs_realpath(const struct path *path);
char *cs_get_exe(void);
int cs_audit_log(struct cs_request_info *r);
int cs_check_acl(struct cs_request_info *r, const bool clear);
void cs_del_condition(struct list_head *element);
void cs_fill_path_info(struct cs_path_info *ptr);
void cs_notify_gc(struct cs_io_buffer *head, const bool is_register);
void cs_populate_patharg(struct cs_request_info *r, const bool first);
void cs_warn_oom(const char *function);
int cs_start_execve(struct linux_binprm *bprm);

/* Variable definition for internal use. */

extern bool cs_policy_loaded;
extern struct cs_path_info cs_null_name;
extern struct list_head cs_acl_list[CS_MAX_MAC_INDEX];
extern struct list_head cs_condition_list;
extern struct list_head cs_name_list[CS_MAX_HASH];
extern struct mutex cs_policy_lock;
extern struct srcu_struct cs_ss;
extern unsigned int cs_memory_used[CS_MAX_MEMORY_STAT];

/* Inlined functions for internal use. */

/**
 * cs_pathcmp - strcmp() for "struct cs_path_info" structure.
 *
 * @a: Pointer to "struct cs_path_info".
 * @b: Pointer to "struct cs_path_info".
 *
 * Returns true if @a != @b, false otherwise.
 */
static inline bool cs_pathcmp(const struct cs_path_info *a,
			      const struct cs_path_info *b)
{
	return a->hash != b->hash || strcmp(a->name, b->name);
}

/**
 * cs_read_lock - Take lock for protecting policy.
 *
 * Returns index number for cs_read_unlock().
 */
static inline int cs_read_lock(void)
{
	return srcu_read_lock(&cs_ss);
}

/**
 * cs_read_unlock - Release lock for protecting policy.
 *
 * @idx: Index number returned by cs_read_lock().
 *
 * Returns nothing.
 */
static inline void cs_read_unlock(const int idx)
{
	srcu_read_unlock(&cs_ss, idx);
}

/**
 * cs_put_condition - Drop reference on "struct cs_condition".
 *
 * @cond: Pointer to "struct cs_condition". Maybe NULL.
 *
 * Returns nothing.
 */
static inline void cs_put_condition(struct cs_condition *cond)
{
	if (cond)
		atomic_dec(&cond->head.users);
}

/**
 * cs_put_name - Drop reference on "struct cs_name".
 *
 * @name: Pointer to "struct cs_path_info". Maybe NULL.
 *
 * Returns nothing.
 */
static inline void cs_put_name(const struct cs_path_info *name)
{
	if (name)
		atomic_dec(&container_of(name, struct cs_name, entry)->
			   head.users);
}

#endif
