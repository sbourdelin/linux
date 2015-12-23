#ifndef _LINUX_SYSDATA_H
#define _LINUX_SYSDATA_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/gfp.h>

/*
 * System Data internals
 *
 * Copyright (C) 2015 Luis R. Rodriguez <mcgrof@do-not-panic.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

struct sysdata_file {
	size_t size;
	const u8 *data;

	/* sysdata loader private fields */
	void *priv;
};

/**
 * enum sync_data_mode - system data mode of operation
 *
 * SYNCDATA_SYNC: your call to request system data is synchronous. We will
 * 	look for the system data file you have requested immediatley.
 * SYNCDATA_ASYNC: your call to request system data is asynchronous. We will
 * 	schedule the search for your system data file to be run at a later
 * 	time.
 */
enum sync_data_mode {
	SYNCDATA_SYNC,
	SYNCDATA_ASYNC,
};

/* one per sync_data_mode */
union sysdata_file_cbs {
	struct {
		int __must_check (*found_cb)(void *, const struct sysdata_file *);
		void *found_context;

		int __must_check (*opt_fail_cb)(void *);
		void *opt_fail_context;
	} sync;
	struct {
		void (*found_cb)(const struct sysdata_file *, void *);
		void *found_context;

		void (*opt_fail_cb)(void *);
		void *opt_fail_context;
	} async;
};

struct sysdata_file_sync_reqs {
	enum sync_data_mode mode;
	struct module *module;
	gfp_t gfp;
};

/**
 * struct sysdata_file_desc - system data file descriptor
 * @optional: if true it is not a hard requirement by the caller that this
 *	file be present. An error will not be recorded if the file is not
 *	found.
 * @keep: if set the caller wants to claim ownership over the system data
 *	through one of its callbacks, it must later free it with
 *	release_sysdata_file(). By default this is set to false and the kernel
 *	will release the system data file for you after callback processing
 *	has completed.
 * @sync_reqs: synchronization requirements, this will be taken care for you
 *	by default if you are usingy sdata_file_request(), otherwise you
 *	should provide your own requirements.
 *
 * This structure is set the by the driver and passed to the system data
 * file helpers sysdata_file_request() or sysdata_file_request_async().
 * It is intended to carry all requirements and specifications required
 * to complete the task to get the requested system date file to the caller.
 * If you wish to extend functionality of system data file requests you
 * should extend this data structure and make use of the extensions on
 * the callers to avoid unnecessary collateral evolutions.
 *
 * You are allowed to provide a callback to handle if a system data file was
 * found or not. You do not need to provide a callback. You may also set
 * an optional flag which would enable you to declare that the system data
 * file is optional and that if it is not found an alternative callback be
 * run for you.
 *
 * Refer to sysdata_file_request() and sysdata_file_request_async() for more
 * details.
 */
struct sysdata_file_desc {
	bool optional;
	bool keep;
	struct sysdata_file_sync_reqs sync_reqs;
	const union sysdata_file_cbs cbs;
};

/*
 * We keep these template definitions to a minimum for the most
 * popular requests.
 */

/* Typical sync data case */
#define SYSDATA_SYNC_FOUND(__found_cb, __context)			\
	.cbs.sync.found_cb = __found_cb,				\
	.cbs.sync.found_context = __context

/* If you have one fallback routine */
#define SYSDATA_SYNC_OPT_CB(__found_cb, __context)			\
	.cbs.sync.opt_fail_cb = __found_cb,				\
	.cbs.sync.opt_fail_context = __context

/*
 * Used to define the default asynchronization requirements for
 * sysdata_file_request_async(). Drivers can override.
 */
#define SYSDATA_DEFAULT_ASYNC(__found_cb, __context)			\
	.sync_reqs = {							\
		.mode = SYNCDATA_ASYNC,					\
		.module = THIS_MODULE,					\
		.gfp = GFP_KERNEL,					\
	},								\
	.cbs.async = {							\
		.found_cb = __found_cb,					\
		.found_context = __context,				\
	}

#define desc_sync_found_cb(desc)	((desc)->cbs.sync.found_cb)
#define desc_sync_found_context(desc)	((desc)->cbs.sync.found_context)
static inline int desc_sync_found_call_cb(const struct sysdata_file_desc *desc,
					  const struct sysdata_file *sysdata)
{
	if (desc->sync_reqs.mode != SYNCDATA_SYNC)
		return -EINVAL;
	if (!desc_sync_found_cb(desc)) {
		if (sysdata)
			return 0;
		return -ENOENT;
	}
	return desc_sync_found_cb(desc)(desc_sync_found_context(desc),
					sysdata);
}

#define desc_sync_opt_cb(desc)		((desc)->cbs.sync.opt_fail_cb)
#define desc_sync_opt_context(desc)	((desc)->cbs.sync.opt_fail_context)
static inline int desc_sync_opt_call_cb(const struct sysdata_file_desc *desc)
{
	if (desc->sync_reqs.mode != SYNCDATA_SYNC)
		return -EINVAL;
	if (!desc_sync_opt_cb(desc))
		return 0;
	return desc_sync_opt_cb(desc)(desc_sync_opt_context(desc));
}

#define desc_async_found_cb(desc)	((desc)->cbs.async.found_cb)
#define desc_async_found_context(desc)	((desc)->cbs.async.found_context)
static inline void desc_async_found_call_cb(const struct sysdata_file *sysdata,
					    const struct sysdata_file_desc *desc)
{
	if (desc->sync_reqs.mode != SYNCDATA_ASYNC)
		return;
	if (!desc_async_found_cb(desc))
		return;
	desc_async_found_cb(desc)(sysdata, desc_async_found_context(desc));
}

#define desc_async_opt_cb(desc)		((desc)->cbs.async.opt_fail_cb)
#define desc_async_opt_context(desc)	((desc)->cbs.async.opt_fail_context)
static inline void desc_async_opt_call_cb(const struct sysdata_file_desc *desc)
{
	if (desc->sync_reqs.mode != SYNCDATA_ASYNC)
		return;
	if (!desc_async_opt_cb(desc))
		return;
	desc_async_opt_cb(desc)(desc_async_opt_context(desc));
}


#if defined(CONFIG_FW_LOADER) || (defined(CONFIG_FW_LOADER_MODULE) && defined(MODULE))
int sysdata_file_request(const char *name,
			 const struct sysdata_file_desc *desc,
			 struct device *device);
int sysdata_file_request_async(const char *name,
			       const struct sysdata_file_desc *desc,
			       struct device *device);
void release_sysdata_file(const struct sysdata_file *sysdata);
#else
static inline int sysdata_file_request(const char *name,
				       const struct sysdata_file_desc *desc,
				       struct device *device)
{
	return -EINVAL;
}

static inline int sysdata_file_request_async(const char *name,
					     const struct sysdata_file_desc *desc,
					     struct device *device);
{
	return -EINVAL;
}

static inline void release_sysdata_file(const struct sysdata_file *sysdata)
{
}
#endif

#endif /* _LINUX_SYSDATA_H */
