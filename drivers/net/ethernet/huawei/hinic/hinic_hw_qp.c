/*
 * Huawei HiNIC PCI Express Linux driver
 * Copyright(c) 2017 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/errno.h>
#include <linux/sizes.h>

#include "hinic_hw_if.h"
#include "hinic_hw_wq.h"
#include "hinic_hw_qp.h"

#define SQ_DB_OFF		SZ_2K

/**
 * alloc_sq_priv - allocate sq private data space
 * @sq: HW Send Queue
 *
 * Return 0 - Success, negative - Failure
 **/
static int alloc_sq_priv(struct hinic_sq *sq)
{
	struct hinic_wq *wq = sq->wq;
	size_t priv_size = wq->q_depth * sizeof(*sq->priv);

	sq->priv = vzalloc(priv_size);
	if (!sq->priv)
		return -ENOMEM;

	return 0;
}

/**
 * free_sq_priv - free sq private data space
 * @sq: HW Send Queue
 **/
static void free_sq_priv(struct hinic_sq *sq)
{
	vfree(sq->priv);
}

/**
 * alloc_rq_priv - allocate rq private data space
 * @rq: HW Receive Queue
 *
 * Return 0 - Success, negative - Failure
 **/
static int alloc_rq_priv(struct hinic_rq *rq)
{
	struct hinic_wq *wq = rq->wq;
	size_t priv_size = wq->q_depth * sizeof(*rq->priv);

	rq->priv = vzalloc(priv_size);
	if (!rq->priv)
		return -ENOMEM;

	return 0;
}

/**
 * free_rq_priv - free rq private data space
 * @rq: HW Receive Queue
 **/
static void free_rq_priv(struct hinic_rq *rq)
{
	vfree(rq->priv);
}

/**
 * hinic_init_sq - Initialize HW Send Queue
 * @sq: HW Send Queue
 * @hwif: HW Interface for accessing HW
 * @wq: Work Queue for the data of the SQ
 * @entry: msix entry for sq
 * @ci_addr: address for reading the current HW consumer index
 * @ci_dma_addr: dma address for reading the current HW consumer index
 * @db_base: doorbell base address
 *
 * Return 0 - Success, negative - Failure
 **/
int hinic_init_sq(struct hinic_sq *sq, struct hinic_hwif *hwif,
		  struct hinic_wq *wq, struct msix_entry *entry,
		  void *ci_addr, dma_addr_t ci_dma_addr,
		  void __iomem *db_base)
{
	sq->hwif = hwif;

	sq->wq = wq;

	sq->irq = entry->vector;
	sq->msix_entry = entry->entry;

	sq->hw_ci_addr = ci_addr;
	sq->hw_ci_dma_addr = ci_dma_addr;

	sq->db_base = db_base + SQ_DB_OFF;

	return alloc_sq_priv(sq);
}

/**
 * hinic_clean_sq - Clean HW Send Queue's Resources
 * @sq: Send Queue
 **/
void hinic_clean_sq(struct hinic_sq *sq)
{
	free_sq_priv(sq);
}

/**
 * alloc_rq_cqe - allocate rq completion queue elements
 * @rq: HW Receive Queue
 *
 * Return 0 - Success, negative - Failure
 **/
static int alloc_rq_cqe(struct hinic_rq *rq)
{
	struct hinic_hwif *hwif = rq->hwif;
	struct pci_dev *pdev = hwif->pdev;
	struct hinic_wq *wq = rq->wq;
	size_t cqe_dma_size, cqe_size;
	int j, i;

	cqe_size = wq->q_depth * sizeof(*rq->cqe);
	rq->cqe = vzalloc(cqe_size);
	if (!rq->cqe)
		return -ENOMEM;

	cqe_dma_size = wq->q_depth * sizeof(*rq->cqe_dma);
	rq->cqe_dma = vzalloc(cqe_dma_size);
	if (!rq->cqe_dma)
		goto cqe_dma_arr_alloc_err;

	for (i = 0; i < wq->q_depth; i++) {
		rq->cqe[i] = dma_zalloc_coherent(&pdev->dev,
						 sizeof(*rq->cqe[i]),
						 &rq->cqe_dma[i], GFP_KERNEL);
		if (!rq->cqe[i])
			goto cqe_alloc_err;
	}

	return 0;

cqe_alloc_err:
	for (j = 0; j < i; j++)
		dma_free_coherent(&pdev->dev, sizeof(*rq->cqe[j]), rq->cqe[j],
				  rq->cqe_dma[j]);

	vfree(rq->cqe_dma);

cqe_dma_arr_alloc_err:
	vfree(rq->cqe);
	return -ENOMEM;
}

/**
 * free_rq_cqe - free rq completion queue elements
 * @rq: HW Receive Queue
 **/
static void free_rq_cqe(struct hinic_rq *rq)
{
	struct hinic_hwif *hwif = rq->hwif;
	struct pci_dev *pdev = hwif->pdev;
	struct hinic_wq *wq = rq->wq;
	int i;

	for (i = 0; i < wq->q_depth; i++)
		dma_free_coherent(&pdev->dev, sizeof(*rq->cqe[i]), rq->cqe[i],
				  rq->cqe_dma[i]);

	vfree(rq->cqe_dma);
	vfree(rq->cqe);
}

/**
 * hinic_init_rq - Initialize HW Receive Queue
 * @rq: HW Receive Queue
 * @hwif: HW Interface for accessing HW
 * @wq: Work Queue for the data of the RQ
 * @entry: msix entry for rq
 *
 * Return 0 - Success, negative - Failure
 **/
int hinic_init_rq(struct hinic_rq *rq, struct hinic_hwif *hwif,
		  struct hinic_wq *wq, struct msix_entry *entry)
{
	struct pci_dev *pdev = hwif->pdev;
	size_t pi_size;
	int err;

	rq->hwif = hwif;

	rq->wq = wq;

	rq->irq = entry->vector;
	rq->msix_entry = entry->entry;

	rq->buf_sz = HINIC_RX_BUF_SZ;

	err = alloc_rq_priv(rq);
	if (err) {
		pr_err("Failed to allocate rq priv data\n");
		return err;
	}

	err = alloc_rq_cqe(rq);
	if (err) {
		pr_err("Failed to allocate rq cqe\n");
		goto alloc_rq_cqe_err;
	}

	/* HW requirements: Must be at least 32 bit */
	pi_size = ALIGN(sizeof(*rq->pi_virt_addr), sizeof(u32));
	rq->pi_virt_addr = dma_zalloc_coherent(&pdev->dev, pi_size,
					       &rq->pi_dma_addr, GFP_KERNEL);
	if (!rq->pi_virt_addr) {
		dev_err(&pdev->dev, "Failed to allocate PI address\n");
		err = -ENOMEM;
		goto pi_virt_err;
	}

	return 0;

pi_virt_err:
	free_rq_cqe(rq);

alloc_rq_cqe_err:
	free_rq_priv(rq);
	return err;
}

/**
 * hinic_clean_rq - Clean HW Receive Queue's Resources
 * @rq: HW Receive Queue
 **/
void hinic_clean_rq(struct hinic_rq *rq)
{
	struct hinic_hwif *hwif = rq->hwif;
	struct pci_dev *pdev = hwif->pdev;
	size_t pi_size;

	pi_size = ALIGN(sizeof(*rq->pi_virt_addr), sizeof(u32));
	dma_free_coherent(&pdev->dev, pi_size, rq->pi_virt_addr,
			  rq->pi_dma_addr);

	free_rq_cqe(rq);
	free_rq_priv(rq);
}
