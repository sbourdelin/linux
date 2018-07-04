// SPDX-License-Identifier: GPL-2.0

#ifndef _QCOM_BANDWIDTH_PERF_EVENTS_H_
#define _QCOM_BANDWIDTH_PERF_EVENTS_H_

#include<linux/bitops.h>

/*
 * General constants
 */


#define BANDWIDTH_NUM_EVENT_COUNTERS    12
#define BANDWIDTH_NUM_TOTAL_COUNTERS    BANDWIDTH_NUM_EVENT_COUNTERS
#define BANDWIDTH_LAR_KEY               0xC5ACCE55

/*
 * Register offsets
 */

/* ID and Coresight registers */

#define BANDWIDTH_LAR           0xFB0


/* Event counter registers */

/*
 * Because of interleaving, some gaps in the map exists
 * (7th bit cannot be used).
 * To accommodate this mapping,
 * we have different offsets for different sets of counters.
 */


static inline u32 qcom_bandwidth_ec_source_sel(u8 __cntr)
{
	if (__cntr >= 0 && __cntr <= 2)
		return (0x240 + ((__cntr) & 0xF) * 24);
	else if (__cntr >= 3 && __cntr <= 7)
		return (0x2C0 + ((__cntr) & 0xF) * 24);
	else if (__cntr >= 8 && __cntr <= 13)
		return (0x340 + ((__cntr) & 0xF) * 24);
	else
		return (0x3C0 + ((__cntr) & 0xF) * 24);
}


#define BANDWIDTH_EC_GLOBAL_CONTROL             0xA00
#define BANDWIDTH_EC_ENABLE_SET                 0xA10
#define BANDWIDTH_EC_ENABLE_CLEAR               0xA18
#define BANDWIDTH_EC_INTERRUPT_ENABLE_SET       0xA20
#define BANDWIDTH_EC_INTERRUPT_ENABLE_CLEAR     0xA28
#define BANDWIDTH_EC_TRIGGER_THRESHOLD_LO       0xA30
#define BANDWIDTH_EC_TRIGGER_THRESHOLD_HI       0xC30
#define BANDWIDTH_EC_GANG                       0xE30
#define BANDWIDTH_EC_GANG_CONFIG0               0xE38
#define BANDWIDTH_EC_GANG_CONFIG1               0xE40
#define BANDWIDTH_EC_GANG_CONFIG2               0xE48
#define BANDWIDTH_EC_OVF_STATUS                 0xF00
#define BANDWIDTH_EC_COUNTER_SEL                0xF08
#define BANDWIDTH_EC_COUNT                      0xF10
#define BANDWIDTH_EC_SWINC                      0x1320
#define BANDWIDTH_EC_IRQ_CONTROL                0x1358

/* IRQ/resource position in ACPI */
#define IRQ_BW                  2
#define RES_BW                  4
#define DDRBW_PMU_NAME_FORMAT   "bwddr_0_%ld"
#define DDRBW_PMU_NAME_LEN      11
#define DDRBW_MAX_RETRIES       3
#define DDR_BW_READ_FAIL        0
/*
 * Bit field definitions, defined as (<size>, <shift>)
 * Please note that fields that take up the whole register
 * are not included here, as those can be set/read directly.
 */

/* BANDWIDTH_EC_SOURCE_SEL */
#define ECSOURCESEL                     (7, 16)
#define ECEVENTSEL                      (4,  0)



/* BANDWIDTH_EC_GLOBAL_CONTROL/MONACO_TC_GLOBAL_CONTROL */

#define GLOBAL_TRIGOVRD                 (1, 4)
#define CAPTURE                         (1, 3)
#define RETRIEVAL_MODE                  (1, 2)
#define GLOBAL_RESET                    (1, 1)
#define GLOBAL_ENABLE                   (1, 0)

/* MONACO_EC_ROLLOVER_CONTROL */

#define ECSATURATEEN(__cntr)            (1, ((__cntr) & 0xF))

/* MONACO_EC_ENABLE_SET */

#define ECENSET(__cntr)                 (1, ((__cntr) & 0xF))

/* MONACO_EC_ENABLE_CLEAR */

#define ECENCLEAR(__cntr)               (1, ((__cntr) & 0xF))

/* MONACO_EC_INTERRUPT_ENABLE_SET */

#define ECINTENSET(__cntr)              (1, ((__cntr) & 0xF))

/* MONACO_EC_INTERRUPT_ENABLE_CLEAR */

#define ECINTENCLR(__cntr)              (1, ((__cntr) & 0xF))

/* MONACO_EC_GANG */

#define ECGANGEN(__pair)                (1, (((__pair) & 0x7) * 2 + 1))

/* MONACO_EC_OVF_STATUS */

#define ECOVF(__cntr)                   (1, ((__cntr) & 0xF))

/* MONACO_EC_COUNTER_SEL */

#define ECSEL                           (4, 0)

/* MONACO_EC_SWINC */

#define ECSWINC(__cntr)                 (1, ((__cntr) & 0xF))


/* MONACO_LSR */

#define NTT                             (1,  2)
#define SLK                             (1,  1)
#define SLI                             (1,  0)

/*
 * Bit field manipulation macros.
 * These use the bitfield definitions above to set or get the given field.
 */

#define __SIZE(__sz, __sh) __sz
#define __SHIFT(__sz, __sh) __sh
#define __SETI(__b, __s, __v) ((u32)(((__v) & GENMASK((__b - 1), 0)) << (__s)))
#define __CLRI(__b, __s, __v) ((u32)((__v) & ~(GENMASK((__b - 1) + __s, __s))))
#define __GETI(__b, __s, __v) ((u32)(((__v) >> (__s)) & GENMASK((__b - 1), 0)))

/* Return a value with the given bitfield set to the given value */
#define SET(__f, __v) __SETI(__SIZE __f, __SHIFT __f, (__v))

/* Return a value with the given bitfield set to zero */
#define CLR(__f, __v) __CLRI(__SIZE __f, __SHIFT __f, (__v))

/* Retrieve the given bitfield from the given value */
#define GET(__f, __v) __GETI(__SIZE __f, __SHIFT __f, (__v))

#endif
