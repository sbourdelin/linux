/*
 * This module provides an interface to trigger and test firmware loading.
 *
 * It is designed to be used for basic evaluation of the firmware loading
 * subsystem (for example when validating firmware verification). It lacks
 * any extra dependencies, and will not normally be loaded by the system
 * unless explicitly requested by name.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/completion.h>
#include <linux/firmware.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#define TEST_FIRMWARE_NAME	"test-firmware.bin"
#define TEST_FIRMWARE_NUM_REQS	4

static DEFINE_MUTEX(test_fw_mutex);
static const struct firmware *test_firmware;

/**
 * test_config - represents configuration for the test for different triggers
 *
 * @name: the name of the firmware file to look for
 * @test_result: a test may use this to collect the result from the call
 *	of the request_firmware*() calls used in their tests. In order of
 *	priority we always keep first any setup error. If no setup errors were
 *	found then we move on to the first error encountered while running the
 *	API. Note that for async calls this typically will be a successful
 *	result (0) unless of course you've used bogus parameters, or the system
 *	is out of memory.  In the async case the callback is expected to do a
 *	bit more homework to figure out what happened, unfortunately the only
 *	information passed today on error is the fact that no firmware was
 *	found so we can only assume -ENOENT on async calls if the firmware is
 *	NULL.
 *
 *	Errors you can expect:
 *
 *	API specific:
 *
 *	0:		success for sync, for async it means request was sent
 *	-EINVAL:	invalid parameters or request
 *	-ENOENT:	files not found
 *
 *	System environment:
 *
 *	-ENOMEM:	memory pressure on system
 *	-ENODEV:	out of number of devices to test
 *	-EINVAL:	an unexpected error has occured
 * @sync_direct: when the sync trigger is used if this is true
 * 	request_firmware_direct() will be used instead.
 * @req_firmware: if @sync_direct is true this is set to
 * 	request_firmware_direct(), otherwise request_firmware()
 * @send_uevent: whether or not to send a uevent for async requests
 * @num_requests: number of requests to try per test case. This is trigger
 * 	specific.
 */
struct test_config {
	char *name;
	int test_result;
	bool sync_direct;
	int (*req_firmware)(const struct firmware **fw, const char *name,
			    struct device *device);
	bool send_uevent;
	u8 num_requests;
};

struct test_config *config;

struct test_batched_req {
	u8 idx;
	int rc;
	bool sent;
	const struct firmware *fw;
	const char *name;
	struct completion completion;
	struct task_struct *task;
	struct device *dev;
};

static ssize_t test_fw_misc_read(struct file *f, char __user *buf,
				 size_t size, loff_t *offset)
{
	ssize_t rc = 0;

	mutex_lock(&test_fw_mutex);
	if (test_firmware)
		rc = simple_read_from_buffer(buf, size, offset,
					     test_firmware->data,
					     test_firmware->size);
	mutex_unlock(&test_fw_mutex);
	return rc;
}

static const struct file_operations test_fw_fops = {
	.owner          = THIS_MODULE,
	.read           = test_fw_misc_read,
};

static void __test_firmware_config_free(void)
{
	kfree_const(config->name);
	config->name = NULL;
}

/*
 * XXX: move to kstrncpy() once merged.
 *
 * Users should use kfree_const() when freeing these.
 */
static int __kstrncpy(char **dst, const char *name, size_t count, gfp_t gfp)
{
	*dst = kstrndup(name, count, gfp);
	if (!*dst)
		return -ENOSPC;
	return count;
}

static int __test_firmware_config_init(void)
{
	int ret;

	ret = __kstrncpy(&config->name, TEST_FIRMWARE_NAME,
			 strlen(TEST_FIRMWARE_NAME), GFP_KERNEL);
	if (ret < 0)
		goto out;

	config->num_requests = TEST_FIRMWARE_NUM_REQS;
	config->send_uevent = true;
	config->sync_direct = false;
	config->req_firmware = request_firmware;
	config->test_result = 0;

	return 0;

out:
	__test_firmware_config_free();
	return ret;
}

static ssize_t reset_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	int ret;

	mutex_lock(&test_fw_mutex);

	__test_firmware_config_free();

	ret = __test_firmware_config_init();
	if (ret < 0) {
		ret = -ENOMEM;
		pr_err("could not alloc settings for config trigger: %d\n",
		       ret);
		goto out;
	}

	pr_info("reset\n");
	ret = count;

out:
	mutex_unlock(&test_fw_mutex);

	return ret;
}
static DEVICE_ATTR_WO(reset);

static ssize_t config_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int len = 0;

	mutex_lock(&test_fw_mutex);

	len += snprintf(buf, PAGE_SIZE,
			"Custom trigger configuration for: %s\n",
			dev_name(dev));

	if (config->name)
		len += snprintf(buf+len, PAGE_SIZE,
				"name:\t%s\n",
				config->name);
	else
		len += snprintf(buf+len, PAGE_SIZE,
				"name:\tEMTPY\n");

	len += snprintf(buf+len, PAGE_SIZE,
			"num_requests:\t%u\n", config->num_requests);

	len += snprintf(buf+len, PAGE_SIZE,
			"send_uevent:\t\t%s\n",
			config->send_uevent ? "FW_ACTION_HOTPLUG" : "FW_ACTION_NOHOTPLUG");
	len += snprintf(buf+len, PAGE_SIZE,
			"sync_direct:\t\t%s\n",
			config->sync_direct ? "true" : "false");

	mutex_unlock(&test_fw_mutex);

	return len;
}
static DEVICE_ATTR_RO(config);

static ssize_t config_name_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;

	mutex_lock(&test_fw_mutex);
	kfree_const(config->name);
	ret = __kstrncpy(&config->name, buf, count, GFP_KERNEL);
	mutex_unlock(&test_fw_mutex);

	return ret;
}

/*
 * As per sysfs_kf_seq_show() the buf is max PAGE_SIZE.
 */
static ssize_t config_test_show_str(char *dst,
				    char *src)
{
	int len;

	mutex_lock(&test_fw_mutex);
	len = snprintf(dst, PAGE_SIZE, "%s\n", src);
	mutex_unlock(&test_fw_mutex);

	return len;
}

static int test_dev_config_update_bool(const char *buf, size_t size,
				       bool *cfg)
{
	int ret;

	mutex_lock(&test_fw_mutex);
	if (strtobool(buf, cfg) < 0)
		ret = -EINVAL;
	else
		ret = size;
	mutex_unlock(&test_fw_mutex);

	return ret;
}

static ssize_t
test_dev_config_show_bool(char *buf,
			  bool config)
{
	bool val;

	mutex_lock(&test_fw_mutex);
	val = config;
	mutex_unlock(&test_fw_mutex);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t test_dev_config_show_int(char *buf, int cfg)
{
	int val;

	mutex_lock(&test_fw_mutex);
	val = cfg;
	mutex_unlock(&test_fw_mutex);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static int test_dev_config_update_u8(const char *buf, size_t size, u8 *cfg)
{
	int ret;
	long new;

	ret = kstrtol(buf, 10, &new);
	if (ret)
		return ret;

	if (new > U8_MAX)
		return -EINVAL;

	mutex_lock(&test_fw_mutex);
	*(u8 *)cfg = new;
	mutex_unlock(&test_fw_mutex);

	/* Always return full write size even if we didn't consume all */
	return size;
}

static ssize_t test_dev_config_show_u8(char *buf, u8 cfg)
{
	u8 val;

	mutex_lock(&test_fw_mutex);
	val = cfg;
	mutex_unlock(&test_fw_mutex);

	return snprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t config_name_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return config_test_show_str(buf, config->name);
}
static DEVICE_ATTR(config_name, 0644, config_name_show, config_name_store);

static ssize_t config_num_requests_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	return test_dev_config_update_u8(buf, count, &config->num_requests);
}

static ssize_t config_num_requests_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return test_dev_config_show_u8(buf, config->num_requests);
}
static DEVICE_ATTR(config_num_requests, 0644, config_num_requests_show,
		   config_num_requests_store);

static ssize_t config_sync_direct_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int rc = test_dev_config_update_bool(buf, count, &config->sync_direct);

	if (rc == count)
		config->req_firmware = config->sync_direct ?
				       request_firmware_direct :
				       request_firmware;
	return rc;
}

static ssize_t config_sync_direct_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return test_dev_config_show_bool(buf, config->sync_direct);
}
static DEVICE_ATTR(config_sync_direct, 0644, config_sync_direct_show,
		   config_sync_direct_store);

static ssize_t config_send_uevent_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	return test_dev_config_update_bool(buf, count, &config->send_uevent);
}

static ssize_t config_send_uevent_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return test_dev_config_show_bool(buf, config->send_uevent);
}
static DEVICE_ATTR(config_send_uevent, 0644, config_send_uevent_show,
		   config_send_uevent_store);

static ssize_t test_result_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return test_dev_config_show_int(buf, config->test_result);
}
static DEVICE_ATTR_RO(test_result);

static ssize_t trigger_request_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int rc;
	char *name;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	pr_info("loading '%s'\n", name);

	mutex_lock(&test_fw_mutex);
	release_firmware(test_firmware);
	test_firmware = NULL;
	rc = request_firmware(&test_firmware, name, dev);
	if (rc) {
		pr_info("load of '%s' failed: %d\n", name, rc);
		goto out;
	}
	pr_info("loaded: %zu\n", test_firmware->size);
	rc = count;

out:
	mutex_unlock(&test_fw_mutex);

	kfree(name);

	return rc;
}
static DEVICE_ATTR_WO(trigger_request);

static DECLARE_COMPLETION(async_fw_done);

static void trigger_async_request_cb(const struct firmware *fw, void *context)
{
	test_firmware = fw;
	complete(&async_fw_done);
}

static ssize_t trigger_async_request_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int rc;
	char *name;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	pr_info("loading '%s'\n", name);

	mutex_lock(&test_fw_mutex);
	release_firmware(test_firmware);
	test_firmware = NULL;
	rc = request_firmware_nowait(THIS_MODULE, 1, name, dev, GFP_KERNEL,
				     NULL, trigger_async_request_cb);
	if (rc) {
		pr_info("async load of '%s' failed: %d\n", name, rc);
		kfree(name);
		goto out;
	}
	/* Free 'name' ASAP, to test for race conditions */
	kfree(name);

	wait_for_completion(&async_fw_done);

	if (test_firmware) {
		pr_info("loaded: %zu\n", test_firmware->size);
		rc = count;
	} else {
		pr_err("failed to async load firmware\n");
		rc = -ENODEV;
	}

out:
	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_async_request);

static ssize_t trigger_custom_fallback_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	int rc;
	char *name;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	pr_info("loading '%s' using custom fallback mechanism\n", name);

	mutex_lock(&test_fw_mutex);
	release_firmware(test_firmware);
	test_firmware = NULL;
	rc = request_firmware_nowait(THIS_MODULE, FW_ACTION_NOHOTPLUG, name,
				     dev, GFP_KERNEL, NULL,
				     trigger_async_request_cb);
	if (rc) {
		pr_info("async load of '%s' failed: %d\n", name, rc);
		kfree(name);
		goto out;
	}
	/* Free 'name' ASAP, to test for race conditions */
	kfree(name);

	wait_for_completion(&async_fw_done);

	if (test_firmware) {
		pr_info("loaded: %zu\n", test_firmware->size);
		rc = count;
	} else {
		pr_err("failed to async load firmware\n");
		rc = -ENODEV;
	}

out:
	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_custom_fallback);

static int test_fw_run_batch_request(void *data)
{
	struct test_batched_req *req = data;

	if (!req) {
		config->test_result = -EINVAL;
		return -EINVAL;
	}

	req->rc = config->req_firmware(&req->fw, req->name, req->dev);
	if (req->rc) {
		pr_info("#%u: batched sync load failed: %d\n",
			req->idx, req->rc);
		if (!config->test_result)
			config->test_result = req->rc;
	} else if (req->fw) {
		req->sent = true;
		pr_info("#%u: batched sync loaded %zu\n",
			req->idx, req->fw->size);
	}
	complete(&req->completion);

	req->task = NULL;

	return 0;
}

/*
 * We use a kthread as otherwise the kernel serializes all our sync requests
 * and we would not be able to mimic batched requests on a sync call. Batched
 * requests on a sync call can for instance happen on a device driver when
 * multiple cards are used and firmware loading happens outside of probe.
 */
static ssize_t trigger_batched_requests_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct test_batched_req *reqs;
	struct test_batched_req *req;
	int rc;
	u8 i;

	reqs = vzalloc(sizeof(struct test_batched_req) * config->num_requests * 2);
	if (!reqs)
		return -ENOMEM;

	mutex_lock(&test_fw_mutex);

	pr_info("batched sync firmware loading '%s' %u times\n",
		config->name, config->num_requests);

	for (i=0; i < config->num_requests; i++) {
		req = &reqs[i];
		if (!req) {
			BUG_ON(1);
			continue;
		}
		req->fw = NULL;
		req->idx = i;
		req->name = config->name;
		req->dev = dev;
		init_completion(&req->completion);
		req->task = kthread_run(test_fw_run_batch_request, req,
					     "%s-%u", KBUILD_MODNAME, req->idx);
		if (!req->task || IS_ERR(req->task)) {
			pr_err("Setting up thread %u failed\n", req->idx);
			req->task = NULL;
			rc = -ENOMEM;
			goto out;
		}
	}

	rc = count;

	/*
	 * We do two passes on our way out to enable more time and delay
	 * calling release_firmware() to improve our chances of forcing a
	 * batched request. If we instead called release_firmware() right away
	 * then we might miss on an opportunity of having a successful firmware
	 * request pass on the opportunity to be come a batched request.
	 */

out:
	for (i=0; i < config->num_requests; i++) {
		req = &reqs[i];
		if (req->task || req->sent)
			wait_for_completion(&req->completion);
	}

	for (i=0; i < config->num_requests; i++) {
		req = &reqs[i];
		if (req->fw)
			release_firmware(req->fw);
	}

	vfree(reqs);

	/* Override any worker error if we had a general setup error */
	if (rc < 0)
		config->test_result = rc;

	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_batched_requests);


/*
 * We wait for each callback to return with the lock held, no need to lock here
 */
static void trigger_batched_cb(const struct firmware *fw, void *context)
{
	struct test_batched_req *req = context;

	if (!req) {
		config->test_result = -EINVAL;
		return;
	}

	/* forces *some* batched requests to queue up */
	if (!req->idx)
		ssleep(2);

	req->fw = fw;

	/*
	 * Unfortunately the firmware API gives us nothing other than a null FW
	 * if the firmware was not found on async requests.  Best we can do is
	 * just assume -ENOENT. A better API would pass the actual return
	 * value to the callback.
	 */
	if (!fw && !config->test_result)
		config->test_result = -ENOENT;

	complete(&req->completion);
}

static
ssize_t trigger_batched_requests_async_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct test_batched_req *reqs;
	struct test_batched_req *req;
	bool send_uevent;
	int rc;
	u8 i;

	reqs = vzalloc(sizeof(struct test_batched_req) *
			config->num_requests * 2);
	if (!reqs)
		return -ENOMEM;

	mutex_lock(&test_fw_mutex);

	pr_info("batched loading '%s' custom fallback mechanism %u times\n",
		config->name, config->num_requests);

	send_uevent = config->send_uevent ? FW_ACTION_HOTPLUG :
		FW_ACTION_NOHOTPLUG;

	for (i=0; i < config->num_requests; i++) {
		req = &reqs[i];
		if (!req) {
			BUG_ON(1);
			continue;
		}
		req->name = config->name;
		req->fw = NULL;
		req->idx = i;
		init_completion(&req->completion);
		rc = request_firmware_nowait(THIS_MODULE, send_uevent,
					     req->name,
					     dev, GFP_KERNEL, req,
					     trigger_batched_cb);
		if (rc) {
			pr_info("#%u: batched async load failed setup: %d\n",
				i, rc);
			req->rc = rc;
			goto out;
		} else
			req->sent = true;
	}

	rc = count;

out:
	/*
	 * We do two passes on our way out to enable more time and delay
	 * calling release_firmware() to improve our chances of forcing a
	 * batched request. If we instead called release_firmware() right away
	 * then we might miss on an opportunity of having a successful firmware
	 * request pass on the opportunity to be come a batched request.
	 */

	for (i=0; i < config->num_requests; i++) {
		req = &reqs[i];
		if (req->sent)
			wait_for_completion(&req->completion);
	}

	for (i=0; i < config->num_requests; i++) {
		req = &reqs[i];
		if (req->fw) {
			pr_info("#%u: loaded %zu\n", i, req->fw->size);
			release_firmware(req->fw);
		} else {
			pr_err("#%u: failed to async load firmware\n", i);
			if (rc > 0)
				rc = -ENODEV;
		}
	}

	vfree(reqs);

	/* Override any worker error if we had a general setup error */
	if (rc < 0)
		config->test_result = rc;

	mutex_unlock(&test_fw_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_batched_requests_async);

#define TEST_FW_DEV_ATTR(name)          &dev_attr_##name.attr

static struct attribute *test_dev_attrs[] = {
	TEST_FW_DEV_ATTR(reset),

	TEST_FW_DEV_ATTR(config),
	TEST_FW_DEV_ATTR(config_name),
	TEST_FW_DEV_ATTR(config_num_requests),
	TEST_FW_DEV_ATTR(config_sync_direct),
	TEST_FW_DEV_ATTR(config_send_uevent),

	/* These don't use the config at all - they could be ported! */
	TEST_FW_DEV_ATTR(trigger_request),
	TEST_FW_DEV_ATTR(trigger_async_request),
	TEST_FW_DEV_ATTR(trigger_custom_fallback),

	/* These use the config and can use the test_result */
	TEST_FW_DEV_ATTR(trigger_batched_requests),
	TEST_FW_DEV_ATTR(trigger_batched_requests_async),

	TEST_FW_DEV_ATTR(test_result),
	NULL,
};

ATTRIBUTE_GROUPS(test_dev);

static struct miscdevice test_fw_misc_device = {
	.minor          = MISC_DYNAMIC_MINOR,
	.name           = "test_firmware",
	.fops           = &test_fw_fops,
	.groups 	= test_dev_groups,
};

static int __init test_firmware_init(void)
{
	int rc;

	config = kzalloc(sizeof(struct test_config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	rc = __test_firmware_config_init();
	if (rc)
		return rc;

	rc = misc_register(&test_fw_misc_device);
	if (rc) {
		kfree(config);
		pr_err("could not register misc device: %d\n", rc);
		return rc;
	}

	pr_warn("interface ready\n");

	return 0;
}

module_init(test_firmware_init);

static void __exit test_firmware_exit(void)
{
	mutex_lock(&test_fw_mutex);
	release_firmware(test_firmware);
	misc_deregister(&test_fw_misc_device);
	__test_firmware_config_free();
	kfree(config);
	mutex_unlock(&test_fw_mutex);

	pr_warn("removed interface\n");
}

module_exit(test_firmware_exit);

MODULE_AUTHOR("Kees Cook <keescook@chromium.org>");
MODULE_LICENSE("GPL");
