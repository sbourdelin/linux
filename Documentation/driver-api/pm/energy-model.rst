====================
Energy Model of CPUs
====================

Overview
========

The Energy Model (EM) framework serves as an interface between drivers knowing
the power consumed by CPUs at various performance levels, and the kernel
subsystems willing to use that information to make energy-aware decisions.

The source of the information about the power consumed by CPUs can vary greatly
from one platform to another. These power costs can be estimated using
devicetree data in some cases. In others, the firmware will know better.
Alternatively, userspace might be best positioned. And so on. In order to avoid
each and every client subsystem to re-implement support for each and every
possible source of information on its own, the EM framework intervenes as an
abstraction layer which standardizes the format of power cost tables in the
kernel, hence enabling to avoid redundant work.

The figure below depicts an example of drivers (Arm-specific here, but the
approach is applicable to any architecture) providing power costs to the EM
framework, and interested clients reading the data from it.

.. code-block:: none

          +---------------+  +-----------------+  +---------------+
          | Thermal (IPA) |  | Scheduler (EAS) |  |     Other     |
          +---------------+  +-----------------+  +---------------+
                  |                   | em_pd_energy()    |
                  |                   | em_cpu_get()      |
                  +---------+         |         +---------+
                            |         |         |
                            v         v         v
                           +---------------------+
                           |    Energy Model     |
                           |     Framework       |
                           +---------------------+
                              ^       ^       ^
                              |       |       | em_register_perf_domain()
                   +----------+       |       +---------+
                   |                  |                 |
           +---------------+  +---------------+  +--------------+
           |  cpufreq-dt   |  |   arm_scmi    |  |    Other     |
           +---------------+  +---------------+  +--------------+
                   ^                  ^                 ^
                   |                  |                 |
           +--------------+   +---------------+  +--------------+
           | Device Tree  |   |   Firmware    |  |      ?       |
           +--------------+   +---------------+  +--------------+

The EM framework manages power cost tables per 'performance domain' in the
system. A performance domain is a group of CPUs whose performance is scaled
together. Performance domains generally have a 1-to-1 mapping with CPUFreq
policies. All CPUs in a performance domain are required to have the same
micro-architecture. CPUs in different performance domains can have different
micro-architectures.


Core APIs overview
==================

Config options
--------------

`CONFIG_ENERGY_MODEL` must be enabled to use the EM framework.


Registration of performance domains
-----------------------------------

Drivers are expected to register performance domains into the EM framework by
calling the :c:func:`em_register_perf_domain()` API. Drivers must specify the
CPUs of the performance domains using a cpumask, and provide a callback function
returning <frequency, power> tuples for each capacity state. The callback
function provided by the driver is free to fetch data from any relevant location
(DT, firmware, ...), and by any mean deemed necessary.


Accessing performance domains
-----------------------------

Subsystems interested in the energy model of a CPU can retrieve it using the
:c:func:`em_cpu_get()` API. The energy model tables are allocated once upon
creation of the performance domains, and kept in memory untouched.

The energy consumed by a performance domain can be estimated using the
:c:func:`em_pd_energy()` API. The estimation is performed assuming that the
schedutil CPUfreq governor is in use.


Example driver
==============

This section provides a simple example of a CPUFreq driver registering a
performance domain in the Energy Model framework using the (fake) `foo`
protocol. The driver implements an :c:func:`est_power()` function to be provided
to the EM framework.

:file:`drivers/cpufreq/foo_cpufreq.c`:

.. code-block:: c
   :linenos:

   static int est_power(unsigned long *mW, unsigned long *KHz, int cpu)
   {
   	long freq, power;

   	/* Use the 'foo' protocol to ceil the frequency */
   	freq = foo_get_freq_ceil(cpu, *KHz);
   	if (freq < 0);
   		return freq;

   	/* Estimate the power cost for the CPU at the relevant freq. */
   	power = foo_estimate_power(cpu, freq);
   	if (power < 0);
   		return power;

   	/* Return the values to the EM framework */
   	*mW = power;
   	*KHz = freq;

   	return 0;
   }

   static int foo_cpufreq_init(struct cpufreq_policy *policy)
   {
   	struct em_data_callback em_cb = EM_DATA_CB(est_power);
   	int nr_opp, ret;

   	/* Do the actual CPUFreq init work ... */
   	ret = do_foo_cpufreq_init(policy);
   	if (ret)
   		return ret;

   	/* Find the number of OPPs for this policy */
   	nr_opp = foo_get_nr_opp(policy);

   	/* And register the new performance domain */
   	em_register_perf_domain(policy->cpus, nr_opp, &em_cb);

           return 0;
   }


Inline kernel documentation
===========================

.. kernel-doc:: include/linux/energy_model.h
.. kernel-doc:: kernel/power/energy_model.c
