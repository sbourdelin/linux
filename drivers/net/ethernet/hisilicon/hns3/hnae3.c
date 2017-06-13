/*
 * Copyright (c) 2016-2017 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include "hnae3.h"

static LIST_HEAD(hnae3_ae_algo_list);
static LIST_HEAD(hnae3_client_list);
static LIST_HEAD(hnae3_ae_dev_list);

static DEFINE_SPINLOCK(hnae3_list_ae_algo_lock);
static DEFINE_SPINLOCK(hnae3_list_client_lock);
static DEFINE_SPINLOCK(hnae3_list_ae_dev_lock);

static void hnae3_list_add(spinlock_t *lock, struct list_head *node,
			   struct list_head *head)
{
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	list_add_tail_rcu(node, head);
	spin_unlock_irqrestore(lock, flags);
}

static void hnae3_list_del(spinlock_t *lock, struct list_head *node)
{
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	list_del_rcu(node);
	spin_unlock_irqrestore(lock, flags);
}

static bool hnae3_client_match(enum hnae3_client_type client_type,
			       enum hnae3_dev_type dev_type)
{
	if (dev_type == HNAE3_DEV_KNIC) {
		switch (client_type) {
		case HNAE3_CLIENT_KNIC:
		case HNAE3_CLIENT_ROCE:
			return true;
		default:
			return false;
		}
	} else if (dev_type == HNAE3_DEV_UNIC) {
		switch (client_type) {
		case HNAE3_CLIENT_UNIC:
			return true;
		default:
			return false;
		}
	} else {
		return false;
	}
}

int hnae3_register_client(struct hnae3_client *client)
{
	struct hnae3_client *client_tmp;
	struct hnae3_ae_dev *ae_dev;
	int ret;

	/* One system should only have one client for every type */
	list_for_each_entry(client_tmp, &hnae3_client_list, node) {
		if (client_tmp->type == client->type)
			return 0;
	}

	hnae3_list_add(&hnae3_list_client_lock, &client->node,
		       &hnae3_client_list);

	/* Check if there are matched ae_dev */
	list_for_each_entry(ae_dev, &hnae3_ae_dev_list, node) {
		if (hnae3_client_match(client->type, ae_dev->dev_type) &&
		    hnae_get_bit(ae_dev->flag, HNAE3_DEV_INITED_B)) {
			if (ae_dev->ops && ae_dev->ops->register_client) {
				ret = ae_dev->ops->register_client(client,
								   ae_dev);
				if (ret) {
					dev_err(&ae_dev->pdev->dev,
						"init ae_dev error.\n");
					return ret;
				}
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(hnae3_register_client);

void hnae3_unregister_client(struct hnae3_client *client)
{
	struct hnae3_ae_dev *ae_dev;

	/* Check if there are matched ae_dev */
	list_for_each_entry(ae_dev, &hnae3_ae_dev_list, node) {
		if (hnae3_client_match(client->type, ae_dev->dev_type) &&
		    hnae_get_bit(ae_dev->flag, HNAE3_DEV_INITED_B))
			if (ae_dev->ops && ae_dev->ops->unregister_client)
				ae_dev->ops->unregister_client(client, ae_dev);
	}
	hnae3_list_del(&hnae3_list_client_lock, &client->node);
}
EXPORT_SYMBOL(hnae3_unregister_client);

/* hnae_ae_register - register a AE engine to hnae framework
 * @hdev: the hnae ae engine device
 * @owner:  the module who provides this dev
 * NOTE: the duplicated name will not be checked
 */
int hnae3_register_ae_algo(struct hnae3_ae_algo *ae_algo)
{
	struct hnae3_ae_dev *ae_dev;
	struct hnae3_client *client;
	const struct pci_device_id *id;
	int ret;

	hnae3_list_add(&hnae3_list_ae_algo_lock, &ae_algo->node,
		       &hnae3_ae_algo_list);

	/* Check if there are matched ae_dev */
	list_for_each_entry(ae_dev, &hnae3_ae_dev_list, node) {
		id = pci_match_id(ae_algo->pdev_id_table, ae_dev->pdev);
		if (!id)
			continue;

		/* ae_dev init should set flag */
		ae_dev->ops = ae_algo->ops;
		ret = ae_algo->ops->init_ae_dev(ae_dev);
		if (!ret) {
			hnae_set_bit(ae_dev->flag, HNAE3_DEV_INITED_B, 1);
		} else {
			dev_err(&ae_dev->pdev->dev, "init ae_dev error.\n");
			return ret;
		}

		list_for_each_entry(client, &hnae3_client_list, node) {
			if (hnae3_client_match(client->type,
					       ae_dev->dev_type) &&
			    hnae_get_bit(ae_dev->flag, HNAE3_DEV_INITED_B)) {
				if (ae_dev->ops &&
				    ae_dev->ops->register_client) {
					ret = ae_dev->ops->register_client(
						client, ae_dev);
					if (ret) {
						dev_err(&ae_dev->pdev->dev,
							"init ae_dev error.\n");
						return ret;
					}
				}
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(hnae3_register_ae_algo);

/* hnae_ae_unregister - unregisters a HNAE AE engine
 * @cdev: the device to unregister
 */
void hnae3_unregister_ae_algo(struct hnae3_ae_algo *ae_algo)
{
	struct hnae3_ae_dev *ae_dev;
	struct hnae3_client *client;
	const struct pci_device_id *id;

	/* Check if there are matched ae_dev */
	list_for_each_entry(ae_dev, &hnae3_ae_dev_list, node) {
		id = pci_match_id(ae_algo->pdev_id_table, ae_dev->pdev);
		if (!id)
			continue;

		/* Check if there are matched client */
		list_for_each_entry(client, &hnae3_client_list, node) {
			if (hnae3_client_match(client->type,
					       ae_dev->dev_type) &&
			    hnae_get_bit(ae_dev->flag, HNAE3_DEV_INITED_B)) {
				hnae3_unregister_client(client);
				continue;
			}
		}

		ae_algo->ops->uninit_ae_dev(ae_dev);
		hnae_set_bit(ae_dev->flag, HNAE3_DEV_INITED_B, 0);
	}

	hnae3_list_del(&hnae3_list_ae_algo_lock, &ae_algo->node);
}
EXPORT_SYMBOL(hnae3_unregister_ae_algo);

/* hnae_ae_register - register a AE engine to hnae framework
 * @hdev: the hnae ae engine device
 * @owner:  the module who provides this dev
 * NOTE: the duplicated name will not be checked
 */
int hnae3_register_ae_dev(struct hnae3_ae_dev *ae_dev)
{
	struct hnae3_ae_algo *ae_algo;
	struct hnae3_client *client;
	const struct pci_device_id *id;
	int ret;

	hnae3_list_add(&hnae3_list_ae_dev_lock, &ae_dev->node,
		       &hnae3_ae_dev_list);

	/* Check if there are matched ae_algo */
	list_for_each_entry(ae_algo, &hnae3_ae_algo_list, node) {
		id = pci_match_id(ae_algo->pdev_id_table, ae_dev->pdev);
		if (!id)
			continue;

		ae_dev->ops = ae_algo->ops;

		/* ae_dev init should set flag */
		ret = ae_dev->ops->init_ae_dev(ae_dev);
		if (!ret) {
			hnae_set_bit(ae_dev->flag, HNAE3_DEV_INITED_B, 1);
			break;
		}

		dev_err(&ae_dev->pdev->dev, "init ae_dev error.\n");
		return ret;
	}

	if (!ae_dev->ops)
		return 0;

	list_for_each_entry(client, &hnae3_client_list, node) {
		if (hnae3_client_match(client->type, ae_dev->dev_type) &&
		    hnae_get_bit(ae_dev->flag, HNAE3_DEV_INITED_B)) {
			if (ae_dev->ops && ae_dev->ops->register_client) {
				ret = ae_dev->ops->register_client(client,
								   ae_dev);
				if (ret) {
					dev_err(&ae_dev->pdev->dev,
						"init ae_dev error.\n");
					return ret;
				}
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(hnae3_register_ae_dev);

/* hnae_ae_unregister - unregisters a HNAE AE engine
 * @cdev: the device to unregister
 */
void hnae3_unregister_ae_dev(struct hnae3_ae_dev *ae_dev)
{
	struct hnae3_ae_algo *ae_algo;
	struct hnae3_client *client;
	const struct pci_device_id *id;

	/* Check if there are matched ae_algo */
	list_for_each_entry(ae_algo, &hnae3_ae_algo_list, node) {
		id = pci_match_id(ae_algo->pdev_id_table, ae_dev->pdev);
		if (!id)
			continue;

		/* Check if there are matched client */
		list_for_each_entry(client, &hnae3_client_list, node) {
			if (hnae3_client_match(client->type,
					       ae_dev->dev_type) &&
			    hnae_get_bit(ae_dev->flag, HNAE3_DEV_INITED_B)) {
				hnae3_unregister_client(client);
				continue;
			}
		}

		ae_algo->ops->uninit_ae_dev(ae_dev);
		hnae_set_bit(ae_dev->flag, HNAE3_DEV_INITED_B, 0);
	}

	hnae3_list_del(&hnae3_list_ae_dev_lock, &ae_dev->node);
}
EXPORT_SYMBOL(hnae3_unregister_ae_dev);

static int __init hnae3_init(void)
{
	return 0;
}

static void __exit hnae3_exit(void)
{
}

subsys_initcall(hnae3_init);
module_exit(hnae3_exit);

MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HNAE3(Hisilicon Network Acceleration Engine) Framework");
