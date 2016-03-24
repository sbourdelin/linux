#undef TRACE_SYSTEM
#define TRACE_SYSTEM mmc

#if !defined(_TRACE_MMC_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MMC_H

#include <linux/blkdev.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(mmc_request,

	TP_PROTO(struct request *rq),

	TP_ARGS(rq),

	TP_STRUCT__entry(
		__field(sector_t,		sector)
		__field(unsigned int,		data_len)
		__field(int,			cmd_dir)
		__field(struct request *,	rq)
	),

	TP_fast_assign(
		__entry->sector = blk_rq_pos(rq);
		__entry->data_len = blk_rq_bytes(rq);
		__entry->cmd_dir = rq_data_dir(rq);
		__entry->rq = rq;
	),

	TP_printk("struct request[%p]:sector=%lu rw=%d len=%u",
		  (struct request *)__entry->rq,
		  (unsigned long)__entry->sector,
		  (int)__entry->cmd_dir,
		  (unsigned int)__entry->data_len)
);

DEFINE_EVENT(mmc_request, mmc_queue_fetch,

	TP_PROTO(struct request *rq),

	TP_ARGS(rq)

);

DEFINE_EVENT(mmc_request, mmc_block_packed_req,

	TP_PROTO(struct request *rq),

	TP_ARGS(rq)

);

DEFINE_EVENT(mmc_request, mmc_block_req_done,

	TP_PROTO(struct request *rq),

	TP_ARGS(rq)

);

TRACE_EVENT(mmc_request_start,

	TP_PROTO(struct mmc_host *host, struct mmc_request *mrq),

	TP_ARGS(host, mrq),

	TP_STRUCT__entry(
		__field(u32,			cmd_opcode)
		__field(u32,			cmd_arg)
		__field(unsigned int,		cmd_flags)
		__field(u32,			stop_opcode)
		__field(u32,			stop_arg)
		__field(unsigned int,		stop_flags)
		__field(u32,			sbc_opcode)
		__field(u32,			sbc_arg)
		__field(unsigned int,		sbc_flags)
		__field(unsigned int,		blocks)
		__field(unsigned int,		blksz)
		__field(unsigned int,		data_flags)
		__field(struct mmc_request *,	mrq)
		__field(struct mmc_host *,	host)
	),

	TP_fast_assign(
		__entry->cmd_opcode = mrq->cmd->opcode;
		__entry->cmd_arg = mrq->cmd->arg;
		__entry->cmd_flags = mrq->cmd->flags;
		__entry->stop_opcode = mrq->stop ? mrq->stop->opcode : 0;
		__entry->stop_arg = mrq->stop ? mrq->stop->arg : 0;
		__entry->stop_flags = mrq->stop ? mrq->stop->flags : 0;
		__entry->sbc_opcode = mrq->sbc ? mrq->sbc->opcode : 0;
		__entry->sbc_arg = mrq->sbc ? mrq->sbc->arg : 0;
		__entry->sbc_flags = mrq->sbc ? mrq->sbc->flags : 0;
		__entry->blksz = mrq->data ? mrq->data->blksz : 0;
		__entry->blocks = mrq->data ? mrq->data->blocks : 0;
		__entry->data_flags = mrq->data ? mrq->data->flags : 0;
		__entry->mrq = mrq;
		__entry->host = host;
	),

	TP_printk("%s: start struct mmc_request[%p]: "
		  "cmd_opcode=%u cmd_arg=0x%x cmd_flags=0x%x "
		  "stop_opcode=%u stop_arg=0x%x stop_flags=0x%x "
		  "sbc_opcode=%u sbc_arg=0x%x sbc_flags=0x%x "
		  "blocks=%u blksz=%u data_flags=0x%x",
		  mmc_hostname((struct mmc_host *)__entry->host),
		  (struct mmc_request *)__entry->mrq,
		  (u32)__entry->cmd_opcode, (u32)__entry->cmd_arg,
		  (unsigned int)__entry->cmd_flags,
		  (u32)__entry->stop_opcode, (u32)__entry->stop_arg,
		  (unsigned int)__entry->stop_flags,
		  (u32)__entry->sbc_opcode, (u32)__entry->sbc_arg,
		  (unsigned int)__entry->sbc_flags,
		  (unsigned int)__entry->blocks,
		  (unsigned int)__entry->blksz,
		  (unsigned int)__entry->data_flags)
);

TRACE_EVENT(mmc_request_done,

	TP_PROTO(struct mmc_host *host, struct mmc_request *mrq),

	TP_ARGS(host, mrq),

	TP_STRUCT__entry(
		__field(u32,		cmd_opcode)
		__field(int,		cmd_err)
		__array(u32,		cmd_resp,	4)
		__field(u32,		stop_opcode)
		__field(int,		stop_err)
		__array(u32,		stop_resp,	4)
		__field(u32,		sbc_opcode)
		__field(int,		sbc_err)
		__array(u32,		sbc_resp,	4)
		__field(unsigned int,		bytes_xfered)
		__field(int,			data_err)
		__field(struct mmc_request *,	mrq)
		__field(struct mmc_host *,	host)
	),

	TP_fast_assign(
		__entry->cmd_opcode = mrq->cmd->opcode;
		__entry->cmd_err = mrq->cmd->error;
		memcpy(__entry->cmd_resp, mrq->cmd->resp, 4);
		__entry->stop_opcode = mrq->stop ? mrq->stop->opcode : 0;
		__entry->stop_err = mrq->stop ? mrq->stop->error : 0;
		__entry->stop_resp[0] = mrq->stop ? mrq->stop->resp[0] : 0;
		__entry->stop_resp[1] = mrq->stop ? mrq->stop->resp[1] : 0;
		__entry->stop_resp[2] = mrq->stop ? mrq->stop->resp[2] : 0;
		__entry->stop_resp[3] = mrq->stop ? mrq->stop->resp[3] : 0;
		__entry->sbc_opcode = mrq->sbc ? mrq->sbc->opcode : 0;
		__entry->sbc_err = mrq->sbc ? mrq->sbc->error : 0;
		__entry->sbc_resp[0] = mrq->sbc ? mrq->sbc->resp[0] : 0;
		__entry->sbc_resp[1] = mrq->sbc ? mrq->sbc->resp[1] : 0;
		__entry->sbc_resp[2] = mrq->sbc ? mrq->sbc->resp[2] : 0;
		__entry->sbc_resp[3] = mrq->sbc ? mrq->sbc->resp[3] : 0;
		__entry->bytes_xfered = mrq->data ? mrq->data->bytes_xfered : 0;
		__entry->data_err = mrq->data ? mrq->data->error : 0;
		__entry->host = host;
		__entry->mrq = mrq;
	),

	TP_printk("%s: end struct mmc_request[%p]: "
		  "cmd_opcode=%u cmd_err=%d cmd_resp=0x%x 0x%x 0x%x 0x%x "
		  "stop_opcode=%u stop_err=%d stop_resp=0x%x 0x%x 0x%x 0x%x "
		  "sbc_opcode=%u sbc_err=%d sbc_resp=0x%x 0x%x 0x%x 0x%x "
		  "bytes_xfered=%u data_err=%d",
		  mmc_hostname((struct mmc_host *)__entry->host),
		  (struct mmc_request *)__entry->mrq,
		  (u32)__entry->cmd_opcode, (int)__entry->cmd_err,
		  (u32)__entry->cmd_resp[0], (u32)__entry->cmd_resp[1],
		  (u32)__entry->cmd_resp[2], (u32)__entry->cmd_resp[3],
		  (u32)__entry->stop_opcode, (int)__entry->stop_err,
		  (u32)__entry->stop_resp[0], (u32)__entry->stop_resp[1],
		  (u32)__entry->stop_resp[2], (u32)__entry->stop_resp[3],
		  (u32)__entry->sbc_opcode, (int)__entry->sbc_err,
		  (u32)__entry->sbc_resp[0], (u32)__entry->sbc_resp[1],
		  (u32)__entry->sbc_resp[2], (u32)__entry->sbc_resp[3],
		  (unsigned int)__entry->bytes_xfered,
		  (int)__entry->data_err)
);

#endif /* _TRACE_MMC_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
