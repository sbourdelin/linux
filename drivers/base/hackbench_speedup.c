

#define pr_fmt(fmt) "HACKBENCH: " fmt

#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define POLLING_DELAY 1000

struct task_data {
	int id;
	struct delayed_work wq;
	struct mutex lock;
	int polling_delay;
	struct list_head node;
	struct list_head subtasks;
	bool container;
	int (*get_status)(struct task_data *td);
};


static LIST_HEAD(tasks_list);
static DEFINE_MUTEX(tasks_list_lock);


static int task_container_status(struct task_data *master)
{
	struct task_data *td;

	pr_info("container task status\n");

	if (!master || !master->container)
		return -EINVAL;

	if (list_empty(&master->subtasks))
		return -EINVAL;

	list_for_each_entry(td, &master->subtasks, subtasks) {
		/* mutex_lock(&td->lock); */
		/* get status */
		/* mutex_unlock(&td->lock); */
	}


	return 0;
}

static int task_status(struct task_data *td)
{
	pr_info("single task status\n");

	return 0;
}

static void task_status_check(struct work_struct *work)
{
	int ret;
	struct task_data *td = container_of(work, struct task_data, wq.work);

	if (!td->get_status)
		return;

	/* mutex_lock(&td->lock); */
	ret = td->get_status(td);
	/* mutex_unlock(&td->lock); */

	mod_delayed_work(system_freezable_wq, &td->wq,
			 msecs_to_jiffies(POLLING_DELAY));
}

static int __init task_status_init(void)
{
	int i;
	struct task_data *task;
	struct task_data *master;
	int total_tasks = 4;

	task = kzalloc(sizeof(struct task_data), GFP_KERNEL);
	if (!task)
		return -ENOMEM;

	INIT_LIST_HEAD(&task->subtasks);
	task->container = true;

	mutex_lock(&tasks_list_lock);
	list_add_tail(&task->node, &tasks_list);
	mutex_unlock(&tasks_list_lock);

	INIT_DELAYED_WORK(&task->wq, task_status_check);

	master = task;
	master->get_status = task_container_status;

	for (i = 1; i < total_tasks; i++) {

		task = kzalloc(sizeof(struct task_data), GFP_KERNEL);
		if (!task)
			goto clean;

		mutex_lock(&tasks_list_lock);
		list_add_tail(&task->node, &tasks_list);
		mutex_unlock(&tasks_list_lock);

		INIT_DELAYED_WORK(&task->wq, task_status_check);

		list_add_tail(&task->subtasks, &master->subtasks);

		task->get_status = task_status;

		mod_delayed_work(system_freezable_wq, &task->wq,
				 msecs_to_jiffies(POLLING_DELAY));
	}

	mod_delayed_work(system_freezable_wq, &master->wq,
			 msecs_to_jiffies(POLLING_DELAY));

	return 0;

clean:
	return -ENOMEM;
}


static void __exit task_exit(void)
{
	struct task_data *td, *tmp;

	list_for_each_entry_safe(td, tmp, &tasks_list, node) {
		mutex_lock(&td->lock);
		cancel_delayed_work(&td->wq);
		mutex_unlock(&td->lock);

		list_del(&td->node);
		kfree(td);
	}

	mutex_destroy(&tasks_list_lock);
}

module_init(task_status_init);
module_exit(task_exit);

MODULE_AUTHOR("Lukasz Luba <l.luba@partner.samsung.com>");
MODULE_DESCRIPTION("Test module which shows speed up in hackbench");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
