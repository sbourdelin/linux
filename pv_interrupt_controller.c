
The pv interrupt (PVI) hypercall is proposed to support one guest sending
interrupts to another guest using hypercalls. The following pseduocode shows how
a PVI is sent from the guest:

#define KVM_HC_PVI 9
kvm_hypercall2(KVM_HC_PVI, guest_uuid, guest_gsi);

The new hypercall number, KVM_HC_PVI, is used for the purpose of sending PVIs.
guest_uuid is used to identify the guest that the interrupt will be sent to.
guest_gsi identifies the interrupt source of that guest.

The PVI hypercall handler in KVM iterates the VM list (the vm_list field in
the kvm struct), finds the guest with the passed guest_uuid, and injects an
interrupt to the guest with the guest_gsi number.

Finally, it's about the permission of sending PVI from one guest to another.
In the PVI setup phase, the PVI receiver should get the sender's UUID (e.g. via
the vhost-user protocol extension implemented between QEMUs), and pass it to KVM.
Two new fields will be added to the struct kvm{ }:

+uuid_t uuid; // the guest uuid
+uuid_t pvi_sender_uuid[MAX_NUM]; // the sender's uuid should be registered here

PVI will not be injected to the receiver guest if the sender's uuid does not appear 
in the receiver's pvi_sender_uuid table.

