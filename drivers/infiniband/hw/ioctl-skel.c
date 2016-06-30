/* Vendor demo */

#include <rdma/uverbs_ioctl_cmd.h>

/*
 * Vendor could have its own object model as well
 * enum {
 *	MLX5_VENDOR_TYPE_OBJ
 * };
 */

struct mlx5_ib_create_qp_vendor_cmd {
	__u64	buf_addr;
	__u64	db_addr;
	__u32	sq_wqe_count;
	__u32	rq_wqe_count;
	__u32	rq_wqe_shift;
	__u32	flags;
	__u32	uidx;
	__u32	reserved0;
	__u64	sq_buf_addr;
};

struct mlx5_ib_create_qp_vendor_resp {
	__u32	uuar_index;
};

struct mlx5_ib_create_cq_vendor_cmd {
	__u64	buf_addr;
	__u64	db_addr;
	__u32	cqe_size;
	__u32	reserved;
};

struct mlx5_ib_create_cq_vendor_resp {
	__u32	cqn;
	__u32	reserved;
};

/* Could be per vendor handler */
static int create_qp_handler(struct ib_device *ib_dev,
			     struct ib_ucontext *ucontext,
			     struct uverbs_attr_array *common,
			     struct uverbs_attr_array *vendor,
			     void *priv)
{
	/* Some smart things here */
	return 0;
}

/* Could be per vendor handler */
static int create_cq_handler(struct ib_device *ib_dev,
			     struct ib_ucontext *ucontext,
			     struct uverbs_attr_array *common,
			     struct uverbs_attr_array *vendor,
			     void *priv)
{
	/* Some smart things here */
	return 0;
}

enum mlx5_qp_commands {
	MLX5_QP_COMMAND_CREATE,
	MLX5_QP_COMMAND_DESTROY
	/* TODO: Other commands */
};

enum mlx5_cq_commands {
	MLX5_CQ_COMMAND_CREATE,
};

enum mlx5_qp_create {
	MLX5_CREATE_QP_VENDOR_CMD = IB_UVERBS_VENDOR_FLAG,
	MLX5_CREATE_QP_VENDOR_RESP,
};

DECLARE_UVERBS_TYPE(mlx5_ib_qp,
	UVERBS_ACTION(
		MLX5_QP_COMMAND_CREATE, create_qp_handler, NULL, &uverbs_create_qp_spec,
		&UVERBS_ATTR_CHAIN_SPEC(
			UVERBS_ATTR_PTR_IN(MLX5_CREATE_QP_VENDOR_CMD,
					   sizeof(struct mlx5_ib_create_qp_vendor_cmd)),
			UVERBS_ATTR_PTR_OUT(MLX5_CREATE_QP_VENDOR_RESP,
					    sizeof(struct mlx5_ib_create_qp_vendor_resp)),
			/*
			 * User could have its own objects and IDRs
			 * UVERBS_ATTR_IDR_IN(MLX_CREATE_QP_VENDOR_OBJ,
			 *		   UVERBS_COMMON_TYPE_QP, 0)
			 */
			),
	),
	UVERBS_ACTION(MLX5_QP_COMMAND_DESTROY, uverbs_destroy_qp_handler, NULL,
		      &uverbs_destroy_qp_spec),
);

enum mlx5_cq_create {
	MLX5_CREATE_CQ_VENDOR_CMD = IB_UVERBS_VENDOR_FLAG,
	MLX5_CREATE_CQ_VENDOR_RESP,
};

struct uverbs_types objects = UVERBS_TYPES(
	/* Decalre types by pointer */
	UVERBS_TYPE(UVERBS_TYPE_QP, mlx5_ib_qp),
	/* Types could only declared in-lined */
	UVERBS_TYPE_ACTIONS(
		UVERBS_TYPE_CQ,
		UVERBS_ACTION(
			MLX5_CQ_COMMAND_CREATE, create_cq_handler, NULL,
			&uverbs_create_cq_spec,
			&UVERBS_ATTR_CHAIN_SPEC(
				UVERBS_ATTR_PTR_IN(MLX5_CREATE_CQ_VENDOR_CMD,
						   sizeof(struct mlx5_ib_create_cq_vendor_cmd)),
				UVERBS_ATTR_PTR_OUT(MLX5_CREATE_QP_VENDOR_RESP,
						    sizeof(struct mlx5_ib_create_cq_vendor_resp)),
			),
		),
	)
);

