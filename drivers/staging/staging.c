#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>

static int staging_init(void)
{
	return 0;
}

static void staging_exit(void)
{
}

module_init(staging_init);
module_exit(staging_exit);

MODULE_AUTHOR("Greg Kroah-Hartman");
MODULE_DESCRIPTION("Staging Core");
MODULE_LICENSE("GPL");
