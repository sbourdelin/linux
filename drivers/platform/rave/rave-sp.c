/*
 * Multifunction core driver for Zodiac Inflight Innovations
 * SP MCU that is connected via dedicated UART port
 *
 * Copyright (C) 2017 Zodiac Inflight Innovations
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <asm/unaligned.h>
#include <linux/atomic.h>
#include <linux/crc-ccitt.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/rave-sp.h>
#include <linux/sched.h>
#include <linux/serdev.h>

/*
 * UART protocol using following entities:
 *  - message to MCU => ACK response
 *  - event from MCU => event ACK
 *
 * Frame structure:
 * <STX> <DATA> <CHECKSUM> <ETX>
 * Where:
 * - STX - is start of transmission character
 * - ETX - end of transmission
 * - DATA - payload
 * - CHECKSUM - checksum calculated on <DATA>
 *
 * If <DATA> or <CHECKSUM> contain one of control characters, then it is
 * escaped using <DLE> control code. Added <DLE> does not participate in
 * checksum calculation.
 */
#define RAVE_SP_STX			0x02
#define RAVE_SP_ETX			0x03
#define RAVE_SP_DLE			0x10

#define RAVE_SP_MAX_DATA_SIZE		64
#define RAVE_SP_CHECKSUM_SIZE		2  /* Worst case scenario on RDU2 */
/*
 * We don't store STX, ETX and unescaped bytes, so Rx is only
 * DATA + CSUM
 */
#define RAVE_SP_RX_BUFFER_SIZE				\
	(RAVE_SP_MAX_DATA_SIZE + RAVE_SP_CHECKSUM_SIZE)

#define RAVE_SP_STX_ETX_SIZE		2
/*
 * For Tx we have to have space for everything, STX, EXT and
 * potentially stuffed DATA + CSUM data + csum
 */
#define RAVE_SP_TX_BUFFER_SIZE				\
	(RAVE_SP_STX_ETX_SIZE + 2 * RAVE_SP_RX_BUFFER_SIZE)

#define RAVE_SP_BOOT_SOURCE_GET		0
#define RAVE_SP_BOOT_SOURCE_SET		1

#define RAVE_SP_RDU2_BOARD_TYPE_RMB	0
#define RAVE_SP_RDU2_BOARD_TYPE_DEB	1

#define RAVE_SP_BOOT_SOURCE_SD		0
#define RAVE_SP_BOOT_SOURCE_EMMC	1
#define RAVE_SP_BOOT_SOURCE_NOR		2

/**
 * enum rave_sp_deframer_state - Possible state for de-framer
 *
 * @RAVE_SP_EXPECT_SOF:		 Scanning input for start-of-frame marker
 * @RAVE_SP_EXPECT_DATA:	 Got start of frame marker, collecting frame
 * @RAVE_SP_EXPECT_ESCAPED_DATA: Got escape character, collecting escaped byte
 */
enum rave_sp_deframer_state {
	RAVE_SP_EXPECT_SOF,
	RAVE_SP_EXPECT_DATA,
	RAVE_SP_EXPECT_ESCAPED_DATA,
};

/**
 * struct rave_sp - Device protocol deframer
 *
 * @state:  Current state of the deframer
 * @data:   Buffer used to collect deframed data
 * @length: Number of bytes de-framed so far
 */
struct rave_sp_deframer {
	enum rave_sp_deframer_state state;
	unsigned char data[RAVE_SP_RX_BUFFER_SIZE];
	size_t length;
};

/**
 * struct rave_sp_reply - reply as per RAVE device protocol
 *
 * @length:	Expected reply length
 * @data:	Buffer to store reply payload in
 * @code:	Expected reply code
 * @ackid:	Expected reply ACK ID
 * @completion: Successful reply reception completion
 */
struct rave_sp_reply {
	size_t length;
	void  *data;
	u8     code;
	u8     ackid;
	struct completion received;
};

/**
 * struct rave_sp_checksum - Variant specific checksum implementation details
 *
 * @length:	Caculated checksum length
 * @subroutine: Utilized checksum algorithm implementation
 */
struct rave_sp_checksum {
	size_t length;
	void (*subroutine)(const u8 *, size_t, u8 *);
};

/**
 * struct rave_sp_variant_cmds - Variant specific command routines
 *
 * @translate:	     Generic to variant specific command mapping routine
 * @get_boot_source: Pointer to "get boot source" implementation
 * @set_boot_source: Pointer to "set boot source" implementation
 */
struct rave_sp_variant_cmds {
	int (*translate)(enum rave_sp_command);
	int (*get_boot_source)(struct rave_sp *);
	int (*set_boot_source)(struct rave_sp *, u8);
};

/**
 * struct rave_sp_variant - RAVE supervisory processor core variant
 *
 * @checksum:	Variant specific checksum implementation
 * @cmd:	Variant specific command pointer table
 * @init:	Variant specific initialization sequence implementation
 * @group:	Attribute group for exposed sysfs entries
 */
struct rave_sp_variant {
	const struct rave_sp_checksum *checksum;
	struct rave_sp_variant_cmds cmd;
	void (*init)(struct rave_sp *);
	struct attribute_group group;
};

/**
 * struct rave_sp - RAVE supervisory processor core
 *
 * @serdev:			Pointer to underlying serdev
 * @deframer:			Stored state of the protocol deframer
 * @ackid:			ACK ID used in last reply sent to the device
 * @bus_lock:			Lock to serialize access to the device
 * @reply_lock:			Lock protecting @reply
 * @reply:			Pointer to memory to store reply payload
 *
 * @part_number_firmware:	Firmware version
 * @part_number_bootloader:	Bootloader version
 * @reset_reason:		Reset reason
 * @copper_rev_rmb:		Manufacturing version info
 * @copper_rev_deb:		Manufacturing version info
 * @silicon_devid:		MCU silicon version info
 * @silicon_devrev:		MCU silicon version info
 * @copper_mod_rmb:		Manufacturing version info
 * @copper_mod_deb:		Manufacturing version info
 *
 * @variant:			Device variant specific parameters and
 *				functions
 * @event_notifier_list:	Input event notification chain (used with
 *				corresponding input MFD cell driver)
 * @group:			Attrubute group for exposed sysfs entries
 *
 *
 * part_number_*, reset_reason, copper_*, and silicon_* fields are all
 * strings retrived from the device during device probing and made
 * available for later userspace consumption via sysfs
 *
 */
struct rave_sp {
	struct serdev_device *serdev;

	struct rave_sp_deframer deframer;
	atomic_t ackid;

	struct mutex bus_lock;
	struct mutex reply_lock;
	struct rave_sp_reply *reply;

	const char *part_number_firmware;
	const char *part_number_bootloader;

	const char *reset_reason;
	const char *copper_rev_rmb;
	const char *copper_rev_deb;
	const char *silicon_devid;
	const char *silicon_devrev;

	const char *copper_mod_rmb;
	const char *copper_mod_deb;

	const struct rave_sp_variant *variant;

	struct blocking_notifier_head event_notifier_list;

	struct attribute_group *group;
};

struct rave_sp_rsp_status {
	u8 bl_bytes[6];
	u8 fw_bytes[6];
	u8 gs_format;
} __packed;

static bool rave_sp_id_is_event(u8 code)
{
	return (code & 0xF0) == RAVE_SP_EVNT_BASE;
}

static void rave_sp_unregister_event_notifier(struct device *dev, void *res)
{
	struct rave_sp *sp = dev_get_drvdata(dev->parent);
	struct notifier_block *nb = *(struct notifier_block **)res;
	struct blocking_notifier_head *bnh = &sp->event_notifier_list;

	WARN_ON(blocking_notifier_chain_unregister(bnh, nb));
}

int devm_rave_sp_register_event_notifier(struct device *dev,
					 struct notifier_block *nb)
{
	struct rave_sp *sp = dev_get_drvdata(dev->parent);
	struct notifier_block **rcnb;
	int ret;

	rcnb = devres_alloc(rave_sp_unregister_event_notifier,
			    sizeof(*rcnb), GFP_KERNEL);
	if (!rcnb)
		return -ENOMEM;

	ret = blocking_notifier_chain_register(&sp->event_notifier_list, nb);
	if (!ret) {
		*rcnb = nb;
		devres_add(dev, rcnb);
	} else {
		devres_free(rcnb);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(devm_rave_sp_register_event_notifier);

static const char *devm_rave_sp_version(struct device *dev, const char *buf)
{
	/*
	 * NOTE: The format string below uses %02d to display u16
	 * intentionally for the sake of backwards compatibility with
	 * legacy software.
	 */
	return devm_kasprintf(dev, GFP_KERNEL, "%02d%02d%02d.%c%c\n",
			      buf[0], get_unaligned_le16(&buf[1]),
			      buf[3], buf[4], buf[5]);
}

static int rave_sp_get_status(struct rave_sp *sp,
			      struct rave_sp_rsp_status *status)
{
	u8 cmd[] = {
		[0] = RAVE_SP_CMD_STATUS,
		[1] = 0
	};
	return rave_sp_exec(sp, cmd, sizeof(cmd), status, sizeof(*status));
}

#define RAVE_SP_ATTR_RO_STRING(name)					\
	static ssize_t							\
	name##_show(struct device *dev,					\
		    struct device_attribute *attr,			\
		    char *buf)						\
	{								\
		struct rave_sp *sp = dev_get_drvdata(dev);		\
		return sprintf(buf, "%s", sp->name);			\
	}								\
	static DEVICE_ATTR_RO(name)

RAVE_SP_ATTR_RO_STRING(part_number_firmware);
RAVE_SP_ATTR_RO_STRING(part_number_bootloader);
RAVE_SP_ATTR_RO_STRING(copper_rev_rmb);
RAVE_SP_ATTR_RO_STRING(copper_rev_deb);
RAVE_SP_ATTR_RO_STRING(reset_reason);
RAVE_SP_ATTR_RO_STRING(silicon_devrev);
RAVE_SP_ATTR_RO_STRING(silicon_devid);

RAVE_SP_ATTR_RO_STRING(copper_mod_rmb);
RAVE_SP_ATTR_RO_STRING(copper_mod_deb);

static ssize_t
i2c_device_status_show(struct device *dev,
		    struct device_attribute *attr,
		    char *buf)
{
	ssize_t ret;
	struct rave_sp *sp = dev_get_drvdata(dev);
	u8 status[2];
	u8 cmd[] = {
		[0] = RAVE_SP_CMD_GET_I2C_DEVICE_STATUS,
		[1] = 0
	};

	ret = rave_sp_exec(sp, cmd, sizeof(cmd), &status, sizeof(status));
	if (ret < 0)
		return ret;

	return sprintf(buf, "%04x\n", get_unaligned_le16(status));
}
static DEVICE_ATTR_RO(i2c_device_status);

static int rave_sp_rdu1_get_boot_source(struct rave_sp *sp)
{
	struct rave_sp_rsp_status status;
	int ret;

	ret = rave_sp_get_status(sp, &status);

	return (ret < 0) ? ret : (status.gs_format >> 2) & 0x03;
}

static int rave_sp_rdu1_set_boot_source(struct rave_sp *sp, u8 boot_source)
{
	return -ENOTSUPP;
}

static int rave_sp_common_set_boot_source(struct rave_sp *sp, u8 boot_source)
{
	u8 cmd[] = {
		[0] = RAVE_SP_CMD_BOOT_SOURCE,
		[1] = 0,
		[2] = RAVE_SP_BOOT_SOURCE_SET,
		[3] = boot_source,
	};

	return rave_sp_exec(sp, cmd, sizeof(cmd), NULL, 0);
}

static int rave_sp_common_get_boot_source(struct rave_sp *sp)
{
	u8 cmd[] = {
		[0] = RAVE_SP_CMD_BOOT_SOURCE,
		[1] = 0,
		[2] = RAVE_SP_BOOT_SOURCE_GET,
		[3] = 0,
	};
	u8 boot_source;
	int ret;

	ret = rave_sp_exec(sp, cmd, sizeof(cmd),
			   &boot_source, sizeof(boot_source));

	return (ret < 0) ? ret : boot_source;
}

static ssize_t rave_sp_show_boot_source(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct rave_sp *sp = dev_get_drvdata(dev);
	int ret;

	ret = sp->variant->cmd.get_boot_source(sp);

	return (ret < 0) ? ret : sprintf(buf, "%d\n", ret);
}

static ssize_t rave_sp_store_boot_source(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct rave_sp *sp = dev_get_drvdata(dev);
	u8 boot_source;
	int ret;

	ret = kstrtou8(buf, 0, &boot_source);
	if (ret)
		return ret;

	if (boot_source != RAVE_SP_BOOT_SOURCE_SD &&
	    boot_source != RAVE_SP_BOOT_SOURCE_EMMC &&
	    boot_source != RAVE_SP_BOOT_SOURCE_NOR)
		return -EINVAL;

	ret = sp->variant->cmd.set_boot_source(sp, boot_source);

	return (ret < 0) ? ret : count;
}

static DEVICE_ATTR(boot_source, 0640,
		   rave_sp_show_boot_source, rave_sp_store_boot_source);

static void devm_rave_sp_sysfs_group_release(struct device *dev, void *res)
{
	struct rave_sp *sp = *(struct rave_sp **)res;
	const struct attribute_group *group = &sp->variant->group;
	struct kobject *root = &sp->serdev->dev.kobj;

	sysfs_remove_group(root, group);
}

static int devm_rave_sysfs_create_group(struct rave_sp *sp)
{
	struct rave_sp **rcsp;
	struct device *dev = &sp->serdev->dev;
	const struct attribute_group *group = &sp->variant->group;
	struct kobject *root = &dev->kobj;
	int ret;

	rcsp = devres_alloc(devm_rave_sp_sysfs_group_release,
			     sizeof(*rcsp), GFP_KERNEL);
	if (!rcsp)
		return -ENOMEM;

	ret = sysfs_create_group(root, group);
	if (!ret) {
		*rcsp = sp;
		devres_add(dev, rcsp);
	} else {
		devres_free(rcsp);
	}

	return ret;
}

static void csum_8b2c(const u8 *buf, size_t size, u8 *crc)
{
	*crc = *buf++;
	size--;

	while (size--)
		*crc += *buf++;

	*crc = 1 + ~(*crc);
}

static void csum_ccitt(const u8 *buf, size_t size, u8 *crc)
{

	const u16 calculated = crc_ccitt_false(0xffff, buf, size);

	/*
	 * While the rest of the wire protocol is little-endian,
	 * CCITT-16 CRC in RDU2 device is sent out in big-endian order.
	 */
	put_unaligned_be16(calculated, crc);
}

static void *stuff(unsigned char *dest, const unsigned char *src, size_t n)
{
	while (n--) {
		const unsigned char byte = *src++;

		switch (byte) {
		case RAVE_SP_STX:
		case RAVE_SP_ETX:
		case RAVE_SP_DLE:
			*dest++ = RAVE_SP_DLE;
		default:
			*dest++ = byte;
		}
	}

	return dest;
}

static int rave_sp_write(struct rave_sp *sp, const u8 *data, u8 data_size)
{
	const size_t checksum_length = sp->variant->checksum->length;
	unsigned char frame[RAVE_SP_TX_BUFFER_SIZE];
	unsigned char crc[checksum_length];
	unsigned char *dest = frame;
	size_t length;

	if (WARN_ON(data_size > sizeof(frame)))
		return -ENOMEM;

	sp->variant->checksum->subroutine(data, data_size, crc);

	*dest++ = RAVE_SP_STX;
	dest = stuff(dest, data, data_size);
	dest = stuff(dest, crc, checksum_length);
	*dest++ = RAVE_SP_ETX;

	length = dest - frame;

	print_hex_dump(KERN_DEBUG, "rave-sp tx: ", DUMP_PREFIX_NONE,
		       16, 1, frame, length, false);

	return serdev_device_write(sp->serdev, frame, length, HZ);
}

static u8 rave_sp_reply_code(u8 command)
{
	/*
	 * There isn't a single rule that describes command code ->
	 * ACK code transformation, but, going through various
	 * versions of ICDs, there appear to be three distinct groups
	 * that can be described by simple transformation.
	 */
	switch (command) {
	case 0xA0 ... 0xBE:
		/*
		 * Commands implemented by firmware found in RDU1 and
		 * older devices all seem to obey the following rule
		 */
		return command + 0x20;
	case 0xE0 ... 0xEF:
		/*
		 * Events emitted by all versions of the firmare use
		 * least significant bit to get an ACK code
		 */
		return command | 0x01;
	default:
		/*
		 * Commands implemented by firmware found in RDU2 are
		 * similar to "old" commands, but they use slightly
		 * different offset
		 */
		return command + 0x40;
	}
}

int rave_sp_exec(struct rave_sp *sp,
		 void *__data,  size_t data_size,
		 void *reply_data, size_t reply_data_size)
{
	struct rave_sp_reply reply = {
		.data     = reply_data,
		.length   = reply_data_size,
		.received = COMPLETION_INITIALIZER_ONSTACK(reply.received),
	};
	unsigned char *data = __data;
	int command, ret = 0;
	u8 ackid;

	command = sp->variant->cmd.translate(data[0]);
	if (command < 0)
		return command;

	ackid       = atomic_inc_return(&sp->ackid);
	reply.ackid = ackid;
	reply.code  = rave_sp_reply_code((u8)command),

	mutex_lock(&sp->bus_lock);

	mutex_lock(&sp->reply_lock);
	sp->reply = &reply;
	mutex_unlock(&sp->reply_lock);

	data[0] = command;
	data[1] = ackid;

	rave_sp_write(sp, data, data_size);

	if (!wait_for_completion_timeout(&reply.received, HZ)) {
		dev_err(&sp->serdev->dev, "Command timeout\n");
		ret = -ETIMEDOUT;

		mutex_lock(&sp->reply_lock);
		sp->reply = NULL;
		mutex_unlock(&sp->reply_lock);
	}

	mutex_unlock(&sp->bus_lock);
	return ret;
}
EXPORT_SYMBOL(rave_sp_exec);

static void rave_sp_receive_event(struct rave_sp *sp,
				  const unsigned char *data, size_t length)
{
	u8 cmd[] = {
		[0] = rave_sp_reply_code(data[0]),
		[1] = data[1],
	};

	rave_sp_write(sp, cmd, sizeof(cmd));

	blocking_notifier_call_chain(&sp->event_notifier_list,
				     rave_sp_action_pack(data[0], data[2]),
				     NULL);
}

static void rave_sp_receive_reply(struct rave_sp *sp,
				  const unsigned char *data, size_t length)
{
	struct device *dev = &sp->serdev->dev;
	struct rave_sp_reply *reply;
	const  size_t payload_length = length - 2;

	mutex_lock(&sp->reply_lock);
	reply = sp->reply;

	if (reply) {
		if (reply->code == data[0] && reply->ackid == data[1] &&
		    payload_length >= reply->length) {
			/*
			 * We are relying on memcpy(dst, src, 0) to be a no-op
			 * when handling commands that have a no-payload reply
			 */
			memcpy(reply->data, &data[2], reply->length);
			complete(&reply->received);
			sp->reply = NULL;
		} else {
			dev_err(dev, "Ignoring incorrect reply\n");
			dev_dbg(dev, "Code:   expected = 0x%08x received = 0x%08x\n",
				reply->code, data[0]);
			dev_dbg(dev, "ACK ID: expected = 0x%08x received = 0x%08x\n",
				reply->ackid, data[1]);
			dev_dbg(dev, "Length: expected = %zu received = %zu\n",
				reply->length, payload_length);
		}
	}

	mutex_unlock(&sp->reply_lock);
}

static void rave_sp_receive_frame(struct rave_sp *sp,
				  const unsigned char *data,
				  size_t length)
{
	const size_t checksum_length = sp->variant->checksum->length;
	const size_t payload_length  = length - checksum_length;
	const u8 *crc_reported       = &data[payload_length];
	struct device *dev           = &sp->serdev->dev;
	u8 crc_calculated[checksum_length];

	print_hex_dump(KERN_DEBUG, "rave-sp rx: ", DUMP_PREFIX_NONE,
		       16, 1, data, length, false);

	if (unlikely(length <= checksum_length)) {
		dev_warn(dev, "Dropping short frame\n");
		return;
	}

	sp->variant->checksum->subroutine(data, payload_length,
					  crc_calculated);

	if (memcmp(crc_calculated, crc_reported, checksum_length)) {
		dev_warn(dev, "Dropping bad frame\n");
		return;
	}

	if (rave_sp_id_is_event(data[0]))
		rave_sp_receive_event(sp, data, length);
	else
		rave_sp_receive_reply(sp, data, length);
}

static int rave_sp_receive_buf(struct serdev_device *serdev,
			       const unsigned char *buf, size_t size)
{
	struct device *dev  = &serdev->dev;
	struct rave_sp *sp = dev_get_drvdata(dev);
	struct rave_sp_deframer *deframer = &sp->deframer;
	const unsigned char *src = buf;
	const unsigned char *end = buf + size;
	bool reset_framer = false;

	while (src < end) {
		const unsigned char byte = *src++;

		switch (deframer->state) {

		case RAVE_SP_EXPECT_SOF:
			if (byte == RAVE_SP_STX)
				deframer->state = RAVE_SP_EXPECT_DATA;
			continue;

		case RAVE_SP_EXPECT_DATA:
			switch (byte) {
			case RAVE_SP_ETX:
				rave_sp_receive_frame(sp,
						      deframer->data,
						      deframer->length);
				reset_framer = true;
				break;
			case RAVE_SP_STX:
				dev_warn(dev, "Bad frame: STX before ETX\n");
				reset_framer = true;
				break;
			case RAVE_SP_DLE:
				deframer->state = RAVE_SP_EXPECT_ESCAPED_DATA;
				continue;
			}

		case RAVE_SP_EXPECT_ESCAPED_DATA: /* FALLTHROUGH */
			deframer->data[deframer->length++] = byte;

			if (deframer->length == sizeof(deframer->data)) {
				dev_warn(dev, "Bad frame: Too long\n");
				reset_framer = true;
				break;
			}

			deframer->state = RAVE_SP_EXPECT_DATA;
			break;
		}
	}

	if (reset_framer) {
		deframer->state  = RAVE_SP_EXPECT_SOF;
		deframer->length = 0;
	}

	return src - buf;
}

static int rave_sp_rdu1_cmd_translate(enum rave_sp_command command)
{
	if (command >= RAVE_SP_CMD_STATUS &&
	    command <= RAVE_SP_CMD_CONTROL_EVENTS)
		return command;

	return -EINVAL;
}

static int rave_sp_rdu2_cmd_translate(enum rave_sp_command command)
{
	if (command >= RAVE_SP_CMD_GET_FIRMWARE_VERSION &&
	    command <= RAVE_SP_CMD_GET_GPIO_STATE)
		return command;

	if (command == RAVE_SP_CMD_REQ_COPPER_REV) {
		/*
		 * As per RDU2 ICD 3.4.47 CMD_GET_COPPER_REV code is
		 * different from that for RDU1 and it is set to 0x28
		 */
		return 0x28;
	}

	return rave_sp_rdu1_cmd_translate(command);
}

static int rave_sp_default_cmd_translate(enum rave_sp_command command)
{
	/*
	 * All of the following command codes were taken from "Table :
	 * Communications Protocol Message Types" in section 3.3
	 * "MESSAGE TYPES" of Rave PIC24 ICD
	 */
	switch (command) {
	case RAVE_SP_CMD_GET_FIRMWARE_VERSION:
		return 0x11;
	case RAVE_SP_CMD_GET_BOOTLOADER_VERSION:
		return 0x12;
	case RAVE_SP_CMD_BOOT_SOURCE:
		return 0x14;
	case RAVE_SP_CMD_SW_WDT:
		return 0x1C;
	case RAVE_SP_CMD_RESET:
		return 0x1E;
	case RAVE_SP_CMD_RESET_REASON:
		return 0x1F;
	default:
		return -EINVAL;
	}
}

static void rave_sp_load_reset_reason(struct rave_sp *sp)
{
	struct device *dev = &sp->serdev->dev;
	u8 cmd[] = {
		[0] = RAVE_SP_CMD_RESET_REASON,
		[1] = 0,
	};
	u8 reason;
	int ret;

	ret = rave_sp_exec(sp, cmd, sizeof(cmd), &reason, sizeof(reason));
	if (ret) {
		dev_err(dev, "CMD_RESET_REASON failed %d\n", ret);
		return;
	}

	sp->reset_reason = devm_kasprintf(dev, GFP_KERNEL, "%02x\n", reason);
}

static const char *rave_sp_silicon_to_string(struct device *dev, u32 version)
{
	return devm_kasprintf(dev, GFP_KERNEL, "%08x\n", version);
}

static const char *rave_sp_copper_to_string(struct device *dev, uint8_t version)
{
	return devm_kasprintf(dev, GFP_KERNEL, "%02x\n", version);
}

static void rave_sp_load_silicon_rev(struct rave_sp *sp)
{
	struct device *dev = &sp->serdev->dev;
	u8 cmd[] = {
		[0] = RAVE_SP_CMD_GET_SP_SILICON_REV,
		[1] = 0
	};
	struct {
		__le32 devid;
		__le32 devrev;
	} __packed reply;
	int ret;

	ret = rave_sp_exec(sp, cmd, sizeof(cmd), &reply, sizeof(reply));
	if (ret) {
		dev_err(dev, "CMD_GET_SP_SILICON_REV failed %d\n", ret);
		return;
	}

	reply.devid  = le32_to_cpu(reply.devid);
	reply.devrev = le32_to_cpu(reply.devrev);

	sp->silicon_devid  = rave_sp_silicon_to_string(dev, reply.devid);
	sp->silicon_devrev = rave_sp_silicon_to_string(dev, reply.devrev);
}

static void rave_sp_rdu1_init(struct rave_sp *sp)
{
	struct device *dev = &sp->serdev->dev;
	u8 cmd[] = {
		[0] = RAVE_SP_CMD_REQ_COPPER_REV,
		[1] = 0
	};
	struct rave_sp_rsp_status status;
	u8 revision[2];
	int ret;

	ret = rave_sp_get_status(sp, &status);
	if (ret) {
		dev_err(dev, "CMD_STATUS failed %d\n", ret);
	} else {
		sp->part_number_firmware =
			devm_rave_sp_version(dev, status.fw_bytes);
		sp->part_number_bootloader =
			devm_rave_sp_version(dev, status.bl_bytes);
	}

	ret = rave_sp_exec(sp, cmd, sizeof(cmd), &revision, sizeof(revision));
	if (ret) {
		dev_err(dev, "CMD_REQ_COPPER_REV failed %d\n", ret);
		return;
	}

	sp->copper_rev_rmb = rave_sp_copper_to_string(dev, revision[0]);
	sp->copper_rev_deb = rave_sp_copper_to_string(dev, revision[1]);
}

static void rave_sp_common_init(struct rave_sp *sp)
{
	struct device *dev = &sp->serdev->dev;
	u8 version[6];
	u8 cmd[2];
	int ret;

	cmd[0] = RAVE_SP_CMD_GET_FIRMWARE_VERSION;
	ret = rave_sp_exec(sp, cmd, sizeof(cmd), version, sizeof(version));
	if (ret)
		dev_warn(dev, "CMD_GET_FIRMWARE_VERSION failed %d\n", ret);
	else
		sp->part_number_firmware = devm_rave_sp_version(dev, version);

	cmd[0] = RAVE_SP_CMD_GET_BOOTLOADER_VERSION;
	ret = rave_sp_exec(sp, cmd, sizeof(cmd), version, sizeof(version));
	if (ret) {
		dev_warn(dev, "CMD_GET_BOOTLOADER_VERSION failed %d\n", ret);
		return;
	}

	sp->part_number_bootloader = devm_rave_sp_version(dev, version);
}

static void rave_sp_rdu2_init(struct rave_sp *sp)
{
	struct device *dev = &sp->serdev->dev;
	u8 cmd[] = {
		[0] = RAVE_SP_CMD_REQ_COPPER_REV,
		[1] = 0,
		[2] = RAVE_SP_RDU2_BOARD_TYPE_RMB,
	};
	u8 copper_rev;
	int ret;

	rave_sp_common_init(sp);

	ret = rave_sp_exec(sp, cmd, sizeof(cmd),
			   &copper_rev, sizeof(copper_rev));
	if (ret) {
		dev_warn(dev,
			 "RAVE_SP_CMD_REQ_COPPER_REV(RMB) failed %d\n", ret);
	} else {
		sp->copper_rev_rmb =
			rave_sp_copper_to_string(dev, copper_rev & 0x1F);
		sp->copper_mod_rmb =
			rave_sp_copper_to_string(dev, copper_rev >> 5);
	}

	cmd[2] = RAVE_SP_RDU2_BOARD_TYPE_DEB;

	ret = rave_sp_exec(sp, cmd, sizeof(cmd),
			   &copper_rev, sizeof(copper_rev));
	if (ret) {
		dev_warn(dev,
			 "RAVE_SP_CMD_REQ_COPPER_REV(DEB) failed %d\n", ret);
		return;
	}

	sp->copper_rev_deb = rave_sp_copper_to_string(dev,
						      copper_rev & 0x1F);
	sp->copper_mod_deb = rave_sp_copper_to_string(dev,
						      copper_rev >> 5);
}

static const struct rave_sp_checksum rave_sp_checksum_8b2c = {
	.length     = 1,
	.subroutine = csum_8b2c,
};

static const struct rave_sp_checksum rave_sp_checksum_ccitt = {
	.length     = 2,
	.subroutine = csum_ccitt,
};

static struct attribute *rave_sp_common_attrs[] = {
	&dev_attr_part_number_firmware.attr,
	&dev_attr_part_number_bootloader.attr,
	&dev_attr_boot_source.attr,
	&dev_attr_reset_reason.attr,
	NULL
};

static struct attribute *rave_sp_rdu1_attrs[] = {
	&dev_attr_part_number_firmware.attr,
	&dev_attr_part_number_bootloader.attr,
	&dev_attr_boot_source.attr,
	&dev_attr_reset_reason.attr,
	&dev_attr_copper_rev_deb.attr,
	&dev_attr_copper_rev_rmb.attr,
	&dev_attr_i2c_device_status.attr,
	&dev_attr_silicon_devid.attr,
	&dev_attr_silicon_devrev.attr,

	NULL
};

static struct attribute *rave_sp_rdu2_attrs[] = {
	&dev_attr_part_number_firmware.attr,
	&dev_attr_part_number_bootloader.attr,
	&dev_attr_boot_source.attr,
	&dev_attr_reset_reason.attr,
	&dev_attr_copper_rev_deb.attr,
	&dev_attr_copper_rev_rmb.attr,
	&dev_attr_i2c_device_status.attr,
	&dev_attr_silicon_devid.attr,
	&dev_attr_silicon_devrev.attr,

	&dev_attr_copper_mod_rmb.attr,
	&dev_attr_copper_mod_deb.attr,

	NULL
};

static const struct rave_sp_variant rave_sp_legacy = {
	.checksum = &rave_sp_checksum_8b2c,
	.cmd = {
		.translate = rave_sp_default_cmd_translate,
		.get_boot_source = rave_sp_common_get_boot_source,
		.set_boot_source = rave_sp_common_set_boot_source,
	},
	.group = {
		.attrs = rave_sp_common_attrs,
	},
	.init = rave_sp_common_init,
};

static const struct rave_sp_variant rave_sp_rdu1 = {
	.checksum = &rave_sp_checksum_8b2c,
	.cmd = {
		.translate = rave_sp_rdu1_cmd_translate,
		.get_boot_source = rave_sp_rdu1_get_boot_source,
		.set_boot_source = rave_sp_rdu1_set_boot_source,
	},
	.group = {
		.attrs = rave_sp_rdu1_attrs,
	},
	.init = rave_sp_rdu1_init,
};

static const struct rave_sp_variant rave_sp_rdu2 = {
	.checksum = &rave_sp_checksum_ccitt,
	.cmd = {
		.translate = rave_sp_rdu2_cmd_translate,
		.get_boot_source = rave_sp_common_get_boot_source,
		.set_boot_source = rave_sp_common_set_boot_source,
	},
	.group = {
		.attrs = rave_sp_rdu2_attrs,
	},
	.init = rave_sp_rdu2_init,
};

static const struct of_device_id rave_sp_dt_ids[] = {
	{ .compatible = COMPATIBLE_RAVE_SP_NIU,  .data = &rave_sp_legacy },
	{ .compatible = COMPATIBLE_RAVE_SP_MEZZ, .data = &rave_sp_legacy },
	{ .compatible = COMPATIBLE_RAVE_SP_ESB,  .data = &rave_sp_legacy },
	{ .compatible = COMPATIBLE_RAVE_SP_RDU1, .data = &rave_sp_rdu1   },
	{ .compatible = COMPATIBLE_RAVE_SP_RDU2, .data = &rave_sp_rdu2   },
	{ /* sentinel */ }
};

static const struct serdev_device_ops rave_sp_serdev_device_ops = {
	.receive_buf  = rave_sp_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

static int rave_sp_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	const char *unknown = "unknown\n";
	struct rave_sp *sp;
	u32 baud;
	int ret;

	if (of_property_read_u32(dev->of_node, "current-speed", &baud)) {
		dev_err(dev,
			"'current-speed' is not specified in device node\n");
		return -EINVAL;
	}

	sp = devm_kzalloc(dev, sizeof(*sp), GFP_KERNEL);
	if (!sp)
		return -ENOMEM;

	sp->serdev = serdev;
	dev_set_drvdata(dev, sp);

	sp->variant = of_device_get_match_data(dev);
	if (!sp->variant)
		return -ENODEV;

	mutex_init(&sp->bus_lock);
	mutex_init(&sp->reply_lock);
	BLOCKING_INIT_NOTIFIER_HEAD(&sp->event_notifier_list);

	serdev_device_set_client_ops(serdev, &rave_sp_serdev_device_ops);
	ret = serdev_device_open(serdev);
	if (ret)
		return ret;

	serdev_device_set_baudrate(serdev, baud);

	sp->silicon_devid		= unknown;
	sp->silicon_devrev		= unknown;
	sp->copper_rev_deb		= unknown;
	sp->copper_rev_rmb		= unknown;
	sp->copper_mod_deb		= unknown;
	sp->copper_mod_rmb		= unknown;
	sp->reset_reason		= unknown;
	sp->part_number_firmware	= unknown;
	sp->part_number_bootloader	= unknown;

	sp->variant->init(sp);

	/*
	 * Those strings already have a \n embedded so no need to have
	 * one in format string.
	 */
	dev_info(dev, "Firmware version: %s", sp->part_number_firmware);
	dev_info(dev, "Bootloader version: %s", sp->part_number_bootloader);

	rave_sp_load_reset_reason(sp);
	rave_sp_load_silicon_rev(sp);

	ret = devm_rave_sysfs_create_group(sp);
	if (ret) {
		serdev_device_close(serdev);
		return ret;
	}

	return of_platform_default_populate(dev->of_node, NULL, dev);
}

static void rave_sp_remove(struct serdev_device *serdev)
{
	of_platform_depopulate(&serdev->dev);
	serdev_device_close(serdev);
}

MODULE_DEVICE_TABLE(of, rave_sp_dt_ids);

static struct serdev_device_driver rave_sp_drv = {
	.probe			= rave_sp_probe,
	.remove			= rave_sp_remove,
	.driver = {
		.name		= "rave-sp",
		.of_match_table	= rave_sp_dt_ids,
	},
};
module_serdev_device_driver(rave_sp_drv);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Andrey Vostrikov <andrey.vostrikov@cogentembedded.com>");
MODULE_AUTHOR("Nikita Yushchenko <nikita.yoush@cogentembedded.com>");
MODULE_AUTHOR("Andrey Smirnov <andrew.smirnov@gmail.com>");
MODULE_DESCRIPTION("RAVE SP core driver");
