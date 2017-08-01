/*
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/device-mapper.h>
#include <linux/dax.h>
#include <linux/uio.h>

#include "dm.h"

extern sector_t linear_map_sector(struct dm_target *ti, sector_t bi_sector);
extern sector_t max_io_len(sector_t sector, struct dm_target *ti);

long linear_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn)
{
	long ret;
	struct linear_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	struct dax_device *dax_dev = lc->dev->dax_dev;
	sector_t dev_sector, sector = pgoff * PAGE_SECTORS;

	dev_sector = linear_map_sector(ti, sector);
	ret = bdev_dax_pgoff(bdev, dev_sector, nr_pages * PAGE_SIZE, &pgoff);
	if (ret)
		return ret;
	return dax_direct_access(dax_dev, pgoff, nr_pages, kaddr, pfn);
}

size_t linear_dax_copy_from_iter(struct dm_target *ti, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i)
{
	struct linear_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	struct dax_device *dax_dev = lc->dev->dax_dev;
	sector_t dev_sector, sector = pgoff * PAGE_SECTORS;

	dev_sector = linear_map_sector(ti, sector);
	if (bdev_dax_pgoff(bdev, dev_sector, ALIGN(bytes, PAGE_SIZE), &pgoff))
		return 0;
	return dax_copy_from_iter(dax_dev, pgoff, addr, bytes, i);
}

void linear_dax_flush(struct dm_target *ti, pgoff_t pgoff, void *addr,
		size_t size)
{
	struct linear_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	struct dax_device *dax_dev = lc->dev->dax_dev;
	sector_t dev_sector, sector = pgoff * PAGE_SECTORS;

	dev_sector = linear_map_sector(ti, sector);
	if (bdev_dax_pgoff(bdev, dev_sector, ALIGN(size, PAGE_SIZE), &pgoff))
		return;
	dax_flush(dax_dev, pgoff, addr, size);
}

long origin_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn)
{
#define DM_MSG_PREFIX "snapshots"
	DMWARN("device does not support dax.");
	return -EIO;
}
EXPORT_SYMBOL_GPL(origin_dax_direct_access);

extern void stripe_map_sector(struct stripe_c *sc, sector_t sector,
			      uint32_t *stripe, sector_t *result);
long stripe_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn)
{
	sector_t dev_sector, sector = pgoff * PAGE_SECTORS;
	struct stripe_c *sc = ti->private;
	struct dax_device *dax_dev;
	struct block_device *bdev;
	uint32_t stripe;
	long ret;

	stripe_map_sector(sc, sector, &stripe, &dev_sector);
	dev_sector += sc->stripe[stripe].physical_start;
	dax_dev = sc->stripe[stripe].dev->dax_dev;
	bdev = sc->stripe[stripe].dev->bdev;

	ret = bdev_dax_pgoff(bdev, dev_sector, nr_pages * PAGE_SIZE, &pgoff);
	if (ret)
		return ret;
	return dax_direct_access(dax_dev, pgoff, nr_pages, kaddr, pfn);
}

size_t stripe_dax_copy_from_iter(struct dm_target *ti, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i)
{
	sector_t dev_sector, sector = pgoff * PAGE_SECTORS;
	struct stripe_c *sc = ti->private;
	struct dax_device *dax_dev;
	struct block_device *bdev;
	uint32_t stripe;

	stripe_map_sector(sc, sector, &stripe, &dev_sector);
	dev_sector += sc->stripe[stripe].physical_start;
	dax_dev = sc->stripe[stripe].dev->dax_dev;
	bdev = sc->stripe[stripe].dev->bdev;

	if (bdev_dax_pgoff(bdev, dev_sector, ALIGN(bytes, PAGE_SIZE), &pgoff))
		return 0;
	return dax_copy_from_iter(dax_dev, pgoff, addr, bytes, i);
}

void stripe_dax_flush(struct dm_target *ti, pgoff_t pgoff, void *addr,
		size_t size)
{
	sector_t dev_sector, sector = pgoff * PAGE_SECTORS;
	struct stripe_c *sc = ti->private;
	struct dax_device *dax_dev;
	struct block_device *bdev;
	uint32_t stripe;

	stripe_map_sector(sc, sector, &stripe, &dev_sector);
	dev_sector += sc->stripe[stripe].physical_start;
	dax_dev = sc->stripe[stripe].dev->dax_dev;
	bdev = sc->stripe[stripe].dev->bdev;

	if (bdev_dax_pgoff(bdev, dev_sector, ALIGN(size, PAGE_SIZE), &pgoff))
		return;
	dax_flush(dax_dev, pgoff, addr, size);
}

long io_err_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn)
{
	return -EIO;
}

static struct dm_target *dm_dax_get_live_target(struct mapped_device *md,
		sector_t sector, int *srcu_idx)
{
	struct dm_table *map;
	struct dm_target *ti;

	map = dm_get_live_table(md, srcu_idx);
	if (!map)
		return NULL;

	ti = dm_table_find_target(map, sector);
	if (!dm_target_is_valid(ti))
		return NULL;

	return ti;
}

long dm_dax_direct_access(struct dax_device *dax_dev, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn)
{
	struct mapped_device *md = dax_get_private(dax_dev);
	sector_t sector = pgoff * PAGE_SECTORS;
	struct dm_target *ti;
	long len, ret = -EIO;
	int srcu_idx;

	ti = dm_dax_get_live_target(md, sector, &srcu_idx);

	if (!ti)
		goto out;
	if (!ti->type->direct_access)
		goto out;
	len = max_io_len(sector, ti) / PAGE_SECTORS;
	if (len < 1)
		goto out;
	nr_pages = min(len, nr_pages);
	if (ti->type->direct_access)
		ret = ti->type->direct_access(ti, pgoff, nr_pages, kaddr, pfn);

 out:
	dm_put_live_table(md, srcu_idx);

	return ret;
}

size_t dm_dax_copy_from_iter(struct dax_device *dax_dev, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i)
{
	struct mapped_device *md = dax_get_private(dax_dev);
	sector_t sector = pgoff * PAGE_SECTORS;
	struct dm_target *ti;
	long ret = 0;
	int srcu_idx;

	ti = dm_dax_get_live_target(md, sector, &srcu_idx);

	if (!ti)
		goto out;
	if (!ti->type->dax_copy_from_iter) {
		ret = copy_from_iter(addr, bytes, i);
		goto out;
	}
	ret = ti->type->dax_copy_from_iter(ti, pgoff, addr, bytes, i);
 out:
	dm_put_live_table(md, srcu_idx);

	return ret;
}

void dm_dax_flush(struct dax_device *dax_dev, pgoff_t pgoff, void *addr,
		size_t size)
{
	struct mapped_device *md = dax_get_private(dax_dev);
	sector_t sector = pgoff * PAGE_SECTORS;
	struct dm_target *ti;
	int srcu_idx;

	ti = dm_dax_get_live_target(md, sector, &srcu_idx);

	if (!ti)
		goto out;
	if (ti->type->dax_flush)
		ti->type->dax_flush(ti, pgoff, addr, size);
 out:
	dm_put_live_table(md, srcu_idx);
}
