/*
 * Synopsys DesignWare Ethernet Driver
 *
 * Copyright (c) 2014-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * This file is free software; you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
 * your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *     The Synopsys DWC ETHER XGMAC Software Driver and documentation
 *     (hereinafter "Software") is an unsupported proprietary work of Synopsys,
 *     Inc. unless otherwise expressly agreed to in writing between Synopsys
 *     and you.
 *
 *     The Software IS NOT an item of Licensed Software or Licensed Product
 *     under any End User Software License Agreement or Agreement for Licensed
 *     Product with Synopsys or any supplement thereto.  Permission is hereby
 *     granted, free of charge, to any person obtaining a copy of this software
 *     annotated with this license and the Software, to deal in the Software
 *     without restriction, including without limitation the rights to use,
 *     copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *     of the Software, and to permit persons to whom the Software is furnished
 *     to do so, subject to the following conditions:
 *
 *     The above copyright notice and this permission notice shall be included
 *     in all copies or substantial portions of the Software.
 *
 *     THIS SOFTWARE IS BEING DISTRIBUTED BY SYNOPSYS SOLELY ON AN "AS IS"
 *     BASIS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *     TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *     PARTICULAR PURPOSE ARE HEREBY DISCLAIMED. IN NO EVENT SHALL SYNOPSYS
 *     BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *     CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *     SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *     INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *     ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *     THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DWC_ETH_REGACC_H__
#define __DWC_ETH_REGACC_H__

/* DMA register offsets */
#define DMA_MR				0x3000
#define DMA_SBMR			0x3004
#define DMA_ISR				0x3008
#define DMA_AXIARCR			0x3010
#define DMA_AXIAWCR			0x3018
#define DMA_DSR0			0x3020
#define DMA_DSR1			0x3024
#define DMA_TXEDMACR			0x3040
#define DMA_RXEDMACR			0x3044

/* DMA register entry bit positions and sizes */
#define DMA_AXIARCR_DRC_POS		0
#define DMA_AXIARCR_DRC_LEN		4
#define DMA_AXIARCR_DRD_POS		4
#define DMA_AXIARCR_DRD_LEN		2
#define DMA_AXIARCR_TEC_POS		8
#define DMA_AXIARCR_TEC_LEN		4
#define DMA_AXIARCR_TED_POS		12
#define DMA_AXIARCR_TED_LEN		2
#define DMA_AXIARCR_THC_POS		16
#define DMA_AXIARCR_THC_LEN		4
#define DMA_AXIARCR_THD_POS		20
#define DMA_AXIARCR_THD_LEN		2
#define DMA_AXIAWCR_DWC_POS		0
#define DMA_AXIAWCR_DWC_LEN		4
#define DMA_AXIAWCR_DWD_POS		4
#define DMA_AXIAWCR_DWD_LEN		2
#define DMA_AXIAWCR_RPC_POS		8
#define DMA_AXIAWCR_RPC_LEN		4
#define DMA_AXIAWCR_RPD_POS		12
#define DMA_AXIAWCR_RPD_LEN		2
#define DMA_AXIAWCR_RHC_POS		16
#define DMA_AXIAWCR_RHC_LEN		4
#define DMA_AXIAWCR_RHD_POS		20
#define DMA_AXIAWCR_RHD_LEN		2
#define DMA_AXIAWCR_TDC_POS		24
#define DMA_AXIAWCR_TDC_LEN		4
#define DMA_AXIAWCR_TDD_POS		28
#define DMA_AXIAWCR_TDD_LEN		2
#define DMA_ISR_MACIS_POS		17
#define DMA_ISR_MACIS_LEN		1
#define DMA_ISR_MTLIS_POS		16
#define DMA_ISR_MTLIS_LEN		1
#define DMA_MR_SWR_POS			0
#define DMA_MR_SWR_LEN			1
#define DMA_SBMR_EAME_POS		11
#define DMA_SBMR_EAME_LEN		1
#define DMA_SBMR_BLEN_64_POS		5
#define DMA_SBMR_BLEN_64_LEN		1
#define DMA_SBMR_BLEN_128_POS		6
#define DMA_SBMR_BLEN_128_LEN		1
#define DMA_SBMR_BLEN_256_POS		7
#define DMA_SBMR_BLEN_256_LEN		1
#define DMA_SBMR_UNDEF_POS		0
#define DMA_SBMR_UNDEF_LEN		1
#define DMA_TXEDMACR_TDPS_POS		0
#define DMA_TXEDMACR_TDPS_LEN		3
#define DMA_TXEDMACR_TEDM_POS		30
#define DMA_TXEDMACR_TEDM_LEN		2
#define DMA_RXEDMACR_RDPS_POS		0
#define DMA_RXEDMACR_RDPS_LEN		3
#define DMA_RXEDMACR_REDM_POS		30
#define DMA_RXEDMACR_REDM_LEN		2

/* DMA register values */
#define DMA_DSR_RPS_LEN			4
#define DMA_DSR_TPS_LEN			4
#define DMA_DSR_Q_LEN			(DMA_DSR_RPS_LEN + DMA_DSR_TPS_LEN)
#define DMA_DSR0_RPS_START		8
#define DMA_DSR0_TPS_START		12
#define DMA_DSRX_FIRST_QUEUE		3
#define DMA_DSRX_INC			4
#define DMA_DSRX_QPR			4
#define DMA_DSRX_RPS_START		0
#define DMA_DSRX_TPS_START		4
#define DMA_TPS_STOPPED			0x00
#define DMA_TPS_SUSPENDED		0x06

/* DMA channel register offsets
 *   Multiple channels can be active.  The first channel has registers
 *   that begin at 0x3100.  Each subsequent channel has registers that
 *   are accessed using an offset of 0x80 from the previous channel.
 */
#define DMA_CH_BASE			0x3100
#define DMA_CH_INC			0x80

#define DMA_CH_CR			0x00
#define DMA_CH_TCR			0x04
#define DMA_CH_RCR			0x08
#define DMA_CH_TDLR_HI			0x10
#define DMA_CH_TDLR_LO			0x14
#define DMA_CH_RDLR_HI			0x18
#define DMA_CH_RDLR_LO			0x1c
#define DMA_CH_TDTR_LO			0x24
#define DMA_CH_RDTR_LO			0x2c
#define DMA_CH_TDRLR			0x30
#define DMA_CH_RDRLR			0x34
#define DMA_CH_IER			0x38
#define DMA_CH_RIWT			0x3c
#define DMA_CH_CATDR_LO			0x44
#define DMA_CH_CARDR_LO			0x4c
#define DMA_CH_CATBR_HI			0x50
#define DMA_CH_CATBR_LO			0x54
#define DMA_CH_CARBR_HI			0x58
#define DMA_CH_CARBR_LO			0x5c
#define DMA_CH_SR			0x60

/* DMA channel register entry bit positions and sizes */
#define DMA_CH_CR_PBLX8_POS		16
#define DMA_CH_CR_PBLX8_LEN		1
#define DMA_CH_CR_SPH_POS		24
#define DMA_CH_CR_SPH_LEN		1
#define DMA_CH_IER_AIE_POS		15
#define DMA_CH_IER_AIE_LEN		1
#define DMA_CH_IER_FBEE_POS		12
#define DMA_CH_IER_FBEE_LEN		1
#define DMA_CH_IER_NIE_POS		16
#define DMA_CH_IER_NIE_LEN		1
#define DMA_CH_IER_RBUE_POS		7
#define DMA_CH_IER_RBUE_LEN		1
#define DMA_CH_IER_RIE_POS		6
#define DMA_CH_IER_RIE_LEN		1
#define DMA_CH_IER_RSE_POS		8
#define DMA_CH_IER_RSE_LEN		1
#define DMA_CH_IER_TBUE_POS		2
#define DMA_CH_IER_TBUE_LEN		1
#define DMA_CH_IER_TIE_POS		0
#define DMA_CH_IER_TIE_LEN		1
#define DMA_CH_IER_TXSE_POS		1
#define DMA_CH_IER_TXSE_LEN		1
#define DMA_CH_RCR_PBL_POS		16
#define DMA_CH_RCR_PBL_LEN		6
#define DMA_CH_RCR_RBSZ_POS		1
#define DMA_CH_RCR_RBSZ_LEN		14
#define DMA_CH_RCR_SR_POS		0
#define DMA_CH_RCR_SR_LEN		1
#define DMA_CH_RIWT_RWT_POS		0
#define DMA_CH_RIWT_RWT_LEN		8
#define DMA_CH_SR_FBE_POS		12
#define DMA_CH_SR_FBE_LEN		1
#define DMA_CH_SR_RBU_POS		7
#define DMA_CH_SR_RBU_LEN		1
#define DMA_CH_SR_RI_POS		6
#define DMA_CH_SR_RI_LEN		1
#define DMA_CH_SR_RPS_POS		8
#define DMA_CH_SR_RPS_LEN		1
#define DMA_CH_SR_TBU_POS		2
#define DMA_CH_SR_TBU_LEN		1
#define DMA_CH_SR_TI_POS		0
#define DMA_CH_SR_TI_LEN		1
#define DMA_CH_SR_TPS_POS		1
#define DMA_CH_SR_TPS_LEN		1
#define DMA_CH_TCR_OSP_POS		4
#define DMA_CH_TCR_OSP_LEN		1
#define DMA_CH_TCR_PBL_POS		16
#define DMA_CH_TCR_PBL_LEN		6
#define DMA_CH_TCR_ST_POS		0
#define DMA_CH_TCR_ST_LEN		1
#define DMA_CH_TCR_TSE_POS		12
#define DMA_CH_TCR_TSE_LEN		1

/* DMA channel register values */
#define DMA_OSP_DISABLE			0x00
#define DMA_OSP_ENABLE			0x01
#define DMA_PBL_1			1
#define DMA_PBL_2			2
#define DMA_PBL_4			4
#define DMA_PBL_8			8
#define DMA_PBL_16			16
#define DMA_PBL_32			32
#define DMA_PBL_64			64      /* 8 x 8 */
#define DMA_PBL_128			128     /* 8 x 16 */
#define DMA_PBL_256			256     /* 8 x 32 */
#define DMA_PBL_X8_DISABLE		0x00
#define DMA_PBL_X8_ENABLE		0x01

/* MAC register offsets */
#define MAC_TCR				0x0000
#define MAC_RCR				0x0004
#define MAC_PFR				0x0008
#define MAC_WTR				0x000c
#define MAC_HTR0			0x0010
#define MAC_VLANTR			0x0050
#define MAC_VLANHTR			0x0058
#define MAC_VLANIR			0x0060
#define MAC_IVLANIR			0x0064
#define MAC_RETMR			0x006c
#define MAC_Q0TFCR			0x0070
#define MAC_RFCR			0x0090
#define MAC_RQC0R			0x00a0
#define MAC_RQC1R			0x00a4
#define MAC_RQC2R			0x00a8
#define MAC_RQC3R			0x00ac
#define MAC_ISR				0x00b0
#define MAC_IER				0x00b4
#define MAC_RTSR			0x00b8
#define MAC_PMTCSR			0x00c0
#define MAC_RWKPFR			0x00c4
#define MAC_LPICSR			0x00d0
#define MAC_LPITCR			0x00d4
#define MAC_VR				0x0110
#define MAC_DR				0x0114
#define MAC_HWF0R			0x011c
#define MAC_HWF1R			0x0120
#define MAC_HWF2R			0x0124
#define MAC_MDIOSCAR			0x0200
#define MAC_MDIOSCCDR			0x0204
#define MAC_GPIOCR			0x0278
#define MAC_GPIOSR			0x027c
#define MAC_MACA0HR			0x0300
#define MAC_MACA0LR			0x0304
#define MAC_MACA1HR			0x0308
#define MAC_MACA1LR			0x030c
#define MAC_RSSCR			0x0c80
#define MAC_RSSAR			0x0c88
#define MAC_RSSDR			0x0c8c
#define MAC_TSCR			0x0d00
#define MAC_SSIR			0x0d04
#define MAC_STSR			0x0d08
#define MAC_STNR			0x0d0c
#define MAC_STSUR			0x0d10
#define MAC_STNUR			0x0d14
#define MAC_TSAR			0x0d18
#define MAC_TSSR			0x0d20
#define MAC_TXSNR			0x0d30
#define MAC_TXSSR			0x0d34

#define MAC_QTFCR_INC			4
#define MAC_MACA_INC			4
#define MAC_HTR_INC			4

#define MAC_RQC2_INC			4
#define MAC_RQC2_Q_PER_REG		4

/* MAC register entry bit positions and sizes */
#define MAC_HWF0R_ADDMACADRSEL_POS	18
#define MAC_HWF0R_ADDMACADRSEL_LEN	5
#define MAC_HWF0R_ARPOFFSEL_POS		9
#define MAC_HWF0R_ARPOFFSEL_LEN		1
#define MAC_HWF0R_EEESEL_POS		13
#define MAC_HWF0R_EEESEL_LEN		1
#define MAC_HWF0R_PHYIFSEL_POS		1
#define MAC_HWF0R_PHYIFSEL_LEN		2
#define MAC_HWF0R_MGKSEL_POS		7
#define MAC_HWF0R_MGKSEL_LEN		1
#define MAC_HWF0R_MMCSEL_POS		8
#define MAC_HWF0R_MMCSEL_LEN		1
#define MAC_HWF0R_RWKSEL_POS		6
#define MAC_HWF0R_RWKSEL_LEN		1
#define MAC_HWF0R_RXCOESEL_POS		16
#define MAC_HWF0R_RXCOESEL_LEN		1
#define MAC_HWF0R_SAVLANINS_POS		27
#define MAC_HWF0R_SAVLANINS_LEN		1
#define MAC_HWF0R_SMASEL_POS		5
#define MAC_HWF0R_SMASEL_LEN		1
#define MAC_HWF0R_TSSEL_POS		12
#define MAC_HWF0R_TSSEL_LEN		1
#define MAC_HWF0R_TSSTSSEL_POS		25
#define MAC_HWF0R_TSSTSSEL_LEN		2
#define MAC_HWF0R_TXCOESEL_POS		14
#define MAC_HWF0R_TXCOESEL_LEN		1
#define MAC_HWF0R_VLHASH_POS		4
#define MAC_HWF0R_VLHASH_LEN		1
#define MAC_HWF1R_ADDR64_POS		14
#define MAC_HWF1R_ADDR64_LEN		2
#define MAC_HWF1R_ADVTHWORD_POS		13
#define MAC_HWF1R_ADVTHWORD_LEN		1
#define MAC_HWF1R_DBGMEMA_POS		19
#define MAC_HWF1R_DBGMEMA_LEN		1
#define MAC_HWF1R_DCBEN_POS		16
#define MAC_HWF1R_DCBEN_LEN		1
#define MAC_HWF1R_HASHTBLSZ_POS		24
#define MAC_HWF1R_HASHTBLSZ_LEN		3
#define MAC_HWF1R_L3L4FNUM_POS		27
#define MAC_HWF1R_L3L4FNUM_LEN		4
#define MAC_HWF1R_NUMTC_POS		21
#define MAC_HWF1R_NUMTC_LEN		3
#define MAC_HWF1R_RSSEN_POS		20
#define MAC_HWF1R_RSSEN_LEN		1
#define MAC_HWF1R_RXFIFOSIZE_POS	0
#define MAC_HWF1R_RXFIFOSIZE_LEN	5
#define MAC_HWF1R_SPHEN_POS		17
#define MAC_HWF1R_SPHEN_LEN		1
#define MAC_HWF1R_TSOEN_POS		18
#define MAC_HWF1R_TSOEN_LEN		1
#define MAC_HWF1R_TXFIFOSIZE_POS	6
#define MAC_HWF1R_TXFIFOSIZE_LEN	5
#define MAC_HWF2R_AUXSNAPNUM_POS	28
#define MAC_HWF2R_AUXSNAPNUM_LEN	3
#define MAC_HWF2R_PPSOUTNUM_POS		24
#define MAC_HWF2R_PPSOUTNUM_LEN		3
#define MAC_HWF2R_RXCHCNT_POS		12
#define MAC_HWF2R_RXCHCNT_LEN		4
#define MAC_HWF2R_RXQCNT_POS		0
#define MAC_HWF2R_RXQCNT_LEN		4
#define MAC_HWF2R_TXCHCNT_POS		18
#define MAC_HWF2R_TXCHCNT_LEN		4
#define MAC_HWF2R_TXQCNT_POS		6
#define MAC_HWF2R_TXQCNT_LEN		4
#define MAC_IER_TSIE_POS		12
#define MAC_IER_TSIE_LEN		1
#define MAC_ISR_MMCRXIS_POS		9
#define MAC_ISR_MMCRXIS_LEN		1
#define MAC_ISR_MMCTXIS_POS		10
#define MAC_ISR_MMCTXIS_LEN		1
#define MAC_ISR_PMTIS_POS		4
#define MAC_ISR_PMTIS_LEN		1
#define MAC_ISR_TSIS_POS		12
#define MAC_ISR_TSIS_LEN		1
#define MAC_MACA1HR_AE_POS		31
#define MAC_MACA1HR_AE_LEN		1
#define MAC_PFR_HMC_POS			2
#define MAC_PFR_HMC_LEN			1
#define MAC_PFR_HPF_POS			10
#define MAC_PFR_HPF_LEN			1
#define MAC_PFR_HUC_POS			1
#define MAC_PFR_HUC_LEN			1
#define MAC_PFR_PM_POS			4
#define MAC_PFR_PM_LEN			1
#define MAC_PFR_PR_POS			0
#define MAC_PFR_PR_LEN			1
#define MAC_PFR_VTFE_POS		16
#define MAC_PFR_VTFE_LEN		1
#define MAC_PMTCSR_MGKPKTEN_POS		1
#define MAC_PMTCSR_MGKPKTEN_LEN		1
#define MAC_PMTCSR_PWRDWN_POS		0
#define MAC_PMTCSR_PWRDWN_LEN		1
#define MAC_PMTCSR_RWKFILTRST_POS	31
#define MAC_PMTCSR_RWKFILTRST_LEN	1
#define MAC_PMTCSR_RWKPKTEN_POS		2
#define MAC_PMTCSR_RWKPKTEN_LEN		1
#define MAC_Q0TFCR_PT_POS		16
#define MAC_Q0TFCR_PT_LEN		16
#define MAC_Q0TFCR_TFE_POS		1
#define MAC_Q0TFCR_TFE_LEN		1
#define MAC_RCR_ACS_POS			1
#define MAC_RCR_ACS_LEN			1
#define MAC_RCR_CST_POS			2
#define MAC_RCR_CST_LEN			1
#define MAC_RCR_DCRCC_POS		3
#define MAC_RCR_DCRCC_LEN		1
#define MAC_RCR_HDSMS_POS		12
#define MAC_RCR_HDSMS_LEN		3
#define MAC_RCR_IPC_POS			9
#define MAC_RCR_IPC_LEN			1
#define MAC_RCR_JE_POS			8
#define MAC_RCR_JE_LEN			1
#define MAC_RCR_LM_POS			10
#define MAC_RCR_LM_LEN			1
#define MAC_RCR_RE_POS			0
#define MAC_RCR_RE_LEN			1
#define MAC_RFCR_PFCE_POS		8
#define MAC_RFCR_PFCE_LEN		1
#define MAC_RFCR_RFE_POS		0
#define MAC_RFCR_RFE_LEN		1
#define MAC_RFCR_UP_POS			1
#define MAC_RFCR_UP_LEN			1
#define MAC_RQC0R_RXQ0EN_POS		0
#define MAC_RQC0R_RXQ0EN_LEN		2
#define MAC_RSSAR_ADDRT_POS		2
#define MAC_RSSAR_ADDRT_LEN		1
#define MAC_RSSAR_CT_POS		1
#define MAC_RSSAR_CT_LEN		1
#define MAC_RSSAR_OB_POS		0
#define MAC_RSSAR_OB_LEN		1
#define MAC_RSSAR_RSSIA_POS		8
#define MAC_RSSAR_RSSIA_LEN		8
#define MAC_RSSCR_IP2TE_POS		1
#define MAC_RSSCR_IP2TE_LEN		1
#define MAC_RSSCR_RSSE_POS		0
#define MAC_RSSCR_RSSE_LEN		1
#define MAC_RSSCR_TCP4TE_POS		2
#define MAC_RSSCR_TCP4TE_LEN		1
#define MAC_RSSCR_UDP4TE_POS		3
#define MAC_RSSCR_UDP4TE_LEN		1
#define MAC_RSSDR_DMCH_POS		0
#define MAC_RSSDR_DMCH_LEN		4
#define MAC_SSIR_SNSINC_POS		8
#define MAC_SSIR_SNSINC_LEN		8
#define MAC_SSIR_SSINC_POS		16
#define MAC_SSIR_SSINC_LEN		8
#define MAC_TCR_SS_POS			28
#define MAC_TCR_SS_LEN			3
#define MAC_TCR_TE_POS			0
#define MAC_TCR_TE_LEN			1
#define MAC_TSCR_AV8021ASMEN_POS	28
#define MAC_TSCR_AV8021ASMEN_LEN	1
#define MAC_TSCR_SNAPTYPSEL_POS		16
#define MAC_TSCR_SNAPTYPSEL_LEN		2
#define MAC_TSCR_TSADDREG_POS		5
#define MAC_TSCR_TSADDREG_LEN		1
#define MAC_TSCR_TSCFUPDT_POS		1
#define MAC_TSCR_TSCFUPDT_LEN		1
#define MAC_TSCR_TSCTRLSSR_POS		9
#define MAC_TSCR_TSCTRLSSR_LEN		1
#define MAC_TSCR_TSENA_POS		0
#define MAC_TSCR_TSENA_LEN		1
#define MAC_TSCR_TSENALL_POS		8
#define MAC_TSCR_TSENALL_LEN		1
#define MAC_TSCR_TSEVNTENA_POS		14
#define MAC_TSCR_TSEVNTENA_LEN		1
#define MAC_TSCR_TSINIT_POS		2
#define MAC_TSCR_TSINIT_LEN		1
#define MAC_TSCR_TSIPENA_POS		11
#define MAC_TSCR_TSIPENA_LEN		1
#define MAC_TSCR_TSIPV4ENA_POS		13
#define MAC_TSCR_TSIPV4ENA_LEN		1
#define MAC_TSCR_TSIPV6ENA_POS		12
#define MAC_TSCR_TSIPV6ENA_LEN		1
#define MAC_TSCR_TSMSTRENA_POS		15
#define MAC_TSCR_TSMSTRENA_LEN		1
#define MAC_TSCR_TSVER2ENA_POS		10
#define MAC_TSCR_TSVER2ENA_LEN		1
#define MAC_TSCR_TXTSSTSM_POS		24
#define MAC_TSCR_TXTSSTSM_LEN		1
#define MAC_TSSR_TXTSC_POS		15
#define MAC_TSSR_TXTSC_LEN		1
#define MAC_TXSNR_TXTSSTSMIS_POS	31
#define MAC_TXSNR_TXTSSTSMIS_LEN	1
#define MAC_VLANHTR_VLHT_POS		0
#define MAC_VLANHTR_VLHT_LEN		16
#define MAC_VLANIR_VLTI_POS		20
#define MAC_VLANIR_VLTI_LEN		1
#define MAC_VLANIR_CSVL_POS		19
#define MAC_VLANIR_CSVL_LEN		1
#define MAC_VLANTR_DOVLTC_POS		20
#define MAC_VLANTR_DOVLTC_LEN		1
#define MAC_VLANTR_ERSVLM_POS		19
#define MAC_VLANTR_ERSVLM_LEN		1
#define MAC_VLANTR_ESVL_POS		18
#define MAC_VLANTR_ESVL_LEN		1
#define MAC_VLANTR_ETV_POS		16
#define MAC_VLANTR_ETV_LEN		1
#define MAC_VLANTR_EVLS_POS		21
#define MAC_VLANTR_EVLS_LEN		2
#define MAC_VLANTR_EVLRXS_POS		24
#define MAC_VLANTR_EVLRXS_LEN		1
#define MAC_VLANTR_VL_POS		0
#define MAC_VLANTR_VL_LEN		16
#define MAC_VLANTR_VTHM_POS		25
#define MAC_VLANTR_VTHM_LEN		1
#define MAC_VLANTR_VTIM_POS		17
#define MAC_VLANTR_VTIM_LEN		1
#define MAC_VR_DEVID_POS		8
#define MAC_VR_DEVID_LEN		8
#define MAC_VR_SNPSVER_POS		0
#define MAC_VR_SNPSVER_LEN		8
#define MAC_VR_USERVER_POS		16
#define MAC_VR_USERVER_LEN		8
#define MAC_MDIOSCAR_DA_POS		21
#define MAC_MDIOSCAR_DA_LEN		5
#define MAC_MDIOSCAR_PA_POS		16
#define MAC_MDIOSCAR_PA_LEN		5
#define MAC_MDIOSCAR_RA_POS		0
#define MAC_MDIOSCAR_RA_LEN		16
#define MAC_MDIOSCCDR_BUSY_POS		22
#define MAC_MDIOSCCDR_BUSY_LEN		1
#define MAC_MDIOSCCDR_CR_POS		19
#define MAC_MDIOSCCDR_CR_LEN		3
#define MAC_MDIOSCCDR_SADDR_POS		18
#define MAC_MDIOSCCDR_SADDR_LEN		1
#define MAC_MDIOSCCDR_CMD_POS		16
#define MAC_MDIOSCCDR_CMD_LEN		2
#define MAC_MDIOSCCDR_SDATA_POS		0
#define MAC_MDIOSCCDR_SDATA_LEN		16

/* MMC register offsets */
#define MMC_CR				0x0800
#define MMC_RISR			0x0804
#define MMC_TISR			0x0808
#define MMC_RIER			0x080c
#define MMC_TIER			0x0810
#define MMC_TXOCTETCOUNT_GB_LO		0x0814
#define MMC_TXOCTETCOUNT_GB_HI		0x0818
#define MMC_TXFRAMECOUNT_GB_LO		0x081c
#define MMC_TXFRAMECOUNT_GB_HI		0x0820
#define MMC_TXBROADCASTFRAMES_G_LO	0x0824
#define MMC_TXBROADCASTFRAMES_G_HI	0x0828
#define MMC_TXMULTICASTFRAMES_G_LO	0x082c
#define MMC_TXMULTICASTFRAMES_G_HI	0x0830
#define MMC_TX64OCTETS_GB_LO		0x0834
#define MMC_TX64OCTETS_GB_HI		0x0838
#define MMC_TX65TO127OCTETS_GB_LO	0x083c
#define MMC_TX65TO127OCTETS_GB_HI	0x0840
#define MMC_TX128TO255OCTETS_GB_LO	0x0844
#define MMC_TX128TO255OCTETS_GB_HI	0x0848
#define MMC_TX256TO511OCTETS_GB_LO	0x084c
#define MMC_TX256TO511OCTETS_GB_HI	0x0850
#define MMC_TX512TO1023OCTETS_GB_LO	0x0854
#define MMC_TX512TO1023OCTETS_GB_HI	0x0858
#define MMC_TX1024TOMAXOCTETS_GB_LO	0x085c
#define MMC_TX1024TOMAXOCTETS_GB_HI	0x0860
#define MMC_TXUNICASTFRAMES_GB_LO	0x0864
#define MMC_TXUNICASTFRAMES_GB_HI	0x0868
#define MMC_TXMULTICASTFRAMES_GB_LO	0x086c
#define MMC_TXMULTICASTFRAMES_GB_HI	0x0870
#define MMC_TXBROADCASTFRAMES_GB_LO	0x0874
#define MMC_TXBROADCASTFRAMES_GB_HI	0x0878
#define MMC_TXUNDERFLOWERROR_LO		0x087c
#define MMC_TXUNDERFLOWERROR_HI		0x0880
#define MMC_TXOCTETCOUNT_G_LO		0x0884
#define MMC_TXOCTETCOUNT_G_HI		0x0888
#define MMC_TXFRAMECOUNT_G_LO		0x088c
#define MMC_TXFRAMECOUNT_G_HI		0x0890
#define MMC_TXPAUSEFRAMES_LO		0x0894
#define MMC_TXPAUSEFRAMES_HI		0x0898
#define MMC_TXVLANFRAMES_G_LO		0x089c
#define MMC_TXVLANFRAMES_G_HI		0x08a0
#define MMC_RXFRAMECOUNT_GB_LO		0x0900
#define MMC_RXFRAMECOUNT_GB_HI		0x0904
#define MMC_RXOCTETCOUNT_GB_LO		0x0908
#define MMC_RXOCTETCOUNT_GB_HI		0x090c
#define MMC_RXOCTETCOUNT_G_LO		0x0910
#define MMC_RXOCTETCOUNT_G_HI		0x0914
#define MMC_RXBROADCASTFRAMES_G_LO	0x0918
#define MMC_RXBROADCASTFRAMES_G_HI	0x091c
#define MMC_RXMULTICASTFRAMES_G_LO	0x0920
#define MMC_RXMULTICASTFRAMES_G_HI	0x0924
#define MMC_RXCRCERROR_LO		0x0928
#define MMC_RXCRCERROR_HI		0x092c
#define MMC_RXRUNTERROR			0x0930
#define MMC_RXJABBERERROR		0x0934
#define MMC_RXUNDERSIZE_G		0x0938
#define MMC_RXOVERSIZE_G		0x093c
#define MMC_RX64OCTETS_GB_LO		0x0940
#define MMC_RX64OCTETS_GB_HI		0x0944
#define MMC_RX65TO127OCTETS_GB_LO	0x0948
#define MMC_RX65TO127OCTETS_GB_HI	0x094c
#define MMC_RX128TO255OCTETS_GB_LO	0x0950
#define MMC_RX128TO255OCTETS_GB_HI	0x0954
#define MMC_RX256TO511OCTETS_GB_LO	0x0958
#define MMC_RX256TO511OCTETS_GB_HI	0x095c
#define MMC_RX512TO1023OCTETS_GB_LO	0x0960
#define MMC_RX512TO1023OCTETS_GB_HI	0x0964
#define MMC_RX1024TOMAXOCTETS_GB_LO	0x0968
#define MMC_RX1024TOMAXOCTETS_GB_HI	0x096c
#define MMC_RXUNICASTFRAMES_G_LO	0x0970
#define MMC_RXUNICASTFRAMES_G_HI	0x0974
#define MMC_RXLENGTHERROR_LO		0x0978
#define MMC_RXLENGTHERROR_HI		0x097c
#define MMC_RXOUTOFRANGETYPE_LO		0x0980
#define MMC_RXOUTOFRANGETYPE_HI		0x0984
#define MMC_RXPAUSEFRAMES_LO		0x0988
#define MMC_RXPAUSEFRAMES_HI		0x098c
#define MMC_RXFIFOOVERFLOW_LO		0x0990
#define MMC_RXFIFOOVERFLOW_HI		0x0994
#define MMC_RXVLANFRAMES_GB_LO		0x0998
#define MMC_RXVLANFRAMES_GB_HI		0x099c
#define MMC_RXWATCHDOGERROR		0x09a0

/* MMC register entry bit positions and sizes */
#define MMC_CR_CR_POS				0
#define MMC_CR_CR_LEN				1
#define MMC_CR_CSR_POS				1
#define MMC_CR_CSR_LEN				1
#define MMC_CR_ROR_POS				2
#define MMC_CR_ROR_LEN				1
#define MMC_CR_MCF_POS				3
#define MMC_CR_MCF_LEN				1
#define MMC_CR_MCT_POS				4
#define MMC_CR_MCT_LEN				2
#define MMC_RIER_ALL_INTERRUPTS_POS		0
#define MMC_RIER_ALL_INTERRUPTS_LEN		23
#define MMC_RISR_RXFRAMECOUNT_GB_POS		0
#define MMC_RISR_RXFRAMECOUNT_GB_LEN		1
#define MMC_RISR_RXOCTETCOUNT_GB_POS		1
#define MMC_RISR_RXOCTETCOUNT_GB_LEN		1
#define MMC_RISR_RXOCTETCOUNT_G_POS		2
#define MMC_RISR_RXOCTETCOUNT_G_LEN		1
#define MMC_RISR_RXBROADCASTFRAMES_G_POS	3
#define MMC_RISR_RXBROADCASTFRAMES_G_LEN	1
#define MMC_RISR_RXMULTICASTFRAMES_G_POS	4
#define MMC_RISR_RXMULTICASTFRAMES_G_LEN	1
#define MMC_RISR_RXCRCERROR_POS			5
#define MMC_RISR_RXCRCERROR_LEN			1
#define MMC_RISR_RXRUNTERROR_POS		6
#define MMC_RISR_RXRUNTERROR_LEN		1
#define MMC_RISR_RXJABBERERROR_POS		7
#define MMC_RISR_RXJABBERERROR_LEN		1
#define MMC_RISR_RXUNDERSIZE_G_POS		8
#define MMC_RISR_RXUNDERSIZE_G_LEN		1
#define MMC_RISR_RXOVERSIZE_G_POS		9
#define MMC_RISR_RXOVERSIZE_G_LEN		1
#define MMC_RISR_RX64OCTETS_GB_POS		10
#define MMC_RISR_RX64OCTETS_GB_LEN		1
#define MMC_RISR_RX65TO127OCTETS_GB_POS		11
#define MMC_RISR_RX65TO127OCTETS_GB_LEN		1
#define MMC_RISR_RX128TO255OCTETS_GB_POS	12
#define MMC_RISR_RX128TO255OCTETS_GB_LEN	1
#define MMC_RISR_RX256TO511OCTETS_GB_POS	13
#define MMC_RISR_RX256TO511OCTETS_GB_LEN	1
#define MMC_RISR_RX512TO1023OCTETS_GB_POS	14
#define MMC_RISR_RX512TO1023OCTETS_GB_LEN	1
#define MMC_RISR_RX1024TOMAXOCTETS_GB_POS	15
#define MMC_RISR_RX1024TOMAXOCTETS_GB_LEN	1
#define MMC_RISR_RXUNICASTFRAMES_G_POS		16
#define MMC_RISR_RXUNICASTFRAMES_G_LEN		1
#define MMC_RISR_RXLENGTHERROR_POS		17
#define MMC_RISR_RXLENGTHERROR_LEN		1
#define MMC_RISR_RXOUTOFRANGETYPE_POS		18
#define MMC_RISR_RXOUTOFRANGETYPE_LEN		1
#define MMC_RISR_RXPAUSEFRAMES_POS		19
#define MMC_RISR_RXPAUSEFRAMES_LEN		1
#define MMC_RISR_RXFIFOOVERFLOW_POS		20
#define MMC_RISR_RXFIFOOVERFLOW_LEN		1
#define MMC_RISR_RXVLANFRAMES_GB_POS		21
#define MMC_RISR_RXVLANFRAMES_GB_LEN		1
#define MMC_RISR_RXWATCHDOGERROR_POS		22
#define MMC_RISR_RXWATCHDOGERROR_LEN		1
#define MMC_TIER_ALL_INTERRUPTS_POS		0
#define MMC_TIER_ALL_INTERRUPTS_LEN		18
#define MMC_TISR_TXOCTETCOUNT_GB_POS		0
#define MMC_TISR_TXOCTETCOUNT_GB_LEN		1
#define MMC_TISR_TXFRAMECOUNT_GB_POS		1
#define MMC_TISR_TXFRAMECOUNT_GB_LEN		1
#define MMC_TISR_TXBROADCASTFRAMES_G_POS	2
#define MMC_TISR_TXBROADCASTFRAMES_G_LEN	1
#define MMC_TISR_TXMULTICASTFRAMES_G_POS	3
#define MMC_TISR_TXMULTICASTFRAMES_G_LEN	1
#define MMC_TISR_TX64OCTETS_GB_POS		4
#define MMC_TISR_TX64OCTETS_GB_LEN		1
#define MMC_TISR_TX65TO127OCTETS_GB_POS		5
#define MMC_TISR_TX65TO127OCTETS_GB_LEN		1
#define MMC_TISR_TX128TO255OCTETS_GB_POS	6
#define MMC_TISR_TX128TO255OCTETS_GB_LEN	1
#define MMC_TISR_TX256TO511OCTETS_GB_POS	7
#define MMC_TISR_TX256TO511OCTETS_GB_LEN	1
#define MMC_TISR_TX512TO1023OCTETS_GB_POS	8
#define MMC_TISR_TX512TO1023OCTETS_GB_LEN	1
#define MMC_TISR_TX1024TOMAXOCTETS_GB_POS	9
#define MMC_TISR_TX1024TOMAXOCTETS_GB_LEN	1
#define MMC_TISR_TXUNICASTFRAMES_GB_POS		10
#define MMC_TISR_TXUNICASTFRAMES_GB_LEN		1
#define MMC_TISR_TXMULTICASTFRAMES_GB_POS	11
#define MMC_TISR_TXMULTICASTFRAMES_GB_LEN	1
#define MMC_TISR_TXBROADCASTFRAMES_GB_POS	12
#define MMC_TISR_TXBROADCASTFRAMES_GB_LEN	1
#define MMC_TISR_TXUNDERFLOWERROR_POS		13
#define MMC_TISR_TXUNDERFLOWERROR_LEN		1
#define MMC_TISR_TXOCTETCOUNT_G_POS		14
#define MMC_TISR_TXOCTETCOUNT_G_LEN		1
#define MMC_TISR_TXFRAMECOUNT_G_POS		15
#define MMC_TISR_TXFRAMECOUNT_G_LEN		1
#define MMC_TISR_TXPAUSEFRAMES_POS		16
#define MMC_TISR_TXPAUSEFRAMES_LEN		1
#define MMC_TISR_TXVLANFRAMES_G_POS		17
#define MMC_TISR_TXVLANFRAMES_G_LEN		1

/* MTL register offsets */
#define MTL_OMR				0x1000
#define MTL_FDCR			0x1008
#define MTL_FDSR			0x100c
#define MTL_FDDR			0x1010
#define MTL_ISR				0x1020
#define MTL_RQDCM0R			0x1030
#define MTL_TCPM0R			0x1040
#define MTL_TCPM1R			0x1044

#define MTL_RQDCM_INC			4
#define MTL_RQDCM_Q_PER_REG		4
#define MTL_TCPM_INC			4
#define MTL_TCPM_TC_PER_REG		4

/* MTL register entry bit positions and sizes */
#define MTL_OMR_ETSALG_POS		5
#define MTL_OMR_ETSALG_LEN		2
#define MTL_OMR_RAA_POS			2
#define MTL_OMR_RAA_LEN			1

/* MTL queue register offsets
 *   Multiple queues can be active.  The first queue has registers
 *   that begin at 0x1100.  Each subsequent queue has registers that
 *   are accessed using an offset of 0x80 from the previous queue.
 */
#define MTL_Q_BASE			0x1100
#define MTL_Q_INC			0x80

#define MTL_Q_TQOMR			0x00
#define MTL_Q_TQUR			0x04
#define MTL_Q_TQDR			0x08
#define MTL_Q_RQOMR			0x40
#define MTL_Q_RQMPOCR			0x44
#define MTL_Q_RQDR			0x48
#define MTL_Q_RQFCR			0x50
#define MTL_Q_IER			0x70
#define MTL_Q_ISR			0x74

/* MTL queue register entry bit positions and sizes */
#define MTL_Q_RQDR_PRXQ_POS		16
#define MTL_Q_RQDR_PRXQ_LEN		14
#define MTL_Q_RQDR_RXQSTS_POS		4
#define MTL_Q_RQDR_RXQSTS_LEN		2
#define MTL_Q_RQFCR_RFA_POS		1
#define MTL_Q_RQFCR_RFA_LEN		6
#define MTL_Q_RQFCR_RFD_POS		17
#define MTL_Q_RQFCR_RFD_LEN		6
#define MTL_Q_RQOMR_EHFC_POS		7
#define MTL_Q_RQOMR_EHFC_LEN		1
#define MTL_Q_RQOMR_RQS_POS		16
#define MTL_Q_RQOMR_RQS_LEN		9
#define MTL_Q_RQOMR_RSF_POS		5
#define MTL_Q_RQOMR_RSF_LEN		1
#define MTL_Q_RQOMR_FEP_POS		4
#define MTL_Q_RQOMR_FEP_LEN		1
#define MTL_Q_RQOMR_FUP_POS		3
#define MTL_Q_RQOMR_FUP_LEN		1
#define MTL_Q_RQOMR_RTC_POS		0
#define MTL_Q_RQOMR_RTC_LEN		2
#define MTL_Q_TQOMR_FTQ_POS		0
#define MTL_Q_TQOMR_FTQ_LEN		1
#define MTL_Q_TQOMR_Q2TCMAP_POS		8
#define MTL_Q_TQOMR_Q2TCMAP_LEN		3
#define MTL_Q_TQOMR_TQS_POS		16
#define MTL_Q_TQOMR_TQS_LEN		10
#define MTL_Q_TQOMR_TSF_POS		1
#define MTL_Q_TQOMR_TSF_LEN		1
#define MTL_Q_TQOMR_TTC_POS		4
#define MTL_Q_TQOMR_TTC_LEN		3
#define MTL_Q_TQOMR_TXQEN_POS		2
#define MTL_Q_TQOMR_TXQEN_LEN		2

/* MTL queue register value */
#define MTL_RSF_DISABLE			0x00
#define MTL_RSF_ENABLE			0x01
#define MTL_TSF_DISABLE			0x00
#define MTL_TSF_ENABLE			0x01

#define MTL_RX_THRESHOLD_64		0x00
#define MTL_RX_THRESHOLD_96		0x02
#define MTL_RX_THRESHOLD_128		0x03
#define MTL_TX_THRESHOLD_32		0x01
#define MTL_TX_THRESHOLD_64		0x00
#define MTL_TX_THRESHOLD_96		0x02
#define MTL_TX_THRESHOLD_128		0x03
#define MTL_TX_THRESHOLD_192		0x04
#define MTL_TX_THRESHOLD_256		0x05
#define MTL_TX_THRESHOLD_384		0x06
#define MTL_TX_THRESHOLD_512		0x07

#define MTL_ETSALG_WRR			0x00
#define MTL_ETSALG_WFQ			0x01
#define MTL_ETSALG_DWRR			0x02
#define MTL_RAA_SP			0x00
#define MTL_RAA_WSP			0x01

#define MTL_Q_DISABLED			0x00
#define MTL_Q_ENABLED			0x02

#define MTL_RQDCM0R_Q0MDMACH		0x0
#define MTL_RQDCM0R_Q1MDMACH		0x00000100
#define MTL_RQDCM0R_Q2MDMACH		0x00020000
#define MTL_RQDCM0R_Q3MDMACH		0x03000000
#define MTL_RQDCM1R_Q4MDMACH		0x00000004
#define MTL_RQDCM1R_Q5MDMACH		0x00000500
#define MTL_RQDCM1R_Q6MDMACH		0x00060000
#define MTL_RQDCM1R_Q7MDMACH		0x07000000
#define MTL_RQDCM2R_Q8MDMACH		0x00000008
#define MTL_RQDCM2R_Q9MDMACH		0x00000900
#define MTL_RQDCM2R_Q10MDMACH		0x000A0000
#define MTL_RQDCM2R_Q11MDMACH		0x0B000000

/* MTL traffic class register offsets
 *   Multiple traffic classes can be active.  The first class has registers
 *   that begin at 0x1100.  Each subsequent queue has registers that
 *   are accessed using an offset of 0x80 from the previous queue.
 */
#define MTL_TC_BASE			MTL_Q_BASE
#define MTL_TC_INC			MTL_Q_INC

#define MTL_TC_ETSCR			0x10
#define MTL_TC_ETSSR			0x14
#define MTL_TC_QWR			0x18

/* MTL traffic class register entry bit positions and sizes */
#define MTL_TC_ETSCR_TSA_POS		0
#define MTL_TC_ETSCR_TSA_LEN		2
#define MTL_TC_QWR_QW_POS		0
#define MTL_TC_QWR_QW_LEN		21

/* MTL traffic class register value */
#define MTL_TSA_SP			0x00
#define MTL_TSA_ETS			0x02

/* Descriptor/Packet entry bit positions and sizes */
#define RX_PACKET_ERRORS_CRC_POS		2
#define RX_PACKET_ERRORS_CRC_LEN		1
#define RX_PACKET_ERRORS_FRAME_POS		3
#define RX_PACKET_ERRORS_FRAME_LEN		1
#define RX_PACKET_ERRORS_LENGTH_POS		0
#define RX_PACKET_ERRORS_LENGTH_LEN		1
#define RX_PACKET_ERRORS_OVERRUN_POS		1
#define RX_PACKET_ERRORS_OVERRUN_LEN		1

#define RX_PACKET_ATTRIBUTES_CSUM_DONE_POS	0
#define RX_PACKET_ATTRIBUTES_CSUM_DONE_LEN	1
#define RX_PACKET_ATTRIBUTES_VLAN_CTAG_POS	1
#define RX_PACKET_ATTRIBUTES_VLAN_CTAG_LEN	1
#define RX_PACKET_ATTRIBUTES_INCOMPLETE_POS	2
#define RX_PACKET_ATTRIBUTES_INCOMPLETE_LEN	1
#define RX_PACKET_ATTRIBUTES_CONTEXT_NEXT_POS	3
#define RX_PACKET_ATTRIBUTES_CONTEXT_NEXT_LEN	1
#define RX_PACKET_ATTRIBUTES_CONTEXT_POS	4
#define RX_PACKET_ATTRIBUTES_CONTEXT_LEN	1
#define RX_PACKET_ATTRIBUTES_RX_TSTAMP_POS	5
#define RX_PACKET_ATTRIBUTES_RX_TSTAMP_LEN	1
#define RX_PACKET_ATTRIBUTES_RSS_HASH_POS	6
#define RX_PACKET_ATTRIBUTES_RSS_HASH_LEN	1

#define RX_NORMAL_DESC0_OVT_POS			0
#define RX_NORMAL_DESC0_OVT_LEN			16
#define RX_NORMAL_DESC2_HL_POS			0
#define RX_NORMAL_DESC2_HL_LEN			10
#define RX_NORMAL_DESC3_CDA_POS			27
#define RX_NORMAL_DESC3_CDA_LEN			1
#define RX_NORMAL_DESC3_CTXT_POS		30
#define RX_NORMAL_DESC3_CTXT_LEN		1
#define RX_NORMAL_DESC3_ES_POS			15
#define RX_NORMAL_DESC3_ES_LEN			1
#define RX_NORMAL_DESC3_ETLT_POS		16
#define RX_NORMAL_DESC3_ETLT_LEN		4
#define RX_NORMAL_DESC3_FD_POS			29
#define RX_NORMAL_DESC3_FD_LEN			1
#define RX_NORMAL_DESC3_INTE_POS		30
#define RX_NORMAL_DESC3_INTE_LEN		1
#define RX_NORMAL_DESC3_L34T_POS		20
#define RX_NORMAL_DESC3_L34T_LEN		4
#define RX_NORMAL_DESC3_LD_POS			28
#define RX_NORMAL_DESC3_LD_LEN			1
#define RX_NORMAL_DESC3_OWN_POS			31
#define RX_NORMAL_DESC3_OWN_LEN			1
#define RX_NORMAL_DESC3_PL_POS			0
#define RX_NORMAL_DESC3_PL_LEN			14
#define RX_NORMAL_DESC3_RSV_POS			26
#define RX_NORMAL_DESC3_RSV_LEN			1

#define RX_DESC3_L34T_IPV4_TCP			1
#define RX_DESC3_L34T_IPV4_UDP			2
#define RX_DESC3_L34T_IPV4_ICMP			3
#define RX_DESC3_L34T_IPV6_TCP			9
#define RX_DESC3_L34T_IPV6_UDP			10
#define RX_DESC3_L34T_IPV6_ICMP			11

#define RX_CONTEXT_DESC3_TSA_POS		4
#define RX_CONTEXT_DESC3_TSA_LEN		1
#define RX_CONTEXT_DESC3_TSD_POS		6
#define RX_CONTEXT_DESC3_TSD_LEN		1

#define TX_PACKET_ATTRIBUTES_CSUM_ENABLE_POS	0
#define TX_PACKET_ATTRIBUTES_CSUM_ENABLE_LEN	1
#define TX_PACKET_ATTRIBUTES_TSO_ENABLE_POS	1
#define TX_PACKET_ATTRIBUTES_TSO_ENABLE_LEN	1
#define TX_PACKET_ATTRIBUTES_VLAN_CTAG_POS	2
#define TX_PACKET_ATTRIBUTES_VLAN_CTAG_LEN	1
#define TX_PACKET_ATTRIBUTES_PTP_POS		3
#define TX_PACKET_ATTRIBUTES_PTP_LEN		1

#define TX_CONTEXT_DESC2_MSS_POS		0
#define TX_CONTEXT_DESC2_MSS_LEN		15
#define TX_CONTEXT_DESC3_CTXT_POS		30
#define TX_CONTEXT_DESC3_CTXT_LEN		1
#define TX_CONTEXT_DESC3_TCMSSV_POS		26
#define TX_CONTEXT_DESC3_TCMSSV_LEN		1
#define TX_CONTEXT_DESC3_VLTV_POS		16
#define TX_CONTEXT_DESC3_VLTV_LEN		1
#define TX_CONTEXT_DESC3_VT_POS			0
#define TX_CONTEXT_DESC3_VT_LEN			16

#define TX_NORMAL_DESC2_HL_B1L_POS		0
#define TX_NORMAL_DESC2_HL_B1L_LEN		14
#define TX_NORMAL_DESC2_IC_POS			31
#define TX_NORMAL_DESC2_IC_LEN			1
#define TX_NORMAL_DESC2_TTSE_POS		30
#define TX_NORMAL_DESC2_TTSE_LEN		1
#define TX_NORMAL_DESC2_VTIR_POS		14
#define TX_NORMAL_DESC2_VTIR_LEN		2
#define TX_NORMAL_DESC3_CIC_POS			16
#define TX_NORMAL_DESC3_CIC_LEN			2
#define TX_NORMAL_DESC3_CPC_POS			26
#define TX_NORMAL_DESC3_CPC_LEN			2
#define TX_NORMAL_DESC3_CTXT_POS		30
#define TX_NORMAL_DESC3_CTXT_LEN		1
#define TX_NORMAL_DESC3_FD_POS			29
#define TX_NORMAL_DESC3_FD_LEN			1
#define TX_NORMAL_DESC3_FL_POS			0
#define TX_NORMAL_DESC3_FL_LEN			15
#define TX_NORMAL_DESC3_LD_POS			28
#define TX_NORMAL_DESC3_LD_LEN			1
#define TX_NORMAL_DESC3_OWN_POS			31
#define TX_NORMAL_DESC3_OWN_LEN			1
#define TX_NORMAL_DESC3_TCPHDRLEN_POS		19
#define TX_NORMAL_DESC3_TCPHDRLEN_LEN		4
#define TX_NORMAL_DESC3_TCPPL_POS		0
#define TX_NORMAL_DESC3_TCPPL_LEN		18
#define TX_NORMAL_DESC3_TSE_POS			18
#define TX_NORMAL_DESC3_TSE_LEN			1

#define TX_NORMAL_DESC2_VLAN_INSERT		0x2

/* MDIO undefined or vendor specific registers */
#ifndef MDIO_AN_COMP_STAT
#define MDIO_AN_COMP_STAT			0x0030
#endif

/* Bit setting and getting macros
 *  The get macro will extract the current bit field value from within
 *  the variable
 *
 *  The set macro will clear the current bit field value within the
 *  variable and then set the bit field of the variable to the
 *  specified value
 */
#define GET_BITS(_var, _pos, _len) \
	(((_var) >> (_pos)) & ((0x1 << (_len)) - 1))

#define SET_BITS(_var, _pos, _len, _val) \
do {									\
	(_var) &= ~(((0x1 << (_len)) - 1) << (_pos));			\
	(_var) |= (((_val) & ((0x1 << (_len)) - 1)) << (_pos));	\
} while (0)

#define GET_BITS_LE(_var, _pos, _len) \
	((le32_to_cpu((_var)) >> (_pos)) & ((0x1 << (_len)) - 1))

#define SET_BITS_LE(_var, _pos, _len, _val) \
do {									\
	(_var) &= cpu_to_le32(~(((0x1 << (_len)) - 1) << (_pos)));	\
	(_var) |= cpu_to_le32((((_val) &				\
			      ((0x1 << (_len)) - 1)) << (_pos)));	\
} while (0)

/* Bit setting and getting macros based on register fields
 *  The get macro uses the bit field definitions formed using the input
 *  names to extract the current bit field value from within the
 *  variable
 *
 *  The set macro uses the bit field definitions formed using the input
 *  names to set the bit field of the variable to the specified value
 */
#define DWC_ETH_GET_BITS(_var, _prefix, _field)	\
	GET_BITS((_var),						\
		 _prefix##_##_field##_POS,				\
		 _prefix##_##_field##_LEN)

#define DWC_ETH_SET_BITS(_var, _prefix, _field, _val)			\
	SET_BITS((_var),						\
		 _prefix##_##_field##_POS,				\
		 _prefix##_##_field##_LEN, (_val))

#define DWC_ETH_GET_BITS_LE(_var, _prefix, _field)			\
	GET_BITS_LE((_var),						\
		 _prefix##_##_field##_POS,				\
		 _prefix##_##_field##_LEN)

#define DWC_ETH_SET_BITS_LE(_var, _prefix, _field, _val)		\
	SET_BITS_LE((_var),						\
		 _prefix##_##_field##_POS,				\
		 _prefix##_##_field##_LEN, (_val))

/* Macros for reading or writing registers
 *  The ioread macros will get bit fields or full values using the
 *  register definitions formed using the input names
 *
 *  The iowrite macros will set bit fields or full values using the
 *  register definitions formed using the input names
 */
#define DWC_ETH_IOREAD(_pdata, _reg)					\
	ioread32((_pdata)->mac_regs + (_reg))

#define DWC_ETH_IOREAD_BITS(_pdata, _reg, _field)			\
	GET_BITS(DWC_ETH_IOREAD((_pdata), _reg),			\
		 _reg##_##_field##_POS,					\
		 _reg##_##_field##_LEN)

#define DWC_ETH_IOWRITE(_pdata, _reg, _val)				\
	iowrite32((_val), (_pdata)->mac_regs + (_reg))

#define DWC_ETH_IOWRITE_BITS(_pdata, _reg, _field, _val)		\
do {									\
	u32 reg_val = DWC_ETH_IOREAD((_pdata), _reg);			\
	SET_BITS(reg_val,						\
		 _reg##_##_field##_POS,					\
		 _reg##_##_field##_LEN, (_val));			\
	DWC_ETH_IOWRITE((_pdata), _reg, reg_val);			\
} while (0)

/* Macros for reading or writing MTL queue or traffic class registers
 *  Similar to the standard read and write macros except that the
 *  base register value is calculated by the queue or traffic class number
 */
#define DWC_ETH_MTL_IOREAD(_pdata, _n, _reg)				\
	ioread32((_pdata)->mac_regs +					\
		 MTL_Q_BASE + ((_n) * MTL_Q_INC) + (_reg))

#define DWC_ETH_MTL_IOREAD_BITS(_pdata, _n, _reg, _field)		\
	GET_BITS(DWC_ETH_MTL_IOREAD((_pdata), (_n), _reg),		\
		 _reg##_##_field##_POS,					\
		 _reg##_##_field##_LEN)

#define DWC_ETH_MTL_IOWRITE(_pdata, _n, _reg, _val)			\
	iowrite32((_val), (_pdata)->mac_regs +				\
		  MTL_Q_BASE + ((_n) * MTL_Q_INC) + (_reg))

#define DWC_ETH_MTL_IOWRITE_BITS(_pdata, _n, _reg, _field, _val)	\
do {									\
	u32 reg_val = DWC_ETH_MTL_IOREAD((_pdata), (_n), _reg);		\
	SET_BITS(reg_val,						\
		 _reg##_##_field##_POS,					\
		 _reg##_##_field##_LEN, (_val));			\
	DWC_ETH_MTL_IOWRITE((_pdata), (_n), _reg, reg_val);		\
} while (0)

/* Macros for reading or writing DMA channel registers
 *  Similar to the standard read and write macros except that the
 *  base register value is obtained from the ring
 */
#define DWC_ETH_DMA_IOREAD(_channel, _reg)				\
	ioread32((_channel)->dma_regs + (_reg))

#define DWC_ETH_DMA_IOREAD_BITS(_channel, _reg, _field)			\
	GET_BITS(DWC_ETH_DMA_IOREAD((_channel), _reg),			\
		 _reg##_##_field##_POS,					\
		 _reg##_##_field##_LEN)

#define DWC_ETH_DMA_IOWRITE(_channel, _reg, _val)			\
	iowrite32((_val), (_channel)->dma_regs + (_reg))

#define DWC_ETH_DMA_IOWRITE_BITS(_channel, _reg, _field, _val)		\
do {									\
	u32 reg_val = DWC_ETH_DMA_IOREAD((_channel), _reg);		\
	SET_BITS(reg_val,						\
		 _reg##_##_field##_POS,					\
		 _reg##_##_field##_LEN, (_val));			\
	DWC_ETH_DMA_IOWRITE((_channel), _reg, reg_val);			\
} while (0)

/* Macros for building, reading or writing register values or bits
 * using MDIO.  Different from above because of the use of standardized
 * Linux include values.  No shifting is performed with the bit
 * operations, everything works on mask values.
 */
#define DWC_ETH_MDIO_READ(_pdata, _mmd, _reg)				\
	((_pdata)->hw_ops.read_mmd_regs((_pdata), 0,			\
	MII_ADDR_C45 | ((_mmd) << 16) | ((_reg) & 0xffff)))

#define DWC_ETH_MDIO_READ_BITS(_pdata, _mmd, _reg, _mask)		\
	(DWC_ETH_MDIO_READ((_pdata), _mmd, _reg) & (_mask))

#define DWC_ETH_MDIO_WRITE(_pdata, _mmd, _reg, _val)			\
	((_pdata)->hw_ops.write_mmd_regs((_pdata), 0,			\
	MII_ADDR_C45 | ((_mmd) << 16) | ((_reg) & 0xffff), (_val)))

#define DWC_ETH_MDIO_WRITE_BITS(_pdata, _mmd, _reg, _mask, _val)	\
do {									\
	u32 mmd_val = DWC_ETH_MDIO_READ((_pdata), _mmd, _reg);		\
	mmd_val &= ~(_mask);						\
	mmd_val |= (_val);						\
	DWC_ETH_MDIO_WRITE((_pdata), _mmd, _reg, mmd_val);		\
} while (0)

#endif /* __DWC_ETH_REGACC_H__ */
