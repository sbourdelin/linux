/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2014-2018 NXP */

#ifndef _FSL_DPDMAI_CMD_H
#define _FSL_DPDMAI_CMD_H

/* DPDMAI Version */
#define DPDMAI_VER_MAJOR		2
#define DPDMAI_VER_MINOR		2

#define DPDMAI_CMD_BASE_VERSION		0
#define DPDMAI_CMD_ID_OFFSET		4

#define DPDMAI_CMDID_FORMAT(x)		(((x) << DPDMAI_CMD_ID_OFFSET) | \
					DPDMAI_CMD_BASE_VERSION)

/* Command IDs */
#define DPDMAI_CMDID_CLOSE		DPDMAI_CMDID_FORMAT(0x800)
#define DPDMAI_CMDID_OPEN               DPDMAI_CMDID_FORMAT(0x80E)
#define DPDMAI_CMDID_CREATE             DPDMAI_CMDID_FORMAT(0x90E)
#define DPDMAI_CMDID_DESTROY            DPDMAI_CMDID_FORMAT(0x900)

#define DPDMAI_CMDID_ENABLE             DPDMAI_CMDID_FORMAT(0x002)
#define DPDMAI_CMDID_DISABLE            DPDMAI_CMDID_FORMAT(0x003)
#define DPDMAI_CMDID_GET_ATTR           DPDMAI_CMDID_FORMAT(0x004)
#define DPDMAI_CMDID_RESET              DPDMAI_CMDID_FORMAT(0x005)
#define DPDMAI_CMDID_IS_ENABLED         DPDMAI_CMDID_FORMAT(0x006)

#define DPDMAI_CMDID_SET_IRQ            DPDMAI_CMDID_FORMAT(0x010)
#define DPDMAI_CMDID_GET_IRQ            DPDMAI_CMDID_FORMAT(0x011)
#define DPDMAI_CMDID_SET_IRQ_ENABLE     DPDMAI_CMDID_FORMAT(0x012)
#define DPDMAI_CMDID_GET_IRQ_ENABLE     DPDMAI_CMDID_FORMAT(0x013)
#define DPDMAI_CMDID_SET_IRQ_MASK       DPDMAI_CMDID_FORMAT(0x014)
#define DPDMAI_CMDID_GET_IRQ_MASK       DPDMAI_CMDID_FORMAT(0x015)
#define DPDMAI_CMDID_GET_IRQ_STATUS     DPDMAI_CMDID_FORMAT(0x016)
#define DPDMAI_CMDID_CLEAR_IRQ_STATUS	DPDMAI_CMDID_FORMAT(0x017)

#define DPDMAI_CMDID_SET_RX_QUEUE	DPDMAI_CMDID_FORMAT(0x1A0)
#define DPDMAI_CMDID_GET_RX_QUEUE       DPDMAI_CMDID_FORMAT(0x1A1)
#define DPDMAI_CMDID_GET_TX_QUEUE       DPDMAI_CMDID_FORMAT(0x1A2)

#define MC_CMD_HDR_TOKEN_O 32  /* Token field offset */
#define MC_CMD_HDR_TOKEN_S 16  /* Token field size */

#define MAKE_UMASK64(_width) \
	((u64)((_width) < 64 ? ((u64)1 << (_width)) - 1 : \
	(u64)-1)) \

static inline u64 mc_enc(int lsoffset, int width, u64 val)
{
	return (u64)(((u64)val & MAKE_UMASK64(width)) << lsoffset);
}

static inline u64 mc_dec(u64 val, int lsoffset, int width)
{
	return (u64)((val >> lsoffset) & MAKE_UMASK64(width));
}

#define MC_CMD_OP(_cmd, _param, _offset, _width, _type, _arg) \
	((_cmd).params[_param] |= mc_enc((_offset), (_width), _arg))

#define MC_RSP_OP(_cmd, _param, _offset, _width, _type, _arg) \
	(_arg = (_type)mc_dec(_cmd.params[_param], (_offset), (_width)))

#define MC_CMD_HDR_READ_TOKEN(_hdr) \
	((u16)mc_dec((_hdr), MC_CMD_HDR_TOKEN_O, MC_CMD_HDR_TOKEN_S))

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_OPEN(cmd, dpdmai_id) \
	MC_CMD_OP(cmd, 0, 0,  32, int,      dpdmai_id)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_CREATE(cmd, cfg) \
do { \
	MC_CMD_OP(cmd, 0, 8,  8,  u8,  (cfg)->priorities[0]);\
	MC_CMD_OP(cmd, 0, 16, 8,  u8,  (cfg)->priorities[1]);\
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_RSP_IS_ENABLED(cmd, en) \
	MC_RSP_OP(cmd, 0, 0,  1,  int,	    en)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_SET_IRQ(cmd, irq_index, irq_cfg) \
do { \
	MC_CMD_OP(cmd, 0, 0,  8,  u8,  irq_index);\
	MC_CMD_OP(cmd, 0, 32, 32, u32, irq_cfg->val);\
	MC_CMD_OP(cmd, 1, 0,  64, u64, irq_cfg->addr);\
	MC_CMD_OP(cmd, 2, 0,  32, int,	    irq_cfg->irq_num); \
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_GET_IRQ(cmd, irq_index) \
	MC_CMD_OP(cmd, 0, 32, 8,  u8,  irq_index)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_RSP_GET_IRQ(cmd, type, irq_cfg) \
do { \
	MC_RSP_OP(cmd, 0, 0,  32, u32, irq_cfg->val); \
	MC_RSP_OP(cmd, 1, 0,  64, u64, irq_cfg->addr);\
	MC_RSP_OP(cmd, 2, 0,  32, int,	    irq_cfg->irq_num); \
	MC_RSP_OP(cmd, 2, 32, 32, int,	    type); \
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_SET_IRQ_ENABLE(cmd, irq_index, enable_state) \
do { \
	MC_CMD_OP(cmd, 0, 0,  8,  u8,  enable_state); \
	MC_CMD_OP(cmd, 0, 32, 8,  u8,  irq_index); \
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_GET_IRQ_ENABLE(cmd, irq_index) \
	MC_CMD_OP(cmd, 0, 32, 8,  u8,  irq_index)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_RSP_GET_IRQ_ENABLE(cmd, enable_state) \
	MC_RSP_OP(cmd, 0, 0,  8,  u8,  enable_state)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_SET_IRQ_MASK(cmd, irq_index, mask) \
do { \
	MC_CMD_OP(cmd, 0, 0,  32, u32, mask); \
	MC_CMD_OP(cmd, 0, 32, 8,  u8,  irq_index); \
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_GET_IRQ_MASK(cmd, irq_index) \
	MC_CMD_OP(cmd, 0, 32, 8,  u8,  irq_index)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_RSP_GET_IRQ_MASK(cmd, mask) \
	MC_RSP_OP(cmd, 0, 0,  32, u32, mask)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_GET_IRQ_STATUS(cmd, irq_index, status) \
do { \
	MC_CMD_OP(cmd, 0, 0,  32, u32, status);\
	MC_CMD_OP(cmd, 0, 32, 8,  u8,  irq_index);\
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_RSP_GET_IRQ_STATUS(cmd, status) \
	MC_RSP_OP(cmd, 0, 0,  32, u32,  status)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_CLEAR_IRQ_STATUS(cmd, irq_index, status) \
do { \
	MC_CMD_OP(cmd, 0, 0,  32, u32, status); \
	MC_CMD_OP(cmd, 0, 32, 8,  u8,  irq_index); \
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_RSP_GET_ATTR(cmd, attr) \
do { \
	MC_RSP_OP(cmd, 0, 0,  32, int,	(attr)->id); \
	MC_RSP_OP(cmd, 0, 32,  8,  u8,	(attr)->num_of_priorities); \
	MC_RSP_OP(cmd, 1, 0,  16, u16,	(attr)->version.major);\
	MC_RSP_OP(cmd, 1, 16, 16, u16,	(attr)->version.minor);\
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_SET_RX_QUEUE(cmd, priority, cfg) \
do { \
	MC_CMD_OP(cmd, 0, 0,  32, int,	(cfg)->dest_cfg.dest_id); \
	MC_CMD_OP(cmd, 0, 32, 8,  u8,	(cfg)->dest_cfg.priority); \
	MC_CMD_OP(cmd, 0, 40, 8,  u8,	priority); \
	MC_CMD_OP(cmd, 0, 48, 4,  enum dpdmai_dest, \
			(cfg)->dest_cfg.dest_type); \
	MC_CMD_OP(cmd, 1, 0,  64, u64,	(cfg)->user_ctx); \
	MC_CMD_OP(cmd, 2, 0,  32, u32,	(cfg)->options);\
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_GET_RX_QUEUE(cmd, priority) \
	MC_CMD_OP(cmd, 0, 40, 8,  u8,  priority)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_RSP_GET_RX_QUEUE(cmd, attr) \
do { \
	MC_RSP_OP(cmd, 0, 0,  32, int,	(attr)->dest_cfg.dest_id);\
	MC_RSP_OP(cmd, 0, 32, 8,  u8,	(attr)->dest_cfg.priority);\
	MC_RSP_OP(cmd, 0, 48, 4,  enum dpdmai_dest, \
			(attr)->dest_cfg.dest_type);\
	MC_RSP_OP(cmd, 1, 0,  64, u64,  (attr)->user_ctx);\
	MC_RSP_OP(cmd, 2, 0,  32, u32,  (attr)->fqid);\
} while (0)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_CMD_GET_TX_QUEUE(cmd, priority) \
	MC_CMD_OP(cmd, 0, 40, 8,  u8,  priority)

/*                cmd, param, offset, width, type, arg_name */
#define DPDMAI_RSP_GET_TX_QUEUE(cmd, attr) \
	MC_RSP_OP(cmd, 1, 0,  32, u32,  (attr)->fqid)

#endif /* _FSL_DPDMAI_CMD_H */
