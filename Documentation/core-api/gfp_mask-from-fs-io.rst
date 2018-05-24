=================================
GFP masks used from FS/IO context
=================================

:Date: Mapy, 2018
:Author: Michal Hocko <mhocko@kernel.org>

Introduction
============

Code paths in the filesystem and IO stacks must be careful when
allocating memory to prevent recursion deadlocks caused by direct
memory reclaim calling back into the FS or IO paths and blocking on
already held resources (e.g. locks - most commonly those used for the
transaction context).

The traditional way to avoid this deadlock problem is to clear __GFP_FS
resp. __GFP_IO (note the later implies clearing the first as well) in
the gfp mask when calling an allocator. GFP_NOFS resp. GFP_NOIO can be
used as shortcut. It turned out though that above approach has led to
abuses when the restricted gfp mask is used "just in case" without a
deeper consideration which leads to problems because an excessive use
of GFP_NOFS/GFP_NOIO can lead to memory over-reclaim or other memory
reclaim issues.

New API
========

Since 4.12 we do have a generic scope API for both NOFS and NOIO context
``memalloc_nofs_save``, ``memalloc_nofs_restore`` resp. ``memalloc_noio_save``,
``memalloc_noio_restore`` which allow to mark a scope to be a critical
section from the memory reclaim recursion into FS/IO POV. Any allocation
from that scope will inherently drop __GFP_FS resp. __GFP_IO from the given
mask so no memory allocation can recurse back in the FS/IO.

FS/IO code then simply calls the appropriate save function right at the
layer where a lock taken from the reclaim context (e.g. shrinker) and
the corresponding restore function when the lock is released. All that
ideally along with an explanation what is the reclaim context for easier
maintenance.

What about __vmalloc(GFP_NOFS)
==============================

vmalloc doesn't support GFP_NOFS semantic because there are hardcoded
GFP_KERNEL allocations deep inside the allocator which are quite non-trivial
to fix up. That means that calling ``vmalloc`` with GFP_NOFS/GFP_NOIO is
almost always a bug. The good news is that the NOFS/NOIO semantic can be
achieved by the scope api.

In the ideal world, upper layers should already mark dangerous contexts
and so no special care is required and vmalloc should be called without
any problems. Sometimes if the context is not really clear or there are
layering violations then the recommended way around that is to wrap ``vmalloc``
by the scope API with a comment explaining the problem.
