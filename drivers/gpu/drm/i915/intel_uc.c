/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */


#include "i915_drv.h"
#include "intel_uc.h"

/*
 * Read GuC command/status register (SOFT_SCRATCH_0)
 * Return true if it contains a response rather than a command
 */
bool host2guc_action_response(struct drm_i915_private *dev_priv, u32 *status)
{
	u32 val = I915_READ(SOFT_SCRATCH(0));
	*status = val;
	return GUC2HOST_IS_RESPONSE(val);
}

int host2guc_action(struct intel_guc *guc, u32 *data, u32 len)
{
	struct drm_i915_private *dev_priv = guc_to_i915(guc);
	u32 status;
	int i;
	int ret;

	if (WARN_ON(len < 1 || len > 15))
		return -EINVAL;

	mutex_lock(&guc->action_lock);
	intel_uncore_forcewake_get(dev_priv, FORCEWAKE_ALL);

	dev_priv->guc.action_count += 1;
	dev_priv->guc.action_cmd = data[0];

	for (i = 0; i < len; i++)
		I915_WRITE(SOFT_SCRATCH(i), data[i]);

	POSTING_READ(SOFT_SCRATCH(i - 1));

	I915_WRITE(HOST2GUC_INTERRUPT, HOST2GUC_TRIGGER);

	/*
	 * Fast commands should complete in less than 10us, so sample quickly
	 * up to that length of time, then switch to a slower sleep-wait loop.
	 * No HOST2GUC command should ever take longer than 10ms.
	 */
	ret = wait_for_us(host2guc_action_response(dev_priv, &status), 10);
	if (ret)
		ret = wait_for(host2guc_action_response(dev_priv, &status), 10);
	if (status != GUC2HOST_STATUS_SUCCESS) {
		/*
		 * Either the GuC explicitly returned an error (which
		 * we convert to -EIO here) or no response at all was
		 * received within the timeout limit (-ETIMEDOUT)
		 */
		if (ret != -ETIMEDOUT)
			ret = -EIO;

		DRM_WARN("Action 0x%X failed; ret=%d status=0x%08X response=0x%08X\n",
			 data[0], ret, status, I915_READ(SOFT_SCRATCH(15)));

		dev_priv->guc.action_fail += 1;
		dev_priv->guc.action_err = ret;
	}
	dev_priv->guc.action_status = status;

	intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
	mutex_unlock(&guc->action_lock);

	return ret;
}

/*
 * Tell the GuC to allocate or deallocate a specific doorbell
 */

int host2guc_allocate_doorbell(struct intel_guc *guc,
			       struct i915_guc_client *client)
{
	u32 data[2];

	data[0] = HOST2GUC_ACTION_ALLOCATE_DOORBELL;
	data[1] = client->ctx_index;

	return host2guc_action(guc, data, 2);
}

int host2guc_release_doorbell(struct intel_guc *guc,
			      struct i915_guc_client *client)
{
	u32 data[2];

	data[0] = HOST2GUC_ACTION_DEALLOCATE_DOORBELL;
	data[1] = client->ctx_index;

	return host2guc_action(guc, data, 2);
}

int host2guc_sample_forcewake(struct intel_guc *guc,
			      struct i915_guc_client *client)
{
	struct drm_i915_private *dev_priv = guc_to_i915(guc);
	u32 data[2];

	data[0] = HOST2GUC_ACTION_SAMPLE_FORCEWAKE;
	/* WaRsDisableCoarsePowerGating:skl,bxt */
	if (!intel_enable_rc6() || NEEDS_WaRsDisableCoarsePowerGating(dev_priv))
		data[1] = 0;
	else
		/* bit 0 and 1 are for Render and Media domain separately */
		data[1] = GUC_FORCEWAKE_RENDER | GUC_FORCEWAKE_MEDIA;

	return host2guc_action(guc, data, ARRAY_SIZE(data));
}

int host2guc_logbuffer_flush_complete(struct intel_guc *guc)
{
	u32 data[1];

	data[0] = HOST2GUC_ACTION_LOG_BUFFER_FILE_FLUSH_COMPLETE;

	return host2guc_action(guc, data, 1);
}

int host2guc_force_logbuffer_flush(struct intel_guc *guc)
{
	u32 data[2];

	data[0] = HOST2GUC_ACTION_FORCE_LOG_BUFFER_FLUSH;
	data[1] = 0;

	return host2guc_action(guc, data, 2);
}

int host2guc_logging_control(struct intel_guc *guc, u32 control_val)
{
	u32 data[2];

	data[0] = HOST2GUC_ACTION_UK_LOG_ENABLE_LOGGING;
	data[1] = control_val;

	return host2guc_action(guc, data, 2);
}
