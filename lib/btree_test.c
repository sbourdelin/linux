#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/btree.h>
#include <linux/types.h>

#define NODES 24

struct test_node {
	u32 key;
	u32 val;
};

static struct btree_head32 bh;
static struct test_node nodes[NODES];

static void init(void)
{
	int i;

	for (i = 0; i < NODES; i++) {
		nodes[i].key = i;
		nodes[i].val = i;
	}
}
static int __init btree_test_init(void)
{
	u32 key = 0;
	u32 *val = NULL;
	int i, rc;

	pr_alert("btree testing\n");

	init();
	rc = btree_init32(&bh);

	if (rc)
		pr_alert("Unable initialize btree memory\n");

	for (i = 0; i < NODES; i++) {
		rc = btree_insert32(&bh,
				nodes[i].key,
				&nodes[i].val,
				GFP_ATOMIC);

		if (rc)
			pr_alert("Unable to insert key into btree\n");
	}

	pr_alert("========================================\n");

	btree_for_each_safe32(&bh, key, val) {
		pr_alert("val %d\n", *val);
	}

	btree_remove32(&bh, 11);

	pr_alert("========================================\n");
	btree_for_each_safe32(&bh, key, val) {
		pr_alert("val %d\n", *val);
	}

	return 0;
}
static void __exit btree_test_exit(void)
{
	pr_alert("test exit\n");
	btree_destroy32(&bh);

}

module_init(btree_test_init);
module_exit(btree_test_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Leno Hou <lenohou@gmai.com>");
MODULE_DESCRIPTION("Simple In-memory B+ Tree test");
