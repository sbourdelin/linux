#ifndef _DW_MMC_ZX_H_
#define _DW_MMC_ZX_H_

/* dll reg offset*/
#define LB_AON_EMMC_CFG_REG0  0x1B0
#define LB_AON_EMMC_CFG_REG1  0x1B4
#define LB_AON_EMMC_CFG_REG2  0x1B8

/* LB_AON_EMMC_CFG_REG0 register defines */
#define PARA_DLL_START_POINT(x)	(((x) & 0xFF) << 0)
#define DLL_REG_SET		BIT(8)
#define PARA_DLL_LOCK_NUM(x)	(((x) & 7) << 16)
#define PARA_PHASE_DET_SEL(x)	(((x) & 7) << 20)
#define PARA_DLL_BYPASS_MODE	BIT(23)
#define PARA_HALF_CLK_MODE	BIT(24)

/* LB_AON_EMMC_CFG_REG1 register defines */
#define READ_DQS_DELAY(x)	(((x) & 0x7F) << 0)
#define READ_DQS_BYPASS_MODE	BIT(7)
#define CLK_SAMP_DELAY(x)	(((x) & 0x7F) << 8)
#define CLK_SAMP_BYPASS_MODE	BIT(15)

#endif /* _DW_MMC_ZX_H_ */
