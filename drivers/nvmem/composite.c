#define DEBUG 1

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

struct nvmem_composite {
	struct device *dev;
	struct list_head layout;
	size_t layout_size;
};

struct nvmem_composite_item {
	struct nvmem_cell *cell;
	unsigned int idx, start, end, size;
	struct list_head node;
};

static struct nvmem_composite_item *
nvmem_composite_find_first(struct nvmem_composite *ncomp,
			   unsigned int offset)
{
	struct nvmem_composite_item *first;
	list_for_each_entry(first, &ncomp->layout, node) {
		/* 
		 * Skip all of the irrelevant items that end before our offset
		 */
		if (first->end > offset)
			return first;
	}

	return NULL;
}

static int nvmem_composite_read(void *context,
				const void *reg, size_t reg_size,
				void *val, size_t val_size)
{
	struct nvmem_composite *ncomp = context;
	const unsigned int offset = *(u32 *)reg;
	void *dst = val;
	size_t residue = val_size;
	struct nvmem_composite_item *item, *first;
	uint8_t *data;
	unsigned int size, chunk, ii;


	first = item = nvmem_composite_find_first(ncomp, offset);
	if (!first) {
		dev_dbg(ncomp->dev, "Invalid offset\n");
		return -EINVAL;
	}

	list_for_each_entry_from(item, &ncomp->layout, node) {
		/* 
		 * If our first read is not located on item boundary
		 * we have to introduce artificial offset
		 */
		ii = (item == first) ? offset - first->start : 0;

		data = nvmem_cell_read(item->cell, &size);
		if (IS_ERR(data)) {
			dev_dbg(ncomp->dev, "Failed to read nvmem cell\n");
			return PTR_ERR(data);
		}

		chunk = min(residue, item->size - ii);
		memcpy(dst, &data[item->idx + ii], chunk);
		kfree(data);

		dst += chunk;
		residue -= chunk;
	}

	return (residue) ? -EINVAL : 0;
}

static int nvmem_composite_write(void *context, const void *data,
				 size_t count)
{
	return -ENOTSUPP;
}

static const struct regmap_bus nvmem_composite_regmap_bus = {
	.write = nvmem_composite_write,
	.read  = nvmem_composite_read,
};

static int nvmem_composite_validate_dt(struct device_node *np)
{
	/* FIXME */
	return 0;
}

static int nvmem_composite_probe(struct platform_device *pdev)
{
	int i, ret;
	unsigned int start = 0;
	unsigned int len;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct nvmem_device *nvmem;
	struct nvmem_composite *ncomp;
	struct regmap *map;
	struct nvmem_composite_item *item;
	struct nvmem_config  nv_cnf = {0};
	struct regmap_config rm_cnf = {0};
	const __be32 *addr;
	unsigned int item_count;

	ret = nvmem_composite_validate_dt(np);
	if (ret < 0) {
		dev_dbg(dev, "Device validation failed\n");
		return ret;
	}

	ncomp = devm_kzalloc(dev, sizeof(*ncomp), GFP_KERNEL);
	INIT_LIST_HEAD(&ncomp->layout);
	ncomp->dev = dev;
	
	addr = of_get_property(np, "layout", &len);
	item_count = len / (3 * sizeof(__be32));

	for (i = 0; i < item_count; i++) {
		struct device_node *cell_np;
		uint32_t phandle;

		item = devm_kzalloc(dev, sizeof(*item), GFP_KERNEL);

		phandle = be32_to_cpup(addr++);
		cell_np = of_find_node_by_phandle(phandle);
		if (!cell_np) {
			dev_dbg(dev,
				"Couldn't find nvmem cell by its phandle\n");
			return -ENOENT;
		}

		item->cell = of_nvmem_cell_from_device_node(cell_np);
		if (IS_ERR(item->cell)) {
			dev_dbg(dev,
				"Failed to instantiate nvmem cell from "
				"a device tree node\n");
			ret = PTR_ERR(item->cell);
			goto unwind;
		}

		item->start = start;
		item->idx = be32_to_cpup(addr++);
		item->size = be32_to_cpup(addr++);
		item->end  = item->size - 1;
		ncomp->layout_size += item->size;
		start += item->size;

		list_add_tail(&item->node, &ncomp->layout);
	}

	rm_cnf.reg_bits = 32;
	rm_cnf.val_bits = 8;
	rm_cnf.reg_stride = 1;
	rm_cnf.name = "nvmem-composite";
	rm_cnf.max_register = ncomp->layout_size - 1;

	map = devm_regmap_init(dev,
			       &nvmem_composite_regmap_bus,
			       ncomp,
			       &rm_cnf);
	if (IS_ERR(map)) {
		dev_dbg(dev, "Failed to initilize regmap\n");
		return PTR_ERR(map);
	}

	nv_cnf.name = "nvmem-composite";
	nv_cnf.read_only = true;
	nv_cnf.dev = dev;

	nvmem = nvmem_register(&nv_cnf);
	if (IS_ERR(nvmem)) {
		dev_dbg(dev, "Failed to register 'nvmem' device\n");
		return PTR_ERR(nvmem);
	}

	platform_set_drvdata(pdev, nvmem);
	return 0;
unwind:
	/* FIXME  */
	return ret;
}

static int nvmem_composite_remove(struct platform_device *pdev)
{
	struct nvmem_device *nvmem = platform_get_drvdata(pdev);

	/* 
	   FIXME free allocated nvmem cells
	 */
	return nvmem_unregister(nvmem);
}


static const struct of_device_id nvmem_composite_dt_ids[] = {
	{ .compatible = "nvmem-composite", },
	{ },
};
MODULE_DEVICE_TABLE(of, nvmem_composite_dt_ids);

static struct platform_driver nvmem_composite_driver = {
	.probe	= nvmem_composite_probe,
	.remove	= nvmem_composite_remove,
	.driver = {
		.name	= "nvmem-composite",
		.of_match_table = nvmem_composite_dt_ids,
	},
};
module_platform_driver(nvmem_composite_driver);

MODULE_AUTHOR("Andrey Smirnov <andrew.smirnov@gmail.com>");
MODULE_DESCRIPTION("FIXME");
MODULE_LICENSE("GPL v2");
