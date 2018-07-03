
Intel Stratix10 SoC Service Layer
=================================

Some features of the Intel Stratix10 SoC require a level of privilege
higher than the kernel is granted. Such secure features include
FPGA programming. In terms of the ARMv8 architecture, the kernel runs
at Exception Level 1 (EL1), access to the features requires
Exception Level 3 (EL3).

The Intel Stratix10 SoC service layer provides an in kernel API for
drivers to request access to the secure features. The requests are queued
and processed one by one. ARM’s SMCCC is used to pass the execution
of the requests on to a secure monitor (EL3).

.. kernel-doc:: include/linux/stratix10-svc-client.h
   :functions: stratix10_svc_command_code

.. kernel-doc:: include/linux/stratix10-svc-client.h
   :functions: stratix10_svc_client_msg

.. kernel-doc:: include/linux/stratix10-svc-client.h
   :functions: stratix10_svc_command_reconfig_payload

.. kernel-doc:: include/linux/stratix10-svc-client.h
   :functions: stratix10_svc_cb_data

.. kernel-doc:: include/linux/stratix10-svc-client.h
   :functions: stratix10_svc_client

.. kernel-doc:: drivers/misc/stratix10-svc.c
   :export:
