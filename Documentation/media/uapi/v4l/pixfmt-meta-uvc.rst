.. -*- coding: utf-8; mode: rst -*-

.. _v4l2-meta-fmt-uvc:

*******************************
V4L2_META_FMT_UVC ('UVCH')
*******************************

UVC Payload Header Data


Description
===========

This format describes data, supplied by the UVC driver from metadata video
nodes. That data includes UVC Payload Header contents and auxiliary timing
information, required for precise interpretation of timestamps, contained in
those headers. Buffers, streamed via UVC metadata nodes, are composed of blocks
of variable length. Those blocks contain are described by struct uvc_meta_buf
and contain the following fields:

.. flat-table:: UVC Metadata Block
    :widths: 1 4
    :header-rows:  1
    :stub-columns: 0

    * - Field
      - Description
    * - struct timespec ts;
      - system timestamp, measured by the driver upon reception of the payload
    * - __u16 sof;
      - USB Frame Number, also obtained by the driver
    * - :cspan:`1` *The rest is an exact copy of the payload header:*
    * - __u8 length;
      - length of the rest of the block, including this field
    * - __u8 flags;
      - Flags, indicating presence of other standard UVC fields
    * - __u8 buf[];
      - The rest of the header, possibly including UVC PTS and SCR fields
