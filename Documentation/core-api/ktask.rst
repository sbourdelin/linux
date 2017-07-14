============================================
ktask: parallelize cpu-intensive kernel work
============================================

:Date: July, 2017
:Author: Daniel Jordan <daniel.m.jordan@oracle.com>


Introduction
============

ktask is a generic framework for parallelizing cpu-intensive work in the
kernel.  The intended use is for big machines that can use their cpu power to
speed up large tasks that can't otherwise be multithreaded in userland.  The
API is generic enough to add concurrency to many different kinds of tasks--for
example, zeroing a range of pages or evicting a list of inodes--and aims to
save its clients the trouble of splitting up the work, choosing the number of
threads to use, starting these threads, and load balancing the work between
them.


Motivation
==========

Why do we need ktask when the kernel has other APIs for managing concurrency?
After all, kthread_workers and workqueues already provide ways to start
threads, and the kernel can handle large tasks with a single thread by
periodically yielding the cpu with cond_resched (e.g. hugetlbfs_fallocate,
clear_gigantic_page) or performing the work in fixed size batches (e.g. struct
pagevec, struct mmu_gather).

Of the existing concurrency facilities, kthread_worker isn't suited for
providing parallelism because each comes with only a single thread.  Workqueues
are a better fit for this, and in fact ktask is built on an unbound workqueue,
but workqueues aren't designed for splitting up a large task.  ktask instead
uses unbound workqueue threads to run "chunks" of a task.

On top of workqueues, ktask takes care of dividing up the task into chunks,
determining how many threads to use to complete those chunks, starting the
threads, and load balancing across them.  This makes use of otherwise idle
cpus, but if the system is under load, the scheduler still decides when the
ktask threads run: existing cond_resched calls are retained in big loops that
have been parallelized.

This added concurrency boosts the performance of the system in a number of
ways: system startup and shutdown are faster, page fault latency of a gigantic
page goes down (zero the page in parallel), initializing many pages goes
quicker (e.g. populating a range of pages via prefaulting, mlocking, or
fallocating), and pages are freed back to the system in less time (e.g. on a
large munmap(2) or on exit(2) of a large process).


Configuration
=============

To use ktask, configure the kernel with CONFIG_KTASK=y.

If CONFIG_KTASK=n, calls to the ktask API are simply #define'd to run the
thread function that the client provides so that the task is completed without
concurrency in the current thread.


Concept
=======

A little terminology up front:  A 'task' is the total work there is to do and a
'chunk' is a unit of work given to a thread.

To complete a task using the ktask framework, a client provides a thread
function that is responsible for completing one chunk.  The thread function is
defined in a standard way, with start and end arguments that delimit the chunk
as well as an argument that the client uses to pass data specific to the task.

In addition, the client supplies an object representing the start of the task
and an iterator function that knows how to advance some number of units in the
task to yield another object representing the new task position.  The framework
uses the start object and iterator internally to divide the task into chunks.

Finally, the client passes the total task size and a minimum chunk size to
indicate the minimum amount of work that's appropriate to do in one chunk.  The
sizes are given in task-specific units (e.g. pages, inodes, bytes).  The
framework uses these sizes, along with the number of online cpus and an
internal maximum number of threads, to decide how many threads to start and how
many chunks to divide the task into.

For example, consider the task of clearing a gigantic page.  This used to be
done in a single thread with a for loop that calls a page clearing function for
each constituent base page.  To parallelize with ktask, the client first moves
the for loop to the thread function, adapting it to operate on the range passed
to the function.  In this simple case, the thread function's start and end
arguments are just addresses delimiting the portion of the gigantic page to
clear.  Then, where the for loop used to be, the client calls into ktask with
the start address of the gigantic page, the total size of the gigantic page,
and the thread function.  Internally, ktask will divide the address range into
an appropriate number of chunks and start an appropriate number of threads to
complete these chunks.


Interface
=========

.. Include ktask.h inline here.  This file is heavily commented and documents
.. the ktask interface.
.. kernel-doc:: include/linux/ktask.h
