/*
 * This header provides constants for binding nvidia,tegra<chip>-hsp.
 *
 * The number with HSP_DB_MASTER prefix indicates the bit that is
 * associated with a master ID in the doorbell registers.
 */


#ifndef _DT_BINDINGS_MAILBOX_TEGRA186_HSP_H
#define _DT_BINDINGS_MAILBOX_TEGRA186_HSP_H

#define HSP_SHARED_MAILBOX		0
#define HSP_SHARED_SEMAPHORE		1
#define HSP_ARBITRATED_SEMAPHORE	2
#define HSP_DOORBELL			3

#define HSP_DB_MASTER_CCPLEX 17
#define HSP_DB_MASTER_BPMP 19

#endif	/* _DT_BINDINGS_MAILBOX_TEGRA186_HSP_H */
