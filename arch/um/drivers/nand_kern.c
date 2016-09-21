#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nandsim.h>
#include <init.h>
#include <os.h>

static u_char id_bytes[8] = {
	[0 ... 7] = 0xFF,
};
static bool no_oob;
static char *backing_file;
static unsigned int bus_width;

module_param_array(id_bytes, byte, NULL, 0400);
module_param(no_oob, bool, 0400);
module_param(backing_file, charp, 0400);
module_param(bus_width, uint, 0400);

MODULE_PARM_DESC(backing_file, "File to use as backing store");
MODULE_PARM_DESC(id_bytes, "The ID bytes returned by NAND Flash 'read ID' command");
MODULE_PARM_DESC(no_oob, "Set to use an image without OOB data, i.e created by nanddump");
MODULE_PARM_DESC(bus_width, "Chip's bus width (8- or 16-bit)");

struct ns_uml_data {
	int fd;
	void *file_buf;
};

/*
 * We support only one instance so far, just to boot from MTD.
 * If you need more MTDs, use nandsimctl(8).
 */
static struct mtd_info *nsmtd;

static int file_read(struct nandsim *ns, char *addr, unsigned long count,
		     loff_t offset)
{
	struct ns_uml_data *data = nandsim_get_backend_data(ns);

	return os_pread_file(data->fd, addr, count, offset);
}

static ssize_t file_write(struct nandsim *ns, const char *addr, size_t count,
			  loff_t offset)
{
	struct ns_uml_data *data = nandsim_get_backend_data(ns);

	return os_pwrite_file(data->fd, addr, count, offset);
}

static void ns_uml_read_page(struct nandsim *ns, int num)
{
	__ns_file_read_page(ns, num, file_read);
}

static int ns_uml_prog_page(struct nandsim *ns, int num)
{
	struct ns_uml_data *data = nandsim_get_backend_data(ns);

	return __ns_file_prog_page(ns, num, data->file_buf, file_read,
				   file_write);
}

static void ns_uml_erase_sector(struct nandsim *ns)
{
	struct ns_uml_data *data = nandsim_get_backend_data(ns);

	__ns_file_erase_sector(ns, data->file_buf, file_write);
}

static int ns_uml_init(struct nandsim *ns, struct nandsim_params *nsparam)
{
	struct ns_uml_data *data = kzalloc(sizeof(*data), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	data->file_buf = kmalloc(nandsim_get_geom(ns)->pgszoob, GFP_KERNEL);
	if (!data->file_buf) {
		kfree(data);
		return -ENOMEM;
	}

	data->fd = os_open_file(nsparam->cache_file, of_set_rw(OPENFLAGS(), 1, 1), 0);
	if (data->fd < 0) {
		printk(KERN_ERR "Unable to open %s: %i\n", nsparam->cache_file, data->fd);
		kfree(data->file);
		kfree(data);
		return data->fd;
	}

	nandsim_set_backend_data(ns, data);

	return 0;
}

static void ns_uml_destroy(struct nandsim *ns)
{
	struct ns_uml_data *data = nandsim_get_backend_data(ns);

	if (!data)
		return;

	os_close_file(data->fd);
	kfree(data->file_buf);
	kfree(data);
}

static struct ns_backend_ops ns_uml_bops = {
	.erase_sector = ns_uml_erase_sector,
	.prog_page = ns_uml_prog_page,
	.read_page = ns_uml_read_page,
	.init = ns_uml_init,
	.destroy = ns_uml_destroy,
	.name = "uml",
};

static struct nandsim_params params = {
	.bops = &ns_uml_bops,
};

static int __init uml_ns_init(void)
{
	struct mtd_info *ret;

	if (!backing_file)
		return 0;

	params.cache_file = backing_file;
	params.bus_width = bus_width;
	params.no_oob = no_oob;
	memcpy(params.id_bytes, id_bytes, sizeof(params.id_bytes));

	ret = ns_new_instance(&params);
	if (IS_ERR(ret))
		return PTR_ERR(ret);

	nsmtd = ret;

	return 0;
}
late_initcall(uml_ns_init);

static void __exit uml_ns_exit(void)
{
	/*
	 * Since this driver is a singleton we can rely on module refcounting,
	 * and assume that ns_destroy_instance() will succeed in any case.
	 * If not, print a frindly warning. B-)
	 */
	WARN_ON(ns_destroy_instance(nsmtd) != 0);
}
module_exit(uml_ns_exit);

MODULE_AUTHOR("Richard Weinberger");
MODULE_DESCRIPTION("UML nandsim backend");
MODULE_LICENSE("GPL");

