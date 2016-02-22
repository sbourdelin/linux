#define DEBUG 1

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

struct nvmem_blob {
	const u8 *data;
	size_t data_size;
};

static int nvmem_blob_write(void *context, const void *data,
				size_t count)
{
	return -ENOTSUPP;
}

static int nvmem_blob_read(void *context,
			   const void *reg, size_t reg_size,
			   void *val, size_t val_size)
{
	struct nvmem_blob *nblob = context;
	const unsigned int offset = *(u32 *)reg;
	
	memcpy(val, nblob->data + offset,
	       min(val_size, nblob->data_size - offset));
	return 0;
}

static const struct regmap_bus nvmem_blob_regmap_bus = {
	.write = nvmem_blob_write,
	.read  = nvmem_blob_read,
};


static int nvmem_blob_validate_dt(struct device_node *np)
{
	return 0;
}

static int nvmem_blob_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regmap *map;
	struct nvmem_device *nvmem;
	struct nvmem_blob *nblob;
	struct property *pp;
	struct nvmem_config  nv_cnf = {0};
	struct regmap_config rm_cnf = {0};

	ret = nvmem_blob_validate_dt(np);
	if (ret < 0) {
		dev_dbg(dev, "Device tree validation failed\n");
		return ret;
	}

	nblob = devm_kzalloc(dev, sizeof(*nblob), GFP_KERNEL);
	if (!nblob) {
		dev_dbg(dev, "Not enough memory to allocate a blob\n");
		return -ENOMEM;
	}

	pp = of_find_property(np, "data", NULL);
	BUG_ON(!pp);
	
	nblob->data = pp->value;
	nblob->data_size = pp->length;

	rm_cnf.reg_bits = 32;
	rm_cnf.val_bits = 8;
	rm_cnf.reg_stride = 1;
	rm_cnf.name = "nvmem-blob";
	rm_cnf.max_register = nblob->data_size - 1;

	map = devm_regmap_init(dev,
			       &nvmem_blob_regmap_bus,
			       nblob,
			       &rm_cnf);
	if (IS_ERR(map)) {
		dev_dbg(dev, "Failed to initilize regmap\n");
		return PTR_ERR(map);
	}

	nv_cnf.name = "nvmem-blob";
	nv_cnf.read_only = true;
	nv_cnf.dev = dev;
	nv_cnf.owner = THIS_MODULE;

	nvmem = nvmem_register(&nv_cnf);
	if (IS_ERR(nvmem)) {
		dev_dbg(dev, "Filed to register nvmem device\n");
		return PTR_ERR(nvmem);
	}

	platform_set_drvdata(pdev, nblob);
	return 0;
}

static int nvmem_blob_remove(struct platform_device *pdev)
{
	struct nvmem_device *nvmem = platform_get_drvdata(pdev);

	return nvmem_unregister(nvmem);
}

static const struct of_device_id nvmem_blob_dt_ids[] = {
	{ .compatible = "nvmem-blob", },
	{ },
};
MODULE_DEVICE_TABLE(of, nvmem_blob_dt_ids);

static struct platform_driver nvmem_blob_driver = {
	.probe	= nvmem_blob_probe,
	.remove	= nvmem_blob_remove,
	.driver = {
		.name	= "nvmem-blob",
		.of_match_table = nvmem_blob_dt_ids,
	},
};
module_platform_driver(nvmem_blob_driver);

MODULE_AUTHOR("Andrey Smirnov <andrew.smirnov@gmail.com>");
MODULE_DESCRIPTION("FIXME");
MODULE_LICENSE("GPL v2");
