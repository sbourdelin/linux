.. -*- coding: utf-8; mode: rst -*-

.. _FE_GET_PROPERTY:

**************************************************************
ioctl FE_SET_PROPERTY, FE_GET_PROPERTY, FE_SET_PROPERTY_SHORT
**************************************************************

Name
====

FE_SET_PROPERTY and FE_SET_PROPERTY_SHORT set one or more frontend properties. FE_GET_PROPERTY returns one or more frontend properties.


Synopsis
========

.. c:function:: int ioctl( int fd, FE_GET_PROPERTY, struct dtv_properties *argp )
    :name: FE_GET_PROPERTY

.. c:function:: int ioctl( int fd, FE_SET_PROPERTY, struct dtv_properties *argp )
    :name: FE_SET_PROPERTY

.. c:function:: int ioctl( int fd, FE_SET_PROPERTY_SHORT, struct dtv_properties_short *argp )
    :name: FE_SET_PROPERTY_SHORT

Arguments
=========

``fd``
    File descriptor returned by :ref:`open() <frontend_f_open>`.

``argp``
    Pointer to struct :c:type:`dtv_properties` or
	struct :c:type:`dtv_properties_short`.


Description
===========

All DVB frontend devices support the ``FE_SET_PROPERTY``, ``FE_GET_PROPERTY``
and ``FE_SET_PROPERTY_SHORT`` ioctls. The supported  properties and
statistics depends on the delivery system and on the device:

-  ``FE_SET_PROPERTY:``

   -  This ioctl is used to set one or more frontend properties.

   -  This is the basic command to request the frontend to tune into
      some frequency and to start decoding the digital TV signal.

   -  This call requires read/write access to the device.

   -  At return, the values are updated to reflect the actual parameters
      used.

-  ``FE_GET_PROPERTY:``

   -  This ioctl is used to get properties and statistics from the
      frontend.

   -  No properties are changed, and statistics aren't reset.

   -  This call only requires read-only access to the device.

-  ``FE_SET_PROPERTY_SHORT:``

   -  This ioctl is similar to FE_SET_PROPERTY ioctl mentioned above
      except that the arguments of the former utilize a struct :c:type:`dtv_property_short`
      which is smaller in size.

Return Value
============

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.
