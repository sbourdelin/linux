======================
Userspace Verbs Access
======================
The ib_uverbs module, built by enabling CONFIG_INFINIBAND_USER_VERBS,
enables direct userspace access to IB hardware via "verbs," as
described in chapter 11 of the InfiniBand Architecture Specification.

To use the verbs, the libibverbs library, available from
https://github.com/linux-rdma/rdma-core, is required. libibverbs contains a
device-independent API for using the ib_uverbs interface.
libibverbs also requires appropriate device-dependent kernel and
userspace driver for your InfiniBand hardware.  For example, to use
a Mellanox HCA, you will need the ib_mthca kernel module and the
libmthca userspace driver be installed.

User-kernel communication
=========================
Userspace communicates with the kernel for slow path, resource
management operations via the /dev/infiniband/uverbsN character
devices.  Fast path operations are typically performed by writing
directly to hardware registers mmap()ed into userspace, with no
system call or context switch into the kernel.

There are currently two methods for executing commands in the kernel: write() and ioctl().
Older commands are sent to the kernel via write()s on the device files
mentioned earlier. New commands must use the ioctl() method. For completeness,
both mechanisms are described here.

The interface between userspace and kernel is kept in sync by checking the
version number. In the kernel, it is defined by IB_USER_VERBS_ABI_VERSION
(in include/uapi/rdma/ib_user_verbs.h).

Write system call
-----------------
The ABI is defined in drivers/infiniband/include/ib_user_verbs.h.
The structs for commands that require a response from the kernel
contain a 64-bit field used to pass a pointer to an output buffer.
Status is returned to userspace as the return value of the write()
system call.
The entry point to the kernel is the ib_uverbs_write() function, which is
invoked as a response to the 'write' system call. The requested function is
looked up from an array called uverbs_cmd_table which contains function pointers
to the various command handlers.

Write Command Handlers
~~~~~~~~~~~~~~~~~~~~~~
These command handler functions are declared
with the IB_VERBS_DECLARE_CMD macro in drivers/infiniband/core/uverbs.h. There
are also extended commands, which are kept in a similar manner in the
uverbs_ex_cmd_table. The extended commands use 64-bit values in the command
header, as opposed to the 32-bit values used in the regular command table.


Ioctl system call
-----------------
The entry point for the 'ioctl' system call is the ib_uverbs_ioctl() function.
Unlike write(), ioctl() accepts a 'cmd' parameter, which must have the value
defined by RDMA_VERBS_IOCTL. More documentation regarding the ioctl numbering
scheme can be found in: Documentation/ioctl/ioctl-number.txt. The
command-specific information is passed as a pointer in the 'arg' parameter,
which is cast as a 'struct ib_uverbs_ioctl_hdr*'.

The way command handler functions (methods) are looked up is more complicated
than the array index used for write(). Here, the ib_uverbs_cmd_verbs() function
uses a radix tree to search for the correct command handler. If the lookup
succeeds, the method is invoked by ib_uverbs_run_method().

Ioctl Command Handlers
~~~~~~~~~~~~~~~~~~~~~~
Command handlers (also known as 'methods') for ioctl are declared with the
UVERBS_HANDLER macro. The handler is registered for use by the
DECLARE_UVERBS_NAMED_METHOD macro, which binds the name of the handler with its
attributes. By convention, the methods are implemented in files named with the
prefix 'uverbs_std_types_'.

Each method can accept a set of parameters called attributes. There are 6
types of attributes: idr, fd, pointer, enum, const and flags. The idr attribute
declares an indirect (translated) handle for the method, and
specifies the object that the method will act upon. The first attribute should
be a handle to the uobj (ib_uobject) which contains private data. There may be
0 or more
additional attributes, including other handles. The 'pointer' attribute must be
specified as 'in' or 'out', depending on if it is an input from userspace, or
meant to return a value to userspace.

The method also needs to be bound to an object, which is done with the
DECLARE_UVERBS_NAMED_OBJECT macro. This macro takes a variable
number of methods and stores them in an array attached to the object.

Objects are declared using DECLARE_UVERBS_NAMED_OBJECT macro. Most of the
objects (including pd, mw, cq, etc.) are defined in uverbs_std_types.c,
and the remaining objects are declared in files that are prefixed with the
name 'uverbs_std_types_'.

Objects trees are declared using the DECLARE_UVERBS_OBJECT_TREE macro. This
combines all of the objects.

Resource management
===================
Since creation and destruction of all IB resources is done by
commands passed through a file descriptor, the kernel can keep track
of which resources are attached to a given userspace context.  The
ib_uverbs module maintains idr tables that are used to translate
between kernel pointers and opaque userspace handles, so that kernel
pointers are never exposed to userspace and userspace cannot trick
the kernel into following a bogus pointer.

This also allows the kernel to clean up when a process exits and
prevent one process from touching another process's resources.

Memory pinning
==============
Direct userspace I/O requires that memory regions that are potential
I/O targets be kept resident at the same physical address.  The
ib_uverbs module manages pinning and unpinning memory regions via
get_user_pages() and put_page() calls.  It also accounts for the
amount of memory pinned in the process's locked_vm, and checks that
unprivileged processes do not exceed their RLIMIT_MEMLOCK limit.

Pages that are pinned multiple times are counted each time they are
pinned, so the value of locked_vm may be an overestimate of the
number of pages pinned by a process.

/dev files
==========
To create the appropriate character device files automatically with
udev, a rule like::

   KERNEL=="uverbs*", NAME="infiniband/%k"

can be used.  This will create device nodes named::

    /dev/infiniband/uverbs0

and so on.  Since the InfiniBand userspace verbs should be safe for
use by non-privileged processes, it may be useful to add an
appropriate MODE or GROUP to the udev rule.
