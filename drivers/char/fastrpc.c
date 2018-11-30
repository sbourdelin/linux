// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/rpmsg.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#define ADSP_DOMAIN_ID (0)
#define MDSP_DOMAIN_ID (1)
#define SDSP_DOMAIN_ID (2)
#define CDSP_DOMAIN_ID (3)
#define FASTRPC_DEV_MAX		4 /* adsp, mdsp, slpi, cdsp*/
#define FASTRPC_MAX_SESSIONS	9 /*8 compute, 1 cpz*/
#define FASTRPC_CTX_MAX (256)
#define FASTRPC_CTXID_MASK (0xFF0)
#define FASTRPC_DEVICE_NAME	"fastrpc"

#define cdev_to_cctx(d) container_of(d, struct fastrpc_channel_ctx, cdev)

static const char *domains[FASTRPC_DEV_MAX] = { "adsp", "mdsp",
						"sdsp", "cdsp"};
static dev_t fastrpc_major;
static struct class *fastrpc_class;

struct fastrpc_session_ctx {
	struct device *dev;
	int sid;
	bool used;
	bool valid;
	bool secure;
};

struct fastrpc_channel_ctx {
	int domain_id;
	int sesscount;
	struct rpmsg_device *rpdev;
	struct fastrpc_session_ctx session[FASTRPC_MAX_SESSIONS];
	spinlock_t lock;
	struct idr ctx_idr;
	struct list_head users;
	struct cdev cdev;
	struct device dev;
};

struct fastrpc_user {
	struct list_head user;
	struct list_head maps;
	struct list_head pending;

	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_session_ctx *sctx;

	int tgid;
	int pd;
	/* Lock for lists */
	spinlock_t lock;
	/* lock for allocations */
	struct mutex mutex;
	struct device *dev;
};

static const struct of_device_id fastrpc_match_table[] = {
	{ .compatible = "qcom,fastrpc-compute-cb", },
	{}
};

static int fastrpc_device_release(struct inode *inode, struct file *file)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;
	struct fastrpc_channel_ctx *cctx = cdev_to_cctx(inode->i_cdev);

	spin_lock(&cctx->lock);
	list_del(&fl->user);
	spin_unlock(&cctx->lock);

	kfree(fl);
	file->private_data = NULL;

	return 0;
}

static int fastrpc_device_open(struct inode *inode, struct file *filp)
{
	struct fastrpc_channel_ctx *cctx = cdev_to_cctx(inode->i_cdev);
	struct fastrpc_user *fl = NULL;

	fl = kzalloc(sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return -ENOMEM;

	filp->private_data = fl;

	spin_lock_init(&fl->lock);
	mutex_init(&fl->mutex);
	INIT_LIST_HEAD(&fl->pending);
	INIT_LIST_HEAD(&fl->maps);
	INIT_LIST_HEAD(&fl->user);

	fl->tgid = current->tgid;
	fl->cctx = cctx;
	fl->dev = &cctx->rpdev->dev;
	spin_lock(&cctx->lock);
	list_add_tail(&fl->user, &cctx->users);
	spin_unlock(&cctx->lock);

	return 0;
}

static const struct file_operations fastrpc_fops = {
	.open = fastrpc_device_open,
	.release = fastrpc_device_release,
};

static int fastrpc_cb_probe(struct platform_device *pdev)
{
	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_session_ctx *sess;
	struct device *dev = &pdev->dev;
	int i, sessions = 0;

	cctx = dev_get_drvdata(dev->parent);
	if (!cctx)
		return -EINVAL;

	of_property_read_u32(dev->of_node, "nsessions", &sessions);

	spin_lock(&cctx->lock);
	sess = &cctx->session[cctx->sesscount];
	sess->used = false;
	sess->valid = true;
	sess->dev = dev;
	dev_set_drvdata(dev, sess);
	sess->secure = of_property_read_bool(dev->of_node, "secured");

	if (of_property_read_u32(dev->of_node, "reg", &sess->sid))
		dev_err(dev, "FastRPC Session ID not specified in DT\n");

	if (sessions > 0) {
		struct fastrpc_session_ctx *dup_sess;

		for (i = 1; i < sessions; i++) {
			if (cctx->sesscount++ >= FASTRPC_MAX_SESSIONS)
				break;
			dup_sess = &cctx->session[cctx->sesscount];
			memcpy(dup_sess, sess, sizeof(*dup_sess));
		}
	}
	cctx->sesscount++;
	spin_unlock(&cctx->lock);
	dma_set_mask(dev, DMA_BIT_MASK(32));

	return 0;
}

static int fastrpc_cb_remove(struct platform_device *pdev)
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(pdev->dev.parent);
	struct fastrpc_session_ctx *sess = dev_get_drvdata(&pdev->dev);
	int i;

	spin_lock(&cctx->lock);
	for (i = 1; i < FASTRPC_MAX_SESSIONS; i++) {
		if (cctx->session[i].sid == sess->sid) {
			cctx->session[i].valid = false;
			cctx->sesscount--;
		}
	}
	spin_unlock(&cctx->lock);

	return 0;
}

static struct platform_driver fastrpc_cb_driver = {
	.probe = fastrpc_cb_probe,
	.remove = fastrpc_cb_remove,
	.driver = {
		.name = "fastrpc",
		.owner = THIS_MODULE,
		.of_match_table = fastrpc_match_table,
		.suppress_bind_attrs = true,
	},
};

static void fastrpc_cdev_release_device(struct device *dev)
{
	struct fastrpc_channel_ctx *data = dev_get_drvdata(dev->parent);

	cdev_del(&data->cdev);
}

static int fastrpc_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *rdev = &rpdev->dev;
	struct fastrpc_channel_ctx *data;
	struct device *dev;
	int err, domain_id;

	data = devm_kzalloc(rdev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	err = of_property_read_u32(rdev->of_node, "reg", &domain_id);
	if (err) {
		dev_err(rdev, "FastRPC Domain ID not specified in DT\n");
		return err;
	}

	if (domain_id > CDSP_DOMAIN_ID) {
		dev_err(rdev, "FastRPC Invalid Domain ID %d\n", domain_id);
		return -EINVAL;
	}

	dev = &data->dev;
	device_initialize(dev);
	dev->parent = &rpdev->dev;
	dev->class = fastrpc_class;

	cdev_init(&data->cdev, &fastrpc_fops);
	data->cdev.owner = THIS_MODULE;
	dev->devt = MKDEV(MAJOR(fastrpc_major), domain_id);
	dev->id = domain_id;
	dev_set_name(&data->dev, "fastrpc-%s", domains[domain_id]);
	dev->release = fastrpc_cdev_release_device;

	err = cdev_device_add(&data->cdev, &data->dev);
	if (err)
		goto cdev_err;

	dev_set_drvdata(&rpdev->dev, data);
	dma_set_mask_and_coherent(rdev, DMA_BIT_MASK(32));
	INIT_LIST_HEAD(&data->users);
	spin_lock_init(&data->lock);
	idr_init(&data->ctx_idr);
	data->domain_id = domain_id;
	data->rpdev = rpdev;

	return of_platform_populate(rdev->of_node, NULL, NULL, rdev);

cdev_err:
	put_device(dev);
	return err;
}

static void fastrpc_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(&rpdev->dev);

	device_del(&cctx->dev);
	put_device(&cctx->dev);
	of_platform_depopulate(&rpdev->dev);
	kfree(cctx);
}

static int fastrpc_rpmsg_callback(struct rpmsg_device *rpdev, void *data,
				  int len, void *priv, u32 addr)
{
	return 0;
}

static const struct of_device_id fastrpc_rpmsg_of_match[] = {
	{ .compatible = "qcom,fastrpc" },
	{ },
};
MODULE_DEVICE_TABLE(of, fastrpc_rpmsg_of_match);

static struct rpmsg_driver fastrpc_driver = {
	.probe = fastrpc_rpmsg_probe,
	.remove = fastrpc_rpmsg_remove,
	.callback = fastrpc_rpmsg_callback,
	.drv = {
		.name = "qcom,msm_fastrpc_rpmsg",
		.of_match_table = fastrpc_rpmsg_of_match,
	},
};

static int fastrpc_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&fastrpc_major, 0, FASTRPC_DEV_MAX,
				  FASTRPC_DEVICE_NAME);
	if (ret < 0) {
		pr_err("fastrpc: failed to allocate char dev region\n");
		return ret;
	}

	fastrpc_class = class_create(THIS_MODULE, "fastrpc");
	if (IS_ERR(fastrpc_class)) {
		pr_err("failed to create rpmsg class\n");
		ret = PTR_ERR(fastrpc_class);
		goto err_class;
	}

	ret = platform_driver_register(&fastrpc_cb_driver);
	if (ret < 0) {
		pr_err("fastrpc: failed to register cb driver\n");
		goto err_pdev;
	}

	ret = register_rpmsg_driver(&fastrpc_driver);
	if (ret < 0) {
		pr_err("fastrpc: failed to register rpmsg driver\n");
		goto err_rpdrv;
	}

	return 0;
err_rpdrv:
	platform_driver_unregister(&fastrpc_cb_driver);
err_pdev:
	class_destroy(fastrpc_class);
err_class:
	unregister_chrdev_region(fastrpc_major, FASTRPC_DEV_MAX);
	return ret;
}
module_init(fastrpc_init);

static void fastrpc_exit(void)
{
	platform_driver_unregister(&fastrpc_cb_driver);
	unregister_rpmsg_driver(&fastrpc_driver);
	class_destroy(fastrpc_class);
	unregister_chrdev_region(fastrpc_major, FASTRPC_DEV_MAX);
}
module_exit(fastrpc_exit);

MODULE_ALIAS("fastrpc:fastrpc");
MODULE_LICENSE("GPL v2");
