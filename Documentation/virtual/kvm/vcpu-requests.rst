=================
KVM VCPU Requests
=================

Overview
========

KVM supports an internal API enabling threads to request a VCPU thread to
perform some activity.  For example, a thread may request a VCPU to flush
its TLB with a VCPU request.  The API consists of only four calls::

  /* Check if VCPU @vcpu has request @req pending. Clears the request. */
  bool kvm_check_request(int req, struct kvm_vcpu *vcpu);

  /* Check if any requests are pending for VCPU @vcpu. */
  bool kvm_request_pending(struct kvm_vcpu *vcpu);

  /* Make request @req of VCPU @vcpu. */
  void kvm_make_request(int req, struct kvm_vcpu *vcpu);

  /* Make request @req of all VCPUs of the VM with struct kvm @kvm. */
  bool kvm_make_all_cpus_request(struct kvm *kvm, unsigned int req);

Typically a requester wants the VCPU to perform the activity as soon
as possible after making the request.  This means most requests,
kvm_make_request() calls, are followed by a call to kvm_vcpu_kick(),
and kvm_make_all_cpus_request() has the kicking of all VCPUs built
into it.

VCPU Kicks
----------

A VCPU kick does one of three things:

 1) wakes a sleeping VCPU (which sleeps outside guest mode).
 2) sends an IPI to a VCPU currently in guest mode, in order to bring it
    out.
 3) nothing, when the VCPU is already outside guest mode and not sleeping.

VCPU Request Internals
======================

VCPU requests are simply bit indices of the vcpu->requests bitmap.  This
means general bitops[1], e.g. clear_bit(KVM_REQ_UNHALT, &vcpu->requests),
may also be used.  The first 8 bits are reserved for architecture
independent requests, all additional bits are available for architecture
dependent requests.

VCPU Requests with Associated State
===================================

Requesters that want the requested VCPU to handle new state need to ensure
the state is observable to the requested VCPU thread's CPU at the time the
CPU observes the request.  This means a write memory barrier should be
insert between the preparation of the state and the write of the VCPU
request bitmap.  Additionally, on the requested VCPU thread's side, a
corresponding read barrier should be issued after reading the request bit
and before proceeding to use the state associated with it.  See the kernel
memory barrier documentation [2].

VCPU Requests and Guest Mode
============================

As long as the guest is either in guest mode, in which case it gets an IPI
and will definitely see the request, or is outside guest mode, but has yet
to do its final request check, and therefore when it does, it will see the
request, then things will work.  However, the transition from outside to
inside guest mode, after the last request check has been made, opens a
window where a request could be made, but the VCPU would not see until it
exits guest mode some time later.  See the table below.

+------------------+-----------------+----------------+--------------+
| vcpu->mode       | done last check | kick sends IPI | request seen |
+==================+=================+================+==============+
| IN_GUEST_MODE    |      N/A        |      YES       |     YES      |
+------------------+-----------------+----------------+--------------+
| !IN_GUEST_MODE   |      NO         |      NO        |     YES      |
+------------------+-----------------+----------------+--------------+
| !IN_GUEST_MODE   |      YES        |      NO        |     NO       |
+------------------+-----------------+----------------+--------------+

To ensure the third scenario shown in the table above cannot happen, we
need to ensure the VCPU's mode change is observable by all CPUs prior to
its final request check and that a requester's request is observable by
the requested VCPU prior to the kick.  To do that we need general memory
barriers between each pair of operations involving mode and requests, i.e.

  CPU_i                                  CPU_j
-------------------------------------------------------------------------
  vcpu->mode = IN_GUEST_MODE;            kvm_make_request(REQ, vcpu);
  smp_mb();                              smp_mb();
  if (kvm_request_pending(vcpu))         if (vcpu->mode == IN_GUEST_MODE)
      handle_requests();                     send_IPI(vcpu->cpu);

Whether explicit barriers are needed, or reliance on implicit barriers is
sufficient, is architecture dependent.  Alternatively, an architecture may
choose to just always send the IPI, as not sending it, when it's not
necessary, is just an optimization.

Additionally, the error prone third scenario described above also exhibits
why a request-less VCPU kick is almost never correct.  Without the
assurance that a non-IPI generating kick will still result in an action by
the requested VCPU, as the final kvm_request_pending() check does, then
the kick may not initiate anything useful at all.  If, for instance, a
request-less kick was made to a VCPU that was just about to set its mode
to IN_GUEST_MODE, meaning no IPI is sent, then the VCPU may continue its
entry without actually having done whatever it was the kick was meant to
initiate.

References
==========

[1] Documentation/core-api/atomic_ops.rst
[2] Documentation/memory-barriers.txt
