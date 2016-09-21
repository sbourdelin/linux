#ifndef __NANDSIM_USER_H__
#define __NANDSIM_USER_H__

#include <linux/types.h>

#define NANDSIM_IOC_MAGIC 'n'

#define NANDSIM_IOC_NEW_INSTANCE _IOW(NANDSIM_IOC_MAGIC, 0, struct ns_new_instance_req)
#define NANDSIM_IOC_DESTROY_INSTANCE _IOW(NANDSIM_IOC_MAGIC, 1, struct ns_destroy_instance_req)

#define NANDSIM_MAX_DEVICES 32
#define NANDSIM_MAX_PARTS 32

enum ns_backend_type {
	NANDSIM_BACKEND_RAM = 0,
	NANDSIM_BACKEND_CACHEFILE = 1,
	NANDSIM_BACKEND_FILE = 2,
	NANDSIM_BACKEND_MAX,
};

/**
 * struct ns_new_instance_req - Create a new nandsim instance.
 *
 * @id_bytes: NAND ID of the simulated NAND chip
 * @bus_width: bus width to emulate, either 8 or 16
 * @bbt_mode: bad block table mode, 0 OOB, 1 BBT with marker in OOB,
 *            2 BBT with marker in data area
 * @no_oob: backing file contains no OOB data
 * @bch_strength: instead of hamming ECC use BCH with given strength
 * @parts_num: number of MTD partitions to create
 * @parts: partition sizes in physical erase blocks, used then @parts_num > 0
 * @backend: backend type, see @ns_backend_type
 * @file_fd: file describtor of backend, only for @NANDSIM_BACKEND_CACHEFILE
 *           and @NANDSIM_BACKEND_FILE.
 * @bitflips: maximum number of random bit flips per page
 * @overridesize: specifies the NAND size overriding the ID bytes
 * @access_delay: initial page access delay (microseconds)
 * @program_delay: page program delay (microseconds)
 * @erase_delay: sector erase delay (milliseconds)
 * @output_cycle: word output, from flash, time (nanoseconds)
 * @input_cycle: word input, to flash, time (nanoseconds)
 * @simelem_num: number of simulation elements appened to this
 *               data structure. see @ns_simelement_prop
 *
 * This struct is used with the @NANDSIM_IOC_NEW_INSTANCE ioctl command.
 * It creates a new nandsim instance from the given parameter.
 * The ioctl command returns in case of success the nandsim id of the new
 * instance, in case of error a negative value.
 *
 * Not all fields in the struct have to be filled, if nandsim should
 * use a default ignore the value, fill with 0.
 * The only mandatory fields are @id_bytes and @bus_width.
 * When @no_oob is non-zero @bch_strength cannot be used since
 * @no_oob implies that no ECC is used.
 */
struct ns_new_instance_req {
	__s8 id_bytes[8];

	__s8 bus_width;
	__s8 bbt_mode;
	__s8 no_oob;
	__s32 bch_strength;

	__s8 parts_num;
	__s32 parts[NANDSIM_MAX_PARTS];

	__s8 backend;
	__s32 file_fd;

	__s32 bitflips;
	__s32 overridesize;
	__s32 access_delay;
	__s32 program_delay;
	__s32 erase_delay;
	__s32 output_cycle;
	__s32 input_cycle;

	__s32 padding[4];

	__s32 simelem_num;
} __packed;

enum {
	NANDSIM_SIMELEM_BADBLOCK = 0,
	NANDSIM_SIMELEM_WEAKBLOCK,
	NANDSIM_SIMELEM_WEAKPAGE,
	NANDSIM_SIMELEM_GRAVEPAGE,
};

struct ns_simelement_prop {
	__s8 elem_type;
	__s32 elem_id;
	__s32 elem_attr;
	__s8 padding[7];
} __packed;

struct ns_destroy_instance_req {
	__s8 id;
	__s8 padding[7];
} __packed;

#endif /* __NANDSIM_USER_H__ */
