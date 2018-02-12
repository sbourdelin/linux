.. SPDX-License-Identifier: GPL-2.0

Protectable memory allocator
============================

Purpose
-------

The pmalloc library is meant to provide R/O status to data that, for some
reason, could neither be declared as constant, nor could it take advantage
of the qualifier __ro_after_init, but is write-once and read-only in spirit.
It protects data from both accidental and malicious overwrites.

Example: A policy that is loaded from userspace.


Concept
-------

pmalloc builds on top of genalloc, using the same concept of memory pools.

The value added by pmalloc is that now the memory contained in a pool can
become R/O, for the rest of the life of the pool.

Different kernel drivers and threads can use different pools, for finer
control of what becomes R/O and when. And for improved lockless concurrency.


Caveats
-------

- Memory freed while a pool is not yet protected will be reused.

- Once a pool is protected, it's not possible to allocate any more memory
  from it.

- Memory "freed" from a protected pool indicates that such memory is not
  in use anymore by the requester; however, it will not become available
  for further use, until the pool is destroyed.

- Before destroying a pool, all the memory allocated from it must be
  released.

- pmalloc does not provide locking support with respect to allocating vs
  protecting an individual pool, for performance reasons.
  It is recommended not to share the same pool between unrelated functions.
  Should sharing be a necessity, the user of the shared pool is expected
  to implement locking for that pool.

- pmalloc uses genalloc to optimize the use of the space it allocates
  through vmalloc. Some more TLB entries will be used, however less than
  in the case of using vmalloc directly. The exact number depends on the
  size of each allocation request and possible slack.

- Considering that not much data is supposed to be dynamically allocated
  and then marked as read-only, it shouldn't be an issue that the address
  range for pmalloc is limited, on 32-bit systems.

- Regarding SMP systems, the allocations are expected to happen mostly
  during an initial transient, after which there should be no more need to
  perform cross-processor synchronizations of page tables.

- To facilitate the conversion of existing code to pmalloc pools, several
  helper functions are provided, mirroring their kmalloc counterparts.


Use
---

The typical sequence, when using pmalloc, is:

1. create a pool

.. kernel-doc:: include/linux/pmalloc.h
   :functions: pmalloc_create_pool

2. [optional] pre-allocate some memory in the pool

.. kernel-doc:: include/linux/pmalloc.h
   :functions: pmalloc_prealloc

3. issue one or more allocation requests to the pool with locking as needed

.. kernel-doc:: include/linux/pmalloc.h
   :functions: pmalloc

.. kernel-doc:: include/linux/pmalloc.h
   :functions: pzalloc

4. initialize the memory obtained with desired values

5. [optional] iterate over points 3 & 4 as needed

6. write-protect the pool

.. kernel-doc:: include/linux/pmalloc.h
   :functions: pmalloc_protect_pool

7. use in read-only mode the handles obtained through the allocations

8. [optional] release all the memory allocated

.. kernel-doc:: include/linux/pmalloc.h
   :functions: pfree

9. [optional, but depends on point 8] destroy the pool

.. kernel-doc:: include/linux/pmalloc.h
   :functions: pmalloc_destroy_pool

API
---

.. kernel-doc:: include/linux/pmalloc.h
