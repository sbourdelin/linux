.. SPDX-License-Identifier: GPL-2.0+

.. _cbpf:

====================================
Classic Berkley Packet Filter (cBPF)
====================================

For an introduction to BPF and Linux Socket Filtering please see
:ref:`Documentation/userspace-api/socket-filter.rst <socket-filter>`

Under tools/bpf/ there's a small helper tool called bpf_asm which can be
used to write low-level filters for example scenarios mentioned in the
above linked document.  Asm-like syntax mentioned here has been
implemented in bpf_asm and will be used for further explanations
(instead of dealing with less readable opcodes directly, principles are
the same).  The syntax is closely modelled after Steven McCanne's and
Van Jacobson's BPF paper [1].

The BPF architecture consists of the following basic elements:

.. flat-table:: BPF Elements

   * - Element
     - Description

   * - A
     - 32 bit wide accumulator

   * - X
     - 32 bit wide X register

   * - M[]
     - 16 x 32 bit wide misc registers aka "scratch memory store",
       addressable from 0 to 15

A program, that is translated by bpf_asm into "opcodes" is an array that
consists of the following elements (as already mentioned)::

  op:16, jt:8, jf:8, k:32

The element op is a 16 bit wide opcode that has a particular instruction
encoded.  jt and jf are two 8 bit wide jump targets, one for condition
"jump if true", the other one "jump if false".  Eventually, element k
contains a miscellaneous argument that can be interpreted in different
ways depending on the given instruction in op.

The instruction set consists of load, store, branch, alu, miscellaneous
and return instructions that are also represented in bpf_asm syntax.
This table lists all bpf_asm instructions available resp.  what their
underlying opcodes as defined in linux/filter.h stand for:
::

  Instruction      Addressing mode      Description

  ld               1, 2, 3, 4, 10       Load word into A
  ldi              4                    Load word into A
  ldh              1, 2                 Load half-word into A
  ldb              1, 2                 Load byte into A
  ldx              3, 4, 5, 10          Load word into X
  ldxi             4                    Load word into X
  ldxb             5                    Load byte into X

  st               3                    Store A into M[]
  stx              3                    Store X into M[]

  jmp              6                    Jump to label
  ja               6                    Jump to label
  jeq              7, 8                 Jump on A == k
  jneq             8                    Jump on A != k
  jne              8                    Jump on A != k
  jlt              8                    Jump on A <  k
  jle              8                    Jump on A <= k
  jgt              7, 8                 Jump on A >  k
  jge              7, 8                 Jump on A >= k
  jset             7, 8                 Jump on A &  k

  add              0, 4                 A + <x>
  sub              0, 4                 A - <x>
  mul              0, 4                 A * <x>
  div              0, 4                 A / <x>
  mod              0, 4                 A % <x>
  neg                                   !A
  and              0, 4                 A & <x>
  or               0, 4                 A | <x>
  xor              0, 4                 A ^ <x>
  lsh              0, 4                 A << <x>
  rsh              0, 4                 A >> <x>

  tax                                   Copy A into X
  txa                                   Copy X into A

  ret              4, 9                 Return

The next table shows addressing formats from the 2nd column::

  Addressing mode  Syntax               Description

   0               x/%x                 Register X
   1               [k]                  BHW at byte offset k in the packet
   2               [x + k]              BHW at the offset X + k in the packet
   3               M[k]                 Word at offset k in M[]
   4               #k                   Literal value stored in k
   5               4*([k]&0xf)          Lower nibble * 4 at byte offset k in the packet
   6               L                    Jump label L
   7               #k,Lt,Lf             Jump to Lt if true, otherwise jump to Lf
   8               #k,Lt                Jump to Lt if predicate is true
   9               a/%a                 Accumulator A
  10               extension            BPF extension

The Linux kernel also has a couple of BPF extensions that are used along
with the class of load instructions by "overloading" the k argument with
a negative offset + a particular extension offset.  The result of such
BPF extensions are loaded into A.

Possible BPF extensions are shown in the following table::

  Extension                             Description

  len                                   skb->len
  proto                                 skb->protocol
  type                                  skb->pkt_type
  poff                                  Payload start offset
  ifidx                                 skb->dev->ifindex
  nla                                   Netlink attribute of type X with offset A
  nlan                                  Nested Netlink attribute of type X with offset A
  mark                                  skb->mark
  queue                                 skb->queue_mapping
  hatype                                skb->dev->type
  rxhash                                skb->hash
  cpu                                   raw_smp_processor_id()
  vlan_tci                              skb_vlan_tag_get(skb)
  vlan_avail                            skb_vlan_tag_present(skb)
  vlan_tpid                             skb->vlan_proto
  rand                                  prandom_u32()

These extensions can also be prefixed with '#'.
Examples for low-level BPF:

ARP packets::

  ldh [12]
  jne #0x806, drop
  ret #-1
  drop: ret #0

IPv4 TCP packets::

  ldh [12]
  jne #0x800, drop
  ldb [23]
  jneq #6, drop
  ret #-1
  drop: ret #0

(Accelerated) VLAN w/ id 10::

  ld vlan_tci
  jneq #10, drop
  ret #-1
  drop: ret #0

icmp random packet sampling, 1 in 4::

  ldh [12]
  jne #0x800, drop
  ldb [23]
  jneq #1, drop
  # get a random uint32 number
  ld rand
  mod #4
  jneq #1, drop
  ret #-1
  drop: ret #0

SECCOMP filter example::

  ld [4]                  /* offsetof(struct seccomp_data, arch) */
  jne #0xc000003e, bad    /* AUDIT_ARCH_X86_64 */
  ld [0]                  /* offsetof(struct seccomp_data, nr) */
  jeq #15, good           /* __NR_rt_sigreturn */
  jeq #231, good          /* __NR_exit_group */
  jeq #60, good           /* __NR_exit */
  jeq #0, good            /* __NR_read */
  jeq #1, good            /* __NR_write */
  jeq #5, good            /* __NR_fstat */
  jeq #9, good            /* __NR_mmap */
  jeq #14, good           /* __NR_rt_sigprocmask */
  jeq #13, good           /* __NR_rt_sigaction */
  jeq #35, good           /* __NR_nanosleep */
  bad: ret #0             /* SECCOMP_RET_KILL_THREAD */
  good: ret #0x7fff0000   /* SECCOMP_RET_ALLOW */

The above example code can be placed into a file (here called "foo"),
and then be passed to the bpf_asm tool for generating opcodes, output
that xt_bpf and cls_bpf understands and can directly be loaded with.
Example with above ARP code::

  $ ./bpf_asm foo
  4,40 0 0 12,21 0 1 2054,6 0 0 4294967295,6 0 0 0,

In copy and paste C-like output::

  $ ./bpf_asm -c foo
  { 0x28,  0,  0, 0x0000000c },
  { 0x15,  0,  1, 0x00000806 },
  { 0x06,  0,  0, 0xffffffff },
  { 0x06,  0,  0, 0000000000 },

In particular, as usage with xt_bpf or cls_bpf can result in more
complex BPF filters that might not be obvious at first, it's good to
test filters before attaching to a live system.  For that purpose,
there's a small tool called bpf_dbg under tools/bpf/ in the kernel
source directory.  This debugger allows for testing BPF filters against
given pcap files, single stepping through the BPF code on the pcap's
packets and to do BPF machine register dumps.

Starting bpf_dbg is trivial and just requires issuing::

  # ./bpf_dbg

In case input and output do not equal stdin/stdout, bpf_dbg takes an
alternative stdin source as a first argument, and an alternative stdout
sink as a second one, e.g. `./bpf_dbg test_in.txt test_out.txt`.

Other than that, a particular libreadline configuration can be set via
file "~/.bpf_dbg_init" and the command history is stored in the file
"~/.bpf_dbg_history".

Interaction in bpf_dbg happens through a shell that also has
auto-completion support (follow-up example commands starting with '>'
denote bpf_dbg shell).  The usual workflow would be to ... ::

  > load bpf 6,40 0 0 12,21 0 3 2048,48 0 0 23,21 0 1 1,6 0 0 65535,6 0 0 0
    Loads a BPF filter from standard output of bpf_asm, or transformed via
    e.g. `tcpdump -iem1 -ddd port 22 | tr '\n' ','`.  Note that for JIT
    debugging (next section), this command creates a temporary socket and
    loads the BPF code into the kernel.  Thus, this will also be useful for
    JIT developers.

  > load pcap foo.pcap
    Loads standard tcpdump pcap file.

  > run [<n>]
  bpf passes:1 fails:9
    Runs through all packets from a pcap to account how many passes and fails
    the filter will generate.  A limit of packets to traverse can be given.

  > disassemble
  l0:	  ldh [12]
  l1:	  jeq #0x800, l2, l5
  l2:	  ldb [23]
  l3:	  jeq #0x1, l4, l5
  l4:	  ret #0xffff
  l5:	  ret #0
    Prints out BPF code disassembly.

  > dump
  /* { op, jt, jf, k }, */
  { 0x28,  0,  0, 0x0000000c },
  { 0x15,  0,  3, 0x00000800 },
  { 0x30,  0,  0, 0x00000017 },
  { 0x15,  0,  1, 0x00000001 },
  { 0x06,  0,  0, 0x0000ffff },
  { 0x06,  0,  0, 0000000000 },
    Prints out C-style BPF code dump.

  > breakpoint 0
  breakpoint at: l0:  	  ldh [12]
  > breakpoint 1
  breakpoint at: l1:	  jeq #0x800, l2, l5
    ...
    Sets breakpoints at particular BPF instructions.  Issuing a `run` command
    will walk through the pcap file continuing from the current packet and
    break when a breakpoint is being hit (another `run` will continue from
    the currently active breakpoint executing next instructions):

  > run
  -- register dump --
  pc:       [0]                       <-- program counter
  code:     [40] jt[0] jf[0] k[12]    <-- plain BPF code of current instruction
  curr:     l0:	ldh [12]              <-- disassembly of current instruction
  A:        [00000000][0]             <-- content of A (hex, decimal)
  X:        [00000000][0]             <-- content of X (hex, decimal)
  M[0,15]:  [00000000][0]             <-- folded content of M (hex, decimal)
  -- packet dump --                   <-- Current packet from pcap (hex)
  len: 42
    0: 00 19 cb 55 55 a4 00 14 a4 43 78 69 08 06 00 01
   16: 08 00 06 04 00 01 00 14 a4 43 78 69 0a 3b 01 26
   32: 00 00 00 00 00 00 0a 3b 01 01
  (breakpoint)
  >

  > breakpoint
  breakpoints: 0 1
    Prints currently set breakpoints.

  > step [-<n>, +<n>]
    Performs single stepping through the BPF program from the current pc
    offset.  Thus, on each step invocation, above register dump is issued.
    This can go forwards and backwards in time, a plain `step` will break
    on the next BPF instruction, thus +1.  (No `run` needs to be issued here.)

  > select <n>
    Selects a given packet from the pcap file to continue from.  Thus, on
    the next `run` or `step`, the BPF program is being evaluated against
    the user pre-selected packet.  Numbering starts just as in Wireshark
    with index 1.

  > quit
  #
    Exits bpf_dbg.

JIT compiler
============

The Linux kernel has a built-in BPF JIT compiler for x86_64, SPARC,
PowerPC, ARM, ARM64, MIPS and s390 which can be enabled through
CONFIG_BPF_JIT.  The JIT compiler is transparently invoked for each
attached filter from user space or for internal kernel users if it has
been previously enabled by root::

  echo 1 > /proc/sys/net/core/bpf_jit_enable

For JIT developers, doing audits etc, each compile run can output the
generated opcode image into the kernel log via::

  echo 2 > /proc/sys/net/core/bpf_jit_enable

Example output from dmesg::

  [ 3389.935842] flen=6 proglen=70 pass=3 image=ffffffffa0069c8f
  [ 3389.935847] JIT code: 00000000: 55 48 89 e5 48 83 ec 60 48 89 5d f8 44 8b 4f 68
  [ 3389.935849] JIT code: 00000010: 44 2b 4f 6c 4c 8b 87 d8 00 00 00 be 0c 00 00 00
  [ 3389.935850] JIT code: 00000020: e8 1d 94 ff e0 3d 00 08 00 00 75 16 be 17 00 00
  [ 3389.935851] JIT code: 00000030: 00 e8 28 94 ff e0 83 f8 01 75 07 b8 ff ff 00 00
  [ 3389.935852] JIT code: 00000040: eb 02 31 c0 c9 c3

When CONFIG_BPF_JIT_ALWAYS_ON is enabled, bpf_jit_enable is permanently
set to 1 and setting any other value than that will return in failure.
This is even the case for setting bpf_jit_enable to 2, since dumping the
final JIT image into the kernel log is discouraged and introspection
through bpftool (under tools/bpf/bpftool/) is the generally recommended
approach instead.

In the kernel source tree under tools/bpf/, there's bpf_jit_disasm for
generating disassembly out of the kernel log's hexdump::

  # ./bpf_jit_disasm
  70 bytes emitted from JIT compiler (pass:3, flen:6)
  ffffffffa0069c8f + <x>:
     0:	  push   %rbp
     1:	  mov    %rsp,%rbp
     4:	  sub    $0x60,%rsp
     8:	  mov    %rbx,-0x8(%rbp)
     c:	  mov    0x68(%rdi),%r9d
    10:	  sub    0x6c(%rdi),%r9d
    14:	  mov    0xd8(%rdi),%r8
    1b:	  mov    $0xc,%esi
    20:	  callq  0xffffffffe0ff9442
    25:	  cmp    $0x800,%eax
    2a:	  jne    0x0000000000000042
    2c:	  mov    $0x17,%esi
    31:	  callq  0xffffffffe0ff945e
    36:	  cmp    $0x1,%eax
    39:	  jne    0x0000000000000042
    3b:	  mov    $0xffff,%eax
    40:	  jmp    0x0000000000000044
    42:	  xor    %eax,%eax
    44:	  leaveq
    45:	  retq

Issuing option `-o` will "annotate" opcodes to resulting assembler
instructions, which can be very useful for JIT developers::

  # ./bpf_jit_disasm -o
  70 bytes emitted from JIT compiler (pass:3, flen:6)
  ffffffffa0069c8f + <x>:
     0:	  push   %rbp
	  55
     1:	  mov    %rsp,%rbp
	  48 89 e5
     4:	  sub    $0x60,%rsp
	  48 83 ec 60
     8:	  mov    %rbx,-0x8(%rbp)
	  48 89 5d f8
     c:	  mov    0x68(%rdi),%r9d
	  44 8b 4f 68
    10:	  sub    0x6c(%rdi),%r9d
	  44 2b 4f 6c
    14:	  mov    0xd8(%rdi),%r8
	  4c 8b 87 d8 00 00 00
    1b:	  mov    $0xc,%esi
	  be 0c 00 00 00
    20:	  callq  0xffffffffe0ff9442
	  e8 1d 94 ff e0
    25:	  cmp    $0x800,%eax
	  3d 00 08 00 00
    2a:	  jne    0x0000000000000042
	  75 16
    2c:	  mov    $0x17,%esi
	  be 17 00 00 00
    31:	  callq  0xffffffffe0ff945e
	  e8 28 94 ff e0
    36:	  cmp    $0x1,%eax
	  83 f8 01
    39:	  jne    0x0000000000000042
	  75 07
    3b:	  mov    $0xffff,%eax
	  b8 ff ff 00 00
    40:	  jmp    0x0000000000000044
	  eb 02
    42:	  xor    %eax,%eax
	  31 c0
    44:	  leaveq
	  c9
    45:	  retq
	  c3

For BPF JIT developers, bpf_jit_disasm, bpf_asm and bpf_dbg provides a
useful toolchain for developing and testing the kernel's JIT compiler.

Reference
=========

The original BPF paper:

[1] Steven McCanne and Van Jacobson. 1993.  The BSD packet filter: a new
architecture for user-level packet capture.  In Proceedings of the
USENIX Winter 1993 Conference Proceedings on USENIX Winter 1993
Conference Proceedings (USENIX'93).  USENIX Association, Berkeley, CA,
USA, 2-2. [http://www.tcpdump.org/papers/bpf-usenix93.pdf]
