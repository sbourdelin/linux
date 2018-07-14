/*
 * Copyright © 2014 Intel Corporation
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
 */

#ifndef _I915_COMPONENT_H_
#define _I915_COMPONENT_H_

#include <linux/mei_cl_bus.h>
#include <linux/mei_hdcp.h>
#include <drm/drm_hdcp.h>

/* MAX_PORT is the number of port
 * It must be sync with I915_MAX_PORTS defined i915_drv.h
 */
#define MAX_PORTS 6

/**
 * struct i915_audio_component_ops - Ops implemented by i915 driver, called by hda driver
 */
struct i915_audio_component_ops {
	/**
	 * @owner: i915 module
	 */
	struct module *owner;
	/**
	 * @get_power: get the POWER_DOMAIN_AUDIO power well
	 *
	 * Request the power well to be turned on.
	 */
	void (*get_power)(struct device *);
	/**
	 * @put_power: put the POWER_DOMAIN_AUDIO power well
	 *
	 * Allow the power well to be turned off.
	 */
	void (*put_power)(struct device *);
	/**
	 * @codec_wake_override: Enable/disable codec wake signal
	 */
	void (*codec_wake_override)(struct device *, bool enable);
	/**
	 * @get_cdclk_freq: Get the Core Display Clock in kHz
	 */
	int (*get_cdclk_freq)(struct device *);
	/**
	 * @sync_audio_rate: set n/cts based on the sample rate
	 *
	 * Called from audio driver. After audio driver sets the
	 * sample rate, it will call this function to set n/cts
	 */
	int (*sync_audio_rate)(struct device *, int port, int pipe, int rate);
	/**
	 * @get_eld: fill the audio state and ELD bytes for the given port
	 *
	 * Called from audio driver to get the HDMI/DP audio state of the given
	 * digital port, and also fetch ELD bytes to the given pointer.
	 *
	 * It returns the byte size of the original ELD (not the actually
	 * copied size), zero for an invalid ELD, or a negative error code.
	 *
	 * Note that the returned size may be over @max_bytes.  Then it
	 * implies that only a part of ELD has been copied to the buffer.
	 */
	int (*get_eld)(struct device *, int port, int pipe, bool *enabled,
		       unsigned char *buf, int max_bytes);
};

/**
 * struct i915_audio_component_audio_ops - Ops implemented by hda driver, called by i915 driver
 */
struct i915_audio_component_audio_ops {
	/**
	 * @audio_ptr: Pointer to be used in call to pin_eld_notify
	 */
	void *audio_ptr;
	/**
	 * @pin_eld_notify: Notify the HDA driver that pin sense and/or ELD information has changed
	 *
	 * Called when the i915 driver has set up audio pipeline or has just
	 * begun to tear it down. This allows the HDA driver to update its
	 * status accordingly (even when the HDA controller is in power save
	 * mode).
	 */
	void (*pin_eld_notify)(void *audio_ptr, int port, int pipe);
};

/**
 * struct i915_audio_component - Used for direct communication between i915 and hda drivers
 */
struct i915_audio_component {
	/**
	 * @dev: i915 device, used as parameter for ops
	 */
	struct device *dev;
	/**
	 * @aud_sample_rate: the array of audio sample rate per port
	 */
	int aud_sample_rate[MAX_PORTS];
	/**
	 * @ops: Ops implemented by i915 driver, called by hda driver
	 */
	const struct i915_audio_component_ops *ops;
	/**
	 * @audio_ops: Ops implemented by hda driver, called by i915 driver
	 */
	const struct i915_audio_component_audio_ops *audio_ops;
};

struct i915_hdcp_component_ops {
	/**
	 * @owner: mei_hdcp module
	 */
	struct module *owner;
	int (*initiate_hdcp2_session)(struct mei_cl_device *cldev,
				      struct mei_hdcp_data *data,
				      struct hdcp2_ake_init *ake_data);
	int
	(*verify_receiver_cert_prepare_km)(struct mei_cl_device *cldev,
					   struct mei_hdcp_data *data,
					   struct hdcp2_ake_send_cert *rx_cert,
					   bool *km_stored,
					   struct hdcp2_ake_no_stored_km
								*ek_pub_km,
					   size_t *msg_sz);
	int (*verify_hprime)(struct mei_cl_device *cldev,
			     struct mei_hdcp_data *data,
			     struct hdcp2_ake_send_hprime *rx_hprime);
	int (*store_pairing_info)(struct mei_cl_device *cldev,
				  struct mei_hdcp_data *data,
				  struct hdcp2_ake_send_pairing_info
								*pairing_info);
	int (*initiate_locality_check)(struct mei_cl_device *cldev,
				       struct mei_hdcp_data *data,
				       struct hdcp2_lc_init *lc_init_data);
	int (*verify_lprime)(struct mei_cl_device *cldev,
			     struct mei_hdcp_data *data,
			     struct hdcp2_lc_send_lprime *rx_lprime);
	int (*get_session_key)(struct mei_cl_device *cldev,
			       struct mei_hdcp_data *data,
			       struct hdcp2_ske_send_eks *ske_data);
	int
	(*repeater_check_flow_prepare_ack)(struct mei_cl_device *cldev,
					   struct mei_hdcp_data *data,
					   struct hdcp2_rep_send_receiverid_list
								*rep_topology,
					   struct hdcp2_rep_send_ack
								*rep_send_ack);
	int (*verify_mprime)(struct mei_cl_device *cldev,
			     struct mei_hdcp_data *data,
			     struct hdcp2_rep_stream_ready *stream_ready);
	int (*enable_hdcp_authentication)(struct mei_cl_device *cldev,
					  struct mei_hdcp_data *data);
	int (*close_hdcp_session)(struct mei_cl_device *cldev,
				  struct mei_hdcp_data *data);
};

/**
 * struct i915_component_master - Used for communication between i915
 * and any other drivers for the services of different feature.
 */
struct i915_component_master {
	/**
	 * @i915_kdev: Kdev of I915. Used from the client component for
	 * removing the reference to mei_cldev.
	 */
	struct device *i915_kdev;
	/**
	 * @mei_cldev: mei client device, used as parameter for ops
	 */
	struct mei_cl_device *mei_cldev;
	/**
	 * @ops: Ops implemented by mei_hdcp driver, used by i915 driver.
	 */
	const struct i915_hdcp_component_ops *hdcp_ops;

	/*
	 * Add here the interface details between I915 and interested modules.
	 */
};

#endif /* _I915_COMPONENT_H_ */
