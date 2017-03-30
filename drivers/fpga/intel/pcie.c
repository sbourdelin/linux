/*
 * Driver for Intel FPGA PCIe device
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *   Zhang Yi <Yi.Z.Zhang@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *   Joseph Grecco <joe.grecco@intel.com>
 *   Enno Luebbers <enno.luebbers@intel.com>
 *   Tim Whisonant <tim.whisonant@intel.com>
 *   Ananda Ravuri <ananda.ravuri@intel.com>
 *   Henry Mitchel <henry.mitchel@intel.com>
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under this directory for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/errno.h>
#include <linux/aer.h>
#include <linux/fpga/fpga-dev.h>

#include "feature-dev.h"

#define DRV_VERSION	"EXPERIMENTAL VERSION"
#define DRV_NAME	"intel-fpga-pci"

#define INTEL_FPGA_DEV	"intel-fpga-dev"

static DEFINE_MUTEX(fpga_id_mutex);

enum fpga_id_type {
	FME_ID,		/* fme id allocation and mapping */
	PORT_ID,	/* port id allocation and mapping */
	FPGA_ID_MAX,
};

/* it is protected by fpga_id_mutex */
static struct idr fpga_ids[FPGA_ID_MAX];

struct cci_drvdata {
	struct device *fme_dev;

	struct mutex lock;
	struct list_head port_dev_list;

	struct list_head regions; /* global list of pci bar mapping region */
};

/* pci bar mapping info */
struct cci_pci_region {
	int bar;
	void __iomem *ioaddr;	/* pointer to mapped bar region */
	struct list_head node;
};

static void fpga_ids_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fpga_ids); i++)
		idr_init(fpga_ids + i);
}

static void fpga_ids_destroy(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fpga_ids); i++)
		idr_destroy(fpga_ids + i);
}

static int alloc_fpga_id(enum fpga_id_type type, struct device *dev)
{
	int id;

	WARN_ON(type >= FPGA_ID_MAX);
	mutex_lock(&fpga_id_mutex);
	id = idr_alloc(fpga_ids + type, dev, 0, 0, GFP_KERNEL);
	mutex_unlock(&fpga_id_mutex);
	return id;
}

static void free_fpga_id(enum fpga_id_type type, int id)
{
	WARN_ON(type >= FPGA_ID_MAX);
	mutex_lock(&fpga_id_mutex);
	idr_remove(fpga_ids + type, id);
	mutex_unlock(&fpga_id_mutex);
}

static void cci_pci_add_port_dev(struct pci_dev *pdev,
				 struct platform_device *port_dev)
{
	struct cci_drvdata *drvdata = dev_get_drvdata(&pdev->dev);
	struct feature_platform_data *pdata = dev_get_platdata(&port_dev->dev);

	mutex_lock(&drvdata->lock);
	list_add(&pdata->node, &drvdata->port_dev_list);
	get_device(&pdata->dev->dev);
	mutex_unlock(&drvdata->lock);
}

static void cci_pci_remove_port_devs(struct pci_dev *pdev)
{
	struct cci_drvdata *drvdata = dev_get_drvdata(&pdev->dev);
	struct feature_platform_data *pdata, *ptmp;

	mutex_lock(&drvdata->lock);
	list_for_each_entry_safe(pdata, ptmp, &drvdata->port_dev_list, node) {
		struct platform_device *port_dev = pdata->dev;

		/* the port should be unregistered first. */
		WARN_ON(device_is_registered(&port_dev->dev));
		list_del(&pdata->node);
		free_fpga_id(PORT_ID, port_dev->id);
		put_device(&port_dev->dev);
	}
	mutex_unlock(&drvdata->lock);
}

/* info collection during feature dev build. */
struct build_feature_devs_info {
	struct pci_dev *pdev;

	/*
	 * PCI BAR mapping info. Parsing feature list starts from
	 * BAR 0 and switch to different BARs to parse Port
	 */
	void __iomem *ioaddr;
	void __iomem *ioend;
	int current_bar;

	/* points to FME header where the port offset is figured out. */
	void __iomem *pfme_hdr;

	/* the container device for all feature devices */
	struct fpga_dev *parent_dev;

	/* current feature device */
	struct platform_device *feature_dev;
};

static void cci_pci_release_regions(struct pci_dev *pdev)
{
	struct cci_drvdata *drvdata = dev_get_drvdata(&pdev->dev);
	struct cci_pci_region *tmp, *region;

	list_for_each_entry_safe(region, tmp, &drvdata->regions, node) {
		list_del(&region->node);
		if (region->ioaddr)
			pci_iounmap(pdev, region->ioaddr);
		devm_kfree(&pdev->dev, region);
	}
}

static void __iomem *cci_pci_ioremap_bar(struct pci_dev *pdev, int bar)
{
	struct cci_drvdata *drvdata = dev_get_drvdata(&pdev->dev);
	struct cci_pci_region *region;

	list_for_each_entry(region, &drvdata->regions, node)
		if (region->bar == bar) {
			dev_dbg(&pdev->dev, "BAR %d region exists\n", bar);
			return region->ioaddr;
		}

	region = devm_kzalloc(&pdev->dev, sizeof(*region), GFP_KERNEL);
	if (!region)
		return NULL;

	region->bar = bar;
	region->ioaddr = pci_ioremap_bar(pdev, bar);
	if (!region->ioaddr) {
		dev_err(&pdev->dev, "can't ioremap memory from BAR %d.\n", bar);
		devm_kfree(&pdev->dev, region);
		return NULL;
	}

	list_add(&region->node, &drvdata->regions);
	return region->ioaddr;
}

static int parse_start_from(struct build_feature_devs_info *binfo, int bar)
{
	binfo->ioaddr = cci_pci_ioremap_bar(binfo->pdev, bar);
	if (!binfo->ioaddr)
		return -ENOMEM;

	binfo->current_bar = bar;
	binfo->ioend = binfo->ioaddr + pci_resource_len(binfo->pdev, bar);
	return 0;
}

static int parse_start(struct build_feature_devs_info *binfo)
{
	/* fpga feature list starts from BAR 0 */
	return parse_start_from(binfo, 0);
}

/* switch the memory mapping to BAR# @bar */
static int parse_switch_to(struct build_feature_devs_info *binfo, int bar)
{
	return parse_start_from(binfo, bar);
}

static struct build_feature_devs_info *
build_info_alloc_and_init(struct pci_dev *pdev)
{
	struct build_feature_devs_info *binfo;

	binfo = devm_kzalloc(&pdev->dev, sizeof(*binfo), GFP_KERNEL);
	if (binfo)
		binfo->pdev = pdev;

	return binfo;
}

static enum fpga_id_type feature_dev_id_type(struct platform_device *pdev)
{
	if (!strcmp(pdev->name, FPGA_FEATURE_DEV_FME))
		return FME_ID;

	if (!strcmp(pdev->name, FPGA_FEATURE_DEV_PORT))
		return PORT_ID;

	WARN_ON(1);
	return FPGA_ID_MAX;
}

/*
 * register current feature device, it is called when we need to switch to
 * another feature parsing or we have parsed all features
 */
static int build_info_commit_dev(struct build_feature_devs_info *binfo)
{
	int ret;

	if (!binfo->feature_dev)
		return 0;

	ret = platform_device_add(binfo->feature_dev);
	if (!ret) {
		struct cci_drvdata *drvdata;

		drvdata = dev_get_drvdata(&binfo->pdev->dev);
		if (feature_dev_id_type(binfo->feature_dev) == PORT_ID)
			cci_pci_add_port_dev(binfo->pdev, binfo->feature_dev);
		else
			drvdata->fme_dev = get_device(&binfo->feature_dev->dev);

		/*
		 * reset it to avoid build_info_free() freeing their resource.
		 *
		 * The resource of successfully registered feature devices
		 * will be freed by platform_device_unregister(). See the
		 * comments in build_info_create_dev().
		 */
		binfo->feature_dev = NULL;
	}

	return ret;
}

static int
build_info_create_dev(struct build_feature_devs_info *binfo,
		      enum fpga_id_type type, int feature_nr, const char *name)
{
	struct platform_device *fdev;
	struct resource *res;
	struct feature_platform_data *pdata;
	int ret;

	/* we will create a new device, commit current device first */
	ret = build_info_commit_dev(binfo);
	if (ret)
		return ret;

	/*
	 * we use -ENODEV as the initialization indicator which indicates
	 * whether the id need to be reclaimed
	 */
	fdev = binfo->feature_dev = platform_device_alloc(name, -ENODEV);
	if (!fdev)
		return -ENOMEM;

	fdev->id = alloc_fpga_id(type, &fdev->dev);
	if (fdev->id < 0)
		return fdev->id;

	fdev->dev.parent = &binfo->parent_dev->dev;

	/*
	 * we need not care the memory which is associated with the
	 * platform device. After call platform_device_unregister(),
	 * it will be automatically freed by device's
	 * release() callback, platform_device_release().
	 */
	pdata = feature_platform_data_alloc_and_init(fdev, feature_nr);
	if (!pdata)
		return -ENOMEM;

	/*
	 * the count should be initialized to 0 to make sure
	 *__fpga_port_enable() following __fpga_port_disable()
	 * works properly for port device.
	 * and it should always be 0 for fme device.
	 */
	WARN_ON(pdata->disable_count);

	fdev->dev.platform_data = pdata;
	fdev->num_resources = feature_nr;
	fdev->resource = kcalloc(feature_nr, sizeof(*res), GFP_KERNEL);
	if (!fdev->resource)
		return -ENOMEM;

	return 0;
}

static int remove_feature_dev(struct device *dev, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);

	platform_device_unregister(pdev);
	return 0;
}

static int remove_parent_dev(struct device *dev, void *data)
{
	/* remove platform devices attached in the parent device */
	device_for_each_child(dev, NULL, remove_feature_dev);
	fpga_dev_destroy(to_fpga_dev(dev));
	return 0;
}

static void remove_all_devs(struct pci_dev *pdev)
{
	/* remove parent device and all its children. */
	device_for_each_child(&pdev->dev, NULL, remove_parent_dev);
}

static void build_info_free(struct build_feature_devs_info *binfo)
{
	if (!IS_ERR_OR_NULL(binfo->parent_dev))
		remove_all_devs(binfo->pdev);

	/*
	 * it is a valid id, free it. See comments in
	 * build_info_create_dev()
	 */
	if (binfo->feature_dev && binfo->feature_dev->id >= 0)
		free_fpga_id(feature_dev_id_type(binfo->feature_dev),
			     binfo->feature_dev->id);

	platform_device_put(binfo->feature_dev);

	devm_kfree(&binfo->pdev->dev, binfo);
}

#define FEATURE_TYPE_AFU	0x1
#define FEATURE_TYPE_PRIVATE	0x3

/* FME and PORT GUID are fixed */
#define FEATURE_FME_GUID "f9e17764-38f0-82fe-e346-524ae92aafbf"
#define FEATURE_PORT_GUID "6b355b87-b06c-9642-eb42-8d139398b43a"

static bool feature_is_fme(struct feature_afu_header *afu_hdr)
{
	uuid_le u;

	uuid_le_to_bin(FEATURE_FME_GUID, &u);

	return !uuid_le_cmp(u, afu_hdr->guid);
}

static bool feature_is_port(struct feature_afu_header *afu_hdr)
{
	uuid_le u;

	uuid_le_to_bin(FEATURE_PORT_GUID, &u);

	return !uuid_le_cmp(u, afu_hdr->guid);
}

/*
 * UAFU GUID is dynamic as it can be changed after FME downloads different
 * Green Bitstream to the port, so we treat the unknown GUIDs which are
 * attached on port's feature list as UAFU.
 */
static bool feature_is_UAFU(struct build_feature_devs_info *binfo)
{
	if (!binfo->feature_dev ||
	      feature_dev_id_type(binfo->feature_dev) != PORT_ID)
		return false;

	return true;
}

static void
build_info_add_sub_feature(struct build_feature_devs_info *binfo,
			   int feature_id, const char *feature_name,
			   resource_size_t resource_size, void __iomem *start)
{

	struct platform_device *fdev = binfo->feature_dev;
	struct feature_platform_data *pdata = dev_get_platdata(&fdev->dev);
	struct resource *res = &fdev->resource[feature_id];

	res->start = pci_resource_start(binfo->pdev, binfo->current_bar) +
		start - binfo->ioaddr;
	res->end = res->start + resource_size - 1;
	res->flags = IORESOURCE_MEM;
	res->name = feature_name;

	feature_platform_data_add(pdata, feature_id,
				  feature_name, feature_id, start);
}

struct feature_info {
	const char *name;
	resource_size_t resource_size;
	int feature_index;
};

/* indexed by fme feature IDs which are defined in 'enum fme_feature_id'. */
static struct feature_info fme_features[] = {
	{
		.name = FME_FEATURE_HEADER,
		.resource_size = sizeof(struct feature_fme_header),
		.feature_index = FME_FEATURE_ID_HEADER,
	},
	{
		.name = FME_FEATURE_THERMAL_MGMT,
		.resource_size = sizeof(struct feature_fme_thermal),
		.feature_index = FME_FEATURE_ID_THERMAL_MGMT,
	},
	{
		.name = FME_FEATURE_POWER_MGMT,
		.resource_size = sizeof(struct feature_fme_power),
		.feature_index = FME_FEATURE_ID_POWER_MGMT,
	},
	{
		.name = FME_FEATURE_GLOBAL_PERF,
		.resource_size = sizeof(struct feature_fme_gperf),
		.feature_index = FME_FEATURE_ID_GLOBAL_PERF,
	},
	{
		.name = FME_FEATURE_GLOBAL_ERR,
		.resource_size = sizeof(struct feature_fme_err),
		.feature_index = FME_FEATURE_ID_GLOBAL_ERR,
	},
	{
		.name = FME_FEATURE_PR_MGMT,
		.resource_size = sizeof(struct feature_fme_pr),
		.feature_index = FME_FEATURE_ID_PR_MGMT,
	}
};

/* indexed by port feature IDs which are defined in 'enum port_feature_id'. */
static struct feature_info port_features[] = {
	{
		.name = PORT_FEATURE_HEADER,
		.resource_size = sizeof(struct feature_port_header),
		.feature_index = PORT_FEATURE_ID_HEADER,
	},
	{
		.name = PORT_FEATURE_ERR,
		.resource_size = sizeof(struct feature_port_error),
		.feature_index = PORT_FEATURE_ID_ERROR,
	},
	{
		.name = PORT_FEATURE_UMSG,
		.resource_size = sizeof(struct feature_port_umsg),
		.feature_index = PORT_FEATURE_ID_UMSG,
	},
	{
		/* This feature isn't available for now */
		.name = PORT_FEATURE_PR,
		.resource_size = 0,
		.feature_index = PORT_FEATURE_ID_PR,
	},
	{
		.name = PORT_FEATURE_STP,
		.resource_size = sizeof(struct feature_port_stp),
		.feature_index = PORT_FEATURE_ID_STP,
	},
	{
		/*
		 * For User AFU feature, its region size is not fixed, but
		 * reported by register PortCapability.mmio_size. Resource
		 * size of UAFU will be set while parse port device.
		 */
		.name = PORT_FEATURE_UAFU,
		.resource_size = 0,
		.feature_index = PORT_FEATURE_ID_UAFU,
	},
};

static int
create_feature_instance(struct build_feature_devs_info *binfo,
			void __iomem *start, struct feature_info *finfo)
{
	if (binfo->ioend - start < finfo->resource_size)
		return -EINVAL;

	build_info_add_sub_feature(binfo, finfo->feature_index, finfo->name,
				   finfo->resource_size, start);
	return 0;
}

static int parse_feature_fme(struct build_feature_devs_info *binfo,
			     void __iomem *start)
{
	struct cci_drvdata *drvdata = dev_get_drvdata(&binfo->pdev->dev);
	int ret;

	ret = build_info_create_dev(binfo, FME_ID, fme_feature_num(),
					FPGA_FEATURE_DEV_FME);
	if (ret)
		return ret;

	if (drvdata->fme_dev) {
		dev_err(&binfo->pdev->dev, "Multiple FMEs are detected.\n");
		return -EINVAL;
	}

	return create_feature_instance(binfo, start,
				       &fme_features[FME_FEATURE_ID_HEADER]);
}

static int parse_feature_fme_private(struct build_feature_devs_info *binfo,
				     struct feature_header *hdr)
{
	struct feature_header header;

	header.csr = readq(hdr);

	if (header.id >= ARRAY_SIZE(fme_features)) {
		dev_info(&binfo->pdev->dev, "FME feature id %x is not supported yet.\n",
			 header.id);
		return 0;
	}

	return create_feature_instance(binfo, hdr, &fme_features[header.id]);
}

static int parse_feature_port(struct build_feature_devs_info *binfo,
			     void __iomem *start)
{
	int ret;

	ret = build_info_create_dev(binfo, PORT_ID, port_feature_num(),
					FPGA_FEATURE_DEV_PORT);
	if (ret)
		return ret;

	return create_feature_instance(binfo, start,
				       &port_features[PORT_FEATURE_ID_HEADER]);
}

static void enable_port_uafu(struct build_feature_devs_info *binfo,
			     void __iomem *start)
{
	enum port_feature_id id = PORT_FEATURE_ID_UAFU;
	struct feature_port_header *port_hdr;
	struct feature_port_capability capability;

	port_hdr = (struct feature_port_header *)start;
	capability.csr = readq(&port_hdr->capability);
	port_features[id].resource_size = capability.mmio_size << 10;

	/*
	 * To enable User AFU, driver needs to clear reset bit on related port,
	 * otherwise the mmio space of this user AFU will be invalid.
	 */
	if (port_features[id].resource_size)
		fpga_port_reset(binfo->feature_dev);
}

static int parse_feature_port_private(struct build_feature_devs_info *binfo,
				      struct feature_header *hdr)
{
	struct feature_header header;
	enum port_feature_id id;

	header.csr = readq(hdr);
	/*
	 * the region of port feature id is [0x10, 0x13], + 1 to reserve 0
	 * which is dedicated for port-hdr.
	 */
	id = (header.id & 0x000f) + 1;

	if (id >= ARRAY_SIZE(port_features)) {
		dev_info(&binfo->pdev->dev, "Port feature id %x is not supported yet.\n",
			 header.id);
		return 0;
	}

	return create_feature_instance(binfo, hdr, &port_features[id]);
}

static int parse_feature_port_uafu(struct build_feature_devs_info *binfo,
				 struct feature_header *hdr)
{
	enum port_feature_id id = PORT_FEATURE_ID_UAFU;
	int ret;

	if (port_features[id].resource_size) {
		ret = create_feature_instance(binfo, hdr, &port_features[id]);
		port_features[id].resource_size = 0;
	} else {
		dev_err(&binfo->pdev->dev, "the uafu feature header is mis-configured.\n");
		ret = -EINVAL;
	}

	return ret;
}

static int parse_feature_afus(struct build_feature_devs_info *binfo,
			      struct feature_header *hdr)
{
	int ret;
	struct feature_afu_header *afu_hdr, header;
	void __iomem *start;
	void __iomem *end = binfo->ioend;

	start = hdr;
	for (; start < end; start += header.next_afu) {
		if (end - start < (sizeof(*afu_hdr) + sizeof(*hdr)))
			return -EINVAL;

		hdr = start;
		afu_hdr = (struct feature_afu_header *) (hdr + 1);
		header.csr = readq(&afu_hdr->csr);

		if (feature_is_fme(afu_hdr)) {
			ret = parse_feature_fme(binfo, hdr);
			binfo->pfme_hdr = hdr;
			if (ret)
				return ret;
		} else if (feature_is_port(afu_hdr)) {
			ret = parse_feature_port(binfo, hdr);
			enable_port_uafu(binfo, hdr);
			if (ret)
				return ret;
		} else if (feature_is_UAFU(binfo)) {
			ret = parse_feature_port_uafu(binfo, hdr);
			if (ret)
				return ret;
		} else
			dev_info(&binfo->pdev->dev, "AFU GUID %pUl is not supported yet.\n",
				 afu_hdr->guid.b);

		if (!header.next_afu)
			break;
	}

	return 0;
}

static int parse_feature_private(struct build_feature_devs_info *binfo,
				 struct feature_header *hdr)
{
	struct feature_header header;

	header.csr = readq(hdr);

	if (!binfo->feature_dev) {
		dev_err(&binfo->pdev->dev, "the private feature %x does not belong to any AFU.\n",
			header.id);
		return -EINVAL;
	}

	switch (feature_dev_id_type(binfo->feature_dev)) {
	case FME_ID:
		return parse_feature_fme_private(binfo, hdr);
	case PORT_ID:
		return parse_feature_port_private(binfo, hdr);
	default:
		dev_info(&binfo->pdev->dev, "private feature %x belonging to AFU %s is not supported yet.\n",
			 header.id, binfo->feature_dev->name);
	}
	return 0;
}

static int parse_feature(struct build_feature_devs_info *binfo,
			 struct feature_header *hdr)
{
	struct feature_header header;
	int ret = 0;

	header.csr = readq(hdr);

	switch (header.type) {
	case FEATURE_TYPE_AFU:
		ret = parse_feature_afus(binfo, hdr);
		break;
	case FEATURE_TYPE_PRIVATE:
		ret = parse_feature_private(binfo, hdr);
		break;
	default:
		dev_info(&binfo->pdev->dev,
			 "Feature Type %x is not supported.\n", hdr->type);
	};

	return ret;
}

static int
parse_feature_list(struct build_feature_devs_info *binfo, void __iomem *start)
{
	struct feature_header *hdr, header;
	void __iomem *end = binfo->ioend;
	int ret = 0;

	for (; start < end; start += header.next_header_offset) {
		if (end - start < sizeof(*hdr)) {
			dev_err(&binfo->pdev->dev, "The region is too small to contain a feature.\n");
			ret =  -EINVAL;
			break;
		}

		hdr = (struct feature_header *)start;
		ret = parse_feature(binfo, hdr);
		if (ret)
			break;

		header.csr = readq(hdr);
		if (!header.next_header_offset)
			break;
	}

	return ret;
}

static int parse_ports_from_fme(struct build_feature_devs_info *binfo)
{
	struct feature_fme_header *fme_hdr;
	struct feature_fme_port port;
	int i = 0, ret = 0;

	if (binfo->pfme_hdr == NULL) {
		dev_dbg(&binfo->pdev->dev, "VF is detected.\n");
		return ret;
	}

	fme_hdr = binfo->pfme_hdr;

	do {
		port.csr = readq(&fme_hdr->port[i]);
		if (!port.port_implemented)
			break;

		ret = parse_switch_to(binfo, port.port_bar);
		if (ret)
			break;

		ret = parse_feature_list(binfo,
				binfo->ioaddr + port.port_offset);
		if (ret)
			break;
	} while (++i < MAX_FPGA_PORT_NUM);

	return ret;
}

static int create_init_drvdata(struct pci_dev *pdev)
{
	struct cci_drvdata *drvdata;

	drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	mutex_init(&drvdata->lock);
	INIT_LIST_HEAD(&drvdata->port_dev_list);
	INIT_LIST_HEAD(&drvdata->regions);

	dev_set_drvdata(&pdev->dev, drvdata);
	return 0;
}

static void destroy_drvdata(struct pci_dev *pdev)
{
	struct cci_drvdata *drvdata = dev_get_drvdata(&pdev->dev);

	if (drvdata->fme_dev) {
		/* fme device should be unregistered first. */
		WARN_ON(device_is_registered(drvdata->fme_dev));
		free_fpga_id(FME_ID, to_platform_device(drvdata->fme_dev)->id);
		put_device(drvdata->fme_dev);
	}

	cci_pci_remove_port_devs(pdev);
	cci_pci_release_regions(pdev);
	dev_set_drvdata(&pdev->dev, NULL);
	devm_kfree(&pdev->dev, drvdata);
}

static int cci_pci_create_feature_devs(struct pci_dev *pdev)
{
	struct build_feature_devs_info *binfo;
	int ret;

	binfo = build_info_alloc_and_init(pdev);
	if (!binfo)
		return -ENOMEM;

	binfo->parent_dev = fpga_dev_create(&pdev->dev, INTEL_FPGA_DEV);
	if (IS_ERR(binfo->parent_dev)) {
		ret = PTR_ERR(binfo->parent_dev);
		goto free_binfo_exit;
	}

	ret = parse_start(binfo);
	if (ret)
		goto free_binfo_exit;

	ret = parse_feature_list(binfo, binfo->ioaddr);
	if (ret)
		goto free_binfo_exit;

	ret = parse_ports_from_fme(binfo);
	if (ret)
		goto free_binfo_exit;

	ret = build_info_commit_dev(binfo);
	if (ret)
		goto free_binfo_exit;

	/*
	 * everything is okay, reset ->parent_dev to stop it being
	 * freed by build_info_free()
	 */
	binfo->parent_dev = NULL;

free_binfo_exit:
	build_info_free(binfo);
	return ret;
}

/* PCI Device ID */
#define PCIe_DEVICE_ID_PF_INT_5_X	0xBCBD
#define PCIe_DEVICE_ID_PF_INT_6_X	0xBCC0
#define PCIe_DEVICE_ID_PF_DSC_1_X	0x09C4
/* VF Device */
#define PCIe_DEVICE_ID_VF_INT_5_X	0xBCBF
#define PCIe_DEVICE_ID_VF_INT_6_X	0xBCC1
#define PCIe_DEVICE_ID_VF_DSC_1_X	0x09C5

static struct pci_device_id cci_pcie_id_tbl[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_PF_INT_5_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_VF_INT_5_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_PF_INT_6_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_VF_INT_6_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_PF_DSC_1_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_VF_DSC_1_X),},
	{0,}
};
MODULE_DEVICE_TABLE(pci, cci_pcie_id_tbl);

static
int cci_pci_probe(struct pci_dev *pcidev, const struct pci_device_id *pcidevid)
{
	int ret;

	ret = pci_enable_device(pcidev);
	if (ret < 0) {
		dev_err(&pcidev->dev, "Failed to enable device %d.\n", ret);
		goto exit;
	}

	ret = pci_enable_pcie_error_reporting(pcidev);
	if (ret && ret != -EINVAL)
		dev_info(&pcidev->dev, "PCIE AER unavailable %d.\n", ret);

	ret = pci_request_regions(pcidev, DRV_NAME);
	if (ret) {
		dev_err(&pcidev->dev, "Failed to request regions.\n");
		goto disable_error_report_exit;
	}

	pci_set_master(pcidev);
	pci_save_state(pcidev);

	if (!dma_set_mask(&pcidev->dev, DMA_BIT_MASK(64))) {
		dma_set_coherent_mask(&pcidev->dev, DMA_BIT_MASK(64));
	} else if (!dma_set_mask(&pcidev->dev, DMA_BIT_MASK(32))) {
		dma_set_coherent_mask(&pcidev->dev, DMA_BIT_MASK(32));
	} else {
		ret = -EIO;
		dev_err(&pcidev->dev, "No suitable DMA support available.\n");
		goto release_region_exit;
	}

	ret = create_init_drvdata(pcidev);
	if (ret)
		goto release_region_exit;

	ret = cci_pci_create_feature_devs(pcidev);
	if (ret)
		goto destroy_drvdata_exit;

	return 0;

destroy_drvdata_exit:
	destroy_drvdata(pcidev);
release_region_exit:
	pci_release_regions(pcidev);
disable_error_report_exit:
	pci_disable_pcie_error_reporting(pcidev);
	pci_disable_device(pcidev);
exit:
	return ret;
}

static void cci_pci_remove(struct pci_dev *pcidev)
{
	remove_all_devs(pcidev);
	destroy_drvdata(pcidev);
	pci_release_regions(pcidev);
	pci_disable_pcie_error_reporting(pcidev);
	pci_disable_device(pcidev);
}

static struct pci_driver cci_pci_driver = {
	.name = DRV_NAME,
	.id_table = cci_pcie_id_tbl,
	.probe = cci_pci_probe,
	.remove = cci_pci_remove,
};

static int __init ccidrv_init(void)
{
	int ret;

	pr_info("Intel(R) FPGA PCIe Driver: Version %s\n", DRV_VERSION);

	fpga_ids_init();

	ret = pci_register_driver(&cci_pci_driver);
	if (ret)
		fpga_ids_destroy();

	return ret;
}

static void __exit ccidrv_exit(void)
{
	pci_unregister_driver(&cci_pci_driver);
	fpga_ids_destroy();
}

module_init(ccidrv_init);
module_exit(ccidrv_exit);

MODULE_DESCRIPTION("Intel FPGA PCIe Device Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("Dual BSD/GPL");
