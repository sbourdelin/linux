/*
 * Copyright (C) STMicroelectronics SA 2015
 * Authors: Yannick Fertre <yannick.fertre@st.com>
 *          Hugues Fruchet <hugues.fruchet@st.com>
 * License terms:  GNU General Public License (GPL), version 2
 */

#ifndef HVA_HW_H
#define HVA_HW_H

/* HVA Versions */
#define HVA_VERSION_UNKNOWN    0x000
#define HVA_VERSION_V397       0x397
#define HVA_VERSION_V400       0x400

enum hva_hw_cmd_type {
	/* RESERVED = 0x00 */
	/* RESERVED = 0x01 */
	H264_ENC = 0x02,
	JPEG_ENC = 0x03,
	/* SW synchro task (reserved in HW) */
	/* RESERVED = 0x04 */
	/* RESERVED = 0x05 */
	VP8_ENC = 0x06,
	/* RESERVED = 0x07 */
	REMOVE_CLIENT = 0x08,
	FREEZE_CLIENT = 0x09,
	START_CLIENT = 0x0A,
	FREEZE_ALL = 0x0B,
	START_ALL = 0x0C,
	REMOVE_ALL = 0x0D
};

/**
 * hw encode error values
 * NO_ERROR: Success, Task OK
 * JPEG_BITSTREAM_OVERSIZE: VECJPEG Picture size > Max bitstream size
 * H264_BITSTREAM_OVERSIZE: VECH264 Bitstream size > bitstream buffer
 * H264_FRAME_SKIPPED: VECH264 Frame skipped (refers to CPB Buffer Size)
 * H264_SLICE_LIMIT_SIZE: VECH264 MB > slice limit size
 * H264_MAX_SLICE_NUMBER: VECH264 max slice number reached
 * H264_SLICE_READY: VECH264 Slice ready
 * TASK_LIST_FULL: HVA/FPC task list full
		   (discard latest transform command)
 * UNKNOWN_COMMAND: Transform command not known by HVA/FPC
 * WRONG_CODEC_OR_RESOLUTION: Wrong Codec or Resolution Selection
 * NO_INT_COMPLETION: Time-out on interrupt completion
 * LMI_ERR: Local Memory Interface Error
 * EMI_ERR: External Memory Interface Error
 * HECMI_ERR: HEC Memory Interface Error
 */
enum hva_hw_error {
	NO_ERROR = 0x0,
	JPEG_BITSTREAM_OVERSIZE = 0x1,
	H264_BITSTREAM_OVERSIZE = 0x2,
	H264_FRAME_SKIPPED = 0x4,
	H264_SLICE_LIMIT_SIZE = 0x5,
	H264_MAX_SLICE_NUMBER = 0x7,
	H264_SLICE_READY = 0x8,
	TASK_LIST_FULL = 0xF0,
	UNKNOWN_COMMAND = 0xF1,
	WRONG_CODEC_OR_RESOLUTION = 0xF4,
	NO_INT_COMPLETION = 0x100,
	LMI_ERR = 0x101,
	EMI_ERR = 0x102,
	HECMI_ERR = 0x103,
};

int hva_hw_probe(struct platform_device *pdev, struct hva_device *hva);
void hva_hw_remove(struct hva_device *hva);
int hva_hw_runtime_suspend(struct device *dev);
int hva_hw_runtime_resume(struct device *dev);
int hva_hw_execute_task(struct hva_ctx *ctx, enum hva_hw_cmd_type cmd,
			struct hva_buffer *task);

#endif /* HVA_HW_H */
