The Kernel Address Sanitizer (KASAN)
====================================

Overview
--------

KernelAddressSANitizer (KASAN) is a dynamic memory error detector. It provides
a fast and comprehensive solution for finding use-after-free and out-of-bounds
bugs.

KASAN has two modes: classic KASAN (a classic version, similar to user space
ASan) and KHWASAN (a version based on memory tagging, similar to user space
HWASan).

KASAN uses compile-time instrumentation to insert validity checks before every
memory access, and therefore requires a compiler version that supports that.
For classic KASAN you need GCC version 4.9.2 or later. GCC 5.0 or later is
required for detection of out-of-bounds accesses on stack and global variables.
KHWASAN in turns is only supported in clang and requires revision 330044 or
later.

Currently classic KASAN is supported for the x86_64, arm64 and xtensa
architectures, and KHWASAN is supported only for arm64.

Usage
-----

To enable KASAN configure kernel with::

	  CONFIG_KASAN = y

and choose between CONFIG_KASAN_GENERIC (to enable classic KASAN) and
CONFIG_KASAN_HW (to enabled KHWASAN). You also need to choose choose between
CONFIG_KASAN_OUTLINE and CONFIG_KASAN_INLINE. Outline and inline are compiler
instrumentation types. The former produces smaller binary while the latter is
1.1 - 2 times faster. For classic KASAN inline instrumentation requires GCC
version 5.0 or later.

Both KASAN modes work with both SLUB and SLAB memory allocators.
For better bug detection and nicer reporting, enable CONFIG_STACKTRACE.

To disable instrumentation for specific files or directories, add a line
similar to the following to the respective kernel Makefile:

- For a single file (e.g. main.o)::

    KASAN_SANITIZE_main.o := n

- For all files in one directory::

    KASAN_SANITIZE := n

Error reports
~~~~~~~~~~~~~

A typical out-of-bounds access classic KASAN report looks like this::

    ==================================================================
    BUG: KASAN: slab-out-of-bounds in kmalloc_oob_right+0xa8/0xbc [test_kasan]
    Write of size 1 at addr ffff8800696f3d3b by task insmod/2734
    
    CPU: 0 PID: 2734 Comm: insmod Not tainted 4.15.0+ #98
    Hardware name: QEMU Standard PC (i440FX + PIIX, 1996), BIOS 1.10.2-1 04/01/2014
    Call Trace:
     __dump_stack lib/dump_stack.c:17
     dump_stack+0x83/0xbc lib/dump_stack.c:53
     print_address_description+0x73/0x280 mm/kasan/report.c:254
     kasan_report_error mm/kasan/report.c:352
     kasan_report+0x10e/0x220 mm/kasan/report.c:410
     __asan_report_store1_noabort+0x17/0x20 mm/kasan/report.c:505
     kmalloc_oob_right+0xa8/0xbc [test_kasan] lib/test_kasan.c:42
     kmalloc_tests_init+0x16/0x769 [test_kasan]
     do_one_initcall+0x9e/0x240 init/main.c:832
     do_init_module+0x1b6/0x542 kernel/module.c:3462
     load_module+0x6042/0x9030 kernel/module.c:3786
     SYSC_init_module+0x18f/0x1c0 kernel/module.c:3858
     SyS_init_module+0x9/0x10 kernel/module.c:3841
     do_syscall_64+0x198/0x480 arch/x86/entry/common.c:287
     entry_SYSCALL_64_after_hwframe+0x21/0x86 arch/x86/entry/entry_64.S:251
    RIP: 0033:0x7fdd79df99da
    RSP: 002b:00007fff2229bdf8 EFLAGS: 00000202 ORIG_RAX: 00000000000000af
    RAX: ffffffffffffffda RBX: 000055c408121190 RCX: 00007fdd79df99da
    RDX: 00007fdd7a0b8f88 RSI: 0000000000055670 RDI: 00007fdd7a47e000
    RBP: 000055c4081200b0 R08: 0000000000000003 R09: 0000000000000000
    R10: 00007fdd79df5d0a R11: 0000000000000202 R12: 00007fdd7a0b8f88
    R13: 000055c408120090 R14: 0000000000000000 R15: 0000000000000000
    
    Allocated by task 2734:
     save_stack+0x43/0xd0 mm/kasan/common.c:176
     set_track+0x20/0x30 mm/kasan/common.c:188
     kasan_kmalloc+0x9a/0xc0 mm/kasan/kasan.c:372
     kmem_cache_alloc_trace+0xcd/0x1a0 mm/slub.c:2761
     kmalloc ./include/linux/slab.h:512
     kmalloc_oob_right+0x56/0xbc [test_kasan] lib/test_kasan.c:36
     kmalloc_tests_init+0x16/0x769 [test_kasan]
     do_one_initcall+0x9e/0x240 init/main.c:832
     do_init_module+0x1b6/0x542 kernel/module.c:3462
     load_module+0x6042/0x9030 kernel/module.c:3786
     SYSC_init_module+0x18f/0x1c0 kernel/module.c:3858
     SyS_init_module+0x9/0x10 kernel/module.c:3841
     do_syscall_64+0x198/0x480 arch/x86/entry/common.c:287
     entry_SYSCALL_64_after_hwframe+0x21/0x86 arch/x86/entry/entry_64.S:251
    
    The buggy address belongs to the object at ffff8800696f3cc0
     which belongs to the cache kmalloc-128 of size 128
    The buggy address is located 123 bytes inside of
     128-byte region [ffff8800696f3cc0, ffff8800696f3d40)
    The buggy address belongs to the page:
    page:ffffea0001a5bcc0 count:1 mapcount:0 mapping:          (null) index:0x0
    flags: 0x100000000000100(slab)
    raw: 0100000000000100 0000000000000000 0000000000000000 0000000180150015
    raw: ffffea0001a8ce40 0000000300000003 ffff88006d001640 0000000000000000
    page dumped because: kasan: bad access detected
    
    Memory state around the buggy address:
     ffff8800696f3c00: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 fc
     ffff8800696f3c80: fc fc fc fc fc fc fc fc 00 00 00 00 00 00 00 00
    >ffff8800696f3d00: 00 00 00 00 00 00 00 03 fc fc fc fc fc fc fc fc
                                            ^
     ffff8800696f3d80: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 fc fc
     ffff8800696f3e00: fc fc fc fc fc fc fc fc fb fb fb fb fb fb fb fb
    ==================================================================

The header of the report provides a short summary of what kind of bug happened
and what kind of access caused it. It's followed by a stack trace of the bad
access, a stack trace of where the accessed memory was allocated (in case bad
access happens on a slab object), and a stack trace of where the object was
freed (in case of a use-after-free bug report). Next comes a description of
the accessed slab object and information about the accessed memory page.

In the last section the report shows memory state around the accessed address.
Reading this part requires some understanding of how KASAN works.

The state of each 8 aligned bytes of memory is encoded in one shadow byte.
Those 8 bytes can be accessible, partially accessible, freed or be a redzone.
We use the following encoding for each shadow byte: 0 means that all 8 bytes
of the corresponding memory region are accessible; number N (1 <= N <= 7) means
that the first N bytes are accessible, and other (8 - N) bytes are not;
any negative value indicates that the entire 8-byte word is inaccessible.
We use different negative values to distinguish between different kinds of
inaccessible memory like redzones or freed memory (see mm/kasan/kasan.h).

In the report above the arrows point to the shadow byte 03, which means that
the accessed address is partially accessible.

For KHWASAN this last report section shows the memory tags around the accessed
address (see Implementation details section).


Implementation details
----------------------

Classic KASAN
~~~~~~~~~~~~~

From a high level, our approach to memory error detection is similar to that
of kmemcheck: use shadow memory to record whether each byte of memory is safe
to access, and use compile-time instrumentation to insert checks of shadow
memory on each memory access.

Classic KASAN dedicates 1/8th of kernel memory to its shadow memory (e.g. 16TB
to cover 128TB on x86_64) and uses direct mapping with a scale and offset to
translate a memory address to its corresponding shadow address.

Here is the function which translates an address to its corresponding shadow
address::

    static inline void *kasan_mem_to_shadow(const void *addr)
    {
	return ((unsigned long)addr >> KASAN_SHADOW_SCALE_SHIFT)
		+ KASAN_SHADOW_OFFSET;
    }

where ``KASAN_SHADOW_SCALE_SHIFT = 3``.

Compile-time instrumentation is used to insert memory access checks. Compiler
inserts function calls (__asan_load*(addr), __asan_store*(addr)) before each
memory access of size 1, 2, 4, 8 or 16. These functions check whether memory
access is valid or not by checking corresponding shadow memory.

GCC 5.0 has possibility to perform inline instrumentation. Instead of making
function calls GCC directly inserts the code to check the shadow memory.
This option significantly enlarges kernel but it gives x1.1-x2 performance
boost over outline instrumented kernel.

KHWASAN
~~~~~~~

KHWASAN uses the Top Byte Ignore (TBI) feature of modern arm64 CPUs to store
a pointer tag in the top byte of kernel pointers. KHWASAN also uses shadow
memory to store memory tags associated with each 16-byte memory cell (therefore
it dedicates 1/16th of the kernel memory for shadow memory).

On each memory allocation KHWASAN generates a random tag, tags allocated memory
with this tag, and embeds this tag into the returned pointer. KHWASAN uses
compile-time instrumentation to insert checks before each memory access. These
checks make sure that tag of the memory that is being accessed is equal to tag
of the pointer that is used to access this memory. In case of a tag mismatch
KHWASAN prints a bug report.

KHWASAN also has two instrumentation modes (outline, that emits callbacks to
check memory accesses; and inline, that performs the shadow memory checks
inline). With outline instrumentation mode, a bug report is simply printed
from the function that performs the access check. With inline instrumentation
a brk instruction is emitted by the compiler, and a dedicated brk handler is
used to print KHWASAN reports.
