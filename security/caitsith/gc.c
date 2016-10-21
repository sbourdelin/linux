/*
 * security/caitsith/gc.c
 *
 * Copyright (C) 2005-2012  NTT DATA CORPORATION
 */

#include "caitsith.h"

/* The list for "struct cs_io_buffer". */
static LIST_HEAD(cs_io_buffer_list);
/* Lock for protecting cs_io_buffer_list. */
static DEFINE_SPINLOCK(cs_io_buffer_list_lock);

static bool cs_name_used_by_io_buffer(const char *string, const size_t size);
static bool cs_struct_used_by_io_buffer(const struct list_head *element);
static int cs_gc_thread(void *unused);
static void cs_collect_acl(struct list_head *list);
static void cs_collect_entry(void);
static void cs_try_to_gc(const enum cs_policy_id type,
			 struct list_head *element);

/*
 * Lock for syscall users.
 *
 * This lock is held for only protecting single SRCU section.
 */
struct srcu_struct cs_ss;

/**
 * cs_struct_used_by_io_buffer - Check whether the list element is used by /sys/kernel/security/caitsith/ users or not.
 *
 * @element: Pointer to "struct list_head".
 *
 * Returns true if @element is used by /sys/kernel/security/caitsith/ users,
 * false otherwise.
 */
static bool cs_struct_used_by_io_buffer(const struct list_head *element)
{
	struct cs_io_buffer *head;
	bool in_use = false;

	spin_lock(&cs_io_buffer_list_lock);
	list_for_each_entry(head, &cs_io_buffer_list, list) {
		head->users++;
		spin_unlock(&cs_io_buffer_list_lock);
		mutex_lock(&head->io_sem);
		if (head->r.acl == element || head->r.subacl == element ||
		    &head->w.acl->list == element)
			in_use = true;
		mutex_unlock(&head->io_sem);
		spin_lock(&cs_io_buffer_list_lock);
		head->users--;
		if (in_use)
			break;
	}
	spin_unlock(&cs_io_buffer_list_lock);
	return in_use;
}

/**
 * cs_name_used_by_io_buffer - Check whether the string is used by /sys/kernel/security/caitsith/ users or not.
 *
 * @string: String to check.
 * @size:   Memory allocated for @string .
 *
 * Returns true if @string is used by /sys/kernel/security/caitsith/ users,
 * false otherwise.
 */
static bool cs_name_used_by_io_buffer(const char *string, const size_t size)
{
	struct cs_io_buffer *head;
	bool in_use = false;

	spin_lock(&cs_io_buffer_list_lock);
	list_for_each_entry(head, &cs_io_buffer_list, list) {
		int i;

		head->users++;
		spin_unlock(&cs_io_buffer_list_lock);
		mutex_lock(&head->io_sem);
		for (i = 0; i < CS_MAX_IO_READ_QUEUE; i++) {
			const char *w = head->r.w[i];

			if (w < string || w > string + size)
				continue;
			in_use = true;
			break;
		}
		mutex_unlock(&head->io_sem);
		spin_lock(&cs_io_buffer_list_lock);
		head->users--;
		if (in_use)
			break;
	}
	spin_unlock(&cs_io_buffer_list_lock);
	return in_use;
}

/**
 * cs_acl_info_has_sub_acl - Clear "struct cs_acl_info"->acl_info.
 *
 * @list: Pointer to "struct list_head".
 *
 * Returns true if @list is not empty, false otherwise.
 */
static bool cs_acl_info_has_sub_acl(struct list_head *list)
{
	struct cs_acl_info *acl;
	struct cs_acl_info *tmp;

	if (list_empty(list))
		return false;
	mutex_lock(&cs_policy_lock);
	list_for_each_entry_safe(acl, tmp, list, list) {
		cs_try_to_gc(CS_ID_ACL, &acl->list);
	}
	mutex_unlock(&cs_policy_lock);
	return !list_empty(list);
}

/**
 * cs_del_acl - Delete members in "struct cs_acl_info".
 *
 * @element: Pointer to "struct list_head".
 *
 * Returns nothing.
 */
static inline void cs_del_acl(struct list_head *element)
{
	struct cs_acl_info *acl = container_of(element, typeof(*acl), list);

	cs_put_condition(acl->cond);
}

/**
 * cs_del_condition - Delete members in "struct cs_condition".
 *
 * @element: Pointer to "struct list_head".
 *
 * Returns nothing.
 */
void cs_del_condition(struct list_head *element)
{
	struct cs_condition *cond = container_of(element, typeof(*cond),
						 head.list);
	const union cs_condition_element *condp = (typeof(condp)) (cond + 1);

	while ((void *) condp < (void *) ((u8 *) cond) + cond->size) {
		const enum cs_conditions_index right = condp->right;

		condp++;
		if (right == CS_IMM_NAME_ENTRY) {
			if (condp->path != &cs_null_name)
				cs_put_name(condp->path);
			condp++;
		}
	}
}

/**
 * cs_try_to_gc - Try to kfree() an entry.
 *
 * @type:    One of values in "enum cs_policy_id".
 * @element: Pointer to "struct list_head".
 *
 * Returns nothing.
 *
 * Caller holds cs_policy_lock mutex.
 */
static void cs_try_to_gc(const enum cs_policy_id type,
			 struct list_head *element)
{
	/*
	 * __list_del_entry() guarantees that the list element became no longer
	 * reachable from the list which the element was originally on (e.g.
	 * cs_domain_list). Also, synchronize_srcu() guarantees that the list
	 * element became no longer referenced by syscall users.
	 */
	__list_del_entry(element);
	mutex_unlock(&cs_policy_lock);
	synchronize_srcu(&cs_ss);
	/*
	 * However, there are two users which may still be using the list
	 * element. We need to defer until both users forget this element.
	 *
	 * Don't kfree() until "struct cs_io_buffer"->r.{acl,subacl} and
	 * "struct cs_io_buffer"->w.acl forget this element.
	 */
	if (cs_struct_used_by_io_buffer(element))
		goto reinject;
	switch (type) {
	case CS_ID_CONDITION:
		cs_del_condition(element);
		break;
	case CS_ID_NAME:
		/*
		 * Don't kfree() until all "struct cs_io_buffer"->r.w[] forget
		 * this element.
		 */
		if (cs_name_used_by_io_buffer
		    (container_of(element, typeof(struct cs_name),
				  head.list)->entry.name,
		     container_of(element, typeof(struct cs_name),
				  head.list)->size))
			goto reinject;
		break;
	case CS_ID_ACL:
		/*
		 * Don't kfree() until "struct cs_acl_info"->acl_info_list
		 * becomes empty.
		 */
		if (cs_acl_info_has_sub_acl
		    (&container_of(element, typeof(struct cs_acl_info),
				   list)->acl_info_list))
			goto reinject;
		cs_del_acl(element);
		break;
	default:
		break;
	}
	mutex_lock(&cs_policy_lock);
	cs_memory_used[CS_MEMORY_POLICY] -= ksize(element);
	kfree(element);
	return;
reinject:
	/*
	 * We can safely reinject this element here bacause
	 * (1) Appending list elements and removing list elements are protected
	 *     by cs_policy_lock mutex.
	 * (2) Only this function removes list elements and this function is
	 *     exclusively executed by cs_gc_mutex mutex.
	 * are true.
	 */
	mutex_lock(&cs_policy_lock);
	list_add_rcu(element, element->prev);
}

/**
 * cs_collect_acl - Delete elements in "struct cs_acl_info".
 *
 * @list: Pointer to "struct list_head".
 *
 * Returns nothing.
 *
 * Caller holds cs_policy_lock mutex.
 */
static void cs_collect_acl(struct list_head *list)
{
	struct cs_acl_info *acl;
	struct cs_acl_info *tmp;

	list_for_each_entry_safe(acl, tmp, list, list) {
		if (!acl->is_deleted)
			continue;
		cs_try_to_gc(CS_ID_ACL, &acl->list);
	}
}

/**
 * cs_collect_entry - Try to kfree() deleted elements.
 *
 * Returns nothing.
 */
static void cs_collect_entry(void)
{
	int i;

	mutex_lock(&cs_policy_lock);
	for (i = 0; i < CS_MAX_MAC_INDEX; i++) {
		struct cs_acl_info *ptr;
		struct cs_acl_info *tmp;
		struct list_head * const list = &cs_acl_list[i];

		list_for_each_entry_safe(ptr, tmp, list, list) {
			cs_collect_acl(&ptr->acl_info_list);
			if (!ptr->is_deleted)
				continue;
			ptr->is_deleted = CS_GC_IN_PROGRESS;
			cs_try_to_gc(CS_ID_ACL, &ptr->list);
		}
	}
	{
		struct cs_shared_acl_head *ptr;
		struct cs_shared_acl_head *tmp;

		list_for_each_entry_safe(ptr, tmp, &cs_condition_list, list) {
			if (atomic_read(&ptr->users) > 0)
				continue;
			atomic_set(&ptr->users, CS_GC_IN_PROGRESS);
			cs_try_to_gc(CS_ID_CONDITION, &ptr->list);
		}
	}
	for (i = 0; i < CS_MAX_HASH; i++) {
		struct list_head *list = &cs_name_list[i];
		struct cs_shared_acl_head *ptr;
		struct cs_shared_acl_head *tmp;

		list_for_each_entry_safe(ptr, tmp, list, list) {
			if (atomic_read(&ptr->users) > 0)
				continue;
			atomic_set(&ptr->users, CS_GC_IN_PROGRESS);
			cs_try_to_gc(CS_ID_NAME, &ptr->list);
		}
	}
	mutex_unlock(&cs_policy_lock);
}

/**
 * cs_gc_thread - Garbage collector thread function.
 *
 * @unused: Unused.
 *
 * Returns 0.
 */
static int cs_gc_thread(void *unused)
{
	/* Garbage collector thread is exclusive. */
	static DEFINE_MUTEX(cs_gc_mutex);

	if (!mutex_trylock(&cs_gc_mutex))
		goto out;
	cs_collect_entry();
	{
		struct cs_io_buffer *head;
		struct cs_io_buffer *tmp;

		spin_lock(&cs_io_buffer_list_lock);
		list_for_each_entry_safe(head, tmp, &cs_io_buffer_list,
					 list) {
			if (head->users)
				continue;
			list_del(&head->list);
			kfree(head->read_buf);
			kfree(head->write_buf);
			kfree(head);
		}
		spin_unlock(&cs_io_buffer_list_lock);
	}
	mutex_unlock(&cs_gc_mutex);
out:
	/* This acts as do_exit(0). */
	return 0;
}

/**
 * cs_notify_gc - Register/unregister /sys/kernel/security/caitsith/ users.
 *
 * @head:        Pointer to "struct cs_io_buffer".
 * @is_register: True if register, false if unregister.
 *
 * Returns nothing.
 */
void cs_notify_gc(struct cs_io_buffer *head, const bool is_register)
{
	bool is_write = false;

	spin_lock(&cs_io_buffer_list_lock);
	if (is_register) {
		head->users = 1;
		list_add(&head->list, &cs_io_buffer_list);
	} else {
		is_write = head->write_buf != NULL;
		if (!--head->users) {
			list_del(&head->list);
			kfree(head->read_buf);
			kfree(head->write_buf);
			kfree(head);
		}
	}
	spin_unlock(&cs_io_buffer_list_lock);
	if (is_write)
		kthread_run(cs_gc_thread, NULL, "CaitSith's GC");
}
