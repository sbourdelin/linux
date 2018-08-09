.. SPDX-License-Identifier: GPL-2.0+

.. _socket-filter:

======================
Linux Socket Filtering
======================

Linux Socket Filtering (LSF) is derived from the Berkeley Packet
Filter.

Introduction
============

Though there are some distinct differences between the BSD and Linux
kernel socket filtering, but when we speak of BPF or LSF in Linux
context, we mean the very same mechanism of filtering in the Linux
kernel.

BPF allows a user-space program to attach a filter onto any socket and
allow or disallow certain types of data to come through the socket.  LSF
follows exactly the same filter code structure as BSD's BPF, so
referring to the BSD bpf.4 manpage is very helpful in creating filters.

On Linux, BPF is much simpler than on BSD.  One does not have to worry
about devices or anything like that.  You simply create your filter
code, send it to the kernel via the SO_ATTACH_FILTER option and if your
filter code passes the kernel check on it, you then immediately begin
filtering data on that socket.

You can also detach filters from your socket via the SO_DETACH_FILTER
option.  This will probably not be used much since when you close a
socket that has a filter on it the filter is automagically removed.  The
other less common case may be adding a different filter on the same
socket where you had another filter that is still running: the kernel
takes care of removing the old one and placing your new one in its
place, assuming your filter has passed the checks, otherwise if it fails
the old filter will remain on that socket.

SO_LOCK_FILTER option allows locking of the filter attached to a socket.
Once set, a filter cannot be removed or changed.  This allows one
process to setup a socket, attach a filter, lock it then drop privileges
and be assured that the filter will be kept until the socket is closed.

The biggest user of this construct might be libpcap.  Issuing a
high-level filter command like `tcpdump -i em1 port 22` passes through
the libpcap internal compiler that generates a structure that can
eventually be loaded via SO_ATTACH_FILTER to the kernel.  `tcpdump -i
em1 port 22 -ddd` displays what is being placed into this structure.

Although we were only speaking about sockets here, BPF in Linux is used
in many more places.  There's xt_bpf for netfilter, cls_bpf in the
kernel qdisc layer, SECCOMP-BPF (SECure COMPuting), and lots of
other places such as team driver, PTP code, etc. where BPF is being used.

For more information please see the following documents:

- Classic BPF (cBPF) - :ref:`Documentation/userspace-api/cBPF.rst <cbpf>`
- Internal BPF (eBPF) - :ref:`Documentation/userspace-api/eBPF.rst <ebpf>`
- SECCOMP BPF -
  :ref:`Documentation/userspace-api/seccomp_filter.rst <seccomp-filter>`

And the original BPF paper:

Steven McCanne and Van Jacobson. 1993.  The BSD packet filter: a new
architecture for user-level packet capture.  In Proceedings of the
USENIX Winter 1993 Conference Proceedings on USENIX Winter 1993
Conference Proceedings (USENIX'93).  USENIX Association, Berkeley,
CA, USA, 2-2. [http://www.tcpdump.org/papers/bpf-usenix93.pdf]

Structure
=========

User space applications include <linux/filter.h> which contains the
following relevant structures::

  struct sock_filter {	/* Filter block */
	  __u16	code;   /* Actual filter code */
	  __u8	jt;	/* Jump true */
	  __u8	jf;	/* Jump false */
	  __u32	k;      /* Generic multiuse field */
  };

Such a structure is assembled as an array of 4-tuples, that contains
a code, jt, jf and k value.  jt and jf are jump offsets and k a generic
value to be used for a provided code. ::

  struct sock_fprog {		/* Required for SO_ATTACH_FILTER. */
	  unsigned short len;   /* Number of filter blocks. */
	  struct sock_filter __user *filter;
  };

For socket filtering, a pointer to this structure (as shown in the
follow-up example) is being passed to the kernel through setsockopt(2).

Example
=======
::

  #include <sys/socket.h>
  #include <sys/types.h>
  #include <arpa/inet.h>
  #include <linux/if_ether.h>
  /* ... */

  /* From the example above: tcpdump -i em1 port 22 -dd */
  struct sock_filter code[] = {
	  { 0x28,  0,  0, 0x0000000c },
	  { 0x15,  0,  8, 0x000086dd },
	  { 0x30,  0,  0, 0x00000014 },
	  { 0x15,  2,  0, 0x00000084 },
	  { 0x15,  1,  0, 0x00000006 },
	  { 0x15,  0, 17, 0x00000011 },
	  { 0x28,  0,  0, 0x00000036 },
	  { 0x15, 14,  0, 0x00000016 },
	  { 0x28,  0,  0, 0x00000038 },
	  { 0x15, 12, 13, 0x00000016 },
	  { 0x15,  0, 12, 0x00000800 },
	  { 0x30,  0,  0, 0x00000017 },
	  { 0x15,  2,  0, 0x00000084 },
	  { 0x15,  1,  0, 0x00000006 },
	  { 0x15,  0,  8, 0x00000011 },
	  { 0x28,  0,  0, 0x00000014 },
	  { 0x45,  6,  0, 0x00001fff },
	  { 0xb1,  0,  0, 0x0000000e },
	  { 0x48,  0,  0, 0x0000000e },
	  { 0x15,  2,  0, 0x00000016 },
	  { 0x48,  0,  0, 0x00000010 },
	  { 0x15,  0,  1, 0x00000016 },
	  { 0x06,  0,  0, 0x0000ffff },
	  { 0x06,  0,  0, 0x00000000 },
  };

  struct sock_fprog bpf = {
	  .len = ARRAY_SIZE(code),
	  .filter = code,
  };

  sock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (sock < 0)
	  /* ... bail out ... */

  ret = setsockopt(sock, SOL_SOCKET, SO_ATTACH_FILTER, &bpf, sizeof(bpf));
  if (ret < 0)
	  /* ... bail out ... */

  /* ... */
  close(sock);

The above example code attaches a socket filter for a PF_PACKET socket
in order to let all IPv4/IPv6 packets with port 22 pass.  The rest will
be dropped for this socket.

The setsockopt(2) call to SO_DETACH_FILTER doesn't need any arguments
and SO_LOCK_FILTER for preventing the filter to be detached, takes an
integer value with 0 or 1.

Note that socket filters are not restricted to PF_PACKET sockets only,
but can also be used on other socket families.

Summary of system calls::

  setsockopt(sockfd, SOL_SOCKET, SO_ATTACH_FILTER, &val, sizeof(val));
  setsockopt(sockfd, SOL_SOCKET, SO_DETACH_FILTER, &val, sizeof(val));
  setsockopt(sockfd, SOL_SOCKET, SO_LOCK_FILTER,   &val, sizeof(val));

Normally, most use cases for socket filtering on packet sockets will be
covered by libpcap in high-level syntax, so as an application developer
you should stick to that.  libpcap wraps its own layer around all that.

Unless i) using/linking to libpcap is not an option, ii) the required
BPF filters use Linux extensions that are not supported by libpcap's
compiler, iii) a filter might be more complex and not cleanly
implementable with libpcap's compiler, or iv) particular filter codes
should be optimized differently than libpcap's internal compiler does;
then in such cases writing such a filter "by hand" can be of an
alternative.  For example, xt_bpf and cls_bpf users might have
requirements that could result in more complex filter code, or one that
cannot be expressed with libpcap (e.g. different return codes for
various code paths).  Moreover, BPF JIT implementors may wish to
manually write test cases and thus need low-level access to BPF code as
well.

