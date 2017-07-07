=========================================================
KVMi - the kernel virtual machine introspection subsystem
=========================================================

The KVM introspection subsystem provides a facility for applications running
on the host or in a separate VM, to control the execution of other VM-s
(pause, resume, shutdown), query the state of the vCPU-s (GPR-s, MSR-s etc.),
alter the page access bits in the shadow page tables (only for the hardware
backed ones, eg. Intel's EPT) and receive notifications when events of
interest have taken place (shadow page table level faults, key MSR writes,
hypercalls etc.). Some notifications can be responded to with an action
(like preveting an MSR from being written), others are mere informative
(like breakpoint events which are used for execution tracing), though the
option to alter the GPR-s is common to each of them (usually the program
counter is advanced past the instruction that triggered the guest exit).
All events are optional. An application using this subsystem will explicitly
register for them.

The use case that gave way for the creation of this subsystem is to monitor
the guest OS and as such the ABI/API is higly influenced by how the guest
software (kernel, applications) see the world. For example, some events
provide information specific for the host CPU architecture
(eg. MSR_IA32_SYSENTER_EIP) merely because its leveraged by guest software
to implement a critical feature (fast system calls).

At the moment, the target audience for VMI are security software authors
that wish to perform forensics on newly discovered threats (exploits) or
to implement another layer of security like preventing a large set of
kernel rootkits simply by "locking" the kernel image in the shadow page
tables (ie. enforce .text r-x, .rodata rw- etc.). It's the latter case that
made VMI a separate subsystem, even though many of these features are
available in the device manager (eg. qemu). The ability to build a security
application that does not interfere (in terms of performance) with the
guest software asks for a specialized interface that is designed for minimum
overhead.

API/ABI
=======

This chapter describes the VMI interface used to monitor and control local
guests from an user application.

Overview
--------

The interface is socket based, one connection for every VM. One end is in the
host kernel while the other is held by the user application (introspection
tool).

The initial connection is established by an application running on the host
(eg. qemu) that connects to the introspection tool and after a handshake the
socket is passed to the host kernel making all further communication take
place between it and the introspection tool. The initiating party (qemu) can
close its end so that any potential exploits cannot take a hold of it.

The socket protocol allows for commands and events to be multiplexed over
the same connection. A such, it is possible for the introspection tool to
receive an event while waiting for the result of a command. Also, it can
send a command while the host kernel is waiting for a reply to an event.

The kernel side of the socket communication is blocking and will wait for
an answer from its peer indefinitely or until the guest is powered off
(killed) at which point it will wake up and properly cleanup. If the peer
goes away KVM will exit to user space and the device manager will try and
reconnect. If it fails, the device manager will inform KVM to cleanup and
continue normal guest execution as if the introspection subsystem has never
been used on that guest.

All events have a common header::

	struct kvmi_socket_hdr {
		__u16 msg_id;
		__u16 size;
		__u32 seq;
	};

and all need a reply with the same kind of header, having the same
sequence number (seq) and the same message id (msg_id).

Because events from different vCPU threads can send messages at the same
time and the replies can come in any order, the receiver loop uses the
sequence number (seq) to identify which reply belongs to which vCPU, in
order to dispatch the message to the right thread waiting for it.

After 'kvmi_socket_hdr', 'msg_id' specific data of 'kvmi_socket_hdr.size'
bytes will follow.

The message header and its data must be sent with one write() call
to the socket (as a whole). This simplifies the receiver loop and avoids
the recontruction of messages on the other side.

The wire protocol uses the host native byte-order. The introspection tool
must check this during the handshake and do the necessary conversion.

Replies to commands have an error code (__s32) at offset 0 in the message
data. Specific message data will follow this. If the error code is not
zero, all the other data members will have undefined content (not random
heap or stack data, but valid results at the time of the failure), unless
otherwise specified.

In case of an unsupported command, the message data will contain only
the error code (-ENOSYS).

The error code is related to the processing of the corresponding
message. For all the other errors (socket errrors, incomplete messages,
wrong sequence numbers etc.) the socket must be closed and the connection
can be retried.

While all commands will have a reply as soon as possible, the replies
to events will probably be delayed until a set of (new) commands will
complete::

   Host kernel               Tool
   -----------               --------
   event 1 ->
                             <- command 1
   command 1 reply ->
                             <- command 2
   command 2 reply ->
                             <- event 1 reply

If both ends send a message "in the same time"::

   KVMi                      Userland
   ----                     --------
   event X ->               <- command X

the host kernel should reply to 'command X', regardless of the receive time
(before or after the 'event X' was sent).

As it can be seen below, the wire protocol specifies occasional padding. This
is to permit working with the data by directly using C structures. The
members should have the 'natural' alignment of the host.

To describe the commands/events, we reuse some conventions from api.txt:

  - Architectures: which instruction set architectures providing this command/event

  - Versions: which versions provide this command/event

  - Parameters: incoming message data

  - Returns: outgoing/reply message data

Handshake
---------

Allthough this falls out of the scope of the introspection subsystem, below
is a proposal of a handshake that can be used by implementors.

The device manager will connect to the introspection tool and wait for a
cryptographic hash of a cookie that should be known by both peers. If the
hash is correct (the destination has been "authenticated"), the device
manager will send another cryptographic hash and random salt. The peer
recomputes the hash of the cookie bytes including the salt and if they match,
the device manager has been "authenticated" too. This is a rather crude
system that makes it difficult for device manager exploits to trick the
introspection tool into believing its working OK.

The cookie would normally be generated by a management tool (eg. libvirt)
and make it available to the device manager and to a properly authenticated
client. It is the job of a third party to retrieve the cookie from the
management application and pass it over a secure channel to the introspection
tool.

Once the basic "authentication" has taken place, the introspection tool
can receive information on the guest (its UUID) and other flags (endianness
or features supported by the host kernel).

Introspection capabilities
--------------------------

TODO

Commands
--------

The following C structures are meant to be used directly when communicating
over the wire. The peer that detects any size mismatch should simply close
the connection and report the error.

1. KVMI_GET_VERSION
-------------------

:Architectures: all
:Versions: >= 1
:Parameters: {}
:Returns: ↴

::

	struct kvmi_get_version_reply {
		__s32 err;
		__u32 version;
	};

Returns the introspection API version (the KVMI_VERSION constant) and the
error code (zero). In case of an unlikely error, the version will have an
undefined value.

2. KVMI_GET_GUEST_INFO
----------------------

:Architectures: all
:Versions: >= 1
:Parameters: {}
:Returns: ↴

::

	struct kvmi_get_guest_info_reply {
		__s32 err;
		__u16 vcpu_count;
		__u16 padding;
		__u64 tsc_speed;
	};

Returns the number of online vcpus, and the TSC frequency in HZ, if supported
by the architecture (otherwise is 0).

3. KVMI_PAUSE_GUEST
-------------------

:Architectures: all
:Versions: >= 1
:Parameters: {}
:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

This command will pause all vcpus threads, by getting them out of guest mode
and put them in the "waiting introspection commands" state.

4. KVMI_UNPAUSE_GUEST
---------------------

:Architectures: all
:Versions: >= 1
:Parameters: {}
:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Resume the vcpu threads, or at least get them out of "waiting introspection
commands" state.

5. KVMI_SHUTDOWN_GUEST
----------------------

:Architectures: all
:Versions: >= 1
:Parameters: {}
:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Ungracefully shutdown the guest.

6. KVMI_GET_REGISTERS
---------------------

:Architectures: x86 (could be all, but with different input/output)
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_get_registers_x86 {
		__u16 vcpu;
		__u16 nmsrs;
		__u32 msrs_idx[0];
	};

:Returns: ↴

::

	struct kvmi_get_registers_x86_reply {
		__s32 err;
		__u32 mode;
		struct kvm_regs  regs;
		struct kvm_sregs sregs;
		struct kvm_msrs  msrs;
	};

For the given vcpu_id and the nmsrs sized array of MSRs registers, returns
the vCPU mode (in bytes: 2, 4 or 8), the general purpose registers,
the special registers and the requested set of MSR-s.

7. KVMI_SET_REGISTERS
---------------------

:Architectures: x86 (could be all, but with different input)
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_set_registers_x86 {
		__u16 vcpu;
		__u16 padding[3];
		struct kvm_regs regs;
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Sets the general purpose registers for the given vcpu_id.

8. KVMI_GET_MTRR_TYPE
---------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_mtrr_type {
		__u64 gpa;
	};

:Returns: ↴

::

	struct kvmi_mtrr_type_reply {
		__s32 err;
		__u32 type;
	};

Returns the guest memory type for a specific physical address.

9. KVMI_GET_MTRRS
-----------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_mtrrs {
		__u16 vcpu;
		__u16 padding[3];
	};

:Returns: ↴

::

	struct kvmi_mtrrs_reply {
		__s32 err;
		__u32 padding;
		__u64 pat;
		__u64 cap;
		__u64 type;
	};

Returns MSR_IA32_CR_PAT, MSR_MTRRcap and MSR_MTRRdefType for the specified
vCPU.

10. KVMI_GET_XSAVE_INFO
-----------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_xsave_info {
		__u16 vcpu;
		__u16 padding[3];
	};

:Returns: ↴

::

	struct kvmi_xsave_info_reply {
		__s32 err;
		__u32 size;
	};

Returns the xstate size for the specified vCPU.

11. KVMI_GET_PAGE_ACCESS
------------------------

:Architectures: all
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_get_page_access {
		__u16 vcpu;
		__u16 padding[3];
		__u64 gpa;
	};

:Returns: ↴

::

	struct kvmi_get_page_access_reply {
		__s32 err;
		__u32 access;
	};

Returns the spte flags (rwx - present, write & user) for the specified
vCPU and guest physical address.

12. KVMI_SET_PAGE_ACCESS
------------------------

:Architectures: all
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_set_page_access {
		__u16 vcpu;
		__u16 padding;
		__u32 access;
		__u64 gpa;
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Sets the spte flags (rwx - present, write & user) - access - for the specified
vCPU and guest physical address.

13. KVMI_INJECT_PAGE_FAULT
--------------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_page_fault {
		__u16 vcpu;
		__u16 padding;
		__u32 error;
		__u64 gva;
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Injects a vCPU page fault with the specified guest virtual address and
error code.

14. KVMI_INJECT_BREAKPOINT
--------------------------

:Architectures: all
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_inject_breakpoint {
		__u16 vcpu;
		__u16 padding[3];
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Injects a breakpoint for the specified vCPU. This command is usually sent in
response to an event and as such the proper GPR-s will be set with the reply.

15. KVMI_MAP_PHYSICAL_PAGE_TO_GUEST
-----------------------------------

:Architectures: all
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_map_physical_page_to_guest {
		__u64 gpa_src;
		__u64 gfn_dest;
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Maps a page from an introspected guest memory (gpa_src) to the guest running
the introspection tool. 'gfn_dest' points to an anonymous, locked mapping one
page in size.

This command is used to "read" the introspected guest memory and potentially
place patches (eg. INT3-s).

16. KVMI_UNMAP_PHYSICAL_PAGE_FROM_GUEST
---------------------------------------

:Architectures: all
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_unmap_physical_page_from_guest {
		__u64 gfn_dest;
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Unmaps a previously mapped page.

17. KVMI_CONTROL_EVENTS
-----------------------

:Architectures: all
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_control_events {
		__u16 vcpu;
		__u16 padding;
		__u32 events;
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Enables/disables vCPU introspection events, by setting/clearing one or more
of the following bits (see 'Events' below) :

	KVMI_EVENT_CR
	KVMI_EVENT_MSR
	KVMI_EVENT_XSETBV
	KVMI_EVENT_BREAKPOINT
	KVMI_EVENT_USER_CALL
	KVMI_EVENT_PAGE_FAULT
	KVMI_EVENT_TRAP

Trying to enable unsupported events (~KVMI_KNOWN_EVENTS) by the current
architecture would fail and -EINVAL will be returned.

18. KVMI_CR_CONTROL
-------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_cr_control {
		__u8 enable;
		__u8 padding[3];
		__u32 cr;
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Enables/disables introspection for a specific CR register and must
be used in addition to KVMI_CONTROL_EVENTS with the KVMI_EVENT_CR bit
flag set.

Eg. kvmi_cr_control { .enable=1, .cr=3 } will enable introspection
for CR3.

Currently, trying to set any register but CR0, CR3 and CR4 will return
-EINVAL.

19. KVMI_MSR_CONTROL
--------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_msr_control {
		__u8 enable;
		__u8 padding[3];
		__u32 msr;
	};

:Returns: ↴

::

	struct kvmi_error_code {
		__s32 err;
		__u32 padding;
	};

Enables/disables introspection for a specific MSR, and must be used
in addition to KVMI_CONTROL_EVENTS with the KVMI_EVENT_MSR bit flag set.

Currently, only MSRs within the following 3 ranges are supported. Trying
to control any other register will return -EINVAL. ::

	0          ... 0x00001fff
	0x40000000 ... 0x40001fff
	0xc0000000 ... 0xc0001fff

Events
------

All vcpu events are sent using the KVMI_EVENT_VCPU message id. No event will
be sent unless enabled with a KVMI_CONTROL_EVENTS command.

For x86, the message data starts with a common structure::

	struct kvmi_event_x86 {
		__u16 vcpu;
		__u8 mode;
		__u8 padding1;
		__u32 event;
		struct kvm_regs regs;
		struct kvm_sregs sregs;
		struct {
			__u64 sysenter_cs;
			__u64 sysenter_esp;
			__u64 sysenter_eip;
			__u64 efer;
			__u64 star;
			__u64 lstar;
		} msrs;
	};

In order to help the introspection tool with the event analysis while
avoiding unnecessary introspection commands, the message data holds some
registers (kvm_regs, kvm_sregs and a couple of MSR-s) beside
the vCPU id, its mode (in bytes) and the event (one of the flags set
with the KVMI_CONTROL_EVENTS command).

The replies to events also start with a common structure, having the
KVMI_EVENT_VCPU_REPLY message id::

	struct kvmi_event_x86_reply {
		struct kvm_regs regs;
		__u32 actions;
		__u32 padding;
	};

The 'actions' member holds one or more flags. For example, if
KVMI_EVENT_ACTION_SET_REGS is set, the general purpose registers will
be overwritten with the new values (regs) from introspector.

Specific data can follow these common structures.

1. KVMI_EVENT_CR
----------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_event_x86;
	struct kvmi_event_cr {
		__u16 cr;
		__u16 padding[3];
		__u64 old_value;
		__u64 new_value;
	};

:Returns: ↴

::

	struct kvmi_event_x86_reply;
	struct kvmi_event_cr_reply {
		__u64 new_val;
	};

This event is sent when a CR register was modified and the introspection
has already been enabled for this kind of event (KVMI_CONTROL_EVENTS)
and for this specific register (KVMI_CR_CONTROL).

kvmi_event_x86, the CR number, the old value and the new value are
sent to the introspector, which can respond with one or more action flags:

   KVMI_EVENT_ACTION_SET_REGS - override the general purpose registers
   using the values from introspector (regs)

   KVMI_EVENT_ACTION_ALLOW - allow the register modification with the
   value from introspector (new_val), otherwise deny the modification
   but allow the guest to proceed as if the register has been loaded
   with the desired value.

2. KVMI_EVENT_MSR
-----------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_event_x86;
	struct kvmi_event_msr {
		__u32 msr;
		__u32 padding;
		__u64 old_value;
		__u64 new_value;
	};

:Returns: ↴

::

	struct kvmi_event_x86_reply;
	struct kvmi_event_msr_reply {
		__u64 new_val;
	};

This event is sent when a MSR was modified and the introspection has already
been enabled for this kind of event (KVMI_CONTROL_EVENTS) and for this
specific register (KVMI_MSR_CONTROL).

kvmi_event_x86, the MSR number, the old value and the new value are
sent to the introspector, which can respond with one or more action flags:

   KVMI_EVENT_ACTION_SET_REGS - override the general purpose registers
   using the values from introspector (regs)

   KVMI_EVENT_ACTION_ALLOW - allow the register modification with the
   value from introspector (new_val), otherwise deny the modification
   but allow the guest to proceed as if the register has been loaded
   with the desired value.

3. KVMI_EVENT_XSETBV
--------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_event_x86;
	struct kvmi_event_xsetbv {
		__u64 xcr0;
	};

:Returns: ↴

::

	struct kvmi_event_x86_reply;

This event is sent when the extended control register XCR0 was modified
and the introspection has already been enabled for this kind of event
(KVMI_CONTROL_EVENTS).

kvmi_event_x86 and the new value are sent to the introspector, which
can respond with the KVMI_EVENT_ACTION_SET_REGS bit set in 'actions',
instructing KVMi to override the general purpose registers using the
values from introspector (regs).

4. KVMI_EVENT_BREAKPOINT
------------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_event_x86;
	struct kvmi_event_breakpoint {
		__u64 gpa;
	};

:Returns: ↴

::

	struct kvmi_event_x86_reply;

This event is sent when a breakpoint was reached and the introspection has
already been enabled for this kind of event (KVMI_CONTROL_EVENTS).

kvmi_event_x86 and the guest physical address are sent to the introspector,
which can respond with one or more action flags:

   KVMI_EVENT_ACTION_SET_REGS - override the general purpose registers
   using the values from introspector (regs)

   KVMI_EVENT_ACTION_ALLOW - is implied if not specified

5. KVMI_EVENT_USER_CALL
-----------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_event_x86;

:Returns: ↴

::

	struct kvmi_event_x86_reply;

This event is sent on a user hypercall and the introspection has already
already been enabled for this kind of event (KVMI_CONTROL_EVENTS).

kvmi_event_x86 is sent to the introspector, which can respond with the
KVMI_EVENT_ACTION_SET_REGS bit set in 'actions', instructing the host
kernel to override the general purpose registers using the values from
introspector (regs).

6. KVMI_EVENT_PAGE_FAULT
------------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_event_x86;
	struct kvmi_event_page_fault {
		__u64 gva;
		__u64 gpa;
		__u32 mode;
		__u32 padding;
	};

:Returns: ↴

::

	struct kvmi_event_x86_reply;
	struct kvmi_event_page_fault_reply {
		__u32 ctx_size;
		__u8 ctx_data[256];
	};

This event is sent if a hypervisor page fault was encountered, the
introspection has already enabled the reports for this kind of event
(KVMI_CONTROL_EVENTS), and it was generated for a page for which the
introspector has shown interest (ie. has previously touched it by
adjusting the permissions).

kvmi_event_x86, guest virtual address, guest physical address and
the exit qualification (mode) are sent to the introspector, which
can respond with one or more action flags:

   KVMI_EVENT_ACTION_SET_REGS - override the general purpose registers
   using the values from introspector (regs)

   (KVMI_EVENT_ALLOW | KVMI_EVENT_NOEMU) - let the guest re-trigger
   the page fault

   (KVMI_EVENT_ALLOW | KVMI_EVENT_SET_CTX) - allow the page fault
   via emulation but with custom input (ctx_data, ctx_size). This is
   used to trick the guest software into believing it has read
   certain data. In practice it is used to hide the contents of certain
   memory areas

   KVMI_EVENT_ALLOW - allow the page fault via emulation

If KVMI_EVENT_ALLOW is not set, it will fall back to the page fault handler
which usually implies overwriting any spte page access changes made before.
An introspection tool will always set this flag and prevent unwanted changes
to memory by skipping the instruction. It is up to the tool to adjust the
program counter in order to achieve this result.

7. KVMI_EVENT_TRAP
------------------

:Architectures: x86
:Versions: >= 1
:Parameters: ↴

::

	struct kvmi_event_x86;
	struct kvmi_event_trap {
		__u32 vector;
		__u32 type;
		__u32 err;
		__u32 padding;
		__u64 cr2;
	};

:Returns: ↴

::

	struct kvmi_event_x86_reply;

This event is sent if a trap will be delivered to the guest (page fault,
breakpoint, etc.) and the introspection has already enabled the reports
for this kind of event (KVMI_CONTROL_EVENTS).

This is used to inform the introspector of all pending traps giving it
a chance to determine if it should try again later in case a previous
KVMI_INJECT_PAGE_FAULT/KVMI_INJECT_BREAKPOINT command has been overwritten
by an interrupt picked up during guest reentry.

kvmi_event_x86, exception/interrupt number (vector), exception/interrupt
type, exception code (err) and CR2 are sent to the introspector, which can
respond with the KVMI_EVENT_ACTION_SET_REGS bit set in 'actions', instructing
the host kernel to override the general purpose registers using the values
from introspector (regs).
