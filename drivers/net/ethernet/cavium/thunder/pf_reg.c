/*
 * Copyright (C) 2015 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/firmware.h>
#include "pf_globals.h"
#include "pf_locals.h"
#include "tbl_access.h"
#include "linux/lz4.h"

struct tns_table_s tbl_info[TNS_MAX_TABLE];

#define TNS_TDMA_SST_ACC_CMD_ADDR	0x0000842000000270ull

#define BAR0_START 0x842000000000
#define BAR0_END   0x84200000FFFF
#define BAR0_SIZE  (64 * 1024)
#define BAR2_START 0x842040000000
#define BAR2_END   0x84207FFFFFFF
#define BAR2_SIZE  (1024 * 1024 * 1024)

#define NODE1_BAR0_START 0x942000000000
#define NODE1_BAR0_END   0x94200000FFFF
#define NODE1_BAR0_SIZE  (64 * 1024)
#define NODE1_BAR2_START 0x942040000000
#define NODE1_BAR2_END   0x94207FFFFFFF
#define NODE1_BAR2_SIZE  (1024 * 1024 * 1024)
/* Allow a max of 4 chunks for the Indirect Read/Write */
#define MAX_SIZE (64 * 4)
#define CHUNK_SIZE (64)
/* To protect register access */
spinlock_t pf_reg_lock;

u64 iomem0;
u64 iomem2;
u8 tns_enabled;
u64 node1_iomem0;
u64 node1_iomem2;
u8 node1_tns;
int n1_tns;

int tns_write_register_indirect(int node_id, u64 address, u8 size,
				u8 *kern_buffer)
{
	union tns_tdma_sst_acc_cmd acccmd;
	union tns_tdma_sst_acc_stat_t accstat;
	union tns_acc_data data;
	int i, j, w = 0;
	int cnt = 0;
	u32 *dataw = NULL;
	int temp = 0;
	int k = 0;
	int chunks = 0;
	u64 acccmd_address;
	u64 lmem2 = 0, lmem0 = 0;

	if (size == 0 || !kern_buffer) {
		filter_dbg(FERR, "%s data size cannot be zero\n", __func__);
		return TNS_ERROR_INVALID_ARG;
	}
	if (size > MAX_SIZE) {
		filter_dbg(FERR, "%s Max allowed size exceeded\n", __func__);
		return TNS_ERROR_DATA_TOO_LARGE;
	}
	if (node_id) {
		lmem0 = node1_iomem0;
		lmem2 = node1_iomem2;
	} else {
		lmem0 = iomem0;
		lmem2 = iomem2;
	}

	chunks = ((size + (CHUNK_SIZE - 1)) / CHUNK_SIZE);
	acccmd_address = (address & 0x00000000ffffffff);
	spin_lock_bh(&pf_reg_lock);

	for (k = 0; k < chunks; k++) {
		/* Should never happen */
		if (size < 0) {
			filter_dbg(FERR, "%s size mismatch [CHUNK %d]\n",
				   __func__, k);
			break;
		}
		temp = (size > CHUNK_SIZE) ? CHUNK_SIZE : size;
		dataw = (u32 *)(kern_buffer + (k * CHUNK_SIZE));
		cnt = ((temp + 3) / 4);
		data.u = 0ULL;
		for (j = 0, i = 0; i < cnt; i++) {
			/* Odd words go in the upper 32 bits of the data
			 * register
			 */
			if (i & 1) {
				data.s.upper32 = dataw[i];
				writeq_relaxed(data.u, (void *)(lmem0 +
					       TNS_TDMA_SST_ACC_WDATX(j)));
				data.u = 0ULL;
				j++; /* Advance to the next data word */
				w = 0;
			} else {
				/* Lower 32 bits contain words 0, 2, 4, etc. */
				data.s.lower32 = dataw[i];
				w = 1;
			}
		}

		/* If the last word was a partial (< 64 bits) then
		 * see if we need to write it.
		 */
		if (w)
			writeq_relaxed(data.u, (void *)(lmem0 +
				       TNS_TDMA_SST_ACC_WDATX(j)));

		acccmd.u = 0ULL;
		acccmd.s.go = 1; /* Cleared once the request is serviced */
		acccmd.s.size = cnt;
		acccmd.s.addr = (acccmd_address >> 2);
		writeq_relaxed(acccmd.u, (void *)(lmem0 +
			       TDMA_SST_ACC_CMD));
		accstat.u = 0ULL;

		while (!accstat.s.cmd_done && !accstat.s.error)
			accstat.u = readq_relaxed((void *)(lmem0 +
					  TDMA_SST_ACC_STAT));

		if (accstat.s.error) {
			data.u = readq_relaxed((void *)(lmem2 +
					       TDMA_NB_INT_STAT));
			filter_dbg(FERR, "%s Reading data from ", __func__);
			filter_dbg(FERR, "0x%0lx chunk %d failed 0x%0lx",
				   (unsigned long)address, k,
				   (unsigned long)data.u);
			spin_unlock_bh(&pf_reg_lock);
			kfree(kern_buffer);
			return TNS_ERROR_INDIRECT_WRITE;
		}
		/* Calculate the next offset to write */
		acccmd_address = acccmd_address + CHUNK_SIZE;
		size -= CHUNK_SIZE;
	}
	spin_unlock_bh(&pf_reg_lock);

	return 0;
}

int tns_read_register_indirect(int node_id, u64 address, u8 size,
			       u8 *kern_buffer)
{
	union tns_tdma_sst_acc_cmd acccmd;
	union tns_tdma_sst_acc_stat_t accstat;
	union tns_acc_data data;
	int i, j, dcnt;
	int cnt = 0;
	u32 *dataw = NULL;
	int temp = 0;
	int k = 0;
	int chunks = 0;
	u64 acccmd_address;
	u64 lmem2 = 0, lmem0 = 0;

	if (size == 0 || !kern_buffer) {
		filter_dbg(FERR, "%s data size cannot be zero\n", __func__);
		return TNS_ERROR_INVALID_ARG;
	}
	if (size > MAX_SIZE) {
		filter_dbg(FERR, "%s Max allowed size exceeded\n", __func__);
		return TNS_ERROR_DATA_TOO_LARGE;
	}
	if (node_id) {
		lmem0 = node1_iomem0;
		lmem2 = node1_iomem2;
	} else {
		lmem0 = iomem0;
		lmem2 = iomem2;
	}

	chunks = ((size + (CHUNK_SIZE - 1)) / CHUNK_SIZE);
	acccmd_address = (address & 0x00000000ffffffff);
	spin_lock_bh(&pf_reg_lock);
	for (k = 0; k < chunks; k++) {
		/* This should never happen */
		if (size < 0) {
			filter_dbg(FERR, "%s size mismatch [CHUNK:%d]\n",
				   __func__, k);
			break;
		}
		temp = (size > CHUNK_SIZE) ? CHUNK_SIZE : size;
		dataw = (u32 *)(kern_buffer + (k * CHUNK_SIZE));
		cnt = ((temp + 3) / 4);
		acccmd.u = 0ULL;
		acccmd.s.op = 1; /* Read operation */
		acccmd.s.size = cnt;
		acccmd.s.addr = (acccmd_address >> 2);
		acccmd.s.go = 1; /* Execute */
		writeq_relaxed(acccmd.u, (void *)(lmem0 +
			       TDMA_SST_ACC_CMD));
		accstat.u = 0ULL;

		while (!accstat.s.cmd_done && !accstat.s.error)
			accstat.u = readq_relaxed((void *)(lmem0 +
						  TDMA_SST_ACC_STAT));

		if (accstat.s.error) {
			data.u = readq_relaxed((void *)(lmem2 +
					       TDMA_NB_INT_STAT));
			filter_dbg(FERR, "%s Reading data from", __func__);
			filter_dbg(FERR, "0x%0lx chunk %d failed 0x%0lx",
				   (unsigned long)address, k,
				   (unsigned long)data.u);
			spin_unlock_bh(&pf_reg_lock);
			kfree(kern_buffer);
			return TNS_ERROR_INDIRECT_READ;
		}

		dcnt = cnt / 2;
		if (cnt & 1)
			dcnt++;
		for (i = 0, j = 0; (j < dcnt) && (i < cnt); j++) {
			data.u = readq_relaxed((void *)(lmem0 +
					       TNS_TDMA_SST_ACC_RDATX(j)));
			dataw[i++] = data.s.lower32;
			if (i < cnt)
				dataw[i++] = data.s.upper32;
		}
		/* Calculate the next offset to read */
		acccmd_address = acccmd_address + CHUNK_SIZE;
		size -= CHUNK_SIZE;
	}
	spin_unlock_bh(&pf_reg_lock);
	return 0;
}

u64 tns_read_register(u64 start, u64 offset)
{
	return readq_relaxed((void *)(start + offset));
}

void tns_write_register(u64 start, u64 offset, u64 data)
{
	writeq_relaxed(data, (void *)(start + offset));
}

/* Check if TNS is available. If yes return 0 else 1 */
int is_tns_available(void)
{
	union tns_tdma_cap tdma_cap;

	tdma_cap.u = tns_read_register(iomem0, TNS_TDMA_CAP_OFFSET);
	tns_enabled = tdma_cap.s.switch_capable;
	/* In multi-node systems, make sure TNS should be there in both nodes */
	if (nr_node_ids > 1) {
		tdma_cap.u = tns_read_register(node1_iomem0,
					       TNS_TDMA_CAP_OFFSET);
		if (tdma_cap.s.switch_capable)
			n1_tns = 1;
	}
	tns_enabled &= tdma_cap.s.switch_capable;
	return (!tns_enabled);
}

int bist_error_check(void)
{
	int fail = 0, i;
	u64 bist_stat = 0;

	for (i = 0; i < 12; i++) {
		bist_stat = tns_read_register(iomem0, (i * 16));
		if (bist_stat) {
			filter_dbg(FERR, "TNS BIST%d fail 0x%llx\n",
				   i, bist_stat);
			fail = 1;
		}
		if (!n1_tns)
			continue;
		bist_stat = tns_read_register(node1_iomem0, (i * 16));
		if (bist_stat) {
			filter_dbg(FERR, "TNS(N1) BIST%d fail 0x%llx\n",
				   i, bist_stat);
			fail = 1;
		}
	}

	return fail;
}

int replay_indirect_trace(int node, u64 *buf_ptr, int idx)
{
	union _tns_sst_config cmd = (union _tns_sst_config)(buf_ptr[idx]);
	int remaining = cmd.cmd.run;
	u64 io_addr;
	int word_cnt = cmd.cmd.word_cnt;
	int size = (word_cnt + 1) / 2;
	u64 stride = word_cnt;
	u64 acc_cmd = cmd.copy.do_copy;
	u64 lmem2 = 0, lmem0 = 0;
	union tns_tdma_sst_acc_stat_t accstat;
	union tns_acc_data data;

	if (node) {
		lmem0 = node1_iomem0;
		lmem2 = node1_iomem2;
	} else {
		lmem0 = iomem0;
		lmem2 = iomem2;
	}

	if (word_cnt == 0) {
		word_cnt = 16;
		stride = 16;
		size = 8;
	} else {
		// make stride next power of 2
		if (cmd.cmd.powerof2stride)
			while ((stride & (stride - 1)) != 0)
				stride++;
	}
	stride *= 4; //convert stride from 32-bit words to bytes

	do {
		int addr_p = 1;
		/* extract (big endian) data from the config
		 * into the data array
		 */
		while (size > 0) {
			io_addr = lmem0 + TDMA_SST_ACC_CMD + addr_p * 16;
			tns_write_register(io_addr, 0, buf_ptr[idx + size]);
			addr_p += 1;
			size--;
		}
		tns_write_register((lmem0 + TDMA_SST_ACC_CMD), 0, acc_cmd);
		/* TNS Block access registers indirectly, ran memory barrier
		 * between two writes
		 */
		wmb();
		/* Check for completion */
		accstat.u = 0ULL;
		while (!accstat.s.cmd_done && !accstat.s.error)
			accstat.u = readq_relaxed((void *)(lmem0 +
							   TDMA_SST_ACC_STAT));

		/* Check for error, and report it */
		if (accstat.s.error) {
			filter_dbg(FERR, "%s data from 0x%0llx failed 0x%llx\n",
				   __func__, acc_cmd, accstat.u);
			data.u = readq_relaxed((void *)(lmem2 +
							TDMA_NB_INT_STAT));
			filter_dbg(FERR, "Status 0x%llx\n", data.u);
		}
		/* update the address */
		acc_cmd += stride;
		size = (word_cnt + 1) / 2;
		usleep_range(20, 30);
	} while (remaining-- > 0);

	return size;
}

void replay_tns_node(int node, u64 *buf_ptr, int reg_cnt)
{
	int counter = 0;
	u64 offset = 0;
	u64 io_address;
	int datapathmode = 1;
	u64 lmem2 = 0, lmem0 = 0;

	if (node) {
		lmem0 = node1_iomem0;
		lmem2 = node1_iomem2;
	} else {
		lmem0 = iomem0;
		lmem2 = iomem2;
	}
	for (counter = 0; counter < reg_cnt; counter++) {
		if (buf_ptr[counter] == 0xDADADADADADADADAull) {
			datapathmode = 1;
			continue;
		} else if (buf_ptr[counter] == 0xDEDEDEDEDEDEDEDEull) {
			datapathmode = 0;
			continue;
		}
		if (datapathmode == 1) {
			if (buf_ptr[counter] >= BAR0_START &&
			    buf_ptr[counter] <= BAR0_END) {
				offset = buf_ptr[counter] - BAR0_START;
				io_address = lmem0 + offset;
			} else if (buf_ptr[counter] >= BAR2_START &&
				   buf_ptr[counter] <= BAR2_END) {
				offset = buf_ptr[counter] - BAR2_START;
				io_address = lmem2 + offset;
			} else {
				filter_dbg(FERR, "%s Address 0x%llx invalid\n",
					   __func__, buf_ptr[counter]);
				return;
			}

			tns_write_register(io_address, 0, buf_ptr[counter + 1]);
			/* TNS Block access registers indirectly, ran memory
			 * barrier between two writes
			 */
			wmb();
			counter += 1;
			usleep_range(20, 30);
		} else if (datapathmode == 0) {
			int sz = replay_indirect_trace(node, buf_ptr, counter);

			counter += sz;
		}
	}
}

int alloc_table_info(int i, struct table_static_s tbl_sdata[])
{
	tbl_info[i].ddata[0].bitmap = kcalloc(BITS_TO_LONGS(tbl_sdata[i].depth),
					      sizeof(uintptr_t), GFP_KERNEL);
	if (!tbl_info[i].ddata[0].bitmap)
		return 1;

	if (!n1_tns)
		return 0;

	tbl_info[i].ddata[1].bitmap = kcalloc(BITS_TO_LONGS(tbl_sdata[i].depth),
					      sizeof(uintptr_t), GFP_KERNEL);
	if (!tbl_info[i].ddata[1].bitmap) {
		kfree(tbl_info[i].ddata[0].bitmap);
		return 1;
	}

	return 0;
}

void tns_replay_register_trace(const struct firmware *fw, struct device *dev)
{
	int i;
	int node = 0;
	u8 *buffer = NULL;
	u64 *buf_ptr = NULL;
	struct tns_global_st *fw_header = NULL;
	struct table_static_s tbl_sdata[TNS_MAX_TABLE];
	size_t src_len;
	size_t dest_len = TNS_FW_MAX_SIZE;
	int rc;
	u8 *fw2_buf = NULL;
	unsigned char *decomp_dest = NULL;

	fw2_buf = (u8 *)fw->data;
	src_len = fw->size - 8;

	decomp_dest = kcalloc((dest_len * 2), sizeof(char), GFP_KERNEL);
	if (!decomp_dest)
		return;

	memset(decomp_dest, 0, (dest_len * 2));
	rc = lz4_decompress_unknownoutputsize(&fw2_buf[8], src_len, decomp_dest,
					      &dest_len);
	if (rc) {
		filter_dbg(FERR, "Decompress Error %d\n", rc);
		pr_info("Uncompressed destination length %ld\n", dest_len);
		kfree(decomp_dest);
		return;
	}
	fw_header = (struct tns_global_st *)decomp_dest;
	buffer = (u8 *)decomp_dest;

	filter_dbg(FINFO, "TNS Firmware version: %s Loading...\n",
		   fw_header->version);

	memset(tbl_info, 0x0, sizeof(tbl_info));
	buf_ptr = (u64 *)(buffer + sizeof(struct tns_global_st));
	memcpy(tbl_sdata, fw_header->tbl_info, sizeof(fw_header->tbl_info));

	for (i = 0; i < TNS_MAX_TABLE; i++) {
		if (!tbl_sdata[i].valid)
			continue;
		memcpy(&tbl_info[i].sdata, &tbl_sdata[i],
		       sizeof(struct table_static_s));
		if (alloc_table_info(i, tbl_sdata)) {
			kfree(decomp_dest);
			return;
		}
	}

	for (node = 0; node < nr_node_ids; node++)
		replay_tns_node(node, buf_ptr, fw_header->reg_cnt);

	kfree(decomp_dest);
	release_firmware(fw);
}

int tns_init(const struct firmware *fw, struct device *dev)
{
	int result = 0;
	int i = 0;
	int temp;
	union tns_tdma_config tdma_config;
	union tns_tdma_lmacx_config tdma_lmac_cfg;
	u64 reg_init_val;

	spin_lock_init(&pf_reg_lock);

	/* use two regions insted of a single big mapping to save
	 * the kernel virtual space
	 */
	iomem0 = (u64)ioremap(BAR0_START, BAR0_SIZE);
	if (iomem0 == 0ULL) {
		filter_dbg(FERR, "Node0 ioremap failed for BAR0\n");
		result = -EAGAIN;
		goto error;
	} else {
		filter_dbg(FINFO, "ioremap success for BAR0\n");
	}

	if (nr_node_ids > 1) {
		node1_iomem0 = (u64)ioremap(NODE1_BAR0_START, NODE1_BAR0_SIZE);
		if (node1_iomem0 == 0ULL) {
			filter_dbg(FERR, "Node1 ioremap failed for BAR0\n");
			result = -EAGAIN;
			goto error;
		} else {
			filter_dbg(FINFO, "ioremap success for BAR0\n");
		}
	}

	if (is_tns_available()) {
		filter_dbg(FERR, "TNS NOT AVAILABLE\n");
		goto error;
	}

	if (bist_error_check()) {
		filter_dbg(FERR, "BIST ERROR CHECK FAILED");
		goto error;
	}

	/* NIC0-BGX0 is TNS, NIC1-BGX1 is TNS, DISABLE BACKPRESSURE */
	reg_init_val = 0ULL;
	pr_info("NIC Block configured in TNS/TNS mode");
	tns_write_register(iomem0, TNS_RDMA_CONFIG_OFFSET, reg_init_val);
	usleep_range(10, 20);
	if (n1_tns) {
		tns_write_register(node1_iomem0, TNS_RDMA_CONFIG_OFFSET,
				   reg_init_val);
		usleep_range(10, 20);
	}

	// Configure each LMAC with 512 credits in BYPASS mode
	for (i = TNS_MIN_LMAC; i < (TNS_MIN_LMAC + TNS_MAX_LMAC); i++) {
		tdma_lmac_cfg.u = 0ULL;
		tdma_lmac_cfg.s.fifo_cdts = 0x200;
		tns_write_register(iomem0, TNS_TDMA_LMACX_CONFIG_OFFSET(i),
				   tdma_lmac_cfg.u);
		usleep_range(10, 20);
		if (n1_tns) {
			tns_write_register(node1_iomem0,
					   TNS_TDMA_LMACX_CONFIG_OFFSET(i),
					   tdma_lmac_cfg.u);
			usleep_range(10, 20);
		}
	}

	//ENABLE TNS CLOCK AND CSR READS
	temp = tns_read_register(iomem0, TNS_TDMA_CONFIG_OFFSET);
	tdma_config.u = temp;
	tdma_config.s.clk_2x_ena = 1;
	tdma_config.s.clk_ena = 1;
	tns_write_register(iomem0, TNS_TDMA_CONFIG_OFFSET, tdma_config.u);
	if (n1_tns)
		tns_write_register(node1_iomem0, TNS_TDMA_CONFIG_OFFSET,
				   tdma_config.u);

	temp = tns_read_register(iomem0, TNS_TDMA_CONFIG_OFFSET);
	tdma_config.u = temp;
	tdma_config.s.csr_access_ena = 1;
	tns_write_register(iomem0, TNS_TDMA_CONFIG_OFFSET, tdma_config.u);
	if (n1_tns)
		tns_write_register(node1_iomem0, TNS_TDMA_CONFIG_OFFSET,
				   tdma_config.u);

	reg_init_val = 0ULL;
	tns_write_register(iomem0, TNS_TDMA_RESET_CTL_OFFSET, reg_init_val);
	if (n1_tns)
		tns_write_register(node1_iomem0, TNS_TDMA_RESET_CTL_OFFSET,
				   reg_init_val);

	iomem2 = (u64)ioremap(BAR2_START, BAR2_SIZE);
	if (iomem2 == 0ULL) {
		filter_dbg(FERR, "ioremap failed for BAR2\n");
		result = -EAGAIN;
		goto error;
	} else {
		filter_dbg(FINFO, "ioremap success for BAR2\n");
	}

	if (n1_tns) {
		node1_iomem2 = (u64)ioremap(NODE1_BAR2_START, NODE1_BAR2_SIZE);
		if (node1_iomem2 == 0ULL) {
			filter_dbg(FERR, "Node1 ioremap failed for BAR2\n");
			result = -EAGAIN;
			goto error;
		} else {
			filter_dbg(FINFO, "Node1 ioremap success for BAR2\n");
		}
	}
	msleep(1000);
	//We will replay register trace to initialize TNS block
	tns_replay_register_trace(fw, dev);

	return 0;
error:
	if (iomem0 != 0)
		iounmap((void *)iomem0);
	if (iomem2 != 0)
		iounmap((void *)iomem2);

	if (node1_iomem0 != 0)
		iounmap((void *)node1_iomem0);
	if (node1_iomem2 != 0)
		iounmap((void *)node1_iomem2);

	return result;
}

void tns_exit(void)
{
	int i;

	if (iomem0 != 0)
		iounmap((void *)iomem0);
	if (iomem2 != 0)
		iounmap((void *)iomem2);

	if (node1_iomem0 != 0)
		iounmap((void *)node1_iomem0);
	if (node1_iomem2 != 0)
		iounmap((void *)node1_iomem2);

	for (i = 0; i < TNS_MAX_TABLE; i++) {
		if (!tbl_info[i].sdata.valid)
			continue;
		kfree(tbl_info[i].ddata[0].bitmap);
		kfree(tbl_info[i].ddata[n1_tns].bitmap);
	}
}
