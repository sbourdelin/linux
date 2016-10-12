.. -*- coding: utf-8; mode: rst -*-

.. _V4L2-SDR-FMT-SCU18BE:

******************************
V4L2_SDR_FMT_SCU18BE ('SCU18')
******************************

Sliced complex unsigned 18-bit big endian IQ sample


Description
===========

This format contains a sequence of complex number samples. Each complex
number consist of two parts called In-phase and Quadrature (IQ). Both I
and Q are represented as a 18 bit unsigned big endian number. I value
starts first and Q value starts at an offset equalling half of the buffer
size. 16 bit data is stored in 18 bit space with unused stuffed bits
padded with 0.

**Byte Order.**
Each cell is one byte.


.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    -  .. row 1

       -  start + 0:

       -  I'\ :sub:`0[D17:D10]`

       -  I'\ :sub:`0[D9:D2]`

       -  I'\ :sub:`0[D1:D0]`

    -  .. row 2

       -  start + buffer_size/2:

       -  Q'\ :sub:`0[D17:D10]`

       -  Q'\ :sub:`0[D9:D2]`

       -  Q'\ :sub:`0[D1:D0]`
