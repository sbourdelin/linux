i.MX Video Capture Driver
=========================

Introduction
------------

The Freescale i.MX5/6 contains an Image Processing Unit (IPU), which
handles the flow of image frames to and from capture devices and
display devices.

For image capture, the IPU contains the following internal subunits:

- Image DMA Controller (IDMAC)
- Camera Serial Interface (CSI)
- Image Converter (IC)
- Sensor Multi-FIFO Controller (SMFC)
- Image Rotator (IRT)
- Video De-Interlace Controller (VDIC)

The IDMAC is the DMA controller for transfer of image frames to and from
memory. Various dedicated DMA channels exist for both video capture and
display paths.

The CSI is the frontend capture unit that interfaces directly with
capture sensors over Parallel, BT.656/1120, and MIPI CSI-2 busses.

The IC handles color-space conversion, resizing, and rotation
operations. There are three independent "tasks" within the IC that can
carry out conversions concurrently: pre-processing encoding,
pre-processing preview, and post-processing.

The SMFC is composed of four independent channels that each can transfer
captured frames from sensors directly to memory concurrently.

The IRT carries out 90 and 270 degree image rotation operations.

The VDIC handles the conversion of interlaced video to progressive, with
support for different motion compensation modes (low, medium, and high
motion). The deinterlaced output frames from the VDIC can be sent to the
IC pre-process preview task for further conversions.

In addition to the IPU internal subunits, there are also two units
outside the IPU that are also involved in video capture on i.MX:

- MIPI CSI-2 Receiver for camera sensors with the MIPI CSI-2 bus
  interface. This is a Synopsys DesignWare core.
- A video multiplexer for selecting among multiple sensor inputs to
  send to a CSI.

For more info, refer to the latest versions of the i.MX5/6 reference
manuals listed under References.


Features
--------

Some of the features of this driver include:

- Many different pipelines can be configured via media controller API,
  that correspond to the hardware video capture pipelines supported in
  the i.MX.

- Supports parallel, BT.565, and MIPI CSI-2 interfaces.

- Up to four concurrent sensor acquisitions, by configuring each
  sensor's pipeline using independent entities. This is currently
  demonstrated with the SabreSD and SabreLite reference boards with
  independent OV5642 and MIPI CSI-2 OV5640 sensor modules.

- Scaling, color-space conversion, and image rotation via IC task
  subdevs.

- Many pixel formats supported (RGB, packed and planar YUV, partial
  planar YUV).

- The IC pre-process preview subdev supports motion compensated
  de-interlacing using the VDIC, with three motion compensation modes:
  low, medium, and high motion. The mode is specified with a custom
  control. Pipelines are defined that allow sending frames to the
  preview subdev directly from the CSI or from the SMFC.

- Includes a Frame Interval Monitor (FIM) that can correct vertical sync
  problems with the ADV718x video decoders. See below for a description
  of the FIM.


Capture Pipelines
-----------------

The following describe the various use-cases supported by the pipelines.

The links shown do not include the frontend sensor, video mux, or mipi
csi-2 receiver links. This depends on the type of sensor interface
(parallel or mipi csi-2). So in all cases, these pipelines begin with:

sensor -> ipu_csi_mux -> ipu_csi -> ...

for parallel sensors, or:

sensor -> imx-mipi-csi2 -> (ipu_csi_mux) -> ipu_csi -> ...

for mipi csi-2 sensors. The imx-mipi-csi2 receiver may need to route
to the video mux (ipu_csi_mux) before sending to the CSI, depending
on the mipi csi-2 virtual channel, hence ipu_csi_mux is shown in
parenthesis.

Unprocessed Video Capture:
--------------------------

Send frames directly from sensor to camera interface, with no
conversions:

-> ipu_smfc -> camif

Note the ipu_smfc can do pixel reordering within the same colorspace.
For example, its sink pad can take UYVY2X8, but its source pad can
output YUYV2X8.

IC Direct Conversions:
----------------------

This pipeline uses the preprocess encode entity to route frames directly
from the CSI to the IC (bypassing the SMFC), to carry out scaling up to
1024x1024 resolution, CSC, and image rotation:

-> ipu_ic_prpenc -> camif

This can be a useful capture pipeline for heavily loaded memory bus
traffic environments, since it has minimal IDMAC channel usage.

Post-Processing Conversions:
----------------------------

This pipeline routes frames from the SMFC to the post-processing
entity. In addition to CSC and rotation, this entity supports tiling
which allows scaled output beyond the 1024x1024 limitation of the IC
(up to 4096x4096 scaling output is supported):

-> ipu_smfc -> ipu_ic_pp -> camif

Motion Compensated De-interlace:
--------------------------------

This pipeline routes frames from the SMFC to the preprocess preview
entity to support motion-compensated de-interlacing using the VDIC,
scaling up to 1024x1024, and CSC:

-> ipu_smfc -> ipu_ic_prpvf -> camif

This pipeline also carries out the same conversions as above, but routes
frames directly from the CSI to the IC preprocess preview entity for
minimal memory bandwidth usage (note: this pipeline only works in
"high motion" mode):

-> ipu_ic_prpvf -> camif

This pipeline takes the motion-compensated de-interlaced frames and
sends them to the post-processor, to support motion-compensated
de-interlacing, scaling up to 4096x4096, CSC, and rotation:

-> (ipu_smfc) -> ipu_ic_prpvf -> ipu_ic_pp -> camif


Usage Notes
-----------

Many of the subdevs require information from the active sensor in the
current pipeline when configuring pad formats. Therefore the media links
should be established before configuring the media pad formats.

Similarly, the capture v4l2 interface subdev inherits controls from the
active subdevs in the current pipeline at link-setup time. Therefore the
capture links should be the last links established in order for capture
to "see" and inherit all possible controls.

The following platforms have been tested:


SabreLite with OV5642 and OV5640
--------------------------------

This platform requires the OmniVision OV5642 module with a parallel
camera interface, and the OV5640 module with a MIPI CSI-2
interface. Both modules are available from Boundary Devices:

https://boundarydevices.com/products/nit6x_5mp
https://boundarydevices.com/product/nit6x_5mp_mipi

Note that if only one camera module is available, the other sensor
node can be disabled in the device tree.

The following basic example configures unprocessed video capture
pipelines for both sensors. The OV5642 is routed to camif0
(usually /dev/video0), and the OV5640 (transmitting on mipi csi-2
virtual channel 1) is routed to camif1 (usually /dev/video1). Both
sensors are configured to output 640x480, UYVY (not shown: all pad
field types should be set to "NONE"):

.. code-block:: none

   # Setup links for OV5642
   media-ctl -l '"ov5642 1-0042":0 -> "ipu1_csi0_mux":1[1]'
   media-ctl -l '"ipu1_csi0_mux":2 -> "ipu1_csi0":0[1]'
   media-ctl -l '"ipu1_csi0":1 -> "ipu1_smfc0":0[1]'
   media-ctl -l '"ipu1_smfc0":1 -> "camif0":0[1]'
   media-ctl -l '"camif0":1 -> "camif0 devnode":0[1]'
   # Setup links for OV5640
   media-ctl -l '"ov5640_mipi 1-0040":0 -> "imx-mipi-csi2":0[1]'
   media-ctl -l '"imx-mipi-csi2":2 -> "ipu1_csi1":0[1]'
   media-ctl -l '"ipu1_csi1":1 -> "ipu1_smfc1":0[1]'
   media-ctl -l '"ipu1_smfc1":1 -> "camif1":0[1]'
   media-ctl -l '"camif1":1 -> "camif1 devnode":0[1]'
   # Configure pads for OV5642 pipeline
   media-ctl -V "\"ov5642 1-0042\":0 [fmt:YUYV2X8/640x480]"
   media-ctl -V "\"ipu1_csi0_mux\":1 [fmt:YUYV2X8/640x480]"
   media-ctl -V "\"ipu1_csi0_mux\":2 [fmt:YUYV2X8/640x480]"
   media-ctl -V "\"ipu1_csi0\":0 [fmt:YUYV2X8/640x480]"
   media-ctl -V "\"ipu1_csi0\":1 [fmt:YUYV2X8/640x480]"
   media-ctl -V "\"ipu1_smfc0\":0 [fmt:YUYV2X8/640x480]"
   media-ctl -V "\"ipu1_smfc0\":1 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"camif0\":0 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"camif0\":1 [fmt:UYVY2X8/640x480]"
   # Configure pads for OV5640 pipeline
   media-ctl -V "\"ov5640_mipi 1-0040\":0 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"imx-mipi-csi2\":0 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"imx-mipi-csi2\":2 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"ipu1_csi1\":0 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"ipu1_csi1\":1 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"ipu1_smfc1\":0 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"ipu1_smfc1\":1 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"camif1\":0 [fmt:UYVY2X8/640x480]"
   media-ctl -V "\"camif1\":1 [fmt:UYVY2X8/640x480]"

Streaming can then begin independently on device nodes /dev/video0
and /dev/video1.

SabreAuto with ADV7180 decoder
------------------------------

The following example configures a pipeline to capture from the ADV7182
video decoder, assuming NTSC 720x480 input signals, with Motion
Compensated de-interlacing (not shown: all pad field types should be set
as indicated). $outputfmt can be any format supported by the
ipu1_ic_prpvf entity at its output pad:

.. code-block:: none

   # Setup links
   media-ctl -l '"adv7180 3-0021":0 -> "ipu1_csi0_mux":1[1]'
   media-ctl -l '"ipu1_csi0_mux":2 -> "ipu1_csi0":0[1]'
   media-ctl -l '"ipu1_csi0":1 -> "ipu1_smfc0":0[1]'
   media-ctl -l '"ipu1_smfc0":1 -> "ipu1_ic_prpvf":0[1]'
   media-ctl -l '"ipu1_ic_prpvf":1 -> "camif0":0[1]'
   media-ctl -l '"camif0":1 -> "camif0 devnode":0[1]'
   # Configure pads
   # pad field types for below pads must be an interlaced type
   # such as "ALTERNATE"
   media-ctl -V "\"adv7180 3-0021\":0 [fmt:UYVY2X8/720x480]"
   media-ctl -V "\"ipu1_csi0_mux\":1 [fmt:UYVY2X8/720x480]"
   media-ctl -V "\"ipu1_csi0_mux\":2 [fmt:UYVY2X8/720x480]"
   media-ctl -V "\"ipu1_csi0\":0 [fmt:UYVY2X8/720x480]"
   media-ctl -V "\"ipu1_csi0\":1 [fmt:UYVY2X8/720x480]"
   media-ctl -V "\"ipu1_smfc0\":0 [fmt:UYVY2X8/720x480]"
   media-ctl -V "\"ipu1_smfc0\":1 [fmt:UYVY2X8/720x480]"
   media-ctl -V "\"ipu1_ic_prpvf\":0 [fmt:UYVY2X8/720x480]"
   # pad field types for below pads must be "NONE"
   media-ctl -V "\"ipu1_ic_prpvf\":1 [fmt:$outputfmt]"
   media-ctl -V "\"camif0\":0 [fmt:$outputfmt]"
   media-ctl -V "\"camif0\":1 [fmt:$outputfmt]"

Streaming can then begin on /dev/video0.

This platform accepts Composite Video analog inputs to the ADV7180 on
Ain1 (connector J42) and Ain3 (connector J43).

To switch to Ain1:

.. code-block:: none

   # v4l2-ctl -i0

To switch to Ain3:

.. code-block:: none

   # v4l2-ctl -i1


Frame Interval Monitor
----------------------

The adv718x decoders can occasionally send corrupt fields during
NTSC/PAL signal re-sync (too little or too many video lines). When
this happens, the IPU triggers a mechanism to re-establish vertical
sync by adding 1 dummy line every frame, which causes a rolling effect
from image to image, and can last a long time before a stable image is
recovered. Or sometimes the mechanism doesn't work at all, causing a
permanent split image (one frame contains lines from two consecutive
captured images).

From experiment it was found that during image rolling, the frame
intervals (elapsed time between two EOF's) drop below the nominal
value for the current standard, by about one frame time (60 usec),
and remain at that value until rolling stops.

While the reason for this observation isn't known (the IPU dummy
line mechanism should show an increase in the intervals by 1 line
time every frame, not a fixed value), we can use it to detect the
corrupt fields using a frame interval monitor. If the FIM detects a
bad frame interval, a subdev event is sent. In response, userland can
issue a streaming restart to correct the rolling/split image.

The FIM is implemented in the imx-csi entity, and the entities that have
direct connections to the CSI call into the FIM to monitor the frame
intervals: ipu_smfc, ipu_ic_prpenc, and ipu_prpvf (when configured with
a direct link from ipu_csi). Userland can register with the FIM event
notifications on the imx-csi subdev device node
(V4L2_EVENT_IMX_FRAME_INTERVAL).

The imx-csi entity includes custom controls to tweak some dials for FIM.
If one of these controls is changed during streaming, the FIM will be
reset and will continue at the new settings.

- V4L2_CID_IMX_FIM_ENABLE

Enable/disable the FIM.

- V4L2_CID_IMX_FIM_NUM

How many frame interval errors to average before comparing against the
nominal frame interval reported by the sensor. This can reduce noise
from interrupt latency.

- V4L2_CID_IMX_FIM_TOLERANCE_MIN

If the averaged intervals fall outside nominal by this amount, in
microseconds, streaming will be restarted.

- V4L2_CID_IMX_FIM_TOLERANCE_MAX

If any interval errors are higher than this value, those error samples
are discarded and do not enter into the average. This can be used to
discard really high interval errors that might be due to very high
system load, causing excessive interrupt latencies.

- V4L2_CID_IMX_FIM_NUM_SKIP

How many frames to skip after a FIM reset or stream restart before
FIM begins to average intervals. It has been found that there can
be a few bad frame intervals after stream restart which are not
attributed to adv718x sending a corrupt field, so this is used to
skip those frames to prevent unnecessary restarts.

Finally, all the defaults for these controls can be modified via a
device tree child node of the ipu_csi port nodes, see
Documentation/devicetree/bindings/media/imx.txt.


SabreSD with MIPI CSI-2 OV5640
------------------------------

The device tree for SabreSD includes OF graphs for both the parallel
OV5642 and the MIPI CSI-2 OV5640, but as of this writing only the MIPI
CSI-2 OV5640 has been tested, so the OV5642 node is currently disabled.
The OV5640 module connects to MIPI connector J5 (sorry I don't have the
compatible module part number or URL).

The following example configures a post-processing pipeline to capture
from the OV5640 (not shown: all pad field types should be set to
"NONE"). $sensorfmt can be any format supported by the
OV5640. $outputfmt can be any format supported by the ipu1_ic_pp1
entity at its output pad:


.. code-block:: none

   # Setup links
   media-ctl -l '"ov5640_mipi 1-003c":0 -> "imx-mipi-csi2":0[1]'
   media-ctl -l '"imx-mipi-csi2":2 -> "ipu1_csi1":0[1]'
   media-ctl -l '"ipu1_csi1":1 -> "ipu1_smfc1":0[1]'
   media-ctl -l '"ipu1_smfc1":1 -> "ipu1_ic_pp1":0[1]'
   media-ctl -l '"ipu1_ic_pp1":1 -> "camif0":0[1]'
   media-ctl -l '"camif0":1 -> "camif0 devnode":0[1]'
   # Configure pads
   media-ctl -V "\"ov5640_mipi 1-003c\":0 [fmt:$sensorfmt]"
   media-ctl -V "\"imx-mipi-csi2\":0 [fmt:$sensorfmt]"
   media-ctl -V "\"imx-mipi-csi2\":2 [fmt:$sensorfmt]"
   media-ctl -V "\"ipu1_csi1\":0 [fmt:$sensorfmt]"
   media-ctl -V "\"ipu1_csi1\":1 [fmt:$sensorfmt]"
   media-ctl -V "\"ipu1_smfc1\":0 [fmt:$sensorfmt]"
   media-ctl -V "\"ipu1_smfc1\":1 [fmt:$sensorfmt]"
   media-ctl -V "\"ipu1_ic_pp1\":0 [fmt:$sensorfmt]"
   media-ctl -V "\"ipu1_ic_pp1\":1 [fmt:$outputfmt]"
   media-ctl -V "\"camif0\":0 [fmt:$outputfmt]"
   media-ctl -V "\"camif0\":1 [fmt:$outputfmt]"

Streaming can then begin on /dev/video0.



Known Issues
------------

1. When using 90 or 270 degree rotation control at capture resolutions
   near the IC resizer limit of 1024x1024, and combined with planar
   pixel formats (YUV420, YUV422p), frame capture will often fail with
   no end-of-frame interrupts from the IDMAC channel. To work around
   this, use lower resolution and/or packed formats (YUYV, RGB3, etc.)
   when 90 or 270 rotations are needed.


File list
---------

drivers/staging/media/imx/
include/media/imx.h
include/uapi/media/imx.h

References
----------

[1] "i.MX 6Dual/6Quad Applications Processor Reference Manual"
[2] "i.MX 6Solo/6DualLite Applications Processor Reference Manual"


Author
------
Steve Longerbeam <steve_longerbeam@mentor.com>

Copyright (C) 2012-2016 Mentor Graphics Inc.
