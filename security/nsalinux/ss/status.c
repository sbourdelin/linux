/*
 * mmap based event notifications for NSALinux
 *
 * Author: KaiGai Kohei <kaigai@ak.jp.nec.com>
 *
 * Copyright (C) 2010 NEC corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * as published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include "avc.h"
#include "services.h"

/*
 * The nsalinux_status_page shall be exposed to userspace applications
 * using mmap interface on /nsalinux/status.
 * It enables to notify applications a few events that will cause reset
 * of userspace access vector without context switching.
 *
 * The nsalinux_kernel_status structure on the head of status page is
 * protected from concurrent accesses using seqlock logic, so userspace
 * application should reference the status page according to the seqlock
 * logic.
 *
 * Typically, application checks status->sequence at the head of access
 * control routine. If it is odd-number, kernel is updating the status,
 * so please wait for a moment. If it is changed from the last sequence
 * number, it means something happen, so application will reset userspace
 * avc, if needed.
 * In most cases, application shall confirm the kernel status is not
 * changed without any system call invocations.
 */
static struct page *nsalinux_status_page;
static DEFINE_MUTEX(nsalinux_status_lock);

/*
 * nsalinux_kernel_status_page
 *
 * It returns a reference to nsalinux_status_page. If the status page is
 * not allocated yet, it also tries to allocate it at the first time.
 */
struct page *nsalinux_kernel_status_page(void)
{
	struct nsalinux_kernel_status   *status;
	struct page		       *result = NULL;

	mutex_lock(&nsalinux_status_lock);
	if (!nsalinux_status_page) {
		nsalinux_status_page = alloc_page(GFP_KERNEL|__GFP_ZERO);

		if (nsalinux_status_page) {
			status = page_address(nsalinux_status_page);

			status->version = NSALINUX_KERNEL_STATUS_VERSION;
			status->sequence = 0;
			status->enforcing = nsalinux_enforcing;
			/*
			 * NOTE: the next policyload event shall set
			 * a positive value on the status->policyload,
			 * although it may not be 1, but never zero.
			 * So, application can know it was updated.
			 */
			status->policyload = 0;
			status->deny_unknown = !security_get_allow_unknown();
		}
	}
	result = nsalinux_status_page;
	mutex_unlock(&nsalinux_status_lock);

	return result;
}

/*
 * nsalinux_status_update_setenforce
 *
 * It updates status of the current enforcing/permissive mode.
 */
void nsalinux_status_update_setenforce(int enforcing)
{
	struct nsalinux_kernel_status   *status;

	mutex_lock(&nsalinux_status_lock);
	if (nsalinux_status_page) {
		status = page_address(nsalinux_status_page);

		status->sequence++;
		smp_wmb();

		status->enforcing = enforcing;

		smp_wmb();
		status->sequence++;
	}
	mutex_unlock(&nsalinux_status_lock);
}

/*
 * nsalinux_status_update_policyload
 *
 * It updates status of the times of policy reloaded, and current
 * setting of deny_unknown.
 */
void nsalinux_status_update_policyload(int seqno)
{
	struct nsalinux_kernel_status   *status;

	mutex_lock(&nsalinux_status_lock);
	if (nsalinux_status_page) {
		status = page_address(nsalinux_status_page);

		status->sequence++;
		smp_wmb();

		status->policyload = seqno;
		status->deny_unknown = !security_get_allow_unknown();

		smp_wmb();
		status->sequence++;
	}
	mutex_unlock(&nsalinux_status_lock);
}
