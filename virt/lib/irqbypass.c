/*
 * IRQ offload/bypass manager
 *
 * Copyright (C) 2015 Red Hat, Inc.
 * Copyright (c) 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Various virtualization hardware acceleration techniques allow bypassing or
 * offloading interrupts received from devices around the host kernel.  Posted
 * Interrupts on Intel VT-d systems can allow interrupts to be received
 * directly by a virtual machine.  ARM IRQ Forwarding allows forwarded physical
 * interrupts to be directly deactivated by the guest.  This manager allows
 * interrupt producers and consumers to find each other to enable this sort of
 * bypass.
 */

#include <linux/irqbypass.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rculist.h>

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("IRQ bypass manager utility module");

static LIST_HEAD(producers);
static LIST_HEAD(consumers);
static DEFINE_MUTEX(lock);

/* @lock must be held when calling connect */
static int __connect(struct irq_bypass_producer *prod,
		     struct irq_bypass_consumer *cons)
{
	int ret = 0;

	if (prod->stop)
		prod->stop(prod);
	if (cons->stop)
		cons->stop(cons);

	if (prod->add_consumer)
		ret = prod->add_consumer(prod, cons);

	if (!ret) {
		ret = cons->add_producer(cons, prod);
		if (ret && prod->del_consumer)
			prod->del_consumer(prod, cons);
	}

	if (!ret && cons->handle_irq)
		list_add_rcu(&cons->sibling, &prod->consumers);
	return ret;
}

/* @lock must be held when calling disconnect */
static void __disconnect(struct irq_bypass_producer *prod,
			 struct irq_bypass_consumer *cons)
{
	if (prod->stop)
		prod->stop(prod);
	if (cons->stop)
		cons->stop(cons);

	cons->del_producer(cons, prod);

	if (prod->del_consumer)
		prod->del_consumer(prod, cons);

	if (cons->handle_irq) {
		list_del_rcu(&cons->sibling);
		synchronize_srcu(&prod->srcu);
	}

	if (cons->start)
		cons->start(cons);
	if (prod->start)
		prod->start(prod);
}

/**
 * irq_bypass_register_producer - register IRQ bypass producer
 * @producer: pointer to producer structure
 *
 * Add the provided IRQ producer to the list of producers and connect
 * with any matching token found on the IRQ consumers list.
 */
int irq_bypass_register_producer(struct irq_bypass_producer *producer)
{
	struct irq_bypass_producer *tmp;
	struct list_head *node, *next, siblings = LIST_HEAD_INIT(siblings);
	int ret;

	might_sleep();

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	mutex_lock(&lock);

	INIT_LIST_HEAD(&producer->consumers);
	init_srcu_struct(&producer->srcu);

	list_for_each_entry(tmp, &producers, node) {
		if (tmp->token == producer->token) {
			mutex_unlock(&lock);
			module_put(THIS_MODULE);
			return -EBUSY;
		}
	}

	list_for_each_safe(node, next, &consumers) {
		struct irq_bypass_consumer *consumer = container_of(
			node, struct irq_bypass_consumer, node);

		if (consumer->token == producer->token) {
			ret = __connect(producer, consumer);
			if (ret)
				goto error;
			/* Keep the connected consumers temply */
			list_del(&consumer->node);
			list_add_rcu(&consumer->node, &siblings);
		}
	}

	list_for_each_safe(node, next, &siblings) {
		struct irq_bypass_consumer *consumer = container_of(
			node, struct irq_bypass_consumer, node);

		list_del(&consumer->node);
		list_add(&consumer->node, &consumers);
		if (consumer->start)
			consumer->start(consumer);
	}

	if (producer->start)
		producer->start(producer);
	list_add(&producer->node, &producers);

	mutex_unlock(&lock);
	return 0;

error:
	list_for_each_safe(node, next, &siblings) {
		struct irq_bypass_consumer *consumer = container_of(
			node, struct irq_bypass_consumer, node);

		list_del(&consumer->node);
		list_add(&consumer->node, &consumers);
	}
	mutex_unlock(&lock);
	module_put(THIS_MODULE);
	return ret;
}
EXPORT_SYMBOL_GPL(irq_bypass_register_producer);

/**
 * irq_bypass_unregister_producer - unregister IRQ bypass producer
 * @producer: pointer to producer structure
 *
 * Remove a previously registered IRQ producer from the list of producers
 * and disconnect it from any connected IRQ consumer.
 */
void irq_bypass_unregister_producer(struct irq_bypass_producer *producer)
{
	struct irq_bypass_producer *tmp, *n;
	struct irq_bypass_consumer *consumer;

	might_sleep();

	if (!try_module_get(THIS_MODULE))
		return; /* nothing in the list anyway */

	mutex_lock(&lock);

	list_for_each_entry_safe(tmp, n, &producers, node) {
		if (tmp->token != producer->token)
			continue;

		list_for_each_entry(consumer, &consumers, node) {
			if (consumer->token == producer->token) {
				__disconnect(producer, consumer);
				break;
			}
		}

		list_del(&producer->node);
		module_put(THIS_MODULE);
		break;
	}

	cleanup_srcu_struct(&producer->srcu);
	mutex_unlock(&lock);

	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(irq_bypass_unregister_producer);

/**
 * irq_bypass_register_consumer - register IRQ bypass consumer
 * @consumer: pointer to consumer structure
 *
 * Add the provided IRQ consumer to the list of consumers and connect
 * with any matching token found on the IRQ producer list.
 */
int irq_bypass_register_consumer(struct irq_bypass_consumer *consumer)
{
	struct irq_bypass_consumer *tmp;
	struct irq_bypass_producer *producer;

	if (!consumer->add_producer || !consumer->del_producer)
		return -EINVAL;

	if (consumer->handle_irq && !consumer->irq_context)
		return -EINVAL;

	might_sleep();

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	mutex_lock(&lock);

	list_for_each_entry(tmp, &consumers, node) {
		if (tmp == consumer) {
			mutex_unlock(&lock);
			module_put(THIS_MODULE);
			return -EBUSY;
		}
	}

	list_for_each_entry(producer, &producers, node) {
		if (producer->token == consumer->token) {
			int ret = __connect(producer, consumer);
			if (ret) {
				mutex_unlock(&lock);
				module_put(THIS_MODULE);
				return ret;
			}
			if (consumer->start)
				consumer->start(consumer);
			if (producer->start)
				producer->start(producer);
			break;
		}
	}

	list_add(&consumer->node, &consumers);

	mutex_unlock(&lock);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_bypass_register_consumer);

/**
 * irq_bypass_unregister_consumer - unregister IRQ bypass consumer
 * @consumer: pointer to consumer structure
 *
 * Remove a previously registered IRQ consumer from the list of consumers
 * and disconnect it from any connected IRQ producer.
 */
void irq_bypass_unregister_consumer(struct irq_bypass_consumer *consumer)
{
	struct irq_bypass_consumer *tmp, *n;
	struct irq_bypass_producer *producer;

	might_sleep();

	if (!try_module_get(THIS_MODULE))
		return; /* nothing in the list anyway */

	mutex_lock(&lock);

	list_for_each_entry_safe(tmp, n, &consumers, node) {
		if (tmp != consumer)
			continue;

		list_for_each_entry(producer, &producers, node) {
			if (producer->token == consumer->token) {
				__disconnect(producer, consumer);
				break;
			}
		}

		list_del(&consumer->node);
		module_put(THIS_MODULE);
		break;
	}

	mutex_unlock(&lock);

	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(irq_bypass_unregister_consumer);
