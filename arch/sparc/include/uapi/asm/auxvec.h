#ifndef __ASMSPARC_AUXVEC_H
#define __ASMSPARC_AUXVEC_H

#define AT_SYSINFO_EHDR		33

#define AT_ADI_BLKSZ	48
#define AT_ADI_NBITS	49
#define AT_ADI_UEONADI	50

/*
 * Do not add new AT_* definitions here without coordinating with
 * <uapi/linux/auxvec.h>
 */

#define AT_VECTOR_SIZE_ARCH	4

#endif /* !(__ASMSPARC_AUXVEC_H) */
