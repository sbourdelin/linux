#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/bug.h>
#include <linux/export.h>
#include <asm/hypervisor.h>
#include <asm/xen/hypercall.h>

static const size_t evtchnop_len[] = {
	[EVTCHNOP_bind_interdomain] = sizeof(struct evtchn_bind_interdomain),
	[EVTCHNOP_bind_virq]	    = sizeof(struct evtchn_bind_virq),
	[EVTCHNOP_bind_pirq]	    = sizeof(struct evtchn_bind_pirq),
	[EVTCHNOP_close]	    = sizeof(struct evtchn_close),
	[EVTCHNOP_send]		    = sizeof(struct evtchn_send),
	[EVTCHNOP_alloc_unbound]    = sizeof(struct evtchn_alloc_unbound),
	[EVTCHNOP_bind_ipi]	    = sizeof(struct evtchn_bind_ipi),
	[EVTCHNOP_status]	    = sizeof(struct evtchn_status),
	[EVTCHNOP_bind_vcpu]	    = sizeof(struct evtchn_bind_vcpu),
	[EVTCHNOP_unmask]	    = sizeof(struct evtchn_unmask),
};

int xen_event_channel_op_compat(int cmd, void *arg)
{
	struct evtchn_op op = { .cmd = cmd, };
	size_t len;
	int rc;

	if (cmd > ARRAY_SIZE(evtchnop_len))
		return -ENOSYS;

	len = evtchnop_len[cmd];
	memcpy(&op.u, arg, len);
	rc = _hypercall1(int, event_channel_op_compat, &op);
	memcpy(arg, &op.u, len);

	return rc;
}
EXPORT_SYMBOL_GPL(xen_event_channel_op_compat);

static const size_t physdevop_len[] = {
	[PHYSDEVOP_IRQ_UNMASK_NOTIFY] = 0,
	[PHYSDEVOP_irq_status_query]  = sizeof(struct physdev_irq_status_query),
	[PHYSDEVOP_set_iopl]	      = sizeof(struct physdev_set_iopl),
	[PHYSDEVOP_set_iobitmap]      = sizeof(struct physdev_set_iobitmap),
	[PHYSDEVOP_apic_read]	      = sizeof(struct physdev_apic),
	[PHYSDEVOP_apic_write]	      = sizeof(struct physdev_apic),
	[PHYSDEVOP_ASSIGN_VECTOR]     = sizeof(struct physdev_irq),
};

int xen_physdev_op_compat(int cmd, void *arg)
{
	struct physdev_op op = { .cmd = cmd, };
	size_t len;
	int rc;

	if (cmd > ARRAY_SIZE(physdevop_len))
		return -ENOSYS;

	len = physdevop_len[cmd];
	memcpy(&op.u, arg, len);
	rc = _hypercall1(int, physdev_op_compat, &op);
	memcpy(arg, &op.u, len);

	return rc;
}
EXPORT_SYMBOL_GPL(xen_physdev_op_compat);
