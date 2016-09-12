/*
*  Advantech SAS hard disk hot swap button driver
*
*  Copyright (C) 2016 Advantech
*
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License version 2 as
*  published by the Free Software Foundation.
*/


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <asm/siginfo.h>
#include <linux/rcupdate.h>//rcu_read_lock
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/moduleparam.h>
#include "hdd_hp_btn.h"

#define LPC_ADDR 0x900
#define EFB_WB_ADDR (LPC_ADDR+0x52)
#define EFB_WB_WRITE (LPC_ADDR+0x53)
#define EFB_WB_READ (LPC_ADDR+0x54)
#define EFB_WB_CTRL (LPC_ADDR+0x55)
#define EFB_WB_RD_CTL 0x2
#define EFB_WB_WR_CTL 0x1

#define FRU_LED_ADDR (LPC_ADDR+0x16)

#define FPGA_SIRQ_CFG (LPC_ADDR+0x2C)
#define FPGA_SIRQ_REG (LPC_ADDR+0x2E)
#define FPGA_SIRQ_5 0x1
#define FPGA_SIRQ_6 0x2
#define FPGA_SIRQ_7 0x3
#define FPGA_SIRQ_9 0x4
#define FPGA_SIRQ_10 0x5
#define FPGA_SIRQ_11 0x6
#define FPGA_SIRQ_12 0x7
#define FPGA_SIRQ_13 0x8
#define FPGA_SIRQ_14 0x9
#define FPGA_SIRQ_15 0xA
#define FPGA_SIRQ_INTA 0xC
#define FPGA_SIRQ_INTB 0xD
#define FPGA_SIRQ_INTC 0xE
#define FPGA_SIRQ_INTD 0xF
#define FPGA_SIRQ_NUM 15

#define IOCTL_EFB_WB_WRITE 0x7F
#define IOCTL_EFB_WB_READ 0x7E
#define IOCTL_FRU_LED_CTL 0x80
/*************************************************************/
/*   Start of SYSFS kobject define                           */
/*                                                           */

static struct kobject *lpc_kobj;
static struct kobject *lpc_user_led_kobj;
static struct kobject *lpc_register_kobj;
static struct kobject *lpc_rtm_kobj;

static int free_irq_num = FPGA_SIRQ_NUM;
static int pid;
static int signal_num = HDD_SWAP_SIG;

module_param(signal_num, int, 0);

static ssize_t reg40_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x40);
	return sprintf(buf, "%d:%02X\n", reg, reg);
}

static ssize_t reg40_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int reg;

	if (sscanf(buf, "%d", &reg) != 1)
		return -EINVAL;

	outb(reg, LPC_ADDR + 0x40);
	return count;
}

static struct kobj_attribute reg40_attribute =
	__ATTR(40, 0664, reg40_show, reg40_store);

static ssize_t reg41_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x41);
	return sprintf(buf, "%d:%02X\n", reg, reg);
}

static ssize_t reg41_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int reg;

	if (sscanf(buf, "%d", &reg) != 1)
		return -EINVAL;

	outb(reg, LPC_ADDR + 0x41);
	return count;
}

static struct kobj_attribute reg41_attribute =
	__ATTR(41, 0664, reg41_show, reg41_store);

static ssize_t reg42_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x42);
	return sprintf(buf, "%d:%02X\n", reg, reg);
}

static ssize_t reg42_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int reg;

	if (sscanf(buf, "%d", &reg) != 1)
		return -EINVAL;

	outb(reg, LPC_ADDR + 0x42);
	return count;
}

static struct kobj_attribute reg42_attribute =
	__ATTR(42, 0664, reg42_show, reg42_store);

static ssize_t reg43_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x43);
	return sprintf(buf, "%d:%02X\n", reg, reg);
}

static ssize_t reg43_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int reg;

	if (sscanf(buf, "%d", &reg) != 1)
		return -EINVAL;

	if (reg < 255 && reg >= 0)
		outb(reg, LPC_ADDR + 0x43);
	return count;
}

static struct kobj_attribute reg43_attribute =
	__ATTR(43, 0664, reg43_show, reg43_store);

static ssize_t reg30_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x30);
	return sprintf(buf, "%d\n", reg);
}

static ssize_t reg30_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int reg;

	if (sscanf(buf, "%d", &reg) != 1)
		return -EINVAL;

	if (reg < 255 && reg >= 0)
		outb(reg, LPC_ADDR + 0x30);
	return count;
}

static struct kobj_attribute reg30_attribute =
	__ATTR(30, 0664, reg30_show, reg30_store);

static struct attribute *attrs_register[] = {
	&reg40_attribute.attr,
	&reg41_attribute.attr,
	&reg42_attribute.attr,
	&reg43_attribute.attr,
	&reg30_attribute.attr,
	NULL,
};

static DECLARE_WAIT_QUEUE_HEAD(bt_wq0);
static int bt_wait0;
static int bt_flag0;

static ssize_t button0_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x40) & 0x10;
	if (bt_wait0) {
		wait_event_interruptible(bt_wq0, bt_flag0 != 0);
		bt_flag0 = 0;
	}
	return sprintf(buf, "wait=%d,reg=%d\n", bt_wait0, reg);
}

static ssize_t button0_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (sscanf(buf, "%d", &bt_wait0) != 1)
		return -EINVAL;

	return count;
}

static struct kobj_attribute button0_attribute =
	__ATTR(button0, 0664, button0_show, button0_store);

static DECLARE_WAIT_QUEUE_HEAD(bt_wq1);
static int bt_wait1;
static int bt_flag1;

static ssize_t button1_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x40) & 0x20;
	if (bt_wait1) {
		wait_event_interruptible(bt_wq1, bt_flag1 != 0);
		bt_flag1 = 0;
	}
	return sprintf(buf, "wait=%d,reg=%d\n", bt_wait1, reg);
}

static ssize_t button1_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (sscanf(buf, "%d", &bt_wait1) != 1)
		return -EINVAL;

	return count;
}

static struct kobj_attribute button1_attribute =
	__ATTR(button1, 0664, button1_show, button1_store);

static DECLARE_WAIT_QUEUE_HEAD(prsnt_wq0);
static int prsnt_wait0;
static int prsnt_flag0;

static ssize_t prsnt0_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x40) & 0x01;
	if (prsnt_wait0) {
		wait_event_interruptible(prsnt_wq0, prsnt_flag0 != 0);
		prsnt_flag0 = 0;
	}
	return sprintf(buf, "wait=%d,reg=%d\n", prsnt_wait0, reg);
}

static ssize_t prsnt0_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (sscanf(buf, "%d", &prsnt_wait0) != 1)
		return -EINVAL;

	return count;
}

static struct kobj_attribute prsnt0_attribute =
	__ATTR(disk1_present, 0664, prsnt0_show, prsnt0_store);

static DECLARE_WAIT_QUEUE_HEAD(prsnt_wq1);
static int prsnt_wait1;
static int prsnt_flag1;

static ssize_t prsnt1_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x40) & 0x02;
	if (prsnt_wait1) {
		wait_event_interruptible(prsnt_wq1, prsnt_flag1 != 0);
		prsnt_flag1 = 0;
	}
	return sprintf(buf, "wait=%d,reg=%d\n", prsnt_wait1, reg);
}

static ssize_t prsnt1_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (sscanf(buf, "%d", &prsnt_wait1) != 1)
		return -EINVAL;

	return count;
}

static struct kobj_attribute prsnt1_attribute =
	__ATTR(disk2_present, 0664, prsnt1_show, prsnt1_store);

static ssize_t rtmled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int reg;

	reg = inb(LPC_ADDR + 0x43);
	return sprintf(buf, "%d\n", reg);
}

static ssize_t rtmled_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int reg;

	if (sscanf(buf, "%d", &reg) != 1)
		return -EINVAL;

	outb(reg, LPC_ADDR + 0x43);
	return count;
}

static struct kobj_attribute rtmled_attribute =
	__ATTR(led, 0664, rtmled_show, rtmled_store);

static struct attribute *attrs_rtm[] = {
	&button0_attribute.attr,
	&button1_attribute.attr,
	&prsnt0_attribute.attr,
	&prsnt1_attribute.attr,
	&rtmled_attribute.attr,
	NULL,
};


static struct attribute_group attr_register_group = {
	.attrs = attrs_register,
};

static struct attribute_group attr_rtm_group = {
	.attrs = attrs_rtm,
};


static int send_signal(int signal, int event)
{
	struct siginfo info;
	struct task_struct *task;
	int ret = 0;

	memset(&info, 0, sizeof(struct siginfo));
	info.si_signo = signal;
	info.si_code = SI_QUEUE;
	info.si_int = event;

	rcu_read_lock();
	task = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
	if (task == NULL) {
		printk(KERN_INFO "No such pid\n");
		rcu_read_unlock();
		return -ENODEV;
	}
	rcu_read_unlock();

	ret = send_sig_info(signal, &info, task);
	if (ret < 0) {
		printk(KERN_INFO "Sending signal error\n");
		return ret;
	}
	return 0;
}


/*******************************************************************/
/*       Start of DEVFS define                                     */
static ssize_t drv_read(struct file *filp, char *buf, size_t count, loff_t *ppos)
{
	return count;
}

static ssize_t drv_write(struct file *filp, const char *buf, size_t count, loff_t *ppos)
{
	char mybuf[10];
	/* read the value from user space */
	if (count > 10)
		return -EINVAL;

	if (copy_from_user(mybuf, buf, count))
		return -EFAULT;

	if (sscanf(mybuf, "%d", &pid) != 1)
		return -EFAULT;

	printk(KERN_INFO "User pid = %d\n", pid);

	return count;
}

static int drv_open(struct inode *inode, struct file *filp)
{
	return 0;
}

long drv_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ioctl_cmd data;

	memset(&data, 0, sizeof(data));

	switch (cmd) {
	case IOCTL_LED_ON:
		if (copy_from_user(&data, (int __user *) arg, sizeof(data)))
			return -EFAULT;

		if (data.val == 1) {
			outb(0x10, LPC_ADDR + 0x43);
			printk(KERN_INFO"Dev:LED1 ON\n");
		} else if (data.val == 2) {
			outb(0x20, LPC_ADDR + 0x43);
			printk(KERN_INFO"Dev:LED2 ON\n");
		}
		break;
	case IOCTL_LED_OFF:
		if (copy_from_user(&data, (int __user *) arg, sizeof(data)))
			return -EFAULT;

		if (data.val == 1) {
			outb(0x01, LPC_ADDR + 0x43);
			printk(KERN_INFO"Dev:LED1 OFF\n");
		} else if (data.val == 2) {
			outb(0x02, LPC_ADDR + 0x43);
			printk(KERN_INFO"Dev:LED2 OFF\n");
		}
		break;
	}
	return 0;
}

static int drv_release(struct inode *inode, struct file *filp)
{
	return 0;
}

irqreturn_t HDD_IRQ(int irq, void *dev_id)
{
	unsigned char tmp = 0;

	printk(KERN_INFO"HDD_IRQ5:INTERRUPT\n");
	tmp = inb(LPC_ADDR + 0x40);

	if ((tmp & 0x33) == 0)
		return IRQ_NONE;

	outb(tmp, LPC_ADDR + 0x40);

	/*HDD1_HP_SW*/
	if (tmp & 0x10) {
		send_signal(signal_num, SIG_BUTTON1_INVOKE);
		bt_flag0 = 1;
		wake_up_interruptible(&bt_wq0);
	}

	/*HDD2_HP_SW*/
	if (tmp & 0x20) {
		send_signal(signal_num, SIG_BUTTON2_INVOKE);
		bt_flag1 = 1;
		wake_up_interruptible(&bt_wq1);
	}

	/*HDD1_INSERT*/
	if (tmp & 0x01) {
		send_signal(signal_num, SIG_HDD1_INSERT);
		prsnt_flag0 = 1;
		wake_up_interruptible(&prsnt_wq0);
	}

	/*HDD2_INSERT*/
	if (tmp & 0x02) {
		send_signal(signal_num, SIG_HDD2_INSERT);
		prsnt_flag1 = 1;
		wake_up_interruptible(&prsnt_wq1);
	}

	return IRQ_HANDLED;
};

const struct file_operations drv_fops = {
	owner: THIS_MODULE,
	read : drv_read,
	write : drv_write,
	unlocked_ioctl : drv_ioctl,
	open : drv_open,
	release : drv_release,
};

#define DRIVER_NAME "hdd_hp_btn"
static unsigned int demo_chrdev_alloc_major = 0;
static unsigned int num_of_dev = 1;
static struct cdev demo_chrdev_alloc_cdev;
struct class *demo_class;

#define MODULE_NAME "hdd_hp_btn"
static int demo_init(void)
{
	dev_t dev = MKDEV(demo_chrdev_alloc_major, 0);
	int alloc_ret = 0;
	int cdev_ret = 0;
	int kobj_retval = 0;
	int result;

	alloc_ret = alloc_chrdev_region(&dev, 0, num_of_dev, DRIVER_NAME); //$cat /proc/devices
	if (alloc_ret)
		goto error;

	demo_chrdev_alloc_major = MAJOR(dev);
	cdev_init(&demo_chrdev_alloc_cdev, &drv_fops);
	demo_chrdev_alloc_cdev.owner = THIS_MODULE;
	demo_chrdev_alloc_cdev.ops = &drv_fops;
	cdev_ret = cdev_add(&demo_chrdev_alloc_cdev, dev, num_of_dev);

	if (cdev_ret)
		goto error;

	printk(KERN_ALERT "%s driver(major number %d) installed.\n", DRIVER_NAME, demo_chrdev_alloc_major);
	demo_class = class_create(THIS_MODULE, "demo_class");
	if (IS_ERR(demo_class)) {
		printk(KERN_ERR "Err:failed in creating class/n");
		goto error;
	}
	device_create(demo_class, NULL, MKDEV(demo_chrdev_alloc_major, 0), NULL, "hdd_hp_btn");

	if (request_region(LPC_ADDR, 0x80, "hdd_hp_btn") == NULL)
		printk("Err: in request region\n");

	/* Init interrupt*/
	outb(0x11, FPGA_SIRQ_CFG);
	outb(0xff, LPC_ADDR+0x40);
	outb(0x33, LPC_ADDR+0x41);

	/* Init IRQ5 interrupt*/
	result = request_irq(5, HDD_IRQ, IRQF_SHARED, "HDD_IRQ5", (void *)HDD_IRQ);
	if (result) {
		printk(KERN_INFO "short: can't get assigned irq %d,%d\n", FPGA_SIRQ_5, result);
		free_irq_num = 0;
	}

	/* start kobject create*/
	lpc_kobj = kobject_create_and_add("fpga_lpc", kernel_kobj);
	if (!lpc_kobj)
		return -ENOMEM;

	lpc_user_led_kobj = kobject_create_and_add("user_led", lpc_kobj);
	if (!lpc_user_led_kobj) {
		kobject_put(lpc_kobj);
		return -ENOMEM;
	}

	lpc_register_kobj = kobject_create_and_add("register", lpc_kobj);
	if (!lpc_register_kobj) {
		kobject_put(lpc_kobj);
		kobject_put(lpc_user_led_kobj);
		return -ENOMEM;
	}

	lpc_rtm_kobj = kobject_create_and_add("RTM", lpc_kobj);
	if (!lpc_rtm_kobj) {
		kobject_put(lpc_kobj);
		kobject_put(lpc_user_led_kobj);
		kobject_put(lpc_rtm_kobj);
		return -ENOMEM;
	}

	kobj_retval = sysfs_create_group(lpc_register_kobj, &attr_register_group);
	kobj_retval = sysfs_create_group(lpc_rtm_kobj, &attr_rtm_group);
	if (kobj_retval) {
		kobject_put(lpc_user_led_kobj);
		kobject_put(lpc_register_kobj);
		kobject_put(lpc_kobj);
	}

	return kobj_retval;

	error:
	if (cdev_ret == 0)
		cdev_del(&demo_chrdev_alloc_cdev);

	if (alloc_ret == 0)
		unregister_chrdev_region(dev, num_of_dev);
	return -1;
}


static void demo_exit(void)
{
	dev_t dev = MKDEV(demo_chrdev_alloc_major, 0);

	release_region(LPC_ADDR, 0x80);
	cdev_del(&demo_chrdev_alloc_cdev);
	device_destroy(demo_class, MKDEV(demo_chrdev_alloc_major, 0));
	class_destroy(demo_class);
	unregister_chrdev_region(dev, num_of_dev);
	printk(KERN_ALERT "%s driver removed.\n", DRIVER_NAME);

	kobject_put(lpc_rtm_kobj);
	kobject_put(lpc_register_kobj);
	kobject_put(lpc_user_led_kobj);
	kobject_put(lpc_kobj);
	if (free_irq_num != 0) {
		free_irq(free_irq_num, &free_irq_num);
		free_irq(5, (void *)HDD_IRQ);
	}
}

module_init(demo_init);
module_exit(demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Advantech");
MODULE_DESCRIPTION("HDD SWAP Driver");
MODULE_PARM_DESC(signal_num, "A signal number variable");
MODULE_VERSION("1.00");
