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
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>

#include "hinic_hw_if.h"
#include "hinic_hw_wq.h"
#include "hinic_hw_cmdq.h"
#include "hinic_hw_qp_ctxt.h"
#include "hinic_hw_qp.h"
#include "hinic_hw_io.h"

#define CI_Q_ADDR_SIZE			sizeof(u32)

#define CI_ADDR(base_addr, q_id)	((base_addr) + \
					 (q_id) * CI_Q_ADDR_SIZE)

#define CI_TABLE_SIZE(num_qps)		((num_qps) * CI_Q_ADDR_SIZE)

#define DB_IDX(db, db_base)		\
	(((unsigned long)(db) - (unsigned long)(db_base)) / HINIC_DB_PAGE_SIZE)

enum io_cmd {
	IO_CMD_MODIFY_QUEUE_CTXT = 0,
};

static void init_db_area_idx(struct hinic_free_db_area *free_db_area)
{
	int i;

	for (i = 0; i < HINIC_DB_MAX_AREAS; i++)
		free_db_area->db_idx[i] = i;

	free_db_area->alloc_pos = 0;
	free_db_area->return_pos = HINIC_DB_MAX_AREAS;

	free_db_area->num_free = HINIC_DB_MAX_AREAS;

	sema_init(&free_db_area->idx_lock, 1);
}

static int get_db_area(struct hinic_func_to_io *func_to_io,
		       void __iomem **db_base)
{
	struct hinic_free_db_area *free_db_area = &func_to_io->free_db_area;
	int pos, idx;

	down(&free_db_area->idx_lock);

	free_db_area->num_free--;

	if (free_db_area->num_free < 0) {
		free_db_area->num_free++;
		up(&free_db_area->idx_lock);
		return -ENOMEM;
	}

	pos = free_db_area->alloc_pos++;
	pos &= HINIC_DB_MAX_AREAS - 1;

	idx = free_db_area->db_idx[pos];

	free_db_area->db_idx[pos] = -1;

	up(&free_db_area->idx_lock);

	*db_base = func_to_io->db_base + idx * HINIC_DB_PAGE_SIZE;
	return 0;
}

static void return_db_area(struct hinic_func_to_io *func_to_io,
			   void __iomem *db_base)
{
	struct hinic_free_db_area *free_db_area = &func_to_io->free_db_area;
	int pos, idx = DB_IDX(db_base, func_to_io->db_base);

	down(&free_db_area->idx_lock);

	pos = free_db_area->return_pos++;
	pos &= HINIC_DB_MAX_AREAS - 1;

	free_db_area->db_idx[pos] = idx;

	free_db_area->num_free++;

	up(&free_db_area->idx_lock);
}

static int write_sq_ctxts(struct hinic_func_to_io *func_to_io, u16 base_qpn,
			  u16 num_sqs)
{
	struct hinic_cmdq_buf cmdq_buf;
	struct hinic_sq_ctxt_block *sq_ctxt_block;
	struct hinic_sq_ctxt *sq_ctxt;
	struct hinic_qp *qp;
	struct hinic_sq *sq;
	u64 out_param;
	u16 global_qpn, max_sqs = func_to_io->max_qps;
	int err, i;

	err = hinic_alloc_cmdq_buf(&func_to_io->cmdqs, &cmdq_buf);
	if (err) {
		pr_err("Failed to allocate cmdq buf\n");
		return err;
	}

	sq_ctxt_block = cmdq_buf.buf;
	sq_ctxt = sq_ctxt_block->sq_ctxt;

	hinic_qp_prepare_header(&sq_ctxt_block->hdr, HINIC_QP_CTXT_TYPE_SQ,
				num_sqs, max_sqs);
	for (i = 0; i < num_sqs; i++) {
		qp = &func_to_io->qps[i];
		sq = &qp->sq;
		global_qpn = base_qpn + qp->q_id;

		hinic_sq_prepare_ctxt(sq, global_qpn, &sq_ctxt[i]);
	}

	cmdq_buf.size = HINIC_SQ_CTXT_SIZE(num_sqs);

	err = hinic_cmdq_direct_resp(&func_to_io->cmdqs, HINIC_MOD_L2NIC,
				     IO_CMD_MODIFY_QUEUE_CTXT, &cmdq_buf,
				     &out_param);
	if ((err) || (out_param != 0)) {
		pr_err("Failed to set SQ ctxts\n");
		err = -EFAULT;
	}

	hinic_free_cmdq_buf(&func_to_io->cmdqs, &cmdq_buf);
	return err;
}

static int write_rq_ctxts(struct hinic_func_to_io *func_to_io, u16 base_qpn,
			  u16 num_rqs)
{
	struct hinic_cmdq_buf cmdq_buf;
	struct hinic_rq_ctxt_block *rq_ctxt_block;
	struct hinic_rq_ctxt *rq_ctxt;
	struct hinic_qp *qp;
	struct hinic_rq *rq;
	u64 out_param;
	u16 global_qpn, max_rqs = func_to_io->max_qps;
	int err, i;

	err = hinic_alloc_cmdq_buf(&func_to_io->cmdqs, &cmdq_buf);
	if (err) {
		pr_err("Failed to allocate cmdq buf\n");
		return err;
	}

	rq_ctxt_block = cmdq_buf.buf;
	rq_ctxt = rq_ctxt_block->rq_ctxt;

	hinic_qp_prepare_header(&rq_ctxt_block->hdr, HINIC_QP_CTXT_TYPE_RQ,
				num_rqs, max_rqs);
	for (i = 0; i < num_rqs; i++) {
		qp = &func_to_io->qps[i];
		rq = &qp->rq;
		global_qpn = base_qpn + qp->q_id;

		hinic_rq_prepare_ctxt(rq, global_qpn, &rq_ctxt[i]);
	}

	cmdq_buf.size = HINIC_RQ_CTXT_SIZE(num_rqs);

	err = hinic_cmdq_direct_resp(&func_to_io->cmdqs, HINIC_MOD_L2NIC,
				     IO_CMD_MODIFY_QUEUE_CTXT, &cmdq_buf,
				     &out_param);
	if ((err) || (out_param != 0)) {
		pr_err("Failed to set RQ ctxts\n");
		err = -EFAULT;
	}

	hinic_free_cmdq_buf(&func_to_io->cmdqs, &cmdq_buf);
	return err;
}

/**
 * write_qp_ctxts - write the qp ctxt to HW
 * @func_to_io: func to io channel that holds the IO components
 * @base_qpn: first qp number
 * @num_qps: number of qps to write
 *
 * Return 0 - Success, negative - Failure
 **/
static int write_qp_ctxts(struct hinic_func_to_io *func_to_io, u16 base_qpn,
			  u16 num_qps)
{
	return (write_sq_ctxts(func_to_io, base_qpn, num_qps) ||
		write_rq_ctxts(func_to_io, base_qpn, num_qps));
}

/**
 * init_qp - Initialize a Queue Pair
 * @func_to_io: func to io channel that holds the IO components
 * @qp: pointer to the qp to initialize
 * @q_id: the id of the qp
 * @sq_msix_entry: msix entry for sq
 * @rq_msix_entry: msix entry for rq
 *
 * Return 0 - Success, negative - Failure
 **/
static int init_qp(struct hinic_func_to_io *func_to_io,
		   struct hinic_qp *qp, int q_id,
		   struct msix_entry *sq_msix_entry,
		   struct msix_entry *rq_msix_entry)
{
	struct hinic_hwif *hwif = func_to_io->hwif;
	void *ci_addr_base = func_to_io->ci_addr_base;
	dma_addr_t ci_dma_base = func_to_io->ci_dma_base;
	void __iomem *db_base;
	int err;

	qp->q_id = q_id;

	err = hinic_wq_allocate(&func_to_io->wqs, &func_to_io->sq_wq[q_id],
				HINIC_SQ_WQEBB_SIZE, HINIC_SQ_PAGE_SIZE,
				HINIC_SQ_DEPTH, HINIC_SQ_WQE_MAX_SIZE);
	if (err) {
		pr_err("Failed to allocate WQ for SQ\n");
		return err;
	}

	err = hinic_wq_allocate(&func_to_io->wqs, &func_to_io->rq_wq[q_id],
				HINIC_RQ_WQEBB_SIZE, HINIC_RQ_PAGE_SIZE,
				HINIC_RQ_DEPTH, HINIC_RQ_WQE_SIZE);
	if (err) {
		pr_err("Failed to allocate WQ for RQ\n");
		goto rq_alloc_err;
	}

	err = get_db_area(func_to_io, &db_base);
	if (err) {
		pr_err("Failed to get DB area for SQ\n");
		goto get_db_err;
	}

	func_to_io->sq_db[q_id] = db_base;

	err = hinic_init_sq(&qp->sq, hwif, &func_to_io->sq_wq[q_id],
			    sq_msix_entry, CI_ADDR(ci_addr_base, q_id),
			    CI_ADDR(ci_dma_base, q_id), db_base);
	if (err) {
		pr_err("Failed to init SQ\n");
		goto sq_init_err;
	}

	err = hinic_init_rq(&qp->rq, hwif, &func_to_io->rq_wq[q_id],
			    rq_msix_entry);
	if (err) {
		pr_err("Failed to init RQ\n");
		goto rq_init_err;
	}

	return 0;

rq_init_err:
	hinic_clean_sq(&qp->sq);

sq_init_err:
	return_db_area(func_to_io, db_base);

get_db_err:
	hinic_wq_free(&func_to_io->wqs, &func_to_io->rq_wq[q_id]);

rq_alloc_err:
	hinic_wq_free(&func_to_io->wqs, &func_to_io->sq_wq[q_id]);
	return err;
}

/**
 * destroy_qp - Clean the resources of a Queue Pair
 * @func_to_io: func to io channel that holds the IO components
 * @qp: pointer to the qp to clean
 **/
static void destroy_qp(struct hinic_func_to_io *func_to_io,
		       struct hinic_qp *qp)
{
	int q_id = qp->q_id;

	hinic_clean_rq(&qp->rq);
	hinic_clean_sq(&qp->sq);

	return_db_area(func_to_io, func_to_io->sq_db[q_id]);

	hinic_wq_free(&func_to_io->wqs, &func_to_io->rq_wq[q_id]);
	hinic_wq_free(&func_to_io->wqs, &func_to_io->sq_wq[q_id]);
}

/**
 * hinic_io_create_qps - Create Queue Pairs
 * @func_to_io: func to io channel that holds the IO components
 * @base_qpn: base qp number
 * @num_qps: number queue pairs to create
 * @sq_msix_entry: msix entries for sq
 * @rq_msix_entry: msix entries for rq
 *
 * Return 0 - Success, negative - Failure
 **/
int hinic_io_create_qps(struct hinic_func_to_io *func_to_io,
			u16 base_qpn, int num_qps,
			struct msix_entry *sq_msix_entries,
			struct msix_entry *rq_msix_entries)
{
	struct hinic_hwif *hwif = func_to_io->hwif;
	struct pci_dev *pdev = hwif->pdev;
	void *ci_addr_base;
	size_t qps_size, wq_size, db_size, ci_table_size;
	int i, j, err;

	qps_size = num_qps * sizeof(*func_to_io->qps);
	func_to_io->qps = kzalloc(qps_size, GFP_KERNEL);
	if (!func_to_io->qps)
		return -ENOMEM;

	wq_size = num_qps * sizeof(*func_to_io->sq_wq);
	func_to_io->sq_wq = kzalloc(wq_size, GFP_KERNEL);
	if (!func_to_io->sq_wq) {
		err = -ENOMEM;
		goto sq_wq_err;
	}

	wq_size = num_qps * sizeof(*func_to_io->rq_wq);
	func_to_io->rq_wq = kzalloc(wq_size, GFP_KERNEL);
	if (!func_to_io->rq_wq) {
		err = -ENOMEM;
		goto rq_wq_err;
	}

	db_size = num_qps * sizeof(*func_to_io->sq_db);
	func_to_io->sq_db = kzalloc(db_size, GFP_KERNEL);
	if (!func_to_io->sq_db) {
		err = -ENOMEM;
		goto sq_db_err;
	}

	ci_table_size = CI_TABLE_SIZE(num_qps);

	ci_addr_base = dma_zalloc_coherent(&pdev->dev, ci_table_size,
					   &func_to_io->ci_dma_base,
					   GFP_KERNEL);
	if (!ci_addr_base) {
		dev_err(&pdev->dev, "Failed to allocate CI area\n");
		err = -ENOMEM;
		goto ci_base_err;
	}

	func_to_io->ci_addr_base = ci_addr_base;

	for (i = 0; i < num_qps; i++) {
		err = init_qp(func_to_io, &func_to_io->qps[i], i,
			      &sq_msix_entries[i], &rq_msix_entries[i]);
		if (err) {
			pr_err("Failed to create QP %d\n", i);
			goto init_qp_err;
		}
	}

	err = write_qp_ctxts(func_to_io, base_qpn, num_qps);
	if (err) {
		dev_err(&pdev->dev, "Failed to init QP ctxts\n");
		goto write_qp_ctxts_err;
	}

	return 0;

write_qp_ctxts_err:
init_qp_err:
	for (j = 0; j < i; j++)
		destroy_qp(func_to_io, &func_to_io->qps[j]);

	dma_free_coherent(&pdev->dev, ci_table_size, func_to_io->ci_addr_base,
			  func_to_io->ci_dma_base);

ci_base_err:
	kfree(func_to_io->sq_db);

sq_db_err:
	kfree(func_to_io->rq_wq);

rq_wq_err:
	kfree(func_to_io->sq_wq);

sq_wq_err:
	kfree(func_to_io->qps);
	return err;
}

/**
 * hinic_io_destroy_qps - Destroy the IO Queue Pairs
 * @func_to_io: func to io channel that holds the IO components
 * @num_qps: number queue pairs to destroy
 **/
void hinic_io_destroy_qps(struct hinic_func_to_io *func_to_io, int num_qps)
{
	struct hinic_hwif *hwif = func_to_io->hwif;
	struct pci_dev *pdev = hwif->pdev;
	size_t ci_table_size;
	int i;

	ci_table_size = CI_TABLE_SIZE(num_qps);

	for (i = 0; i < num_qps; i++)
		destroy_qp(func_to_io, &func_to_io->qps[i]);

	dma_free_coherent(&pdev->dev, ci_table_size, func_to_io->ci_addr_base,
			  func_to_io->ci_dma_base);

	kfree(func_to_io->sq_db);

	kfree(func_to_io->rq_wq);
	kfree(func_to_io->sq_wq);

	kfree(func_to_io->qps);
}

/**
 * hinic_io_init - Initialize the IO components
 * @func_to_io: func to io channel that holds the IO components
 * @hwif: HW interface for accessing IO
 * @max_qps: maximum QPs in HW
 * @num_ceqs: number completion event queues
 * @ceq_msix_entries: msix entries for ceqs
 *
 * Return 0 - Success, negative - Failure
 **/
int hinic_io_init(struct hinic_func_to_io *func_to_io,
		  struct hinic_hwif *hwif, u16 max_qps, int num_ceqs,
		  struct msix_entry *ceq_msix_entries)
{
	struct pci_dev *pdev = hwif->pdev;
	enum hinic_cmdq_type cmdq, type;
	int err;

	func_to_io->hwif = hwif;
	func_to_io->qps = NULL;
	func_to_io->max_qps = max_qps;

	err = hinic_wqs_alloc(&func_to_io->wqs, 2 * max_qps, hwif);
	if (err) {
		pr_err("Failed to allocate WQS for IO\n");
		return err;
	}

	func_to_io->db_base = pci_ioremap_bar(pdev, HINIC_PCI_DB_BAR);
	if (!func_to_io->db_base) {
		dev_err(&pdev->dev, "Failed to remap IO DB area\n");
		err = -ENOMEM;
		goto db_ioremap_err;
	}

	init_db_area_idx(&func_to_io->free_db_area);

	for (cmdq = HINIC_CMDQ_SYNC; cmdq < HINIC_MAX_CMDQ_TYPES; cmdq++) {
		err = get_db_area(func_to_io, &func_to_io->cmdq_db_area[cmdq]);
		if (err) {
			dev_err(&pdev->dev, "Failed to get cmdq db area\n");
			goto db_area_err;
		}
	}

	err = hinic_init_cmdqs(&func_to_io->cmdqs, hwif,
			       func_to_io->cmdq_db_area);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize cmdqs\n");
		goto init_cmdqs_err;
	}

	return 0;

init_cmdqs_err:
db_area_err:
	for (type = HINIC_CMDQ_SYNC; type < cmdq; type++)
		return_db_area(func_to_io, func_to_io->cmdq_db_area[type]);

	iounmap(func_to_io->db_base);

db_ioremap_err:
	hinic_wqs_free(&func_to_io->wqs);
	return err;
}

/**
 * hinic_io_free - Free the IO components
 * @func_to_io: func to io channel that holds the IO components
 **/
void hinic_io_free(struct hinic_func_to_io *func_to_io)
{
	enum hinic_cmdq_type cmdq;

	hinic_free_cmdqs(&func_to_io->cmdqs);

	for (cmdq = HINIC_CMDQ_SYNC; cmdq < HINIC_MAX_CMDQ_TYPES; cmdq++)
		return_db_area(func_to_io, func_to_io->cmdq_db_area[cmdq]);

	iounmap(func_to_io->db_base);
	hinic_wqs_free(&func_to_io->wqs);
}
