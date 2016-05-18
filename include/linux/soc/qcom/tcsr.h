#ifndef __QCOM_TCSR_H
#define __QCOM_TCSR_H

#ifdef CONFIG_QCOM_TCSR
int qcom_tcsr_phy_sel(u32 val);
#else
static inline int qcom_tcsr_phy_sel(u32 val)
{
	return 0;
}
#endif

#endif
