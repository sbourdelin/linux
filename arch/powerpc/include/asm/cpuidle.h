#ifndef _ASM_POWERPC_CPUIDLE_H
#define _ASM_POWERPC_CPUIDLE_H

#ifdef CONFIG_PPC_POWERNV
/* Used in powernv idle state management */
#define PNV_THREAD_RUNNING              0
#define PNV_THREAD_NAP                  1
#define PNV_THREAD_SLEEP                2
#define PNV_THREAD_WINKLE               3
#define PNV_CORE_IDLE_LOCK_BIT          0x100
#define PNV_CORE_IDLE_THREAD_BITS       0x0FF

/*
 * By default we set the ESL and EC bits in the PSSCR.
 *
 * The MTL and PSLL are set to the maximum value possible as per the
 * ISA, i.e 15.
 *
 * The Transition Rate is set to the Maximum value 3.
 */
#define PSSCR_HV_DEFAULT_VAL    (PSSCR_ESL | PSSCR_EC |		    \
				PSSCR_PSLL_MASK | PSSCR_TR_MASK |   \
				PSSCR_MTL_MASK)

#define PSSCR_HV_DEFAULT_MASK   (PSSCR_ESL | PSSCR_EC |		    \
				PSSCR_PSLL_MASK | PSSCR_TR_MASK |   \
				PSSCR_MTL_MASK | PSSCR_RL_MASK)

#ifndef __ASSEMBLY__
extern u32 pnv_fastsleep_workaround_at_entry[];
extern u32 pnv_fastsleep_workaround_at_exit[];

extern u64 pnv_first_deep_stop_state;

/*
 * Older firmware only populates the RL field in psscr_val and
 * psscr_mask.
 *
 * Set the remaining fields to the default value in case of the
 * older firmware.
 */
static inline u64 compute_psscr_val(u64 psscr_val, u64 psscr_mask)
{
	if (psscr_mask == 0xf)
		return psscr_val | PSSCR_HV_DEFAULT_VAL;
	return psscr_val;
}

static inline u64 compute_psscr_mask(u64 psscr_mask)
{
	if (psscr_mask == 0xf)
		return PSSCR_HV_DEFAULT_MASK;
	return psscr_mask;
}
#endif

#endif

/* Idle state entry routines */
#ifdef	CONFIG_PPC_P7_NAP
#define IDLE_STATE_ENTER_SEQ(IDLE_INST)                         \
	/* Magic NAP/SLEEP/WINKLE mode enter sequence */	\
	std	r0,0(r1);					\
	ptesync;						\
	ld	r0,0(r1);					\
1:	cmpd	cr0,r0,r0;					\
	bne	1b;						\
	IDLE_INST;						\

#define	IDLE_STATE_ENTER_SEQ_NORET(IDLE_INST)			\
	IDLE_STATE_ENTER_SEQ(IDLE_INST)                         \
	b	.
#endif /* CONFIG_PPC_P7_NAP */

#endif
