Spectre side channels
=====================

Spectre is a class of side channel attacks against modern CPUs that
exploit branch prediction and speculative execution to read memory,
possibly bypassing access controls. These exploits do not modify memory.

This document covers Spectre variant 1 and 2.

Affected processors
-------------------

The vulnerability affects a wide range of modern high performance
processors, since most modern high speed processors use branch prediction
and speculative execution.

The following CPUs are vulnerable:

    - Intel Core, Atom, Pentium, Xeon CPUs
    - AMD CPUs like Phenom, EPYC, Zen.
    - IBM processors like POWER and zSeries
    - Higher end ARM processors
    - Apple CPUs
    - Higher end MIPS CPUs
    - Likely most other high performance CPUs. Contact your CPU vendor for details.

This document describes the mitigations on Intel CPUs. Mitigations
on other architectures may be different.

Related CVEs
------------

The following CVE entries describe Spectre variants:

   =============   =======================  ==========
   CVE-2017-5753   Bounds check bypass      Spectre-V1
   CVE-2017-5715   Branch target injection  Spectre-V2

Problem
-------

CPUs have shared caches, such as buffers for branch prediction, which are
later used to guide speculative execution. These buffers are not flushed
over context switches or change in privilege levels. Malicious software
might influence these buffers and trigger specific speculative execution
in the kernel or different user processes.  This speculative execution can
then be used to read data in memory and cause side effects, such as displacing
data in a data cache. The side effect can then later be measured by the
malicious software, and used to determine the memory values read speculatively.

Spectre attacks allow tricking other software to disclose
values in their memory.

In a typical Spectre variant 1 attack, the attacker passes an parameter
to a victim. The victim boundary checks the parameter and rejects illegal
values. However due to speculation over branch prediction the code path
for correct values might be speculatively executed, then reference memory
controlled by the input parameter and leave measurable side effects in
the caches.  The attacker could then measure these side effects
and determine the leaked value.

There are some extensions of Spectre variant 1 attacks for reading
data over the network, see [2]. However the attacks are very
difficult, low bandwidth and fragile and considered low risk.

For Spectre variant 2 the attacker poisons the indirect branch
predictors of the CPU. Then control is passed to the victim, which
executes indirect branches. Due to the poisoned branch predictor data
the CPU can speculatively execute arbitrary code in the victim's
address space, such as a code sequence ("disclosure gadget") that
reads arbitrary data on some input parameter and causes a measurable
cache side effect based on the value. The attacker can then measure
this side effect after gaining control again and determine the value.

The most useful gadgets take an attacker-controlled input parameter so
that the memory read can be controlled. Gadgets without input parameters
might be possible, but the attacker would have very little control over what
memory can be read, reducing the risk of the attack revealing useful data.

Attack scenarios
----------------

Here is a list of attack scenarios that have been anticipated, but
may not cover all possible attack patterns.  Reduing the occurrences of
attack pre-requisites listed can reduce the risk that a spectre attack
leaks useful data.

1. Local User process attacking kernel
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Code in system calls often enforces access controls with conditional
branches based on user data.  These branches are potential targets for
Spectre v2 exploits.  Interrupt handlers, on the other hand, rarely
handle user data or enforce access controls, which makes them unlikely
exploit targets.

For typical variant 2 attack, the attacker may poison the CPU branch
buffers first, and then enter the kernel and trick it into jumping to a
disclosure gadget through an indirect branch. If the attacker wants to control the
memory addresses leaked, it would also need to pass a parameter
to the gadget, either through a register or through a known address in
memory. Finally when it executes again it can measure the side effect.

Necessary Prequisites:
1. Malicious local process passing parameters to kernel
2. Kernel has secrets.

2. User process attacking another user process
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

In this scenario an malicious user process wants to attack another
user process through a context switch.

For variant 1 this generally requires passing some parameter between
the processes, which needs a data passing relationship, such a remote
procedure calls (RPC).

For variant 2 the poisoning can happen through a context switch, or
on CPUs with simultaneous multi-threading (SMT) potentially on the
thread sibling executing in parallel on the same core.  In either case,
controlling the memory leaked by the disclosure gadget also requires a data
passing relationship to the victim process, otherwise while it may
observe values through side effects, it won't know which memory
addresses they relate to.

Necessary Prerequisites:
1. Malicious code running as local process
2. Victim processes containing secrets running on same core.

3. User sandbox attacking runtime in process
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A process, such as a web browser, might be running interpreted or JITed
untrusted code, such as javascript code downloaded from a website.
It uses restrictions in the JIT code generator and checks in a run time
to prevent the untrusted code from attacking the hosting process.

The untrusted code might either use variant 1 or 2 to trick
a disclosure gadget in the run time to read memory inside the process.

Necessary Prerequisites:
1. Sandbox in process running untrusted code.
2. Runtime in same process containing secrets.

4. Kernel sandbox attacking kernel
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The kernel has support for running user-supplied programs within the
kernel.  Specific rules (such as bounds checking) are enforced on these
programs by the kernel to ensure that they do not violate access controls.

eBPF is a kernel sub-system that uses user-supplied program
to execute JITed untrusted byte code inside the kernel. eBPF is used
for manipulating and examining network packets, examining system call
parameters for sand boxes and other uses.

A malicious local process could upload and trigger an malicious
eBPF script to the kernel, with the script attacking the kernel
using variant 1 or 2 and reading memory.

Necessary Prerequisites:
1. Malicious local process
2. eBPF JIT enabled for unprivileged users, attacking kernel with secrets
on the same machine.

5. Virtualization guest attacking host
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

An untrusted guest might attack the host through a hyper call
or other virtualization exit.

Necessary Prerequisites:
1. Untrusted guest attacking host
2. Host has secrets on local machine.

For variant 1 VM exits use appropriate mitigations
("bounds clipping") to prevent speculation leaking data
in kernel code. For variant 2 the kernel flushes the branch buffer.

6. Virtualization guest attacking other guest
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

An untrusted guest attacking another guest containing
secrets. Mitigations are similar to when a guest attack
the host.

Runtime vulnerability information
---------------------------------

The kernel reports the vulnerability and mitigation status in
/sys/devices/system/cpu/vulnerabilities/*

The spectre_v1 file describes the always enabled variant 1
mitigation:

/sys/devices/system/cpu/vulnerabilities/spectre_v1

The value in this file:

  =======================================  =================================
  'Mitigation: __user pointer sanitation'  Protection in kernel on a case by
                                           case base with explicit pointer
                                           sanitation.
  =======================================  =================================

The spectre_v2 kernel file reports if the kernel has been compiled with a
retpoline aware compiler, if the CPU has hardware mitigation, and if the
CPU has microcode support for additional process specific mitigations.

It also reports CPU features enabled by microcode to mitigate attack
between user processes:

1. Indirect Branch Prediction Barrier (IBPB) to add additional
   isolation between processes of different users
2. Single Thread Indirect Branch Prediction (STIBP) to additional
   isolation between CPU threads running on the same core.

These CPU features may impact performance when used and can
be enabled per process on a case-by-case base.

/sys/devices/system/cpu/vulnerabilities/spectre_v2

The values in this file:

  - Kernel status:

  ====================================  =================================
  'Not affected'                        The processor is not vulnerable
  'Vulnerable'                          Vulnerable, no mitigation
  'Mitigation: Full generic retpoline'  Software-focused mitigation
  'Mitigation: Full AMD retpoline'      AMD-specific software mitigation
  'Mitigation: Enhanced IBRS'           Hardware-focused mitigation
  ====================================  =================================

  - Firmware status:

  ========== =============================================================
  'IBRS_FW'  Protection against user program attacks when calling firmware
  ========== =============================================================

  - Indirect branch prediction barrier (IBPB) status for protection between
    processes of different users. This feature can be controlled through
    prctl per process, or through kernel command line options. For more details
    see below.

  ===================   ========================================================
  'IBPB: disabled'      IBPB unused
  'IBPB: always-on'     Use IBPB on all tasks
  'IBPB: conditional'   Use IBPB on SECCOMP or indirect branch restricted tasks
  ===================   ========================================================

  - Single threaded indirect branch prediction (STIBP) status for protection
    between different hyper threads. This feature can be controlled through
    prctl per process, or through kernel command line options. For more details
    see below.

  ====================  ========================================================
  'STIBP: disabled'     STIBP unused
  'STIBP: forced'       Use STIBP on all tasks
  'STIBP: conditional'  Use STIBP on SECCOMP or indirect branch restricted tasks
  ====================  ========================================================

  - Return stack buffer (RSB) protection status:

  =============   ===========================================
  'RSB filling'   Protection of RSB on context switch enabled
  =============   ===========================================

Full mitigations might require an microcode update from the CPU
vendor. When the necessary microcode is not available the kernel
will report vulnerability.

Kernel mitigation
-----------------

The kernel has default on mitigations for Variant 1 and Variant 2
against attacks from user programs or guests. For variant 1 it
annotates vulnerable kernel code (as determined by the sparse code
scanning tool and code audits) to use "bounds clipping" to avoid any
usable disclosure gadgets.

For variant 2 the kernel employs "retpoline" with compiler help to secure
the indirect branches inside the kernel, when CONFIG_RETPOLINE is enabled
and the compiler supports retpoline. On Intel Skylake-era systems the
mitigation covers most, but not all, cases, see [1] for more details.

On CPUs with hardware mitigations for variant 2, retpoline is
automatically disabled at runtime.

Using kernel address space randomization (CONFIG_RANDOMIZE_SLAB=y
and CONFIG_SLAB_FREELIST_RANDOM=y in the kernel configuration)
makes attacks on the kernel generally more difficult.

Host mitigation
---------------

The Linux kernel uses retpoline to eliminate attacks on indirect
branches. It also flushes the Return Branch Stack on every VM exit to
prevent guests from attacking the host kernel when retpoline is
enabled.

Variant 1 attacks are mitigated unconditionally.

The kernel also allows guests to use any microcode based mitigations
they chose to use (such as IBPB or STIBP), assuming the
host has an updated microcode and reports the feature in
/sys/devices/system/cpu/vulnerabilities/spectre_v2.

Mitigation control at kernel build time
---------------------------------------

When the CONFIG_RETPOLINE option is enabled the kernel uses special
code sequences to avoid attacks on indirect branches through
Variant 2 attacks.

The compiler also needs to support retpoline and support the
-mindirect-branch=thunk-extern -mindirect-branch-register options
for gcc, or -mretpoline-external-thunk option for clang.

When the compiler doesn't support these options the kernel
will report that it is vulnerable.

Variant 1 mitigations and other side channel related user APIs are
enabled unconditionally.

Hardware mitigation
-------------------

Some CPUs have hardware mitigations (e.g. enhanced IBRS) for Spectre
variant 2.  The 4.19 kernel has support for detecting this capability
and automatically disable any unnecessary workarounds at runtime.

User program mitigation
-----------------------

For variant 1 user programs can use LFENCE or bounds clipping. For more
details see [3].

For variant 2 user programs can be compiled with retpoline or
restricting its indirect branch speculation via prctl.  (See
Documenation/speculation.txt for detailed API.)

User programs should use address space randomization
(/proc/sys/kernel/randomize_va_space = 1 or 2) to make any attacks
more difficult.

Mitigation control on the kernel command line
---------------------------------------------

Spectre v2 mitigations can be disabled and force enabled at the kernel
command line.

	nospectre_v2	[X86] Disable all mitigations for the Spectre variant 2
			(indirect branch prediction) vulnerability. System may
			allow data leaks with this option, which is equivalent
			to spectre_v2=off.


        spectre_v2=     [X86] Control mitigation of Spectre variant 2
			(indirect branch speculation) vulnerability.
			The default operation protects the kernel from
			user space attacks.

			on   - unconditionally enable, implies
			       spectre_v2_user=on
			off  - unconditionally disable, implies
			       spectre_v2_user=off
			auto - kernel detects whether your CPU model is
			       vulnerable

			Selecting 'on' will, and 'auto' may, choose a
			mitigation method at run time according to the
			CPU, the available microcode, the setting of the
			CONFIG_RETPOLINE configuration option, and the
			compiler with which the kernel was built.

			Selecting 'on' will also enable the mitigation
			against user space to user space task attacks.

			Selecting 'off' will disable both the kernel and
			the user space protections.

			Specific mitigations can also be selected manually:

			retpoline         - replace indirect branches
			retpoline,generic - google's original retpoline
			retpoline,amd     - AMD-specific minimal thunk

			Not specifying this option is equivalent to
			spectre_v2=auto.

For user space mitigation:

        spectre_v2_user=
			[X86] Control mitigation of Spectre variant 2
			(indirect branch speculation) vulnerability between
			user space tasks

			on      - Unconditionally enable mitigations. Is
				  enforced by spectre_v2=on

			off     - Unconditionally disable mitigations. Is
				  enforced by spectre_v2=off

			prctl   - Indirect branch speculation is enabled,
				  but mitigation can be enabled via prctl
				  per thread.  The mitigation control state
				  is inherited on fork.

			prctl,ibpb
				- Like "prctl" above, but only STIBP is
				  controlled per thread. IBPB is issued
				  always when switching between different user
				  space processes.

			seccomp
				- Same as "prctl" above, but all seccomp
				  threads will enable the mitigation unless
				  they explicitly opt out.

			seccomp,ibpb
				- Like "seccomp" above, but only STIBP is
				  controlled per thread. IBPB is issued
				  always when switching between different
				  user space processes.

			auto    - Kernel selects the mitigation depending on
				  the available CPU features and vulnerability.

			Default mitigation:
			If CONFIG_SECCOMP=y then "seccomp", otherwise "prctl"

			Not specifying this option is equivalent to
			spectre_v2_user=auto.

			In general the kernel by default selects
			reasonable mitigations for the current CPU. To
			disable Spectre v2 mitigations boot with
			spectre_v2=off. Spectre v1 mitigations cannot
			be disabled.

APIs for mitigation control of user process
-------------------------------------------

When enabling the "prctl" option for spectre_v2_user boot parameter,
prctl can be used to restrict indirect branch speculation on a process.
See Documenation/speculation.txt for detailed API.

Processes containing secrets, such as cryptographic keys, may invoke
this prctl for extra protection against Spectre v2.

Before running untrusted processes, restricting their indirect branch
speculation will prevent such processes from launching Spectre v2 attacks.

Restricting indirect branch speuclation on a process should be only used
as needed, as restricting speculation reduces both performance of the
process, and also process running on the sibling CPU thread.

Under the "seccomp" option, the processes sandboxed with SECCOMP will
have indirect branch speculation restricted automatically.

References
----------

Intel white papers and documents on Spectre:

https://newsroom.intel.com/wp-content/uploads/sites/11/2018/01/Intel-Analysis-of-Speculative-Execution-Side-Channels.pdf

[1]
https://software.intel.com/security-software-guidance/api-app/sites/default/files/Retpoline-A-Branch-Target-Injection-Mitigation.pdf

https://www.intel.com/content/www/us/en/architecture-and-technology/facts-about-side-channel-analysis-and-intel-products.html

[3] https://software.intel.com/security-software-guidance/

https://software.intel.com/security-software-guidance/insights/deep-dive-single-thread-indirect-branch-predictors

AMD white papers:

https://developer.amd.com/wp-content/resources/90343-B_SoftwareTechniquesforManagingSpeculation_WP_7-18Update_FNL.pdf

https://www.amd.com/en/corporate/security-updates

ARM white papers:

https://developer.arm.com/support/arm-security-updates/speculative-processor-vulnerability/download-the-whitepaper

https://developer.arm.com/support/arm-security-updates/speculative-processor-vulnerability/latest-updates/cache-speculation-issues-update

MIPS:

https://www.mips.com/blog/mips-response-on-speculative-execution-and-side-channel-vulnerabilities/

Academic papers:

https://spectreattack.com/spectre.pdf [original spectre paper]

[2] https://arxiv.org/abs/1807.10535 [NetSpectre]

https://arxiv.org/abs/1811.05441 [generalization of Spectre]

https://arxiv.org/abs/1807.07940 [Spectre RSB, a variant of Spectre v2]
