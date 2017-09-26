#ifndef _UAPI_DELL_WMI_SMBIOS_H_
#define _UAPI_DELL_WMI_SMBIOS_H_

#include <linux/ioctl.h>

/* If called through fallback SMI rather than WMI this structure will be
 * modified by the firmware when we enter system management mode, hence the
 * volatiles
 */
struct calling_interface_buffer {
	u16 class;
	u16 select;
	volatile u32 input[4];
	volatile u32 output[4];
} __packed;

struct wmi_calling_interface_buffer {
	struct calling_interface_buffer smi;
	u32 argattrib;
	u32 blength;
	u8 data[32724];
} __packed;

struct calling_interface_token {
	u16 tokenID;
	u16 location;
	union {
		u16 value;
		u16 stringlength;
	};
};

struct token_ioctl_buffer {
	struct calling_interface_token *tokens;
	u32 num_tokens;
};

#define DELL_WMI_SMBIOS_IOC			'D'
/* run SMBIOS calling interface command
 * note - 32k is too big for size, so this can not be encoded in macro properly
 */
#define DELL_WMI_SMBIOS_CALL_CMD	_IOWR(DELL_WMI_SMBIOS_IOC, 0, u8)

/* query the number of DA tokens on system */
#define DELL_WMI_SMBIOS_GET_NUM_TOKENS_CMD	_IOR(DELL_WMI_SMBIOS_IOC, 1, \
						      u32)
/* query the status, location, and value of all DA tokens from bootup
 * expects userspace to prepare buffer in advance with the number of tokens
 * from DELL_WMI_SMBIOS_GET_NUM_TOKENS_CMD
 */
#define DELL_WMI_SMBIOS_GET_TOKENS_CMD		_IOWR(DELL_WMI_SMBIOS_IOC, 2, \
						      struct token_ioctl_buffer)


#endif /* _UAPI_DELL_WMI_SMBIOS_H_ */
