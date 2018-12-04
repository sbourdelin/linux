// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Copyright 2018 Amazon.com, Inc. or its affiliates.
 */

#include <linux/vmalloc.h>

#include <rdma/efa-abi.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>

#include "efa.h"

#define EFA_MMAP_DB_BAR_MEMORY_FLAG     BIT(61)
#define EFA_MMAP_REG_BAR_MEMORY_FLAG    BIT(62)
#define EFA_MMAP_MEM_BAR_MEMORY_FLAG    BIT(63)
#define EFA_MMAP_BARS_MEMORY_MASK       \
	(EFA_MMAP_REG_BAR_MEMORY_FLAG | EFA_MMAP_MEM_BAR_MEMORY_FLAG | \
	 EFA_MMAP_DB_BAR_MEMORY_FLAG)

struct efa_ucontext {
	struct ib_ucontext      ibucontext;
	/* Protects ucontext state */
	struct mutex            lock;
	struct list_head        link;
	struct list_head        pending_mmaps;
	u64                     mmap_key;
};

#define EFA_AENQ_ENABLED_GROUPS \
	(BIT(EFA_ADMIN_FATAL_ERROR) | BIT(EFA_ADMIN_WARNING) | \
	 BIT(EFA_ADMIN_NOTIFICATION) | BIT(EFA_ADMIN_KEEP_ALIVE))

struct efa_pd {
	struct ib_pd    ibpd;
	u32             pdn;
};

struct efa_mr {
	struct ib_mr     ibmr;
	struct ib_umem  *umem;
	u64 vaddr;
};

struct efa_cq {
	struct ib_cq               ibcq;
	struct efa_ucontext       *ucontext;
	u16                        cq_idx;
	dma_addr_t                 dma_addr;
	void                      *cpu_addr;
	size_t                     size;
};

struct efa_qp {
	struct ib_qp            ibqp;
	enum ib_qp_state        state;
	u32                     qp_handle;
	dma_addr_t              rq_dma_addr;
	void                   *rq_cpu_addr;
	size_t                  rq_size;
};

struct efa_ah {
	struct ib_ah    ibah;
	/* dest_addr */
	u8              id[EFA_GID_SIZE];
};

struct efa_ah_id {
	struct list_head list;
	/* dest_addr */
	u8 id[EFA_GID_SIZE];
	u16 address_handle;
	unsigned int  ref_count;
};

struct efa_mmap_entry {
	struct list_head list;
	void  *obj;
	u64 address;
	u64 length;
	u64 key;
};

static void mmap_entry_insert(struct efa_ucontext *ucontext,
			      struct efa_mmap_entry *entry,
			      u64 mem_flag);

static void mmap_obj_entries_remove(struct efa_ucontext *ucontext,
				    void *obj);

#define EFA_PAGE_SHIFT       12
#define EFA_PAGE_SIZE        BIT(EFA_PAGE_SHIFT)
#define EFA_PAGE_PTR_SIZE    8

#define EFA_CHUNK_ALLOC_SIZE BIT(EFA_PAGE_SHIFT)
#define EFA_CHUNK_PTR_SIZE   sizeof(struct efa_com_ctrl_buff_info)

#define EFA_PAGE_PTRS_PER_CHUNK  \
	((EFA_CHUNK_ALLOC_SIZE - EFA_CHUNK_PTR_SIZE) / EFA_PAGE_PTR_SIZE)

#define EFA_CHUNK_USED_SIZE  \
	((EFA_PAGE_PTRS_PER_CHUNK * EFA_PAGE_PTR_SIZE) + EFA_CHUNK_PTR_SIZE)

#define EFA_SUPPORTED_ACCESS_FLAGS IB_ACCESS_LOCAL_WRITE

struct pbl_chunk {
	u64 *buf;
	u32 length;
	dma_addr_t dma_addr;
};

struct pbl_chunk_list {
	unsigned int size;
	struct pbl_chunk *chunks;
};

struct pbl_context {
	u64 *pbl_buf;
	u32  pbl_buf_size_in_bytes;
	bool physically_continuous;
	union {
		struct {
			dma_addr_t dma_addr;
		} continuous;
		struct {
			u32 pbl_buf_size_in_pages;
			struct scatterlist *sgl;
			int sg_dma_cnt;
			struct pbl_chunk_list chunk_list;
		} indirect;
	} phys;

	struct efa_dev *dev;
	struct device *dmadev;
};

static inline struct efa_dev *to_edev(struct ib_device *ibdev)
{
	return container_of(ibdev, struct efa_dev, ibdev);
}

static inline struct efa_ucontext *to_eucontext(struct ib_ucontext *ibucontext)
{
	return container_of(ibucontext, struct efa_ucontext, ibucontext);
}

static inline struct efa_pd *to_epd(struct ib_pd *ibpd)
{
	return container_of(ibpd, struct efa_pd, ibpd);
}

static inline struct efa_mr *to_emr(struct ib_mr *ibmr)
{
	return container_of(ibmr, struct efa_mr, ibmr);
}

static inline struct efa_qp *to_eqp(struct ib_qp *ibqp)
{
	return container_of(ibqp, struct efa_qp, ibqp);
}

static inline struct efa_cq *to_ecq(struct ib_cq *ibcq)
{
	return container_of(ibcq, struct efa_cq, ibcq);
}

static inline struct efa_ah *to_eah(struct ib_ah *ibah)
{
	return container_of(ibah, struct efa_ah, ibah);
}

#define field_avail(x, fld, sz) (offsetof(typeof(x), fld) + \
				 sizeof(((typeof(x) *)0)->fld) <= (sz))

#define EFA_IS_RESERVED_CLEARED(reserved) \
	!memchr_inv(reserved, 0, sizeof(reserved))

int efa_query_device(struct ib_device *ibdev,
		     struct ib_device_attr *props,
		     struct ib_udata *udata)
{
	struct efa_ibv_ex_query_device_resp resp = {};
	struct efa_com_get_device_attr_result result;
	struct efa_dev *dev = to_edev(ibdev);
	int err;

	pr_debug("--->\n");
	memset(props, 0, sizeof(*props));

	if (udata && udata->inlen &&
	    !ib_is_udata_cleared(udata, 0, udata->inlen)) {
		pr_err_ratelimited("Incompatible ABI params, udata not cleared\n");
		return -EINVAL;
	}

	err = efa_get_device_attributes(dev, &result);
	if (err) {
		pr_err("failed to get device_attr err[%d]!\n", err);
		return err;
	}

	props->max_mr_size              = result.max_mr_pages * PAGE_SIZE;
	props->page_size_cap            = result.page_size_cap;
	props->vendor_id                = result.vendor_id;
	props->vendor_part_id           = result.vendor_part_id;
	props->hw_ver                   = dev->pdev->subsystem_device;
	props->max_qp                   = result.max_sq;
	props->device_cap_flags         = IB_DEVICE_PORT_ACTIVE_EVENT |
					  IB_DEVICE_VIRTUAL_FUNCTION |
					  IB_DEVICE_BLOCK_MULTICAST_LOOPBACK;
	props->max_cq                   = result.max_cq;
	props->max_pd                   = result.max_pd;
	props->max_mr                   = result.max_mr;
	props->max_ah                   = result.max_ah;
	props->max_cqe                  = result.max_cq_depth;
	props->max_qp_wr                = min_t(u16, result.max_sq_depth,
						result.max_rq_depth);
	props->max_send_sge             = result.max_sq_sge;
	props->max_recv_sge             = result.max_rq_sge;

	if (udata && udata->outlen) {
		resp.sub_cqs_per_cq = result.sub_cqs_per_cq;
		resp.max_sq_sge = result.max_sq_sge;
		resp.max_rq_sge = result.max_rq_sge;
		resp.max_sq_wr  = result.max_sq_depth;
		resp.max_rq_wr  = result.max_rq_depth;
		resp.max_inline_data = result.inline_buf_size;

		err = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (err) {
			pr_err_ratelimited("failed to copy udata for query_device.\n");
			return err;
		}
	}

	return err;
}

int efa_query_port(struct ib_device *ibdev, u8 port,
		   struct ib_port_attr *props)
{
	struct efa_dev *dev = to_edev(ibdev);

	pr_debug("--->\n");

	mutex_lock(&dev->efa_dev_lock);
	memset(props, 0, sizeof(*props));

	props->lid = 0;
	props->lmc = 1;
	props->sm_lid = 0;
	props->sm_sl = 0;

	props->state = IB_PORT_ACTIVE;
	props->phys_state = 5;
	props->port_cap_flags = 0;
	props->gid_tbl_len = 1;
	props->pkey_tbl_len = 1;
	props->bad_pkey_cntr = 0;
	props->qkey_viol_cntr = 0;
	props->active_speed = IB_SPEED_EDR;
	props->active_width = IB_WIDTH_4X;
	props->max_mtu = ib_mtu_int_to_enum(dev->mtu);
	props->active_mtu = ib_mtu_int_to_enum(dev->mtu);
	props->max_msg_sz = dev->mtu;
	props->max_vl_num = 1;
	mutex_unlock(&dev->efa_dev_lock);
	return 0;
}

int efa_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *qp_attr,
		 int qp_attr_mask,
		 struct ib_qp_init_attr *qp_init_attr)
{
	struct efa_qp *qp = to_eqp(ibqp);

	pr_debug("--->\n");

	memset(qp_attr, 0, sizeof(*qp_attr));
	memset(qp_init_attr, 0, sizeof(*qp_init_attr));

	qp_attr->qp_state = qp->state;
	qp_attr->cur_qp_state = qp->state;
	qp_attr->port_num = 1;

	qp_init_attr->qp_type = ibqp->qp_type;
	qp_init_attr->recv_cq = ibqp->recv_cq;
	qp_init_attr->send_cq = ibqp->send_cq;

	return 0;
}

int efa_query_gid(struct ib_device *ibdev, u8 port, int index,
		  union ib_gid *gid)
{
	struct efa_dev *dev = to_edev(ibdev);

	pr_debug("port %d gid index %d\n", port, index);

	if (index > 1)
		return -EINVAL;

	mutex_lock(&dev->efa_dev_lock);
	memcpy(gid->raw, dev->addr, sizeof(dev->addr));
	mutex_unlock(&dev->efa_dev_lock);

	return 0;
}

int efa_query_pkey(struct ib_device *ibdev, u8 port, u16 index,
		   u16 *pkey)
{
	pr_debug("--->\n");
	if (index > 1)
		return -EINVAL;

	*pkey = 0xffff;
	return 0;
}

struct ib_pd *efa_alloc_pd(struct ib_device *ibdev,
			   struct ib_ucontext *ibucontext,
			   struct ib_udata *udata)
{
	struct efa_ibv_alloc_pd_resp resp = {};
	struct efa_dev *dev = to_edev(ibdev);
	struct efa_pd *pd;
	int err;

	pr_debug("--->\n");

	if (!ibucontext) {
		pr_err("ibucontext is not valid\n");
		return ERR_PTR(-EOPNOTSUPP);
	}

	if (udata && udata->inlen &&
	    !ib_is_udata_cleared(udata, 0, udata->inlen)) {
		pr_err_ratelimited("Incompatible ABI params, udata not cleared\n");
		return ERR_PTR(-EINVAL);
	}

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd) {
		dev->stats.sw_stats.alloc_pd_alloc_err++;
		return ERR_PTR(-ENOMEM);
	}

	pd->pdn = efa_bitmap_alloc(&dev->pd_bitmap);
	if (pd->pdn == EFA_BITMAP_INVAL) {
		pr_err("Failed to alloc PD (max_pd %u)\n", dev->caps.max_pd);
		dev->stats.sw_stats.alloc_pd_bitmap_full_err++;
		kfree(pd);
		return ERR_PTR(-ENOMEM);
	}

	resp.pdn = pd->pdn;

	if (udata && udata->outlen) {
		err = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (err) {
			pr_err_ratelimited("failed to copy udata for alloc_pd\n");
			efa_bitmap_free(&dev->pd_bitmap, pd->pdn);
			kfree(pd);
			return ERR_PTR(err);
		}
	}

	pr_debug("Allocated pd[%d]\n", pd->pdn);

	return &pd->ibpd;
}

int efa_dealloc_pd(struct ib_pd *ibpd)
{
	struct efa_dev *dev = to_edev(ibpd->device);
	struct efa_pd *pd = to_epd(ibpd);

	pr_debug("Dealloc pd[%d]\n", pd->pdn);
	efa_bitmap_free(&dev->pd_bitmap, pd->pdn);
	kfree(pd);

	return 0;
}

int efa_destroy_qp_handle(struct efa_dev *dev, u32 qp_handle)
{
	struct efa_com_destroy_qp_params params = { .qp_handle = qp_handle };

	return efa_com_destroy_qp(dev->edev, &params);
}

int efa_destroy_qp(struct ib_qp *ibqp)
{
	struct efa_dev *dev = to_edev(ibqp->pd->device);
	struct efa_qp *qp = to_eqp(ibqp);
	struct efa_ucontext *ucontext;

	pr_debug("Destroy qp[%u]\n", ibqp->qp_num);
	ucontext = ibqp->pd->uobject ?
			to_eucontext(ibqp->pd->uobject->context) :
			NULL;

	if (!ucontext)
		return -EOPNOTSUPP;

	efa_destroy_qp_handle(dev, qp->qp_handle);
	mmap_obj_entries_remove(ucontext, qp);

	if (qp->rq_cpu_addr) {
		pr_debug("qp->cpu_addr[%p] freed: size[%lu], dma[%pad]\n",
			 qp->rq_cpu_addr, qp->rq_size,
			 &qp->rq_dma_addr);
		dma_free_coherent(&dev->pdev->dev, qp->rq_size,
				  qp->rq_cpu_addr, qp->rq_dma_addr);
	}

	kfree(qp);
	return 0;
}

static int qp_mmap_entries_setup(struct efa_qp *qp,
				 struct efa_dev *dev,
				 struct efa_ucontext *ucontext,
				 struct efa_com_create_qp_params *params,
				 struct efa_ibv_create_qp_resp *resp)
{
	struct efa_mmap_entry *rq_db_entry;
	struct efa_mmap_entry *sq_db_entry;
	struct efa_mmap_entry *rq_entry;
	struct efa_mmap_entry *sq_entry;

	sq_db_entry = kzalloc(sizeof(*sq_db_entry), GFP_KERNEL);
	sq_entry = kzalloc(sizeof(*sq_entry), GFP_KERNEL);
	if (!sq_db_entry || !sq_entry) {
		dev->stats.sw_stats.mmap_entry_alloc_err++;
		goto err_alloc;
	}

	if (qp->rq_size) {
		rq_entry = kzalloc(sizeof(*rq_entry), GFP_KERNEL);
		rq_db_entry = kzalloc(sizeof(*rq_db_entry), GFP_KERNEL);
		if (!rq_entry || !rq_db_entry) {
			dev->stats.sw_stats.mmap_entry_alloc_err++;
			goto err_alloc_rq;
		}

		rq_db_entry->obj = qp;
		rq_entry->obj    = qp;

		rq_entry->address = virt_to_phys(qp->rq_cpu_addr);
		rq_entry->length = qp->rq_size;
		mmap_entry_insert(ucontext, rq_entry, 0);
		resp->rq_mmap_key = rq_entry->key;
		resp->rq_mmap_size = qp->rq_size;

		rq_db_entry->address = dev->db_bar_addr +
				       resp->rq_db_offset;
		rq_db_entry->length = PAGE_SIZE;
		mmap_entry_insert(ucontext, rq_db_entry,
				  EFA_MMAP_DB_BAR_MEMORY_FLAG);
		resp->rq_db_mmap_key = rq_db_entry->key;
		resp->rq_db_offset &= ~PAGE_MASK;
	}

	sq_db_entry->obj = qp;
	sq_entry->obj    = qp;

	sq_db_entry->address = dev->db_bar_addr + resp->sq_db_offset;
	resp->sq_db_offset &= ~PAGE_MASK;
	sq_db_entry->length = PAGE_SIZE;
	mmap_entry_insert(ucontext, sq_db_entry, EFA_MMAP_DB_BAR_MEMORY_FLAG);
	resp->sq_db_mmap_key = sq_db_entry->key;

	sq_entry->address = dev->mem_bar_addr + resp->llq_desc_offset;
	resp->llq_desc_offset &= ~PAGE_MASK;
	sq_entry->length = PAGE_ALIGN(params->sq_ring_size_in_bytes +
				      resp->llq_desc_offset);
	mmap_entry_insert(ucontext, sq_entry, EFA_MMAP_MEM_BAR_MEMORY_FLAG);
	resp->llq_desc_mmap_key = sq_entry->key;

	return 0;

err_alloc_rq:
	kfree(rq_entry);
	kfree(rq_db_entry);
err_alloc:
	kfree(sq_entry);
	kfree(sq_db_entry);
	return -ENOMEM;
}

static int efa_qp_validate_cap(struct efa_dev *dev,
			       struct ib_qp_init_attr *init_attr)
{
	if (init_attr->cap.max_send_wr > dev->caps.max_sq_depth) {
		pr_err("qp: requested send wr[%u] exceeds the max[%u]\n",
		       init_attr->cap.max_send_wr,
		       dev->caps.max_sq_depth);
		return -EINVAL;
	}
	if (init_attr->cap.max_recv_wr > dev->caps.max_rq_depth) {
		pr_err("qp: requested receive wr[%u] exceeds the max[%u]\n",
		       init_attr->cap.max_recv_wr,
		       dev->caps.max_rq_depth);
		return -EINVAL;
	}
	if (init_attr->cap.max_send_sge > dev->caps.max_sq_sge) {
		pr_err("qp: requested sge send[%u] exceeds the max[%u]\n",
		       init_attr->cap.max_send_sge, dev->caps.max_sq_sge);
		return -EINVAL;
	}
	if (init_attr->cap.max_recv_sge > dev->caps.max_rq_sge) {
		pr_err("qp: requested sge recv[%u] exceeds the max[%u]\n",
		       init_attr->cap.max_recv_sge, dev->caps.max_rq_sge);
		return -EINVAL;
	}
	if (init_attr->cap.max_inline_data > dev->caps.inline_buf_size) {
		pr_warn("requested inline data[%u] exceeds the max[%u]\n",
			init_attr->cap.max_inline_data,
			dev->caps.inline_buf_size);
		return -EINVAL;
	}

	return 0;
}

struct ib_qp *efa_create_qp(struct ib_pd *ibpd,
			    struct ib_qp_init_attr *init_attr,
			    struct ib_udata *udata)
{
	struct efa_com_create_qp_params create_qp_params = {};
	struct efa_com_create_qp_result create_qp_resp;
	struct efa_dev *dev = to_edev(ibpd->device);
	struct efa_ibv_create_qp_resp resp = {};
	struct efa_ibv_create_qp cmd = {};
	struct efa_ucontext *ucontext;
	struct efa_qp *qp;
	int err;

	ucontext = ibpd->uobject ? to_eucontext(ibpd->uobject->context) :
				   NULL;

	err = efa_qp_validate_cap(dev, init_attr);
	if (err)
		return ERR_PTR(err);

	if (!ucontext)
		return ERR_PTR(-EOPNOTSUPP);

	if (init_attr->qp_type != IB_QPT_UD &&
	    init_attr->qp_type != IB_QPT_SRD) {
		pr_err("unsupported qp type %d\n", init_attr->qp_type);
		return ERR_PTR(-EINVAL);
	}

	if (!udata || !field_avail(cmd, srd_qp, udata->inlen)) {
		pr_err_ratelimited("Incompatible ABI params, no input udata\n");
		return ERR_PTR(-EINVAL);
	}

	if (udata->inlen > sizeof(cmd) &&
	    !ib_is_udata_cleared(udata, sizeof(cmd),
				 udata->inlen - sizeof(cmd))) {
		pr_err_ratelimited("Incompatible ABI params, unknown fields in udata\n");
		return ERR_PTR(-EINVAL);
	}

	err = ib_copy_from_udata(&cmd, udata,
				 min(sizeof(cmd), udata->inlen));
	if (err) {
		pr_err_ratelimited("%s: cannot copy udata for create_qp\n",
				   dev_name(&dev->ibdev.dev));
		return ERR_PTR(err);
	}

	if (cmd.comp_mask) {
		pr_err_ratelimited("Incompatible ABI params, unknown fields in udata\n");
		return ERR_PTR(-EINVAL);
	}

	qp = kzalloc(sizeof(*qp), GFP_KERNEL);
	if (!qp) {
		dev->stats.sw_stats.create_qp_alloc_err++;
		return ERR_PTR(-ENOMEM);
	}

	create_qp_params.pd = to_epd(ibpd)->pdn;
	if (init_attr->qp_type == IB_QPT_SRD)
		create_qp_params.qp_type = EFA_ADMIN_QP_TYPE_SRD;
	else
		create_qp_params.qp_type = EFA_ADMIN_QP_TYPE_UD;

	pr_debug("create QP, qp type %d srd qp %d\n",
		 init_attr->qp_type, cmd.srd_qp);
	create_qp_params.send_cq_idx = to_ecq(init_attr->send_cq)->cq_idx;
	create_qp_params.recv_cq_idx = to_ecq(init_attr->recv_cq)->cq_idx;
	create_qp_params.sq_depth = cmd.sq_depth;
	create_qp_params.sq_ring_size_in_bytes = cmd.sq_ring_size;

	create_qp_params.rq_ring_size_in_bytes = cmd.rq_entries *
						 cmd.rq_entry_size;
	qp->rq_size = PAGE_ALIGN(create_qp_params.rq_ring_size_in_bytes);
	if (qp->rq_size) {
		qp->rq_cpu_addr = dma_zalloc_coherent(&dev->pdev->dev,
						      qp->rq_size,
						      &qp->rq_dma_addr,
						      GFP_KERNEL);
		if (!qp->rq_cpu_addr) {
			dev->stats.sw_stats.create_qp_alloc_err++;
			err = -ENOMEM;
			goto err_free_qp;
		}
		pr_debug("qp->cpu_addr[%p] allocated: size[%lu], dma[%pad]\n",
			 qp->rq_cpu_addr, qp->rq_size, &qp->rq_dma_addr);
		create_qp_params.rq_base_addr = qp->rq_dma_addr;
	}

	memset(&resp, 0, sizeof(resp));
	err = efa_com_create_qp(dev->edev, &create_qp_params,
				&create_qp_resp);
	if (err) {
		pr_err("failed to create qp %d\n", err);
		err = -EINVAL;
		goto err_free_dma;
	}

	WARN_ON_ONCE(create_qp_resp.sq_db_offset > dev->db_bar_len);
	WARN_ON_ONCE(create_qp_resp.rq_db_offset > dev->db_bar_len);
	WARN_ON_ONCE(create_qp_resp.llq_descriptors_offset >
		     dev->mem_bar_len);

	resp.sq_db_offset = create_qp_resp.sq_db_offset;
	resp.rq_db_offset = create_qp_resp.rq_db_offset;
	resp.llq_desc_offset = create_qp_resp.llq_descriptors_offset;
	resp.send_sub_cq_idx = create_qp_resp.send_sub_cq_idx;
	resp.recv_sub_cq_idx = create_qp_resp.recv_sub_cq_idx;

	err = qp_mmap_entries_setup(qp, dev, ucontext, &create_qp_params,
				    &resp);
	if (err)
		goto err_destroy_qp;

	qp->qp_handle = create_qp_resp.qp_handle;
	qp->ibqp.qp_num = create_qp_resp.qp_num;
	qp->ibqp.qp_type = init_attr->qp_type;

	if (udata && udata->outlen) {
		err = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (err) {
			pr_err_ratelimited("failed to copy udata for qp[%u]",
					   create_qp_resp.qp_num);
			goto err_mmap_remove;
		}
	}

	pr_debug("Created qp[%d]\n", qp->ibqp.qp_num);

	return &qp->ibqp;

err_mmap_remove:
	mmap_obj_entries_remove(ucontext, qp);
err_destroy_qp:
	efa_destroy_qp_handle(dev, create_qp_resp.qp_handle);
err_free_dma:
	if (qp->rq_size) {
		pr_debug("qp->cpu_addr[%p] freed: size[%lu], dma[%pad]\n",
			 qp->rq_cpu_addr, qp->rq_size, &qp->rq_dma_addr);
		dma_free_coherent(&dev->pdev->dev, qp->rq_size,
				  qp->rq_cpu_addr, qp->rq_dma_addr);
	}
err_free_qp:
	kfree(qp);
	return ERR_PTR(err);
}

static int efa_destroy_cq_idx(struct efa_dev *dev, int cq_idx)
{
	struct efa_com_destroy_cq_params params = { .cq_idx = cq_idx };

	return efa_com_destroy_cq(dev->edev, &params);
}

int efa_destroy_cq(struct ib_cq *ibcq)
{
	struct efa_dev *dev = to_edev(ibcq->device);
	struct efa_cq *cq = to_ecq(ibcq);

	pr_debug("Destroy cq[%d] virt[%p] freed: size[%lu], dma[%pad]\n",
		 cq->cq_idx, cq->cpu_addr, cq->size, &cq->dma_addr);
	if (!cq->ucontext)
		return -EOPNOTSUPP;

	efa_destroy_cq_idx(dev, cq->cq_idx);

	mmap_obj_entries_remove(cq->ucontext, cq);
	dma_free_coherent(&dev->pdev->dev, cq->size,
			  cq->cpu_addr, cq->dma_addr);

	kfree(cq);
	return 0;
}

static int cq_mmap_entries_setup(struct efa_cq *cq,
				 struct efa_ibv_create_cq_resp *resp)
{
	struct efa_mmap_entry *cq_entry;

	cq_entry = kzalloc(sizeof(*cq_entry), GFP_KERNEL);
	if (!cq_entry)
		return -ENOMEM;

	cq_entry->obj = cq;

	cq_entry->address = virt_to_phys(cq->cpu_addr);
	cq_entry->length = cq->size;
	mmap_entry_insert(cq->ucontext, cq_entry, 0);
	resp->q_mmap_key = cq_entry->key;
	resp->q_mmap_size = cq_entry->length;

	return 0;
}

static struct ib_cq *do_create_cq(struct ib_device *ibdev, int entries,
				  int vector, struct ib_ucontext *ibucontext,
				  struct ib_udata *udata)
{
	struct efa_ibv_create_cq_resp resp = {};
	struct efa_com_create_cq_params params;
	struct efa_com_create_cq_result result;
	struct efa_dev *dev = to_edev(ibdev);
	struct efa_ibv_create_cq cmd = {};
	struct efa_cq *cq;
	int err;

	pr_debug("entries %d udata %p\n", entries, udata);

	if (entries < 1 || entries > dev->caps.max_cq_depth) {
		pr_err("cq: requested entries[%u] non-positive or greater than max[%u]\n",
		       entries, dev->caps.max_cq_depth);
		return ERR_PTR(-EINVAL);
	}

	if (!ibucontext) {
		pr_err("context is not valid ");
		return ERR_PTR(-EOPNOTSUPP);
	}

	if (!udata || !field_avail(cmd, num_sub_cqs, udata->inlen)) {
		pr_err_ratelimited("Incompatible ABI params, no input udata\n");
		return ERR_PTR(-EINVAL);
	}

	if (udata->inlen > sizeof(cmd) &&
	    !ib_is_udata_cleared(udata, sizeof(cmd),
				 udata->inlen - sizeof(cmd))) {
		pr_err_ratelimited("Incompatible ABI params, unknown fields in udata\n");
		return ERR_PTR(-EINVAL);
	}

	err = ib_copy_from_udata(&cmd, udata,
				 min(sizeof(cmd), udata->inlen));
	if (err) {
		pr_err_ratelimited("%s: cannot copy udata for create_cq\n",
				   dev_name(&dev->ibdev.dev));
		return ERR_PTR(err);
	}

	if (cmd.comp_mask || !EFA_IS_RESERVED_CLEARED(cmd.reserved_50)) {
		pr_err_ratelimited("Incompatible ABI params, unknown fields in udata\n");
		return ERR_PTR(-EINVAL);
	}

	if (!cmd.cq_entry_size) {
		pr_err("invalid entry size [%u]\n", cmd.cq_entry_size);
		return ERR_PTR(-EINVAL);
	}

	if (cmd.num_sub_cqs != dev->caps.sub_cqs_per_cq) {
		pr_err("invalid number of sub cqs[%u] expected[%u]\n",
		       cmd.num_sub_cqs, dev->caps.sub_cqs_per_cq);
		return ERR_PTR(-EINVAL);
	}

	cq = kzalloc(sizeof(*cq), GFP_KERNEL);
	if (!cq) {
		dev->stats.sw_stats.create_cq_alloc_err++;
		return ERR_PTR(-ENOMEM);
	}

	memset(&resp, 0, sizeof(resp));
	cq->ucontext = to_eucontext(ibucontext);
	cq->size = PAGE_ALIGN(cmd.cq_entry_size * entries * cmd.num_sub_cqs);
	cq->cpu_addr = dma_zalloc_coherent(&dev->pdev->dev,
					   cq->size, &cq->dma_addr,
					   GFP_KERNEL);
	if (!cq->cpu_addr) {
		dev->stats.sw_stats.create_cq_alloc_err++;
		err = -ENOMEM;
		goto err_free_cq;
	}
	pr_debug("cq->cpu_addr[%p] allocated: size[%lu], dma[%pad]\n",
		 cq->cpu_addr, cq->size, &cq->dma_addr);

	params.cq_depth = entries;
	params.dma_addr = cq->dma_addr;
	params.entry_size_in_bytes = cmd.cq_entry_size;
	params.num_sub_cqs = cmd.num_sub_cqs;
	err = efa_com_create_cq(dev->edev, &params, &result);
	if (err) {
		pr_err("failed to create cq [%d]!\n", err);
		goto err_free_dma;
	}

	resp.cq_idx = result.cq_idx;
	cq->cq_idx  = result.cq_idx;
	cq->ibcq.cqe = result.actual_depth;
	WARN_ON_ONCE(entries != result.actual_depth);

	err = cq_mmap_entries_setup(cq, &resp);
	if (err) {
		pr_err("could not setup cq[%u] mmap entries!\n", cq->cq_idx);
		goto err_destroy_cq;
	}

	if (udata && udata->outlen) {
		err = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (err) {
			pr_err_ratelimited("failed to copy udata for %s",
					   dev_name(&dev->ibdev.dev));
			goto err_mmap_remove;
		}
	}

	pr_debug("Created cq[%d], cq depth[%u]. dma[%pad] virt[%p]\n",
		 cq->cq_idx, result.actual_depth, &cq->dma_addr, cq->cpu_addr);

	return &cq->ibcq;

err_mmap_remove:
	mmap_obj_entries_remove(to_eucontext(ibucontext), cq);
err_destroy_cq:
	efa_destroy_cq_idx(dev, cq->cq_idx);
err_free_dma:
	pr_debug("cq->cpu_addr[%p] freed: size[%lu], dma[%pad]\n",
		 cq->cpu_addr, cq->size, &cq->dma_addr);
	dma_free_coherent(&dev->pdev->dev, cq->size, cq->cpu_addr,
			  cq->dma_addr);
err_free_cq:
	kfree(cq);
	return ERR_PTR(err);
}

struct ib_cq *efa_create_cq(struct ib_device *ibdev,
			    const struct ib_cq_init_attr *attr,
			    struct ib_ucontext *ibucontext,
			    struct ib_udata *udata)
{
	pr_debug("--->\n");
	return do_create_cq(ibdev, attr->cqe, attr->comp_vector, ibucontext,
			    udata);
}

static int umem_to_page_list(struct ib_umem *umem,
			     u64 *page_list,
			     u32 hp_cnt,
			     u8 hp_shift)
{
	u32 pages_in_hp = BIT(hp_shift - PAGE_SHIFT);
	unsigned int page_idx = 0;
	unsigned int hp_idx = 0;
	struct scatterlist *sg;
	unsigned int entry;

	if (umem->page_shift != PAGE_SHIFT)
		return -EINVAL;

	pr_debug("hp_cnt[%u], pages_in_hp[%u]\n", hp_cnt, pages_in_hp);

	for_each_sg(umem->sg_head.sgl, sg, umem->nmap, entry) {
		if (unlikely(sg_dma_len(sg) != PAGE_SIZE)) {
			pr_err("sg_dma_len[%u] != PAGE_SIZE[%lu]\n",
			       sg_dma_len(sg), PAGE_SIZE);
			return -EINVAL;
		}

		if (page_idx % pages_in_hp == 0) {
			page_list[hp_idx] = sg_dma_address(sg);
			hp_idx++;
		}
		page_idx++;
	}

	return 0;
}

static struct scatterlist *efa_vmalloc_buf_to_sg(u64 *buf, int page_cnt)
{
	struct scatterlist *sglist;
	struct page *pg;
	int i;

	sglist = kcalloc(page_cnt, sizeof(*sglist), GFP_KERNEL);
	if (!sglist)
		return NULL;
	sg_init_table(sglist, page_cnt);
	for (i = 0; i < page_cnt; i++) {
		pg = vmalloc_to_page(buf);
		if (!pg)
			goto err;
		WARN_ON_ONCE(PageHighMem(pg));
		sg_set_page(&sglist[i], pg, EFA_PAGE_SIZE, 0);
		buf = (u64 *)((u8 *)buf + EFA_PAGE_SIZE);
	}
	return sglist;

err:
	kfree(sglist);
	return NULL;
}

/*
 * create a chunk list of physical pages dma addresses from the supplied
 * scatter gather list
 */
static int pbl_chunk_list_create(struct pbl_context *pbl)
{
	unsigned int entry, npg_in_sg, chunk_list_size, chunk_idx, page_idx;
	struct pbl_chunk_list *chunk_list = &pbl->phys.indirect.chunk_list;
	int page_cnt = pbl->phys.indirect.pbl_buf_size_in_pages;
	struct scatterlist *pages_sgl = pbl->phys.indirect.sgl;
	int sg_dma_cnt = pbl->phys.indirect.sg_dma_cnt;
	struct efa_com_ctrl_buff_info *ctrl_buf;
	u64 *cur_chunk_buf, *prev_chunk_buf;
	struct scatterlist *sg;
	dma_addr_t dma_addr;
	int i;

	/* allocate a chunk list that consists of 4KB chunks */
	chunk_list_size = DIV_ROUND_UP(page_cnt, EFA_PAGE_PTRS_PER_CHUNK);

	chunk_list->size = chunk_list_size;
	chunk_list->chunks = kcalloc(chunk_list_size,
				     sizeof(*chunk_list->chunks),
				     GFP_KERNEL);
	if (!chunk_list->chunks)
		return -ENOMEM;

	pr_debug("chunk_list_size[%u] - pages[%u]\n", chunk_list_size,
		 page_cnt);

	/* allocate chunk buffers: */
	for (i = 0; i < chunk_list_size; i++) {
		chunk_list->chunks[i].buf = kzalloc(EFA_CHUNK_ALLOC_SIZE,
						    GFP_KERNEL);
		if (!chunk_list->chunks[i].buf)
			goto chunk_list_dealloc;

		chunk_list->chunks[i].length = EFA_CHUNK_USED_SIZE;
	}
	chunk_list->chunks[chunk_list_size - 1].length =
		((page_cnt % EFA_PAGE_PTRS_PER_CHUNK) * EFA_PAGE_PTR_SIZE) +
			EFA_CHUNK_PTR_SIZE;

	/* fill the dma addresses of sg list pages to chunks: */
	chunk_idx = 0;
	page_idx  = 0;
	cur_chunk_buf = chunk_list->chunks[0].buf;
	for_each_sg(pages_sgl, sg, sg_dma_cnt, entry) {
		npg_in_sg = sg_dma_len(sg) >> EFA_PAGE_SHIFT;
		for (i = 0; i < npg_in_sg; i++) {
			cur_chunk_buf[page_idx++] = sg_dma_address(sg) +
						    (EFA_PAGE_SIZE * i);

			if (page_idx == EFA_PAGE_PTRS_PER_CHUNK) {
				chunk_idx++;
				cur_chunk_buf = chunk_list->chunks[chunk_idx].buf;
				page_idx = 0;
			}
		}
	}

	/* map chunks to dma and fill chunks next ptrs */
	for (i = chunk_list_size - 1; i >= 0; i--) {
		dma_addr = dma_map_single(pbl->dmadev,
					  chunk_list->chunks[i].buf,
					  chunk_list->chunks[i].length,
					  DMA_TO_DEVICE);
		if (dma_mapping_error(pbl->dmadev, dma_addr)) {
			pr_err("chunk[%u] dma_map_failed\n", i);
			goto chunk_list_unmap;
		}

		chunk_list->chunks[i].dma_addr = dma_addr;
		pr_debug("chunk[%u] mapped at [%pad]\n", i, &dma_addr);

		if (!i)
			break;

		prev_chunk_buf = chunk_list->chunks[i - 1].buf;

		ctrl_buf = (struct efa_com_ctrl_buff_info *)
				&prev_chunk_buf[EFA_PAGE_PTRS_PER_CHUNK];
		ctrl_buf->length = chunk_list->chunks[i].length;

		efa_com_set_dma_addr(dma_addr,
				     &ctrl_buf->address.mem_addr_high,
				     &ctrl_buf->address.mem_addr_low);
	}

	return 0;

chunk_list_unmap:
	for (; i < chunk_list_size; i++) {
		dma_unmap_single(pbl->dmadev, chunk_list->chunks[i].dma_addr,
				 chunk_list->chunks[i].length, DMA_TO_DEVICE);
	}
chunk_list_dealloc:
	for (i = 0; i < chunk_list_size; i++)
		kfree(chunk_list->chunks[i].buf);

	kfree(chunk_list->chunks);
	return -ENOMEM;
}

static void pbl_chunk_list_destroy(struct pbl_context *pbl)
{
	struct pbl_chunk_list *chunk_list = &pbl->phys.indirect.chunk_list;
	int i;

	for (i = 0; i < chunk_list->size; i++) {
		dma_unmap_single(pbl->dmadev, chunk_list->chunks[i].dma_addr,
				 chunk_list->chunks[i].length, DMA_TO_DEVICE);
		kfree(chunk_list->chunks[i].buf);
	}

	kfree(chunk_list->chunks);
}

/* initialize pbl continuous mode: map pbl buffer to a dma address. */
static int pbl_continuous_initialize(struct pbl_context *pbl)
{
	dma_addr_t dma_addr;

	dma_addr = dma_map_single(pbl->dmadev, pbl->pbl_buf,
				  pbl->pbl_buf_size_in_bytes, DMA_TO_DEVICE);
	if (dma_mapping_error(pbl->dmadev, dma_addr)) {
		pr_err("Unable to map pbl to DMA address");
		return -ENOMEM;
	}

	pbl->phys.continuous.dma_addr = dma_addr;
	pr_debug("pbl continuous - dma_addr = %pad, size[%u]\n",
		 &dma_addr, pbl->pbl_buf_size_in_bytes);

	return 0;
}

/*
 * initialize pbl indirect mode:
 * create a chunk list out of the dma addresses of the physical pages of
 * pbl buffer.
 */
static int pbl_indirect_initialize(struct pbl_context *pbl)
{
	u32 size_in_pages = DIV_ROUND_UP(pbl->pbl_buf_size_in_bytes,
					 EFA_PAGE_SIZE);
	struct scatterlist *sgl;
	int sg_dma_cnt, err;

	sgl = efa_vmalloc_buf_to_sg(pbl->pbl_buf, size_in_pages);
	if (!sgl)
		return -ENOMEM;

	sg_dma_cnt = dma_map_sg(pbl->dmadev, sgl, size_in_pages, DMA_TO_DEVICE);
	if (!sg_dma_cnt) {
		err = -EINVAL;
		goto err_map;
	}

	pbl->phys.indirect.pbl_buf_size_in_pages = size_in_pages;
	pbl->phys.indirect.sgl = sgl;
	pbl->phys.indirect.sg_dma_cnt = sg_dma_cnt;
	err = pbl_chunk_list_create(pbl);
	if (err) {
		pr_err("chunk_list creation failed[%d]!\n", err);
		goto err_chunk;
	}

	pr_debug("pbl indirect - size[%u], chunks[%u]\n",
		 pbl->pbl_buf_size_in_bytes,
		 pbl->phys.indirect.chunk_list.size);

	return 0;

err_chunk:
	dma_unmap_sg(pbl->dmadev, sgl, size_in_pages, DMA_TO_DEVICE);
err_map:
	kfree(sgl);
	return err;
}

static void pbl_indirect_terminate(struct pbl_context *pbl)
{
	pbl_chunk_list_destroy(pbl);
	dma_unmap_sg(pbl->dmadev, pbl->phys.indirect.sgl,
		     pbl->phys.indirect.pbl_buf_size_in_pages, DMA_TO_DEVICE);
	kfree(pbl->phys.indirect.sgl);
}

/* create a page buffer list from a mapped user memory region */
static int pbl_create(struct pbl_context *pbl,
		      struct efa_dev *dev,
		      struct ib_umem *umem,
		      int hp_cnt,
		      u8 hp_shift)
{
	int err;

	pbl->dev = dev;
	pbl->dmadev = &dev->pdev->dev;
	pbl->pbl_buf_size_in_bytes = hp_cnt * EFA_PAGE_PTR_SIZE;
	pbl->pbl_buf = kzalloc(pbl->pbl_buf_size_in_bytes,
			       GFP_KERNEL | __GFP_NOWARN);
	if (pbl->pbl_buf) {
		pbl->physically_continuous = true;
		err = umem_to_page_list(umem, pbl->pbl_buf, hp_cnt, hp_shift);
		if (err)
			goto err_continuous;
		err = pbl_continuous_initialize(pbl);
		if (err)
			goto err_continuous;
	} else {
		pbl->physically_continuous = false;
		pbl->pbl_buf = vzalloc(pbl->pbl_buf_size_in_bytes);
		if (!pbl->pbl_buf)
			return -ENOMEM;

		err = umem_to_page_list(umem, pbl->pbl_buf, hp_cnt, hp_shift);
		if (err)
			goto err_indirect;
		err = pbl_indirect_initialize(pbl);
		if (err)
			goto err_indirect;
	}

	pr_debug("user_pbl_created: user_pages[%u], continuous[%u]\n",
		 hp_cnt, pbl->physically_continuous);

	return 0;

err_continuous:
	kfree(pbl->pbl_buf);
	return err;
err_indirect:
	vfree(pbl->pbl_buf);
	return err;
}

static void pbl_destroy(struct pbl_context *pbl)
{
	if (pbl->physically_continuous) {
		dma_unmap_single(pbl->dmadev, pbl->phys.continuous.dma_addr,
				 pbl->pbl_buf_size_in_bytes, DMA_TO_DEVICE);
		kfree(pbl->pbl_buf);
	} else {
		pbl_indirect_terminate(pbl);
		vfree(pbl->pbl_buf);
	}
}

static int efa_create_inline_pbl(struct efa_mr *mr,
				 struct efa_com_reg_mr_params *params)
{
	int err;

	params->inline_pbl = true;
	err = umem_to_page_list(mr->umem, params->pbl.inline_pbl_array,
				params->page_num, params->page_shift);
	if (err) {
		pr_err("failed to create inline pbl[%d]\n", err);
		return err;
	}

	pr_debug("inline_pbl_array - pages[%u]\n", params->page_num);

	return 0;
}

static int efa_create_pbl(struct efa_dev *dev,
			  struct pbl_context *pbl,
			  struct efa_mr *mr,
			  struct efa_com_reg_mr_params *params)
{
	int err;

	err = pbl_create(pbl, dev, mr->umem, params->page_num,
			 params->page_shift);
	if (err) {
		pr_err("failed to create pbl[%d]\n", err);
		return err;
	}

	params->inline_pbl = false;
	params->indirect = !pbl->physically_continuous;
	if (pbl->physically_continuous) {
		params->pbl.pbl.length = pbl->pbl_buf_size_in_bytes;

		efa_com_set_dma_addr(pbl->phys.continuous.dma_addr,
				     &params->pbl.pbl.address.mem_addr_high,
				     &params->pbl.pbl.address.mem_addr_low);
	} else {
		params->pbl.pbl.length =
			pbl->phys.indirect.chunk_list.chunks[0].length;

		efa_com_set_dma_addr(pbl->phys.indirect.chunk_list.chunks[0].dma_addr,
				     &params->pbl.pbl.address.mem_addr_high,
				     &params->pbl.pbl.address.mem_addr_low);
	}

	return 0;
}

static void efa_cont_pages(struct ib_umem *umem, u64 addr,
			   unsigned long max_page_shift,
			   int *count, u8 *shift, u32 *ncont)
{
	unsigned long page_shift = umem->page_shift;
	struct scatterlist *sg;
	u64 base = ~0, p = 0;
	unsigned long tmp;
	unsigned long m;
	u64 len, pfn;
	int i = 0;
	int entry;

	addr = addr >> page_shift;
	tmp = (unsigned long)addr;
	m = find_first_bit(&tmp, BITS_PER_LONG);
	if (max_page_shift)
		m = min_t(unsigned long, max_page_shift - page_shift, m);

	for_each_sg(umem->sg_head.sgl, sg, umem->nmap, entry) {
		len = sg_dma_len(sg) >> page_shift;
		pfn = sg_dma_address(sg) >> page_shift;
		if (base + p != pfn) {
			/*
			 * If either the offset or the new
			 * base are unaligned update m
			 */
			tmp = (unsigned long)(pfn | p);
			if (!IS_ALIGNED(tmp, 1 << m))
				m = find_first_bit(&tmp, BITS_PER_LONG);

			base = pfn;
			p = 0;
		}

		p += len;
		i += len;
	}

	if (i) {
		m = min_t(unsigned long, ilog2(roundup_pow_of_two(i)), m);
		*ncont = DIV_ROUND_UP(i, (1 << m));
	} else {
		m = 0;
		*ncont = 0;
	}

	*shift = page_shift + m;
	*count = i;
}

struct ib_mr *efa_reg_mr(struct ib_pd *ibpd, u64 start, u64 length,
			 u64 virt_addr, int access_flags,
			 struct ib_udata *udata)
{
	struct efa_dev *dev = to_edev(ibpd->device);
	struct efa_com_reg_mr_params params = {};
	struct efa_com_reg_mr_result result = {};
	struct pbl_context pbl;
	struct efa_mr *mr;
	int inline_size;
	int npages;
	int err;

	if (udata && udata->inlen &&
	    !ib_is_udata_cleared(udata, 0, sizeof(udata->inlen))) {
		pr_err_ratelimited("Incompatible ABI params, udata not cleared\n");
		return ERR_PTR(-EINVAL);
	}

	if (access_flags & ~EFA_SUPPORTED_ACCESS_FLAGS) {
		pr_err("Unsupported access flags[%#x], supported[%#x]\n",
		       access_flags, EFA_SUPPORTED_ACCESS_FLAGS);
		return ERR_PTR(-EOPNOTSUPP);
	}

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr) {
		dev->stats.sw_stats.reg_mr_alloc_err++;
		return ERR_PTR(-ENOMEM);
	}

	mr->umem = ib_umem_get(ibpd->uobject->context, start, length,
			       access_flags, 0);
	if (IS_ERR(mr->umem)) {
		err = PTR_ERR(mr->umem);
		pr_err("failed to pin and map user space memory[%d]\n", err);
		goto err;
	}

	params.pd = to_epd(ibpd)->pdn;
	params.iova = virt_addr;
	params.mr_length_in_bytes = length;
	params.permissions = access_flags & 0x1;

	efa_cont_pages(mr->umem, start,
		       EFA_ADMIN_REG_MR_CMD_PHYS_PAGE_SIZE_SHIFT_MASK, &npages,
		       &params.page_shift,  &params.page_num);
	pr_debug("start %#llx length %#llx npages %d params.page_shift %u params.page_num %u\n",
		 start, length, npages, params.page_shift, params.page_num);

	inline_size = ARRAY_SIZE(params.pbl.inline_pbl_array);
	if (params.page_num <= inline_size) {
		err = efa_create_inline_pbl(mr, &params);
		if (err)
			goto err_unmap;

		err = efa_com_register_mr(dev->edev, &params, &result);
		if (err) {
			pr_err("efa_com_register_mr failed - %d!\n", err);
			goto err_unmap;
		}
	} else {
		err = efa_create_pbl(dev, &pbl, mr, &params);
		if (err)
			goto err_unmap;

		err = efa_com_register_mr(dev->edev, &params, &result);
		pbl_destroy(&pbl);

		if (err) {
			pr_err("efa_com_register_mr failed - %d!\n", err);
			goto err_unmap;
		}
	}

	mr->vaddr = virt_addr;
	mr->ibmr.lkey = result.l_key;
	mr->ibmr.rkey = result.r_key;
	mr->ibmr.length = length;
	pr_debug("Registered mr[%d]\n", mr->ibmr.lkey);

	return &mr->ibmr;

err_unmap:
	ib_umem_release(mr->umem);
err:
	kfree(mr);
	return ERR_PTR(err);
}

int efa_dereg_mr(struct ib_mr *ibmr)
{
	struct efa_dev *dev = to_edev(ibmr->device);
	struct efa_com_dereg_mr_params params;
	struct efa_mr *mr = to_emr(ibmr);

	pr_debug("Deregister mr[%d]\n", ibmr->lkey);

	if (mr->umem) {
		params.l_key = mr->ibmr.lkey;
		efa_com_dereg_mr(dev->edev, &params);
		ib_umem_release(mr->umem);
	}

	kfree(mr);

	return 0;
}

int efa_get_port_immutable(struct ib_device *ibdev, u8 port_num,
			   struct ib_port_immutable *immutable)
{
	pr_debug("--->\n");
	immutable->core_cap_flags = RDMA_CORE_CAP_PROT_EFA;
	immutable->gid_tbl_len    = 1;

	return 0;
}

struct ib_ucontext *efa_alloc_ucontext(struct ib_device *ibdev,
				       struct ib_udata *udata)
{
	struct efa_ibv_alloc_ucontext_resp resp = {};
	struct efa_dev *dev = to_edev(ibdev);
	struct efa_ucontext *ucontext;
	int err;

	pr_debug("--->\n");
	/*
	 * it's fine if the driver does not know all request fields,
	 * we will ack input fields in our response.
	 */

	ucontext = kzalloc(sizeof(*ucontext), GFP_KERNEL);
	if (!ucontext) {
		dev->stats.sw_stats.alloc_ucontext_alloc_err++;
		return ERR_PTR(-ENOMEM);
	}

	mutex_init(&ucontext->lock);
	INIT_LIST_HEAD(&ucontext->pending_mmaps);

	mutex_lock(&dev->efa_dev_lock);

	resp.cmds_supp_udata_mask |= EFA_USER_CMDS_SUPP_UDATA_QUERY_DEVICE;
	resp.cmds_supp_udata_mask |= EFA_USER_CMDS_SUPP_UDATA_CREATE_AH;
	resp.kernel_supp_mask |= EFA_KERNEL_SUPP_QPT_SRD;

	if (udata && udata->outlen) {
		err = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (err)
			goto err_resp;
	}

	list_add_tail(&ucontext->link, &dev->ctx_list);
	mutex_unlock(&dev->efa_dev_lock);
	return &ucontext->ibucontext;

err_resp:
	mutex_unlock(&dev->efa_dev_lock);
	kfree(ucontext);
	return ERR_PTR(err);
}

int efa_dealloc_ucontext(struct ib_ucontext *ibucontext)
{
	struct efa_ucontext *ucontext = to_eucontext(ibucontext);
	struct efa_dev *dev = to_edev(ibucontext->device);

	pr_debug("--->\n");

	WARN_ON(!list_empty(&ucontext->pending_mmaps));

	mutex_lock(&dev->efa_dev_lock);
	list_del(&ucontext->link);
	mutex_unlock(&dev->efa_dev_lock);
	kfree(ucontext);
	return 0;
}

static void mmap_obj_entries_remove(struct efa_ucontext *ucontext, void *obj)
{
	struct efa_mmap_entry *entry, *tmp;

	pr_debug("--->\n");

	mutex_lock(&ucontext->lock);
	list_for_each_entry_safe(entry, tmp, &ucontext->pending_mmaps, list) {
		if (entry->obj == obj) {
			list_del(&entry->list);
			pr_debug("mmap: obj[%p] key[0x%llx] addr[0x%llX] len[0x%llX] removed\n",
				 entry->obj, entry->key, entry->address,
				 entry->length);
			kfree(entry);
		}
	}
	mutex_unlock(&ucontext->lock);
}

static struct efa_mmap_entry *mmap_entry_remove(struct efa_ucontext *ucontext,
						u64 key,
						u64 len)
{
	struct efa_mmap_entry *entry, *tmp;

	mutex_lock(&ucontext->lock);
	list_for_each_entry_safe(entry, tmp, &ucontext->pending_mmaps, list) {
		if (entry->key == key && entry->length == len) {
			list_del_init(&entry->list);
			pr_debug("mmap: obj[%p] key[0x%llx] addr[0x%llX] len[0x%llX] removed\n",
				 entry->obj, key, entry->address,
				 entry->length);
			mutex_unlock(&ucontext->lock);
			return entry;
		}
	}
	mutex_unlock(&ucontext->lock);

	return NULL;
}

static void mmap_entry_insert(struct efa_ucontext *ucontext,
			      struct efa_mmap_entry *entry,
			      u64 mem_flag)
{
	mutex_lock(&ucontext->lock);
	entry->key = ucontext->mmap_key | mem_flag;
	ucontext->mmap_key += PAGE_SIZE;
	list_add_tail(&entry->list, &ucontext->pending_mmaps);
	pr_debug("mmap: obj[%p] addr[0x%llx], len[0x%llx], key[0x%llx] inserted\n",
		 entry->obj, entry->address, entry->length, entry->key);
	mutex_unlock(&ucontext->lock);
}

static int __efa_mmap(struct efa_dev *dev,
		      struct vm_area_struct *vma,
		      u64 mmap_flag,
		      u64 address,
		      u64 length)
{
	u64 pfn = address >> PAGE_SHIFT;
	int err;

	switch (mmap_flag) {
	case EFA_MMAP_REG_BAR_MEMORY_FLAG:
		pr_debug("mapping address[0x%llX], length[0x%llX] on register BAR!",
			 address, length);
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		err = io_remap_pfn_range(vma, vma->vm_start, pfn, length,
					 vma->vm_page_prot);
		break;
	case EFA_MMAP_MEM_BAR_MEMORY_FLAG:
		pr_debug("mapping address 0x%llX, length[0x%llX] on memory BAR!",
			 address, length);
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		err = io_remap_pfn_range(vma, vma->vm_start, pfn, length,
					 vma->vm_page_prot);
		break;
	case EFA_MMAP_DB_BAR_MEMORY_FLAG:
		pr_debug("mapping address 0x%llX, length[0x%llX] on DB BAR!",
			 address, length);
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		err = io_remap_pfn_range(vma, vma->vm_start, pfn, length,
					 vma->vm_page_prot);
		break;
	default:
		pr_debug("mapping address[0x%llX], length[0x%llX] of dma buffer!\n",
			 address, length);
		err = remap_pfn_range(vma, vma->vm_start, pfn, length,
				      vma->vm_page_prot);
	}

	return err;
}

int efa_mmap(struct ib_ucontext *ibucontext,
	     struct vm_area_struct *vma)
{
	struct efa_ucontext *ucontext = to_eucontext(ibucontext);
	struct efa_dev *dev = to_edev(ibucontext->device);
	u64 length = vma->vm_end - vma->vm_start;
	u64 key = vma->vm_pgoff << PAGE_SHIFT;
	struct efa_mmap_entry *entry;
	u64 mmap_flag;
	u64 address;

	pr_debug("start 0x%lx, end 0x%lx, length = 0x%llx, key = 0x%llx\n",
		 vma->vm_start, vma->vm_end, length, key);

	if (length % PAGE_SIZE != 0) {
		pr_err("length[0x%llX] is not page size aligned[0x%lX]!",
		       length, PAGE_SIZE);
		return -EINVAL;
	}

	entry = mmap_entry_remove(ucontext, key, length);
	if (!entry) {
		pr_err("key[0x%llX] does not have valid entry!", key);
		return -EINVAL;
	}
	address = entry->address;
	kfree(entry);

	mmap_flag = key & EFA_MMAP_BARS_MEMORY_MASK;
	return __efa_mmap(dev, vma, mmap_flag, address, length);
}

static inline bool efa_ah_id_equal(u8 *id1, u8 *id2)
{
	return !memcmp(id1, id2, EFA_GID_SIZE);
}

static int efa_get_ah_by_id(struct efa_dev *dev, u8 *id,
			    u16 *ah_res, bool ref_update)
{
	struct efa_ah_id *ah_id;

	list_for_each_entry(ah_id, &dev->efa_ah_list, list) {
		if (efa_ah_id_equal(ah_id->id, id)) {
			*ah_res =  ah_id->address_handle;
			if (ref_update)
				ah_id->ref_count++;
			return 0;
		}
	}

	return -EINVAL;
}

static int efa_add_ah_id(struct efa_dev *dev, u8 *id,
			 u16 address_handle)
{
	struct efa_ah_id *ah_id;

	ah_id = kzalloc(sizeof(*ah_id), GFP_KERNEL);
	if (!ah_id)
		return -ENOMEM;

	memcpy(ah_id->id, id, sizeof(ah_id->id));
	ah_id->address_handle = address_handle;
	ah_id->ref_count = 1;
	list_add_tail(&ah_id->list, &dev->efa_ah_list);

	return 0;
}

static void efa_remove_ah_id(struct efa_dev *dev, u8 *id, u32 *ref_count)
{
	struct efa_ah_id *ah_id, *tmp;

	list_for_each_entry_safe(ah_id, tmp, &dev->efa_ah_list, list) {
		if (efa_ah_id_equal(ah_id->id, id)) {
			*ref_count = --ah_id->ref_count;
			if (ah_id->ref_count == 0) {
				list_del(&ah_id->list);
				kfree(ah_id);
				return;
			}
		}
	}
}

static void ah_destroy_on_device(struct efa_dev *dev, u16 device_ah)
{
	struct efa_com_destroy_ah_params params;
	int err;

	params.ah = device_ah;
	err = efa_com_destroy_ah(dev->edev, &params);
	if (err)
		pr_err("efa_com_destroy_ah failed (%d)\n", err);
}

static int efa_create_ah_id(struct efa_dev *dev, u8 *id,
			    u16 *efa_address_handle)
{
	struct efa_com_create_ah_params params = {};
	struct efa_com_create_ah_result result = {};
	int err;

	mutex_lock(&dev->ah_list_lock);
	err = efa_get_ah_by_id(dev, id, efa_address_handle, true);
	if (err) {
		memcpy(params.dest_addr, id, sizeof(params.dest_addr));
		err = efa_com_create_ah(dev->edev, &params, &result);
		if (err) {
			pr_err("efa_com_create_ah failed %d\n", err);
			goto err_unlock;
		}

		pr_debug("create address handle %u for address %pI6\n",
			 result.ah, params.dest_addr);

		err = efa_add_ah_id(dev, id, result.ah);
		if (err) {
			pr_err("efa_add_ah_id failed %d\n", err);
			goto err_destroy_ah;
		}

		*efa_address_handle = result.ah;
	}
	mutex_unlock(&dev->ah_list_lock);

	return 0;

err_destroy_ah:
	ah_destroy_on_device(dev, result.ah);
err_unlock:
	mutex_unlock(&dev->ah_list_lock);
	return err;
}

static void efa_destroy_ah_id(struct efa_dev *dev, u8 *id)
{
	u16 device_ah;
	u32 ref_count;
	int err;

	mutex_lock(&dev->ah_list_lock);
	err = efa_get_ah_by_id(dev, id, &device_ah, false);
	if (err) {
		WARN_ON(1);
		goto out_unlock;
	}

	efa_remove_ah_id(dev, id, &ref_count);
	if (!ref_count)
		ah_destroy_on_device(dev, device_ah);

out_unlock:
	mutex_unlock(&dev->ah_list_lock);
}

struct ib_ah *efa_create_ah(struct ib_pd *ibpd,
			    struct rdma_ah_attr *ah_attr,
			    struct ib_udata *udata)
{
	struct efa_dev *dev = to_edev(ibpd->device);
	struct efa_ibv_create_ah_resp resp = {};
	u16 efa_address_handle;
	struct efa_ah *ah;
	int err;

	pr_debug("--->\n");

	if (udata && udata->inlen &&
	    !ib_is_udata_cleared(udata, 0, udata->inlen)) {
		pr_err_ratelimited("Incompatiable ABI params\n");
		return ERR_PTR(-EINVAL);
	}

	ah = kzalloc(sizeof(*ah), GFP_KERNEL);
	if (!ah) {
		dev->stats.sw_stats.create_ah_alloc_err++;
		return ERR_PTR(-ENOMEM);
	}

	err = efa_create_ah_id(dev, ah_attr->grh.dgid.raw, &efa_address_handle);
	if (err)
		goto err_free;

	resp.efa_address_handle = efa_address_handle;

	if (udata && udata->outlen) {
		err = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (err) {
			pr_err_ratelimited("failed to copy udata for create_ah response\n");
			goto err_destroy_ah;
		}
	}

	memcpy(ah->id, ah_attr->grh.dgid.raw, sizeof(ah->id));
	return &ah->ibah;

err_destroy_ah:
	efa_destroy_ah_id(dev, ah_attr->grh.dgid.raw);
err_free:
	kfree(ah);
	return ERR_PTR(err);
}

int efa_destroy_ah(struct ib_ah *ibah)
{
	struct efa_dev *dev = to_edev(ibah->pd->device);
	struct efa_ah *ah = to_eah(ibah);

	pr_debug("--->\n");
	efa_destroy_ah_id(dev, ah->id);

	kfree(ah);
	return 0;
}

/* In ib callbacks section -  Start of stub funcs */
int efa_post_send(struct ib_qp *ibqp,
		  const struct ib_send_wr *wr,
		  const struct ib_send_wr **bad_wr)
{
	pr_warn("Function not supported\n");
	return -EOPNOTSUPP;
}

int efa_post_recv(struct ib_qp *ibqp,
		  const struct ib_recv_wr *wr,
		  const struct ib_recv_wr **bad_wr)
{
	pr_warn("Function not supported\n");
	return -EOPNOTSUPP;
}

int efa_poll_cq(struct ib_cq *ibcq, int num_entries,
		struct ib_wc *wc)
{
	pr_warn("Function not supported\n");
	return -EOPNOTSUPP;
}

int efa_req_notify_cq(struct ib_cq *ibcq,
		      enum ib_cq_notify_flags flags)
{
	pr_warn("Function not supported\n");
	return -EOPNOTSUPP;
}

struct ib_mr *efa_get_dma_mr(struct ib_pd *ibpd, int acc)
{
	pr_warn("Function not supported\n");
	return ERR_PTR(-EOPNOTSUPP);
}

int efa_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
		  int attr_mask, struct ib_udata *udata)
{
	pr_warn("Function not supported\n");
	return -EOPNOTSUPP;
}

enum rdma_link_layer efa_port_link_layer(struct ib_device *ibdev,
					 u8 port_num)
{
	pr_debug("--->\n");
	return IB_LINK_LAYER_ETHERNET;
}

