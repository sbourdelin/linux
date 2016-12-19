/*
 * Copyright © 2016 Intel Corporation
 *
 * Authors:
 *    Scott  Bauer      <scott.bauer@intel.com>
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ":OPAL: " fmt

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/genhd.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <uapi/linux/sed-opal.h>
#include <linux/sed.h>
#include <linux/sed-opal.h>
#include <linux/string.h>
#include <linux/kdev_t.h>
#include <linux/key.h>

#include "sed-opal_internal.h"

#define IO_BUFFER_LENGTH 2048
#define MAX_TOKS 64

struct opal_dev;
typedef int (cont_fn)(struct opal_dev *dev);

struct opal_cmd {
	cont_fn *cb;
	void *cb_data;

	size_t pos;
	u8 cmd_buf[IO_BUFFER_LENGTH * 2];
	u8 resp_buf[IO_BUFFER_LENGTH * 2];
	u8 *cmd;
	u8 *resp;
};

/*
 * On the parsed response, we don't store again the toks that are already
 * stored in the response buffer. Instead, for each token, we just store a
 * pointer to the position in the buffer where the token starts, and the size
 * of the token in bytes.
 */
struct opal_resp_tok {
	const u8 *pos;
	size_t len;
	enum OPAL_RESPONSE_TOKEN type;
	enum OPAL_ATOM_WIDTH width;
	union {
		u64 u;
		s64 s;
	} stored;
};

/*
 * From the response header it's not possible to know how many tokens there are
 * on the payload. So we hardcode that the maximum will be MAX_TOKS, and later
 * if we start dealing with messages that have more than that, we can increase
 * this number. This is done to avoid having to make two passes through the
 * response, the first one counting how many tokens we have and the second one
 * actually storing the positions.
 */
struct parsed_resp {
	int num;
	struct opal_resp_tok toks[MAX_TOKS];
};

struct opal_dev;

typedef int (*opal_step)(struct opal_dev *dev);

struct opal_suspend_data {
	struct opal_lock_unlock unlk;
	u8 lr;
	size_t key_name_len;
	char key_name[36];
	struct list_head node;
};

/**
 * struct opal_dev - The structure representing a OPAL enabled SED.
 * @sed_ctx:The SED context, contains fn pointers to sec_send/recv.
 * @opal_step:A series of opal methods that are necessary to complete a comannd.
 * @func_data:An array of parameters for the opal methods above.
 * @state:Describes the current opal_step we're working on.
 * @dev_lock:Locks the entire opal_dev structure.
 * @parsed:Parsed response from controller.
 * @prev_data:Data returned from a method to the controller
 * @error_cb:Error function that handles closing sessions after a failed method.
 * @unlk_lst:A list of Locking ranges to unlock on this device during a resume.
 */
struct opal_dev {
	struct sed_context *sed_ctx;
	const opal_step *funcs;
	void **func_data;
	int state;
	struct mutex dev_lock;
	u16 comID;
	u32 HSN;
	u32 TSN;
	u64 align;
	u64 lowest_lba;
	struct opal_cmd cmd;
	struct parsed_resp parsed;
	size_t prev_d_len;
	void *prev_data;
	opal_step error_cb;
	void *error_cb_data;

	struct list_head unlk_lst;
};

DEFINE_SPINLOCK(list_spinlock);

static void print_buffer(const u8 *ptr, u32 length)
{
#ifdef DEBUG
	print_hex_dump_bytes("OPAL: ", DUMP_PREFIX_OFFSET, ptr, length);
	pr_debug("\n");
#endif
}

#define TPER_SYNC_SUPPORTED BIT(0)

static bool check_tper(const void *data)
{
	const struct d0_tper_features *tper = data;
	u8 flags = tper->supported_features;

	if (!(flags & TPER_SYNC_SUPPORTED)) {
		pr_err("TPer sync not supported. flags = %d\n",
		       tper->supported_features);
		return false;
	}

	return true;
}

static bool check_SUM(const void *data)
{
	const struct d0_single_user_mode *sum = data;
	u32 nlo = be32_to_cpu(sum->num_locking_objects);

	if (nlo == 0) {
		pr_err("Need at least one locking object.\n");
		return false;
	}

	pr_debug("Number of locking objects: %d\n", nlo);

	return true;
}

static u16 get_comID_v100(const void *data)
{
	const struct d0_opal_v100 *v100 = data;

	return be16_to_cpu(v100->baseComID);
}

static u16 get_comID_v200(const void *data)
{
	const struct d0_opal_v200 *v200 = data;

	return be16_to_cpu(v200->baseComID);
}

static int opal_send_cmd(struct opal_dev *dev)
{
	return dev->sed_ctx->ops->sec_send(dev->sed_ctx->sec_data,
					   dev->comID, TCG_SECP_01,
					   dev->cmd.cmd, IO_BUFFER_LENGTH);
}

static int opal_recv_cmd(struct opal_dev *dev)
{
	return dev->sed_ctx->ops->sec_recv(dev->sed_ctx->sec_data,
					   dev->comID, TCG_SECP_01,
					   dev->cmd.resp, IO_BUFFER_LENGTH);
}

static int opal_recv_check(struct opal_dev *dev)
{
	size_t buflen = IO_BUFFER_LENGTH;
	void *buffer = dev->cmd.resp;
	struct opal_header *hdr = buffer;
	int ret;

	do {
		pr_debug("Sent OPAL command: outstanding=%d, minTransfer=%d\n",
			 hdr->cp.outstandingData,
			 hdr->cp.minTransfer);

		if (hdr->cp.outstandingData == 0 ||
		    hdr->cp.minTransfer != 0)
			return 0;

		memset(buffer, 0, buflen);
		ret = opal_recv_cmd(dev);
	} while (!ret);

	return ret;
}

static int opal_send_recv(struct opal_dev *dev, cont_fn *cont)
{
	int ret;

	ret = opal_send_cmd(dev);
	if (ret)
		return ret;
	ret = opal_recv_cmd(dev);
	if (ret)
		return ret;
	ret = opal_recv_check(dev);
	if (ret)
		return ret;
	return cont(dev);
}

static void check_geometry(struct opal_dev *dev, const void *data)
{
	const struct d0_geometry_features *geo = data;

	dev->align = geo->alignment_granularity;
	dev->lowest_lba = geo->lowest_aligned_lba;
}

static int next(struct opal_dev *dev)
{
	opal_step func;
	int error = 0;

	do {
		func = dev->funcs[dev->state];
		if (!func)
			break;

		dev->state++;
		error = func(dev);

		if (error) {
			pr_err("Error on step function: %d with error %d: %s\n",
			       dev->state, error,
			       opal_error_to_human(error));

			if (dev->error_cb && dev->state > 2)
				dev->error_cb(dev->error_cb_data);
		}
	} while (!error);

	return error;
}

static int opal_discovery0_end(struct opal_dev *dev)
{
	bool foundComID = false, supported = true, single_user = false;
	const struct d0_header *hdr;
	const u8 *epos, *cpos;
	u16 comID = 0;
	int error = 0;

	epos = dev->cmd.resp;
	cpos = dev->cmd.resp;
	hdr = (struct d0_header *)dev->cmd.resp;

	print_buffer(dev->cmd.resp, be32_to_cpu(hdr->length));

	epos += be32_to_cpu(hdr->length); /* end of buffer */
	cpos += sizeof(*hdr); /* current position on buffer */

	while (cpos < epos && supported) {
		const struct d0_features *body =
			(const struct d0_features *)cpos;

		switch (be16_to_cpu(body->code)) {
		case FC_TPER:
			supported = check_tper(body->features);
			break;
		case FC_SINGLEUSER:
			single_user = check_SUM(body->features);
			break;
		case FC_GEOMETRY:
			check_geometry(dev, body);
			break;
		case FC_LOCKING:
		case FC_ENTERPRISE:
		case FC_DATASTORE:
			/* some ignored properties */
			pr_debug("Found OPAL feature description: %d\n",
				 be16_to_cpu(body->code));
			break;
		case FC_OPALV100:
			comID = get_comID_v100(body->features);
			foundComID = true;
			break;
		case FC_OPALV200:
			comID = get_comID_v200(body->features);
			foundComID = true;
			break;
		case 0xbfff ... 0xffff:
			/* vendor specific, just ignore */
			break;
		default:
			pr_warn("OPAL Unknown feature: %d\n",
				be16_to_cpu(body->code));

		}
		cpos += body->length + 4;
	}

	if (!supported) {
		pr_err("This device is not Opal enabled. Not Supported!\n");
		return 1;
	}

	if (!single_user)
		pr_warn("Device doesn't support single user mode\n");


	if (!foundComID) {
		pr_warn("Could not find OPAL comID for device. Returning early\n");
		return 1;
	}

	dev->comID = comID;

	return 0;
}

static int opal_discovery0(struct opal_dev *dev)
{
	int ret;

	memset(dev->cmd.resp, 0, IO_BUFFER_LENGTH);
	dev->comID = 0x0001;
	ret = opal_recv_cmd(dev);
	if (ret)
		return ret;
	return opal_discovery0_end(dev);
}

static void add_token_u8(struct opal_cmd *cmd, u8 tok)
{
	cmd->cmd[cmd->pos++] = tok;
}

static ssize_t test_and_add_token_u8(struct opal_cmd *cmd, u8 tok)
{
	BUILD_BUG_ON(IO_BUFFER_LENGTH >= SIZE_MAX);

	if (cmd->pos >= IO_BUFFER_LENGTH - 1) {
		pr_err("Error adding u8: end of buffer.\n");
		return -ERANGE;
	}

	add_token_u8(cmd, tok);

	return 0;
}

#define TINY_ATOM_DATA_MASK GENMASK(5, 0)
#define TINY_ATOM_SIGNED BIT(6)

#define SHORT_ATOM_ID BIT(7)
#define SHORT_ATOM_BYTESTRING BIT(5)
#define SHORT_ATOM_SIGNED BIT(4)
#define SHORT_ATOM_LEN_MASK GENMASK(3, 0)

static void add_short_atom_header(struct opal_cmd *cmd, bool bytestring,
				  bool has_sign, int len)
{
	u8 atom;

	atom = SHORT_ATOM_ID;
	atom |= bytestring ? SHORT_ATOM_BYTESTRING : 0;
	atom |= has_sign ? SHORT_ATOM_SIGNED : 0;
	atom |= len & SHORT_ATOM_LEN_MASK;

	add_token_u8(cmd, atom);
}

#define MEDIUM_ATOM_ID (BIT(7) | BIT(6))
#define MEDIUM_ATOM_BYTESTRING BIT(4)
#define MEDIUM_ATOM_SIGNED BIT(3)
#define MEDIUM_ATOM_LEN_MASK GENMASK(2, 0)

static void add_medium_atom_header(struct opal_cmd *cmd, bool bytestring,
				   bool has_sign, int len)
{
	u8 header0;

	header0 = MEDIUM_ATOM_ID;
	header0 |= bytestring ? MEDIUM_ATOM_BYTESTRING : 0;
	header0 |= has_sign ? MEDIUM_ATOM_SIGNED : 0;
	header0 |= (len >> 8) & MEDIUM_ATOM_LEN_MASK;
	cmd->cmd[cmd->pos++] = header0;
	cmd->cmd[cmd->pos++] = len;
}

static void add_token_u64(struct opal_cmd *cmd, u64 number, size_t len)
{
	add_short_atom_header(cmd, false, false, len);

	while (len--) {
		u8 n = number >> (len * 8);

		add_token_u8(cmd, n);
	}
}

static ssize_t test_and_add_token_u64(struct opal_cmd *cmd, u64 number)
{
	int len;
	int msb;

	if (!(number & ~TINY_ATOM_DATA_MASK))
		return test_and_add_token_u8(cmd, number);

	msb = fls(number);
	len = DIV_ROUND_UP(msb, 4);

	if (cmd->pos >= IO_BUFFER_LENGTH - len - 1) {
		pr_err("Error adding u64: end of buffer.\n");
		return -ERANGE;
	}

	add_token_u64(cmd, number, len);

	return 0;
}

static ssize_t add_token_bytestring(struct opal_cmd *cmd,
				    const u8 *bytestring, size_t len)
{
	size_t header_len = 1;
	bool is_short_atom = true;

	if (len & ~SHORT_ATOM_LEN_MASK) {
		header_len = 2;
		is_short_atom = false;
	}

	if (len >= IO_BUFFER_LENGTH - cmd->pos - header_len) {
		pr_err("Error adding bytestring: end of buffer.\n");
		return -ERANGE;
	}

	if (is_short_atom)
		add_short_atom_header(cmd, true, false, len);
	else
		add_medium_atom_header(cmd, true, false, len);

	memcpy(&cmd->cmd[cmd->pos], bytestring, len);
	cmd->pos += len;

	return 0;
}

static ssize_t test_and_add_string(struct opal_cmd *cmd,
				   const u8 *string,
				   size_t len)
{
	return add_token_bytestring(cmd, string, len);
}

static ssize_t test_and_add_token_bytestr(struct opal_cmd *cmd,
					     const u8 *bytestring)
{
	return add_token_bytestring(cmd, bytestring, OPAL_UID_LENGTH);
}

static ssize_t test_and_add_token_half(struct opal_cmd *cmd,
				       const u8 *bytestring)
{
	return add_token_bytestring(cmd, bytestring, OPAL_UID_LENGTH/2);
}

#define LOCKING_RANGE_NON_GLOBAL 0x03

static int build_locking_range(u8 *buffer, size_t length, u8 lr)
{
	if (length < OPAL_UID_LENGTH) {
		pr_err("Can't build locking range. Length OOB\n");
		return -ERANGE;
	}

	memcpy(buffer, OPALUID[OPAL_LOCKINGRANGE_GLOBAL], OPAL_UID_LENGTH);

	if (lr == 0)
		return 0;
	buffer[5] = LOCKING_RANGE_NON_GLOBAL;
	buffer[7] = lr;

	return 0;
}

static int build_locking_user(u8 *buffer, size_t length, u8 lr)
{
	if (length < OPAL_UID_LENGTH) {
		pr_err("Can't build locking range user, Length OOB\n");
		return -ERANGE;
	}

	memcpy(buffer, OPALUID[OPAL_USER1_UID], OPAL_UID_LENGTH);

	buffer[7] = lr + 1;

	return 0;
}

#define ADD_TOKEN_STRING(cmd, key, keylen)		        \
	if (!err)					        \
		err = test_and_add_string(cmd, key, keylen);

#define ADD_TOKEN(type, cmd, tok)				\
	if (!err)						\
		err = test_and_add_token_##type(cmd, tok);

static void set_comID(struct opal_cmd *cmd, u16 comID)
{
	struct opal_header *hdr = (struct opal_header *)cmd->cmd;

	hdr->cp.extendedComID[0] = comID >> 8;
	hdr->cp.extendedComID[1] = comID;
	hdr->cp.extendedComID[2] = 0;
	hdr->cp.extendedComID[3] = 0;
}

static int cmd_finalize(struct opal_cmd *cmd, u32 hsn, u32 tsn)
{
	struct opal_header *hdr;
	int err = 0;


	ADD_TOKEN(u8, cmd, OPAL_ENDOFDATA);
	ADD_TOKEN(u8, cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8, cmd, 0);
	ADD_TOKEN(u8, cmd, 0);
	ADD_TOKEN(u8, cmd, 0);
	ADD_TOKEN(u8, cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error finalizing command.\n");
		return -EFAULT;
	}

	hdr = (struct opal_header *) cmd->cmd;

	hdr->pkt.TSN = cpu_to_be32(tsn);
	hdr->pkt.HSN = cpu_to_be32(hsn);

	hdr->subpkt.length = cpu_to_be32(cmd->pos - sizeof(*hdr));
	while (cmd->pos % 4) {
		if (cmd->pos >= IO_BUFFER_LENGTH) {
			pr_err("Error: Buffer overrun\n");
			return -ERANGE;
		}
		cmd->cmd[cmd->pos++] = 0;
	}
	hdr->pkt.length = cpu_to_be32(cmd->pos - sizeof(hdr->cp) -
				      sizeof(hdr->pkt));
	hdr->cp.length = cpu_to_be32(cmd->pos - sizeof(hdr->cp));

	return 0;
}

static enum OPAL_RESPONSE_TOKEN token_type(const struct parsed_resp *resp,
					   int n)
{
	const struct opal_resp_tok *tok;

	if (n >= resp->num) {
		pr_err("Token number doesn't exist: %d, resp: %d\n",
		       n, resp->num);
		return OPAL_DTA_TOKENID_INVALID;
	}

	tok = &resp->toks[n];
	if (tok->len == 0) {
		pr_err("Token length must be non-zero\n");
		return OPAL_DTA_TOKENID_INVALID;
	}

	return tok->type;
}

/*
 * This function returns 0 in case of invalid token. One should call
 * token_type() first to find out if the token is valid or not.
 */
static enum OPAL_TOKEN response_get_token(const struct parsed_resp *resp,
					  int n)
{
	const struct opal_resp_tok *tok;

	if (n >= resp->num) {
		pr_err("Token number doesn't exist: %d, resp: %d\n",
		       n, resp->num);
		return 0;
	}

	tok = &resp->toks[n];
	if (tok->len == 0) {
		pr_err("Token length must be non-zero\n");
		return 0;
	}

	return tok->pos[0];
}

static size_t response_parse_tiny(struct opal_resp_tok *tok,
				  const u8 *pos)
{
	tok->pos = pos;
	tok->len = 1;
	tok->width = OPAL_WIDTH_TINY;

	if (pos[0] & TINY_ATOM_SIGNED) {
		tok->type = OPAL_DTA_TOKENID_SINT;
	} else {
		tok->type = OPAL_DTA_TOKENID_UINT;
		tok->stored.u = pos[0] & 0x3f;
	}

	return tok->len;
}

static size_t response_parse_short(struct opal_resp_tok *tok,
				   const u8 *pos)
{
	tok->pos = pos;
	tok->len = (pos[0] & SHORT_ATOM_LEN_MASK) + 1;
	tok->width = OPAL_WIDTH_SHORT;

	if (pos[0] & SHORT_ATOM_BYTESTRING) {
		tok->type = OPAL_DTA_TOKENID_BYTESTRING;
	} else if (pos[0] & SHORT_ATOM_SIGNED) {
		tok->type = OPAL_DTA_TOKENID_SINT;
	} else {
		u64 u_integer = 0;
		int i, b = 0;

		tok->type = OPAL_DTA_TOKENID_UINT;
		if (tok->len > 9) {
			pr_warn("uint64 with more than 8 bytes\n");
			return -EINVAL;
		}
		for (i = tok->len - 1; i > 0; i--) {
			u_integer |= ((u64)pos[i] << (8 * b));
			b++;
		}
		tok->stored.u = u_integer;
	}

	return tok->len;
}

static size_t response_parse_medium(struct opal_resp_tok *tok,
				    const u8 *pos)
{
	tok->pos = pos;
	tok->len = (((pos[0] & MEDIUM_ATOM_LEN_MASK) << 8) | pos[1]) + 2;
	tok->width = OPAL_WIDTH_MEDIUM;

	if (pos[0] & MEDIUM_ATOM_BYTESTRING)
		tok->type = OPAL_DTA_TOKENID_BYTESTRING;
	else if (pos[0] & MEDIUM_ATOM_SIGNED)
		tok->type = OPAL_DTA_TOKENID_SINT;
	else
		tok->type = OPAL_DTA_TOKENID_UINT;

	return tok->len;
}

#define LONG_ATOM_ID (BIT(7) | BIT(6) | BIT(5))
#define LONG_ATOM_BYTESTRING BIT(1)
#define LONG_ATOM_SIGNED BIT(0)
static size_t response_parse_long(struct opal_resp_tok *tok,
				  const u8 *pos)
{
	tok->pos = pos;
	tok->len = ((pos[1] << 16) | (pos[2] << 8) | pos[3]) + 4;
	tok->width = OPAL_WIDTH_LONG;

	if (pos[0] & LONG_ATOM_BYTESTRING)
		tok->type = OPAL_DTA_TOKENID_BYTESTRING;
	else if (pos[0] & LONG_ATOM_SIGNED)
		tok->type = OPAL_DTA_TOKENID_SINT;
	else
		tok->type = OPAL_DTA_TOKENID_UINT;

	return tok->len;
}

static size_t response_parse_token(struct opal_resp_tok *tok,
				   const u8 *pos)
{
	tok->pos = pos;
	tok->len = 1;
	tok->type = OPAL_DTA_TOKENID_TOKEN;
	tok->width = OPAL_WIDTH_TOKEN;

	return tok->len;
}

static int response_parse(const u8 *buf, size_t length,
			  struct parsed_resp *resp)
{
	const struct opal_header *hdr;
	struct opal_resp_tok *iter;
	int ret, num_entries = 0;
	u32 cpos = 0, total;
	size_t token_length;
	const u8 *pos;

	if (!buf)
		return -EFAULT;

	if (!resp)
		return -EFAULT;

	hdr = (struct opal_header *)buf;
	pos = buf;
	pos += sizeof(*hdr);

	pr_debug("Response size: cp: %d, pkt: %d, subpkt: %d\n",
		 be32_to_cpu(hdr->cp.length),
		 be32_to_cpu(hdr->pkt.length),
		 be32_to_cpu(hdr->subpkt.length));

	if ((hdr->cp.length == 0)
	    || (hdr->pkt.length == 0)
	    || (hdr->subpkt.length == 0)) {
		pr_err("Bad header length. cp: %d, pkt: %d, subpkt: %d\n",
		       be32_to_cpu(hdr->cp.length),
		       be32_to_cpu(hdr->pkt.length),
		       be32_to_cpu(hdr->subpkt.length));
		print_buffer(pos, sizeof(*hdr));
		return -EINVAL;
	}

	if (pos > buf + length)
		return -EFAULT;

	iter = resp->toks;
	total = be32_to_cpu(hdr->subpkt.length);
	print_buffer(pos, total);
	while (cpos < total) {
		if (!(pos[0] & 0x80)) /* tiny atom */
			token_length = response_parse_tiny(iter, pos);
		else if (!(pos[0] & 0x40)) /* short atom */
			token_length = response_parse_short(iter, pos);
		else if (!(pos[0] & 0x20)) /* medium atom */
			token_length = response_parse_medium(iter, pos);
		else if (!(pos[0] & 0x10)) /* long atom */
			token_length = response_parse_long(iter, pos);
		else /* TOKEN */
			token_length = response_parse_token(iter, pos);

		if (token_length == -EINVAL)
			return -EINVAL;

		pos += token_length;
		cpos += token_length;
		iter++;
		num_entries++;
	}

	if (num_entries == 0) {
		pr_err("Couldn't parse response.\n");
		return -EINVAL;
	resp->num = num_entries;

	return 0;
}

static size_t response_get_string(const struct parsed_resp *resp, int n,
				  const char **store)
{
	*store = NULL;
	if (!resp) {
		pr_err("Response is NULL\n");
		return 0;
	}

	if (n > resp->num) {
		pr_err("Response has %d tokens. Can't access %d\n",
		       resp->num, n);
		return 0;
	}

	if (resp->toks[n].type != OPAL_DTA_TOKENID_BYTESTRING) {
		pr_err("Token is not a byte string!\n");
		return 0;
	}

	*store = resp->toks[n].pos + 1;
	return resp->toks[n].len - 1;
}

static u64 response_get_u64(const struct parsed_resp *resp, int n)
{
	if (!resp) {
		pr_err("Response is NULL\n");
		return 0;
	}

	if (n > resp->num) {
		pr_err("Response has %d tokens. Can't access %d\n",
		       resp->num, n);
		return 0;
	}

	if (resp->toks[n].type != OPAL_DTA_TOKENID_UINT) {
		pr_err("Token is not unsigned it: %d\n",
		       resp->toks[n].type);
		return 0;
	}

	if (!((resp->toks[n].width == OPAL_WIDTH_TINY) ||
	      (resp->toks[n].width == OPAL_WIDTH_SHORT))) {
		pr_err("Atom is not short or tiny: %d\n",
		       resp->toks[n].width);
		return 0;
	}

	return resp->toks[n].stored.u;
}

static u8 response_status(const struct parsed_resp *resp)
{
	if ((token_type(resp, 0) == OPAL_DTA_TOKENID_TOKEN)
	    && (response_get_token(resp, 0) == OPAL_ENDOFSESSION)) {
		return 0;
	}

	if (resp->num < 5)
		return DTAERROR_NO_METHOD_STATUS;

	if ((token_type(resp, resp->num - 1) != OPAL_DTA_TOKENID_TOKEN) ||
	    (token_type(resp, resp->num - 5) != OPAL_DTA_TOKENID_TOKEN) ||
	    (response_get_token(resp, resp->num - 1) != OPAL_ENDLIST) ||
	    (response_get_token(resp, resp->num - 5) != OPAL_STARTLIST))
		return DTAERROR_NO_METHOD_STATUS;

	return response_get_u64(resp, resp->num - 4);
}

/* Parses and checks for errors */
static int parse_and_check_status(struct opal_dev *dev)
{
	struct opal_cmd *cmd;
	int error;

	cmd = &dev->cmd;
	print_buffer(cmd->cmd, cmd->pos);

	error = response_parse(cmd->resp, IO_BUFFER_LENGTH, &dev->parsed);
	if (error) {
		pr_err("Couldn't parse response.\n");
		return error;
	}

	return response_status(&dev->parsed);
}

static void clear_opal_cmd(struct opal_cmd *cmd)
{
	cmd->pos = sizeof(struct opal_header);
	memset(cmd->cmd, 0, IO_BUFFER_LENGTH);
	cmd->cb = NULL;
	cmd->cb_data = NULL;
}

static int start_opal_session_cont(struct opal_dev *dev)
{
	u32 HSN, TSN;
	int error = 0;

	error = parse_and_check_status(dev);
	if (error)
		return error;

	HSN = response_get_u64(&dev->parsed, 4);
	TSN = response_get_u64(&dev->parsed, 5);

	if (HSN == 0 && TSN == 0) {
		pr_err("Couldn't authenticate session\n");
		return -EPERM;
	}

	dev->HSN = HSN;
	dev->TSN = TSN;
	return 0;
}

static inline void opal_dev_get(struct opal_dev *dev)
{
	mutex_lock(&dev->dev_lock);
}

static inline void opal_dev_put(struct opal_dev *dev)
{
	mutex_unlock(&dev->dev_lock);
}

static int add_suspend_info(struct opal_dev *dev, struct opal_suspend_data *sus)
{
	struct opal_suspend_data *iter;
	bool found = false;

	if (list_empty(&dev->unlk_lst))
		goto add_out;

	list_for_each_entry(iter, &dev->unlk_lst, node) {
		if (iter->lr == sus->lr) {
			found = true;
			break;
		}
	}

	if (found) {
		/* Replace the old with the new */
		list_del(&iter->node);
		kfree(iter);
	}

 add_out:
	list_add_tail(&sus->node, &dev->unlk_lst);
	return 0;
}

static int end_session_cont(struct opal_dev *dev)
{
	dev->HSN = 0;
	dev->TSN = 0;
	return parse_and_check_status(dev);
}

static int finalize_and_send(struct opal_dev *dev, struct opal_cmd *cmd,
			     cont_fn cont)
{
	int ret;

	ret = cmd_finalize(cmd, dev->HSN, dev->TSN);
	if (ret) {
		pr_err("Error finalizing command buffer: %d\n", ret);
		return ret;
	}

	print_buffer(cmd->cmd, cmd->pos);

	return opal_send_recv(dev, cont);
}

static int gen_key(struct opal_dev *dev)
{
	const u8 *method;
	u8 uid[OPAL_UID_LENGTH];
	struct opal_cmd *cmd;
	int err = 0;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	memcpy(uid, dev->prev_data, min(sizeof(uid), dev->prev_d_len));
	method = OPALMETHOD[OPAL_GENKEY];
	kfree(dev->prev_data);
	dev->prev_data = NULL;

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, uid);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_GENKEY]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error building gen key command\n");
		return err;

	}
	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int get_active_key_cont(struct opal_dev *dev)
{
	const char *activekey;
	size_t keylen;
	int error = 0;

	error = parse_and_check_status(dev);
	if (error)
		return error;
	keylen = response_get_string(&dev->parsed, 4, &activekey);
	if (!activekey) {
		pr_err("%s: Couldn't extract the Activekey from the response\n",
		       __func__);
		return 0x0A;
	}
	dev->prev_data = kmemdup(activekey, keylen, GFP_KERNEL);

	if (!dev->prev_data)
		return -ENOMEM;

	dev->prev_d_len = keylen;

	return 0;
}

static int get_active_key(struct opal_dev *dev)
{
	u8 uid[OPAL_UID_LENGTH];
	struct opal_cmd *cmd;
	int err = 0;
	u8 *lr;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);
	lr = dev->func_data[dev->state - 1];

	err = build_locking_range(uid, sizeof(uid), *lr);
	if (err)
		return err;

	err = 0;
	ADD_TOKEN(u8, cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, uid);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_GET]);
	ADD_TOKEN(u8, cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8, cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8, cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8, cmd, OPAL_TINY_UINT_03); /* startCloumn */
	ADD_TOKEN(u8, cmd, OPAL_TINY_UINT_10); /* ActiveKey */
	ADD_TOKEN(u8, cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8, cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8, cmd, OPAL_TINY_UINT_04); /* endColumn */
	ADD_TOKEN(u8, cmd, OPAL_TINY_UINT_10); /* ActiveKey */
	ADD_TOKEN(u8, cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8, cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8, cmd, OPAL_ENDLIST);
	if (err) {
		pr_err("Error building get active key command\n");
		return err;
	}

	return finalize_and_send(dev, cmd, get_active_key_cont);
}

static int generic_lr_enable_disable(struct opal_cmd *cmd,
				     u8 *uid, bool rle, bool wle,
				     bool rl, bool wl)
{
	int err = 0;
	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, uid);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_SET]);

	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_VALUES);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_05); /* ReadLockEnabled */
	ADD_TOKEN(u8,      cmd, rle);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_06); /* WriteLockEnabled */
	ADD_TOKEN(u8,      cmd, wle);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_READLOCKED);
	ADD_TOKEN(u8,      cmd, rl);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_WRITELOCKED);
	ADD_TOKEN(u8,      cmd, wl);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	return err;
}

static inline int enable_global_lr(struct opal_cmd *cmd, u8 *uid,
				   struct opal_user_lr_setup *setup)
{
	int err;
	err = generic_lr_enable_disable(cmd, uid, !!setup->RLE, !!setup->WLE,
					0, 0);
	if (err)
		pr_err("Failed to create enable global lr command\n");
	return err;
}

static int setup_locking_range(struct opal_dev *dev)
{
	u8 uid[OPAL_UID_LENGTH];
	struct opal_cmd *cmd;
	struct opal_user_lr_setup *setup;
	u8 lr;
	int err = 0;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	setup = dev->func_data[dev->state - 1];
	lr = setup->session.opal_key.lr;
	err = build_locking_range(uid, sizeof(uid), lr);
	if (err)
		return err;

	if (lr == 0)
		err = enable_global_lr(cmd, uid, setup);
	else {
		ADD_TOKEN(u8,      cmd, OPAL_CALL);
		ADD_TOKEN(bytestr, cmd, uid);
		ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_SET]);

		ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
		ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
		ADD_TOKEN(u8,      cmd, OPAL_VALUES);
		ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);

		ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03); /* Ranges Start */
		ADD_TOKEN(u64,     cmd, setup->range_start);
		ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

		ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_04); /* Ranges length */
		ADD_TOKEN(u64,     cmd, setup->range_length);
		ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

		ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_05); /* ReadLockEnabled */
		ADD_TOKEN(u64,     cmd, !!setup->RLE);
		ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

		ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_06); /* WriteLockEnabled */
		ADD_TOKEN(u64,     cmd, !!setup->WLE);
		ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

		ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
		ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
		ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);


	}
	if (err) {
		pr_err("Error building Setup Locking range command.\n");
		return err;

	}

	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int start_generic_opal_session(struct opal_dev *dev,
				      enum OPAL_UID auth,
				      enum OPAL_UID sp_type,
				      const char *key,
				      u8 key_len)
{
	struct opal_cmd *cmd;
	u32 HSN;
	int err = 0;

	if (key == NULL && auth != OPAL_ANYBODY_UID) {
		pr_err("%s: Attempted to open ADMIN_SP Session without a Host" \
		       "Challenge, and not as the Anybody UID\n", __func__);
		return 1;
	}

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);

	set_comID(cmd, dev->comID);
	HSN = GENERIC_HOST_SESSION_NUM;

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_SMUID_UID]);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_STARTSESSION]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u64,     cmd, HSN);
	ADD_TOKEN(bytestr, cmd, OPALUID[sp_type]);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_01);

	switch (auth) {
	case OPAL_ANYBODY_UID:
		ADD_TOKEN(u8, cmd, OPAL_ENDLIST);
		break;
	case OPAL_ADMIN1_UID:
	case OPAL_SID_UID:
		ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_00); /* HostChallenge */
		ADD_TOKEN_STRING(cmd, key, key_len);
		ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
		ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03); /* HostSignAuth */
		ADD_TOKEN(bytestr, cmd, OPALUID[auth]);
		ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
		ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
		break;
	default:
		pr_err("Cannot start Admin SP session with auth %d\n", auth);
		return 1;
	}

	if (err) {
		pr_err("Error building start adminsp session command.\n");
		return err;
	}

	return finalize_and_send(dev, cmd, start_opal_session_cont);
}

static int start_anybodyASP_opal_session(struct opal_dev *dev)
{
	return start_generic_opal_session(dev, OPAL_ANYBODY_UID,
					  OPAL_ADMINSP_UID, NULL, 0);
}

static int start_SIDASP_opal_session(struct opal_dev *dev)
{
	int ret;
	const u8 *key = dev->prev_data;
	struct opal_key *okey;

	if (!key) {
		okey = dev->func_data[dev->state - 1];
		ret = start_generic_opal_session(dev, OPAL_SID_UID,
						 OPAL_ADMINSP_UID,
						 okey->key,
						 okey->key_len);
	}
	else {
		ret = start_generic_opal_session(dev, OPAL_SID_UID,
						 OPAL_ADMINSP_UID,
						 key, dev->prev_d_len);
		kfree(key);
		dev->prev_data = NULL;
	}
	return ret;
}

static inline int start_admin1LSP_opal_session(struct opal_dev *dev)
{
	struct opal_key *key = dev->func_data[dev->state - 1];
	return start_generic_opal_session(dev, OPAL_ADMIN1_UID,
					  OPAL_LOCKINGSP_UID,
					  key->key, key->key_len);
}

static int start_auth_opal_session(struct opal_dev *dev)
{
	u8 lk_ul_user[OPAL_UID_LENGTH];
	int err = 0;

	struct opal_session_info *session = dev->func_data[dev->state - 1];
	struct opal_cmd *cmd = &dev->cmd;
	size_t keylen = session->opal_key.key_len;
	u8 *key = session->opal_key.key;
	u32 HSN = GENERIC_HOST_SESSION_NUM;

	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	if (session->SUM) {
		err = build_locking_user(lk_ul_user, sizeof(lk_ul_user),
					 session->opal_key.lr);
		if (err)
			return err;

	} else if (session->who != OPAL_ADMIN1 && !session->SUM) {
		err = build_locking_user(lk_ul_user, sizeof(lk_ul_user),
					 session->who - 1);
		if (err)
			return err;
	} else
		memcpy(lk_ul_user, OPALUID[OPAL_ADMIN1_UID], OPAL_UID_LENGTH);

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_SMUID_UID]);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_STARTSESSION]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u64,     cmd, HSN);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_LOCKINGSP_UID]);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_01);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_00);
	ADD_TOKEN_STRING(cmd, key, keylen);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03);
	ADD_TOKEN(bytestr, cmd, lk_ul_user);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error building STARTSESSION command.\n");
		return err;
	}

	return finalize_and_send(dev, cmd, start_opal_session_cont);
}

static int revert_tper(struct opal_dev *dev)
{
	struct opal_cmd *cmd;
	int err = 0;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_ADMINSP_UID]);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_REVERT]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	if (err) {
		pr_err("Error building REVERT TPER command.\n");
		return err;
	}

	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int internal_activate_user(struct opal_dev *dev)
{
	struct opal_session_info *session = dev->func_data[dev->state - 1];
	u8 uid[OPAL_UID_LENGTH];
	struct opal_cmd *cmd;
	int err = 0;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	memcpy(uid, OPALUID[OPAL_USER1_UID], OPAL_UID_LENGTH);
	uid[7] = session->who;

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, uid);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_SET]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_VALUES);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_05); /* Enabled */
	ADD_TOKEN(u8,      cmd, OPAL_TRUE);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error building Activate UserN command.\n");
		return err;
	}

	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int erase_locking_range(struct opal_dev *dev)
{
	struct opal_session_info *session;
	u8 uid[OPAL_UID_LENGTH];
	struct opal_cmd *cmd;
	int err = 0;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);
	session = dev->func_data[dev->state - 1];

	if (build_locking_range(uid, sizeof(uid), session->opal_key.lr) < 0)
		return -ERANGE;

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, uid);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_ERASE]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error building Erase Locking Range Cmmand.\n");
		return err;
	}
	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int set_mbr_done(struct opal_dev *dev)
{
	u8 mbr_done_tf = *(u8 *)dev->func_data[dev->state - 1];
	struct opal_cmd *cmd = &dev->cmd;
	int err = 0;

	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_MBRCONTROL]);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_SET]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_VALUES);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_02); /* Done */
	ADD_TOKEN(u8,      cmd, mbr_done_tf); /* Done T or F */
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
  	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error Building set MBR Done command\n");
		return err;
	}

	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int set_mbr_enable_disable(struct opal_dev *dev)
{
	u8 mbr_en_dis = *(u8 *)dev->func_data[dev->state - 1];
	struct opal_cmd *cmd = &dev->cmd;
	int err = 0;

	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_MBRCONTROL]);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_SET]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_VALUES);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_01);
	ADD_TOKEN(u8,      cmd, mbr_en_dis);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
  	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error Building set MBR done command\n");
		return err;
	}

	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int generic_pw_cmd(u8 *key, size_t key_len, u8 *cpin_uid,
			  struct opal_dev *dev)
{
	struct opal_cmd *cmd = &dev->cmd;
	int err = 0;

	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, cpin_uid);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_SET]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_VALUES);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03); /* PIN */
	ADD_TOKEN_STRING(cmd, key, key_len);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	return err;
}

static int set_new_pw(struct opal_dev *dev)
{
	u8 cpin_uid[OPAL_UID_LENGTH];
	struct opal_session_info *usr = dev->func_data[dev->state - 1];
	struct opal_cmd *cmd = &dev->cmd;

	memcpy(cpin_uid, OPALUID[OPAL_C_PIN_ADMIN1], OPAL_UID_LENGTH);

	if (usr->who != OPAL_ADMIN1) {
		cpin_uid[5] = 0x03;
		if (usr->SUM)
			cpin_uid[7] = usr->opal_key.lr + 1;
		else
			cpin_uid[7] = usr->who;
	}


	if (generic_pw_cmd(usr->opal_key.key, usr->opal_key.key_len,
			   cpin_uid, dev)) {
		pr_err("Error building set password command.\n");
		return -ERANGE;
	}

	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int set_sid_cpin_pin(struct opal_dev *dev)
{
	u8 cpin_uid[OPAL_UID_LENGTH];
	struct opal_key *key = dev->func_data[dev->state - 1];
	struct opal_cmd *cmd = &dev->cmd;

	memcpy(cpin_uid, OPALUID[OPAL_C_PIN_SID], OPAL_UID_LENGTH);
	
	if (generic_pw_cmd(key->key, key->key_len, cpin_uid, dev)) {
		pr_err("Error building Set SID cpin\n");
		return -ERANGE;
	}
	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int add_user_to_lr(struct opal_dev *dev)
{
	u8 lr_buffer[OPAL_UID_LENGTH];
	u8 user_uid[OPAL_UID_LENGTH];
	struct opal_lock_unlock *lkul;
	struct opal_cmd *cmd;
	int err = 0;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	lkul = dev->func_data[dev->state - 1];

	memcpy(lr_buffer, OPALUID[OPAL_LOCKINGRANGE_ACE_RDLOCKED],
	       OPAL_UID_LENGTH);

	if (lkul->l_state == OPAL_RW)
		memcpy(lr_buffer, OPALUID[OPAL_LOCKINGRANGE_ACE_WRLOCKED],
		       OPAL_UID_LENGTH);

	lr_buffer[7] = lkul->session.opal_key.lr;

	memcpy(user_uid, OPALUID[OPAL_USER1_UID], OPAL_UID_LENGTH);

	user_uid[7] = lkul->session.who;

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, lr_buffer);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_SET]);

	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_VALUES);

	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03);

	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(half,    cmd, OPALUID[OPAL_HALF_UID_AUTHORITY_OBJ_REF]);
	ADD_TOKEN(bytestr, cmd, user_uid);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(half,    cmd, OPALUID[OPAL_HALF_UID_AUTHORITY_OBJ_REF]);
	ADD_TOKEN(bytestr, cmd, user_uid);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(half,    cmd, OPALUID[OPAL_HALF_UID_BOOLEAN_ACE]);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_01);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error building add user to locking range command.\n");
		return err;
	}

	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int lock_unlock_locking_range(struct opal_dev *dev)
{
	u8 lr_buffer[OPAL_UID_LENGTH];
	struct opal_cmd *cmd;
	const u8 *method;
	struct opal_lock_unlock *lkul;
	u8 read_locked = 1, write_locked = 1;
	int err = 0;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	method = OPALMETHOD[OPAL_SET];
	lkul = dev->func_data[dev->state - 1];
	if (build_locking_range(lr_buffer, sizeof(lr_buffer),
				lkul->session.opal_key.lr) < 0)
		return -ERANGE;

	switch (lkul->l_state) {
	case OPAL_RO:
		read_locked = 0;
		write_locked = 1;
		break;
	case OPAL_RW:
		read_locked = 0;
		write_locked = 0;
		break;
	case OPAL_LK:
		/* vars are initalized to locked */
		break;
	default:
		pr_err("Tried to set an invalid locking state... returning to uland\n");
		return 1;
	}

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, lr_buffer);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_SET]);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_VALUES);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_READLOCKED);
	ADD_TOKEN(u8,      cmd, read_locked);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_WRITELOCKED);
	ADD_TOKEN(u8,      cmd, write_locked);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error building SET command.\n");
		return err;
	}
	return finalize_and_send(dev, cmd, parse_and_check_status);
}


static int lock_unlock_locking_range_SUM(struct opal_dev *dev)
{
	u8 lr_buffer[OPAL_UID_LENGTH];
	struct opal_cmd *cmd;
	const u8 *method;
	struct opal_lock_unlock *lkul;
	int ret;
	u8 read_locked = 1, write_locked = 1;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	method = OPALMETHOD[OPAL_SET];
	lkul = dev->func_data[dev->state - 1];
	if (build_locking_range(lr_buffer, sizeof(lr_buffer),
				lkul->session.opal_key.lr) < 0)
		return -ERANGE;

	switch (lkul->l_state) {
	case OPAL_RO:
		read_locked = 0;
		write_locked = 1;
		break;
	case OPAL_RW:
		read_locked = 0;
		write_locked = 0;
		break;
	case OPAL_LK:
		/* vars are initalized to locked */
		break;
	default:
		pr_err("Tried to set an invalid locking state.\n");
		return 1;
	}
	ret = generic_lr_enable_disable(cmd, lr_buffer, 1, 1,
					read_locked, write_locked);

	if (ret < 0) {
		pr_err("Error building SET command.\n");
		return ret;
	}
	return finalize_and_send(dev, cmd, parse_and_check_status);
}

int activate_lsp(struct opal_dev *dev)
{
	u8 user_lr[OPAL_UID_LENGTH];
	struct opal_cmd *cmd;
	u8 uint_3 = 0x83;
	int err = 0;
	u8 *lr;

	cmd = &dev->cmd;

	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	lr = dev->func_data[dev->state - 1];

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_LOCKINGSP_UID]);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_ACTIVATE]);
	/* Activating as SUM */
	if (*lr > 0) {
		err = build_locking_range(user_lr, sizeof(user_lr), *lr);
		if (err)
			return err;

		ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
		ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
		ADD_TOKEN(u8,      cmd, uint_3);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_06);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_00);
		ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_00);

		ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
		ADD_TOKEN(bytestr, cmd, user_lr);
		ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
		ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);
		ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	} else {
		ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
		ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	}

	if (err) {
		pr_err("Error building Activate LockingSP command.\n");
		return err;
	}

	return finalize_and_send(dev, cmd, parse_and_check_status);
}

static int get_lsp_lifecycle_cont(struct opal_dev *dev)
{
	u8 lc_status;
	int error = 0;

	error = parse_and_check_status(dev);
	if (error)
		return error;

	lc_status = response_get_u64(&dev->parsed, 4);
	/* 0x08 is Manufacured Inactive */
	/* 0x09 is Manufactured */
	if (lc_status != 0x08) {
		pr_err("Couldn't determine the status of the Lifcycle state\n");
		return -ENODEV;
	}

err_return:
	return 0;
}

/* Determine if we're in the Manufactured Inactive or Active state */
int get_lsp_lifecycle(struct opal_dev *dev)
{
	struct opal_cmd *cmd;
	int err = 0;

	cmd = &dev->cmd;

	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);

	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_LOCKINGSP_UID]);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_GET]);

	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03); /* Start Column */
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_06); /* Lifecycle Column */
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_04); /* End Column */
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_06); /* Lifecycle Column */
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error Building GET Lifecycle Status command\n");
		return err;
	}

	return finalize_and_send(dev, cmd, get_lsp_lifecycle_cont);
}

static int get_msid_cpin_pin_cont(struct opal_dev *dev)
{
	const char *msid_pin;
	size_t strlen;
	int error = 0;

	error = parse_and_check_status(dev);
	if (error)
		return error;

	strlen = response_get_string(&dev->parsed, 4, &msid_pin);
	if (!msid_pin) {
		pr_err("%s: Couldn't extract PIN from response\n", __func__);
		return 11;
	}

	dev->prev_data = kmemdup(msid_pin, strlen, GFP_KERNEL);
	if (!dev->prev_data)
		return -ENOMEM;

	dev->prev_d_len = strlen;

 err_return:
	return 0;
}

static int get_msid_cpin_pin(struct opal_dev *dev)
{
	struct opal_cmd *cmd;
	int err = 0;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);
	set_comID(cmd, dev->comID);


	ADD_TOKEN(u8,      cmd, OPAL_CALL);
	ADD_TOKEN(bytestr, cmd, OPALUID[OPAL_C_PIN_MSID]);
	ADD_TOKEN(bytestr, cmd, OPALMETHOD[OPAL_GET]);

	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);
	ADD_TOKEN(u8,      cmd, OPAL_STARTLIST);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03); /* Start Column */
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03); /* PIN */
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_STARTNAME);
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_04); /* End Column */
	ADD_TOKEN(u8,      cmd, OPAL_TINY_UINT_03); /* Lifecycle Column */
	ADD_TOKEN(u8,      cmd, OPAL_ENDNAME);

	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);
	ADD_TOKEN(u8,      cmd, OPAL_ENDLIST);

	if (err) {
		pr_err("Error building Get MSID CPIN PIN command.\n");
		return err;
	}

	return finalize_and_send(dev, cmd, get_msid_cpin_pin_cont);
}

static int build_end_opal_session(struct opal_dev *dev)
{
	struct opal_cmd *cmd;

	cmd = &dev->cmd;
	clear_opal_cmd(cmd);

	set_comID(cmd, dev->comID);
	return test_and_add_token_u8(cmd, OPAL_ENDOFSESSION);
}

static int end_opal_session(struct opal_dev *dev)
{
	int ret = build_end_opal_session(dev);

	if (ret < 0)
		return ret;
	return finalize_and_send(dev, &dev->cmd, end_session_cont);
}

const opal_step error_end_session[] = {
	end_opal_session,
	NULL,
};

static int end_opal_session_error(struct opal_dev *dev)
{
	dev->funcs = error_end_session;
	dev->state = 0;
	dev->error_cb = NULL;
	return next(dev);
}

struct opal_dev *alloc_opal_dev(struct request_queue *q)
{
	struct opal_dev *opal_dev;
	unsigned long dma_align;
	struct opal_cmd *cmd;

	opal_dev = kzalloc(sizeof(*opal_dev), GFP_KERNEL);
	if (!opal_dev)
		return opal_dev;

	cmd = &opal_dev->cmd;
	cmd->cmd = cmd->cmd_buf;
	cmd->resp = cmd->resp_buf;

	dma_align = (queue_dma_alignment(q) | q->dma_pad_mask) + 1;
	cmd->cmd = (u8 *)round_up((uintptr_t)cmd->cmd, dma_align);
	cmd->resp = (u8 *)round_up((uintptr_t)cmd->resp, dma_align);

	INIT_LIST_HEAD(&opal_dev->unlk_lst);

	opal_dev->state = 0;

	mutex_init(&opal_dev->dev_lock);

	return opal_dev;

}
EXPORT_SYMBOL(alloc_opal_dev);

static int do_cmds(struct opal_dev *dev)
{
	int ret;
	ret = next(dev);
	opal_dev_put(dev);
	return ret;
}

static struct opal_dev *get_opal_dev(struct sed_context *sedc,
				     const opal_step *funcs)
{
	struct opal_dev *dev = sedc->dev;
	if (dev) {
		dev->state = 0;
		dev->funcs = funcs;
		dev->TSN = 0;
		dev->HSN = 0;
		dev->error_cb = end_opal_session_error;
		dev->error_cb_data = dev;
		dev->func_data = NULL;
		dev->sed_ctx = sedc;
		opal_dev_get(dev);
	}
	return dev;
}

int opal_secure_erase_locking_range(struct sed_context *sedc, struct sed_key *key)
{
	struct opal_dev *dev;
	void *data[3] = { NULL };
	const opal_step erase_funcs[] = {
		opal_discovery0,
		start_auth_opal_session,
		get_active_key,
		gen_key,
		end_opal_session,
		NULL,
	};

	dev = get_opal_dev(sedc, erase_funcs);
	if (!dev)
		return -ENODEV;

	dev->func_data = data;
	dev->func_data[1] = &key->opal_session;
	dev->func_data[2] = &key->opal_session.opal_key.lr;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_secure_erase_locking_range);

int opal_erase_locking_range(struct sed_context *sedc, struct sed_key *key)
{
	struct opal_dev *dev;
	void *data[3] = { NULL };
	const opal_step erase_funcs[] = {
		opal_discovery0,
		start_auth_opal_session,
		erase_locking_range,
		end_opal_session,
		NULL,
	};

	dev = get_opal_dev(sedc, erase_funcs);
	if (!dev)
		return -ENODEV;

	dev->func_data = data;
	dev->func_data[1] = &key->opal_session;
	dev->func_data[2] = &key->opal_session;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_erase_locking_range);

int opal_enable_disable_shadow_mbr(struct sed_context *sedc,
				   struct sed_key *key)
{
	void *func_data[6] = { NULL };
	struct opal_dev *dev;
	const opal_step mbr_funcs[] = {
		opal_discovery0,
		start_admin1LSP_opal_session,
		set_mbr_done,
		end_opal_session,
		start_admin1LSP_opal_session,
		set_mbr_enable_disable,
		end_opal_session,
		NULL,
	};

	if (key->opal_mbr.enable_disable != OPAL_MBR_ENABLE &&
	    key->opal_mbr.enable_disable != OPAL_MBR_DISABLE)
		return -EINVAL;

	dev = get_opal_dev(sedc, mbr_funcs);
	if (!dev)
		return -ENODEV;

	dev->func_data = func_data;
	dev->func_data[1] = &key->opal_mbr.key;
	dev->func_data[2] = &key->opal_mbr.enable_disable;
	dev->func_data[4] = &key->opal_mbr.key;
	dev->func_data[5] = &key->opal_mbr.enable_disable;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_enable_disable_shadow_mbr);

int opal_save(struct sed_context *sedc, struct sed_key *key)
{
	struct opal_suspend_data *suspend;
	struct opal_dev *dev;
	int ret;

	dev = get_opal_dev(sedc, NULL);
	if (!dev)
		return -ENODEV;
	suspend = kzalloc(sizeof(*suspend), GFP_KERNEL);
	if(!suspend)
		return -ENOMEM;

	suspend->unlk = key->opal_lk_unlk;
	suspend->lr = key->opal_lk_unlk.session.opal_key.lr;
	ret = add_suspend_info(dev, suspend);
	opal_dev_put(dev);
	return ret;
}
EXPORT_SYMBOL(opal_save);

int opal_add_user_to_lr(struct sed_context *sedc, struct sed_key *key)
{
	void *func_data[3] = { NULL };
	struct opal_dev *dev;
		const opal_step funcs[] = {
		opal_discovery0,
		start_admin1LSP_opal_session,
		add_user_to_lr,
		end_opal_session,
		NULL
	};

	if (key->opal_lk_unlk.l_state != OPAL_RO &&
	    key->opal_lk_unlk.l_state != OPAL_RW) {
		pr_err("Locking state was not RO or RW\n");
		return -EINVAL;
	}
	if (key->opal_lk_unlk.session.who < OPAL_USER1 &&
	    key->opal_lk_unlk.session.who > OPAL_USER9) {
		pr_err("Authority was not within the range of users: %d\n",
		       key->opal_lk_unlk.session.who);
		return -EINVAL;
	}
	if (key->opal_lk_unlk.session.SUM) {
		pr_err("%s not supported in SUM. Use setup locking range\n",
		       __func__);
		return -EINVAL;
	}

	dev = get_opal_dev(sedc, funcs);
	if (!dev)
		return -ENODEV;

	dev->func_data = func_data;
	dev->func_data[1] = &key->opal_lk_unlk.session.opal_key;
	dev->func_data[2] = &key->opal_lk_unlk;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_add_user_to_lr);

int opal_reverttper(struct sed_context *sedc, struct sed_key *key)
{
	void *data[2] = { NULL };
	const opal_step revert_funcs[] = {
		opal_discovery0,
		start_SIDASP_opal_session,
		revert_tper, /* controller will terminate session */
		NULL,
	};
	struct opal_dev *dev = get_opal_dev(sedc, revert_funcs);

	if (!dev)
		return -ENODEV;

	dev->func_data = data;
	dev->func_data[1] = &key->opal;
	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_reverttper);

/* These are global'd because both lock_unlock_internal
 * and opal_unlock_from_suspend need them.
 */
const opal_step ulk_funcs_SUM[] = {
	opal_discovery0,
	start_auth_opal_session,
	lock_unlock_locking_range_SUM,
	end_opal_session,
	NULL
};
const opal_step _unlock_funcs[] = {
	opal_discovery0,
	start_auth_opal_session,
	lock_unlock_locking_range,
	end_opal_session,
	NULL
};
int opal_lock_unlock(struct sed_context *sedc, struct sed_key *key)
{
	void *func_data[3] = { NULL };
	struct opal_dev *dev;

	if (key->opal_lk_unlk.session.who < OPAL_ADMIN1 ||
	    key->opal_lk_unlk.session.who > OPAL_USER9)
		return -EINVAL;

	dev = get_opal_dev(sedc, NULL);
	if (!dev)
		return -ENODEV;

	if (key->opal_lk_unlk.session.SUM)
		dev->funcs = _unlock_funcs;//ulk_funcs_SUM;
	else
		dev->funcs = _unlock_funcs;

	dev->func_data = func_data;
	dev->func_data[1] = &key->opal_lk_unlk.session;
	dev->func_data[2] = &key->opal_lk_unlk;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_lock_unlock);

int opal_take_ownership(struct sed_context *sedc, struct sed_key *key)
{
	const opal_step owner_funcs[] = {
		opal_discovery0,
		start_anybodyASP_opal_session,
		get_msid_cpin_pin,
		end_opal_session,
		start_SIDASP_opal_session,
		set_sid_cpin_pin,
		end_opal_session,
		NULL
	};
	void *data[6] = { NULL };
	struct opal_dev *dev = get_opal_dev(sedc, owner_funcs);

	if (!dev)
		return -ENODEV;

	dev->func_data = data;
	dev->func_data[4] = &key->opal;
	dev->func_data[5] = &key->opal;
	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_take_ownership);

int opal_activate_lsp(struct sed_context *sedc, struct sed_key *key)
{
	void *data[4] = { NULL };
	const opal_step active_funcs[] = {
		opal_discovery0,
		start_SIDASP_opal_session, /* Open session as SID auth */
		get_lsp_lifecycle,
		activate_lsp,
		end_opal_session,
		NULL
	};
	struct opal_dev *dev = get_opal_dev(sedc, active_funcs);

	if (!dev)
		return -ENODEV;
	dev->func_data = data;
	dev->func_data[1] = &key->opal;
	dev->func_data[3] = &key->opal.lr;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_activate_lsp);

int opal_setup_locking_range(struct sed_context *sedc, struct sed_key *pw)
{
	void *data[3] = { NULL };
	const opal_step lr_funcs[] = {
		opal_discovery0,
		start_auth_opal_session,
		setup_locking_range,
		end_opal_session,
		NULL,
	};
	struct opal_dev *dev = get_opal_dev(sedc, lr_funcs);

	if (!dev)
		return -ENODEV;

	dev->func_data = data;
	dev->func_data[1] = &pw->opal_lrs.session;
	dev->func_data[2] = &pw->opal_lrs;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_setup_locking_range);

int opal_set_new_pw(struct sed_context *sedc, struct sed_key *pw)
{
	const opal_step pw_funcs[] = {
		opal_discovery0,
		start_auth_opal_session,
		set_new_pw,
		end_opal_session,
		NULL
	};
	struct opal_dev *dev;
	void *data[3] = { NULL };

	if (pw->sed_type != OPAL_PW)
		return -EINVAL;

	if (pw->opal_pw.session.who < OPAL_ADMIN1 ||
	    pw->opal_pw.session.who > OPAL_USER9  ||
	    pw->opal_pw.new_user_pw.who < OPAL_ADMIN1 ||
	    pw->opal_pw.new_user_pw.who > OPAL_USER9)
		return -EINVAL;

	dev = get_opal_dev(sedc, pw_funcs);
	if (!dev)
		return -ENODEV;

	dev->func_data = data;
	dev->func_data[1] = (void *) &pw->opal_pw.session;
	dev->func_data[2] = (void *) &pw->opal_pw.new_user_pw;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_set_new_pw);

int opal_activate_user(struct sed_context *sedc, struct sed_key *pw)
{
	const opal_step act_funcs[] = {
		opal_discovery0,
		start_admin1LSP_opal_session,
		internal_activate_user,
		end_opal_session,
		NULL
	};
	struct opal_dev *dev;
	void *data[3] = { NULL };

	if (pw->sed_type != OPAL_ACT_USR) {
		pr_err("Sed type was not act user\n");
		return -EINVAL;
	}

	/* We can't activate Admin1 it's active as manufactured */
	if (pw->opal_session.who < OPAL_USER1 &&
	    pw->opal_session.who > OPAL_USER9) {
		pr_err("Who was not a valid user: %d \n", pw->opal_session.who);
		return -EINVAL;
	}

	dev = get_opal_dev(sedc, act_funcs);
	if (!dev)
		return -ENODEV;

	dev->func_data = data;
	dev->func_data[1] = &pw->opal_session.opal_key;
	dev->func_data[2] = &pw->opal_session;

	return do_cmds(dev);
}
EXPORT_SYMBOL(opal_activate_user);

int opal_unlock_from_suspend(struct sed_context *sedc)
{
	struct opal_suspend_data *suspend;
	void *func_data[3] = { NULL };
	bool was_failure = false;
	struct opal_dev *dev = get_opal_dev(sedc, NULL);
	int ret = 0;

	if (!dev)
		return 0;

	dev->func_data = func_data;
	dev->error_cb = end_opal_session_error;
	dev->error_cb_data = dev;

	if (!list_empty(&dev->unlk_lst)) {
		list_for_each_entry(suspend, &dev->unlk_lst, node) {
			dev->state = 0;
			dev->func_data[1] = &suspend->unlk.session;
			dev->func_data[2] = &suspend->unlk;
			if (suspend->unlk.session.SUM)
				dev->funcs = ulk_funcs_SUM;
			else
				dev->funcs = _unlock_funcs;
			dev->TSN = 0;
			dev->HSN = 0;
			ret = next(dev);
			if (ret)
				was_failure = true;
		}
	}
	opal_dev_put(dev);
	return was_failure ? 1 : 0;
}
EXPORT_SYMBOL(opal_unlock_from_suspend);
