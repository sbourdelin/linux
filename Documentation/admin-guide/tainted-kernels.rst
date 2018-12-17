Tainted kernels
---------------

The kernel will mark itself as 'tainted' when something occurs that
might be relevant later when investigating problems. Don't worry
yourself too much about this, most of the time it's not a problem to run
a tainted kernel; the information is mainly of interest once someone
wants to investigate some problem, as its real cause might be the event
that got the kernel tainted. That's why the kernel will remain tainted
even after you undo what caused the taint (i.e. unload a proprietary
kernel module), to indicate the kernel remains not trustworthy. That's
also why the kernel will print the tainted state when it noticed
ainternal problem (a 'kernel bug'), a recoverable error ('kernel oops')
or a nonrecoverable error ('kernel panic') and writes debug information
about this to the logs ``dmesg`` outputs. It's also possible to check
the tainted state at runtime through a file in ``/proc/``.


Tainted flag in bugs, oops or panics messages
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

You find the tainted state near the top after the list of loaded
modules.  The state is part of the line that begins with mentioning CPU
('CPU:'), Process ID ('PID:'), and a shorted name of the executed
command ('Comm:') that triggered the event. When followed by **'Not
tainted: '** the kernel was not tainted at the time of the event; if it
was, then it will print **'Tainted: '** and characters either letters or
blanks. The meaning of those characters is explained in below table. The
output for example might state '``Tainted: P   WO``' when the kernel got
tainted earlier because a proprietary Module (``P``) was loaded, a
warning occurred (``W``), and an externally-built module was loaded
(``O``). To decode other letters use below table.


Decoding tainted state at runtime
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

At runtime, you can query the tainted state by reading
``/proc/sys/kernel/tainted``. If that returns ``0``, the kernel is not
tainted; any other number indicates the reasons why it is. You might
find that number in below table if there was only one reason that got
the kernel tainted. If there were multiple reasons you need to decode
the number, as it is a bitfield, where each bit indicates the absence or
presence of a particular type of taint. You can use the following python
command to decode::

	$ python3 -c 'from pprint import pprint; from itertools import zip_longest; pprint(list(zip_longest(range(1,17), reversed(bin(int(open("/proc/sys/kernel/tainted").read()))[2:]),fillvalue="0")))'
	[(1, '1'),
	 (2, '0'),
	 (3, '0'),
	 (4, '0'),
	 (5, '0'),
	 (6, '0'),
	 (7, '0'),
	 (8, '0'),
	 (9, '0'),
	 (10, '1'),
	 (11, '0'),
	 (12, '0'),
	 (13, '1'),
	 (14, '0'),
	 (15, '0'),
	 (16, '0')]

In this case ``/proc/sys/kernel/tainted`` contained ``4609``, as the
kernel got tainted because a proprietary Module (Bit 1) got loaded, a
warning occurred (Bit 10), and an externally-built module got loaded
(Bit 13). To decode other bits use below table.


Table for decoding tainted state
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

===  ===  ======  ========================================================
Bit  Log     Int  Reason that got the kernel tainted
===  ===  ======  ========================================================
 1)  G/P       0  proprietary module got loaded
 2)  _/F       2  module was force loaded
 3)  _/S       4  SMP kernel oops on a officially SMP incapable processor
 4)  _/R       8  module was force unloaded
 5)  _/M      16  processor reported a Machine Check Exception (MCE)
 6)  _/B      32  bad page referenced or some unexpected page flags
 7)  _/U      64  taint requested by userspace application
 8)  _/D     128  kernel died recently, i.e. there was an OOPS or BUG
 9)  _/A     256  ACPI table overridden by user
10)  _/W     512  kernel issued warning
11)  _/C    1024  staging driver got loaded
12)  _/I    2048  workaround for bug in platform firmware in use
13)  _/O    4096  externally-built ("out-of-tree") module got loaded
14)  _/E    8192  unsigned module was loaded
15)  _/L   16384  soft lockup occurred
16)  _/K   32768  Kernel live patched
===  ===  ======  ========================================================

Note: To make reading easier ``_`` is representing a blank in this
table.

More detailed explanation for tainting
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

 1)  ``G`` if all modules loaded have a GPL or compatible license, ``P`` if
     any proprietary module has been loaded.  Modules without a
     MODULE_LICENSE or with a MODULE_LICENSE that is not recognised by
     insmod as GPL compatible are assumed to be proprietary.

 2)  ``F`` if any module was force loaded by ``insmod -f``, ``' '`` if all
     modules were loaded normally.

 3)  ``S`` if the oops occurred on an SMP kernel running on hardware that
     hasn't been certified as safe to run multiprocessor.
     Currently this occurs only on various Athlons that are not
     SMP capable.

 4)  ``R`` if a module was force unloaded by ``rmmod -f``, ``' '`` if all
     modules were unloaded normally.

 5)  ``M`` if any processor has reported a Machine Check Exception,
     ``' '`` if no Machine Check Exceptions have occurred.

 6)  ``B`` if a page-release function has found a bad page reference or
     some unexpected page flags.

 7)  ``U`` if a user or user application specifically requested that the
     Tainted flag be set, ``' '`` otherwise.

 8)  ``D`` if the kernel has died recently, i.e. there was an OOPS or BUG.

 9)  ``A`` if the ACPI table has been overridden.

 10) ``W`` if a warning has previously been issued by the kernel.
     (Though some warnings may set more specific taint flags.)

 11) ``C`` if a staging driver has been loaded.

 12) ``I`` if the kernel is working around a severe bug in the platform
     firmware (BIOS or similar).

 13) ``O`` if an externally-built ("out-of-tree") module has been loaded.

 14) ``E`` if an unsigned module has been loaded in a kernel supporting
     module signature.

 15) ``L`` if a soft lockup has previously occurred on the system.

 16) ``K`` if the kernel has been live patched.

