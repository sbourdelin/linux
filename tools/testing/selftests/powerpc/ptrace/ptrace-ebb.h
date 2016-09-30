/*
 * Inspired mostly from the EBB selftest
 *
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#define SAMPLE_PERIOD 100	/* EBB event sample persiod */

/* Standard expected values */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define MMCR0_EXP	0x8000008000000001
#else
#define MMCR0_EXP	0x180000080
#endif

#define MMCR2_EXP	0
#define SIER_EXP	0x2000000

struct opd {
	u64 entry;
	u64 toc;
};

void (*ebb_user_func)(void);
extern void ebb_handler(void);	/* Defined in ebb_handle.S */

void ebb_hook(void)		/* Called by ebb_handler */
{
	if (ebb_user_func)
		ebb_user_func();
}

void setup_ebb_handler(void (*callee)(void))
{
	u64 entry;

#if defined(_CALL_ELF) && _CALL_ELF == 2
	entry = (u64)ebb_handler;
#else
	struct opd *opd;

	opd = (struct opd *)ebb_handler;
	entry = opd->entry;
#endif
	ebb_user_func = callee;

	/* Ensure ebb_user_func is set before we set the handler */
	mb();
	mtspr(SPRN_EBBHR, entry);

	/* Make sure the handler is set before we return */
	mb();
}

void reset_ebb_with_clear_mask(unsigned long mmcr0_clear_mask)
{
	u64 val;

	/* 2) clear MMCR0[PMAO] - docs say BESCR[PMEO] should do this */
	/* 3) set MMCR0[PMAE]   - docs say BESCR[PME] should do this */
	val = mfspr(SPRN_MMCR0);
	mtspr(SPRN_MMCR0, (val & ~mmcr0_clear_mask) | MMCR0_PMAE);

	/* 4) clear BESCR[PMEO] */
	mtspr(SPRN_BESCRR, BESCR_PMEO);

	/* 5) set BESCR[PME] */
	mtspr(SPRN_BESCRS, BESCR_PME);

	/* 6) rfebb 1 - done in our caller */
}

void standard_ebb_callee(void)
{
	u64 val;

	val = mfspr(SPRN_BESCR);
	if (!(val & BESCR_PMEO))
		printf("Spurious interrupt\n");

	mtspr(SPRN_PMC1, pmc_sample_period(SAMPLE_PERIOD));
	reset_ebb_with_clear_mask(MMCR0_PMAO | MMCR0_FC);
}

int ebb_event_enable(struct event *e)
{
	int rc;

	rc = ioctl(e->fd, PERF_EVENT_IOC_ENABLE);
	if (rc)
		return rc;
	rc = event_read(e);

	return rc;
}
