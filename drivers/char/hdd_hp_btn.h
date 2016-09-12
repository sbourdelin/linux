//signal
#define HDD_SWAP_SIG 44
#define SIG_BUTTON1_INVOKE  0x01
#define SIG_BUTTON2_INVOKE  0x02
#define SIG_HDD1_INSERT 0x03
#define SIG_HDD2_INSERT 0x04

//IOCTL
struct ioctl_cmd {
	unsigned int reg;
	unsigned int offset;
	unsigned int val;
};

#define IOC_MAGIC 'd'
#define IOCTL_LED_ON _IOW(IOC_MAGIC, 1, struct ioctl_cmd)
#define IOCTL_LED_OFF _IOW(IOC_MAGIC, 2, struct ioctl_cmd)

#define true 1
#define false 0
