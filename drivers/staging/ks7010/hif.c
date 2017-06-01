#include <crypto/hash.h>
#include <uapi/linux/wireless.h>

#include "ks7010.h"
#include "hif.h"
#include "fil.h"

/**
 * DOC: Host Interface Layer - Provides abstraction layer on top of
 * Firmware Interface Layer. When interfacing with the device FIL
 * provides the mechanism, HIF provides the policy.
 */

/**
 * ks7010_hif_tx() - Implements HIF policy for transmit path.
 * @ks: The ks7010 device.
 * @skb: sk_buff from networking stack.
 */
int ks7010_hif_tx(struct ks7010 *ks, struct sk_buff *skb)
{
	ks_debug("not implemented yet");
	return 0;
}

/**
 * ks7010_hif_tx() - HIF response to an rx event.
 * @ks: The ks7010 device.
 * @data: The rx data.
 * @data_size: Size of data.
 */
void ks7010_hif_rx(struct ks7010 *ks, u8 *data, size_t data_size)
{
	ks_debug("not implemented yet");
}

/**
 * ks7010_hif_set_power_mgmt_active() - Disable power save.
 * @ks: The ks7010 device.
 */
void ks7010_hif_set_power_mgmt_active(struct ks7010 *ks)
{
	struct fil_power_mgmt req;

	req.ps_enable = false;
	req.wake_up = true;
	req.receive_dtims = true;

	ks7010_fil_set_power_mgmt(ks, &req);
}

/**
 * ks7010_hif_set_power_mgmt_sleep() - Enable power save, sleep.
 * @ks: The ks7010 device.
 *
 * Power save sleep mode. Wake periodically to receive DTIM's.
 */
void ks7010_hif_set_power_mgmt_sleep(struct ks7010 *ks)
{
	struct fil_power_mgmt req;

	req.ps_enable = true;
	req.wake_up = false;
	req.receive_dtims = true;

	ks7010_fil_set_power_mgmt(ks, &req);
}

/**
 * ks7010_hif_set_power_mgmt_deep_sleep() - Enable power save, deep sleep.
 * @ks: The ks7010 device.
 *
 * Power save deep sleep mode. Do not wake to receive DTIM's.
 */
void ks7010_hif_set_power_mgmt_deep_sleep(struct ks7010 *ks)
{
	struct fil_power_mgmt req;

	req.ps_enable = true;
	req.wake_up = false;
	req.receive_dtims = false;

	ks7010_fil_set_power_mgmt(ks, &req);
}

void ks7010_hif_init(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

void ks7010_hif_cleanup(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

void ks7010_hif_create(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

void ks7010_hif_destroy(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

