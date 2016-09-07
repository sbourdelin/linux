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
 * Authors:
 *    Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>
 *    Jerome Anand <jerome.anand@intel.com>
 *    based on VED patches
 *
 */

/**
 * DOC: LPE Audio integration integration for HDMI or DP playback
 *
 * Motivation:
 * Atom platforms (e.g. valleyview and cherryTrail) integrates a DMA-based
 * interface as an alternative to the traditional HDaudio path. While this
 * mode is unrelated to the LPE aka SST audio engine, the documentation refers
 * to this mode as LPE so we keep this notation for the sake of consistency.
 *
 * The interface is handled by a separate standalone driver maintained in the
 * ALSA subsystem for simplicity. To minimize the interaction between the two
 * subsystems, a bridge is setup between the hdmi-lpe-audio and i915:
 * 1. Create a platform device to share MMIO/IRQ resources
 * 2. Make the platform device child of i915 device for runtime PM.
 * 3. Create IRQ chip to forward the LPE audio irqs.
 * the hdmi-lpe-audio driver probes the lpe audio device and creates a new
 * sound card
 *
 * Threats:
 * Due to the restriction in Linux platform device model, user need manually
 * uninstall the hdmi-lpe-audio driver before uninstalling i915 module,
 * otherwise we might run into use-after-free issues after i915 removes the
 * platform device: even though hdmi-lpe-audio driver is released, the modules
 * is still in "installed" status.
 *
 * Implementation:
 * The MMIO/REG platform resources are created according to the registers
 * specification.
 * When forwarding LPE audio irqs, the flow control handler selection depends
 * on the platform, for example on valleyview handle_simple_irq is enough.
 *
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/pci.h>

#include "i915_drv.h"
#include <linux/delay.h>
#include <drm/intel_lpe_audio.h>

static struct platform_device*
lpe_audio_platdev_create(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;
	int ret;
	struct resource rsc[2] = { {0}, {0} };
	struct platform_device *platdev;
	u64 *dma_mask = NULL;
	struct intel_hdmi_lpe_audio_pdata *pdata;

	if (dev_priv->lpe_audio.irq < 0)
		return ERR_PTR(-EINVAL);

	platdev = platform_device_alloc("hdmi-lpe-audio", -1);
	if (!platdev) {
		ret = -ENOMEM;
		DRM_ERROR("Failed to allocate LPE audio platform device\n");
		goto err;
	}

	/* to work-around check_addr in nommu_map_sg() */
	dma_mask = kmalloc(sizeof(*platdev->dev.dma_mask), GFP_KERNEL);
	if (!dma_mask) {
		ret = -ENOMEM;
		DRM_ERROR("Failed to allocate dma_mask\n");
		goto err_put_dev;
	}
	*dma_mask = DMA_BIT_MASK(31);
	platdev->dev.dma_mask = dma_mask;
	platdev->dev.coherent_dma_mask = *dma_mask;

	rsc[0].start    = rsc[0].end = dev_priv->lpe_audio.irq;
	rsc[0].flags    = IORESOURCE_IRQ;
	rsc[0].name     = "hdmi-lpe-audio-irq";

	DRM_DEBUG("%s: HDMI_AUDIO rsc.start[0] = 0x%x\n",
		__func__, (int)(rsc[0].start));

	rsc[1].start    = pci_resource_start(dev->pdev, 0) +
		I915_HDMI_LPE_AUDIO_BASE;
	rsc[1].end      = pci_resource_start(dev->pdev, 0) +
		I915_HDMI_LPE_AUDIO_BASE + I915_HDMI_LPE_AUDIO_SIZE - 1;
	rsc[1].flags    = IORESOURCE_MEM;
	rsc[1].name     = "hdmi-lpe-audio-mmio";

	DRM_DEBUG("%s: HDMI_AUDIO rsc.start[1] = 0x%x\n",
		__func__, (int)(rsc[1].start));

	ret = platform_device_add_resources(platdev, rsc, 2);
	if (ret) {
		DRM_ERROR("Failed to add resource for platform device: %d\n",
			ret);
		goto err_put_dev;
	}

	pdata = kzalloc(sizeof(struct intel_hdmi_lpe_audio_pdata),
							GFP_KERNEL);
	if (pdata == NULL) {
		ret = -ENOMEM;
		goto err_put_dev;
	}

	platdev->dev.platform_data = pdata;

	/* for LPE audio driver's runtime-PM */
	platdev->dev.parent = dev->dev;
	ret = platform_device_add(platdev);
	if (ret) {
		DRM_ERROR("Failed to add LPE audio platform device: %d\n",
			ret);
		goto err_put_dev;
	}

	return platdev;
err_put_dev:
	platform_device_put(platdev);
err:
	kfree(dma_mask);
	return ERR_PTR(ret);
}

static void lpe_audio_platdev_destroy(struct drm_i915_private *dev_priv)
{
	if (dev_priv->lpe_audio.platdev) {
		platform_device_unregister(dev_priv->lpe_audio.platdev);
		kfree(dev_priv->lpe_audio.platdev->dev.dma_mask);
	}
}

static void lpe_audio_irq_unmask(struct irq_data *d)
{
	struct drm_device *dev = d->chip_data;
	struct drm_i915_private *dev_priv =
		(struct drm_i915_private *) dev->dev_private;
	unsigned long irqflags;
	u32 val = (I915_LPE_PIPE_A_INTERRUPT |
		I915_LPE_PIPE_B_INTERRUPT |
		I915_LPE_PIPE_C_INTERRUPT);

	spin_lock_irqsave(&dev_priv->irq_lock, irqflags);

	/*
	 * VLV_IER is already set in the vlv_display_postinstall(),
	 * we only change VLV_IIR and VLV_IMR
	 */
	dev_priv->irq_mask &= ~val;
	I915_WRITE(VLV_IIR, val);
	I915_WRITE(VLV_IIR, val);
	I915_WRITE(VLV_IMR, dev_priv->irq_mask);
	/* FIXME: this was old code move to valleyview_irq_postinstall()
	 * I915_WRITE(VLV_IER, ~dev_priv->irq_mask);
	 * POSTING_READ(VLV_IER);
	 */
	POSTING_READ(VLV_IMR);

	spin_unlock_irqrestore(&dev_priv->irq_lock, irqflags);
}

static void lpe_audio_irq_mask(struct irq_data *d)
{
	struct drm_device *dev = d->chip_data;
	struct drm_i915_private *dev_priv =
		(struct drm_i915_private *) dev->dev_private;
	unsigned long irqflags;
	u32 val = (I915_LPE_PIPE_A_INTERRUPT |
		I915_LPE_PIPE_B_INTERRUPT |
		I915_LPE_PIPE_C_INTERRUPT);

	spin_lock_irqsave(&dev_priv->irq_lock, irqflags);

	/*
	 * VLV_IER is already set in the vlv_display_postinstall(),
	 * we only change VLV_IIR and VLV_IMR
	 */
	dev_priv->irq_mask |= val;
	I915_WRITE(VLV_IMR, dev_priv->irq_mask);
	I915_WRITE(VLV_IIR, val);
	I915_WRITE(VLV_IIR, val);
	POSTING_READ(VLV_IIR);

	spin_unlock_irqrestore(&dev_priv->irq_lock, irqflags);
}

static struct irq_chip lpe_audio_irqchip = {
	.name = "hdmi_lpe_audio_irqchip",
	.irq_mask = lpe_audio_irq_mask,
	.irq_unmask = lpe_audio_irq_unmask,
};

static int lpe_audio_irq_init(struct drm_i915_private *dev_priv)
{
	int irq = dev_priv->lpe_audio.irq;

	WARN_ON(!intel_irqs_enabled(dev_priv));
	irq_set_chip_and_handler_name(irq,
		&lpe_audio_irqchip,
		handle_simple_irq,
		"hdmi_lpe_audio_irq_handler");

	return irq_set_chip_data(irq, &dev_priv->drm);
}

/**
 * intel_lpe_audio_irq_handler() - forwards the LPE audio irq
 * @dev_priv: the i915 drm device private data
 *
 * the LPE Audio irq is forwarded to the irq handler registered by LPE audio
 * driver.
 */
void intel_lpe_audio_irq_handler(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = generic_handle_irq(dev_priv->lpe_audio.irq);
	if (ret)
		DRM_ERROR_RATELIMITED("error handling LPE audio irq: %d\n",
				ret);
}

/**
 * intel_lpe_audio_detect() - check & setup lpe audio if present
 * @dev_priv: the i915 drm device private data
 *
 * Detect if lpe audio is present
 *
 * Return: true if lpe audio present else Return = false
 */
int intel_lpe_audio_detect(struct drm_i915_private *dev_priv)
{
	int lpe_present = false;
	struct drm_device *dev = &dev_priv->drm;

	if (HAS_LPE_AUDIO(dev)) {
		static const struct pci_device_id atom_hdaudio_ids[] = {
			/* Baytrail */
			{PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x0f04)},
			/* Braswell */
			{PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x2284)},
			{}
		};

		if (!pci_dev_present(atom_hdaudio_ids)) {
			DRM_INFO("%s%s\n", "HDaudio controller not detected,",
				"using LPE audio instead\n");
			lpe_present = true;
		}
	}
	return lpe_present;
}

/**
 * intel_lpe_audio_setup() - setup the bridge between HDMI LPE Audio
 * driver and i915
 * @dev_priv: the i915 drm device private data
 *
 * set up the minimum required resources for the bridge: irq chip,
 * platform resource and platform device. i915 device is set as parent
 * of the new platform device.
 *
 * Return: 0 if successful. non-zero if allocation/initialization fails
 */
int intel_lpe_audio_setup(struct drm_i915_private *dev_priv)
{
	int ret;

	dev_priv->lpe_audio.irq = irq_alloc_descs(-1, 0, 1, 0);
	if (dev_priv->lpe_audio.irq < 0) {
		DRM_ERROR("Failed to allocate IRQ desc: %d\n",
			dev_priv->lpe_audio.irq);
		ret = dev_priv->lpe_audio.irq;
		goto err;
	}

	DRM_DEBUG("%s : irq = %d\n", __func__, dev_priv->lpe_audio.irq);

	ret = lpe_audio_irq_init(dev_priv);

	if (ret) {

		DRM_ERROR("Failed to initialize irqchip for lpe audio: %d\n",
			ret);
		goto err_free_irq;
	}

	dev_priv->lpe_audio.platdev = lpe_audio_platdev_create(dev_priv);

	if (IS_ERR(dev_priv->lpe_audio.platdev)) {
		ret = PTR_ERR(dev_priv->lpe_audio.platdev);
		DRM_ERROR("Failed to create lpe audio platform device: %d\n",
			ret);
		goto err_free_irq;
	}

	return 0;
err_free_irq:
	irq_free_desc(dev_priv->lpe_audio.irq);
err:
	dev_priv->lpe_audio.irq = -1;
	dev_priv->lpe_audio.platdev = NULL;
	return ret;
}

/**
 * intel_lpe_audio_teardown() - destroy the bridge between HDMI LPE
 * audio driver and i915
 * @dev_priv: the i915 drm device private data
 *
 * release all the resources for LPE audio <-> i915 bridge.
 */
void intel_lpe_audio_teardown(struct drm_i915_private *dev_priv)
{
	unsigned long irqflags;
	u32 val = (I915_LPE_PIPE_A_INTERRUPT |
		I915_LPE_PIPE_B_INTERRUPT |
		I915_LPE_PIPE_C_INTERRUPT);

	/**
	 * mask LPE audio irq before destroying
	 */
	spin_lock_irqsave(&dev_priv->irq_lock, irqflags);
	dev_priv->irq_mask |= val;
	I915_WRITE(VLV_IMR, dev_priv->irq_mask);
	I915_WRITE(VLV_IIR, val);
	I915_WRITE(VLV_IIR, val);

	lpe_audio_platdev_destroy(dev_priv);
	if (dev_priv->lpe_audio.irq >= 0)
		irq_free_desc(dev_priv->lpe_audio.irq);
}

/**
 * intel_lpe_audio_notify() - notify lpe audio event
 * audio driver and i915
 * @dev_priv: the i915 drm device private data
 * @eld : ELD data
 * @port: port id
 * @tmds_clk_speed: tmds clock frequency in Hz
 * @connected: hdmi connected/disconnected
 *
 * Notify lpe audio driver of eld change.
 */
void intel_lpe_audio_notify(struct drm_i915_private *dev_priv,
			void *eld, int port, int tmds_clk_speed,
			bool connected)
{
	if (IS_LPE_AUDIO_ENABLED(dev_priv)) {
		struct intel_hdmi_lpe_audio_pdata *pdata = dev_get_platdata(
			&(dev_priv->lpe_audio.platdev->dev));

		if (pdata) {
			if (eld != NULL) {
				memcpy(pdata->eld.eld_data, eld,
					HDMI_MAX_ELD_BYTES);
				pdata->eld.port_id = port;

				if (tmds_clk_speed)
					pdata->tmds_clock_speed =
						tmds_clk_speed;
			}
			pdata->hdmi_connected = connected;
			if (pdata->notify_audio_lpe)
				pdata->notify_audio_lpe(
					(eld != NULL) ? &pdata->eld : NULL);
			else
				pdata->notify_pending = true;
		} else
			DRM_DEBUG_DRIVER("no audio notification\n");
	}
}
