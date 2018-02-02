#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/bug.h>
#include <linux/export.h>
#include <asm/hypervisor.h>
#include <asm/xen/hypercall.h>

int xen_event_channel_op_compat(int cmd, void *arg)
{
	struct evtchn_op op = { .cmd = cmd, };
	size_t len;
	int rc;

	switch (cmd) {
	case EVTCHNOP_bind_interdomain:
		len = sizeof(struct evtchn_bind_interdomain);
		break;
	case EVTCHNOP_bind_virq:
		len = sizeof(struct evtchn_bind_virq);
		break;
	case EVTCHNOP_bind_pirq:
		len = sizeof(struct evtchn_bind_pirq);
		break;
	case EVTCHNOP_close:
		len = sizeof(struct evtchn_close);
		break;
	case EVTCHNOP_send:
		len = sizeof(struct evtchn_send);
		break;
	case EVTCHNOP_alloc_unbound:
		len = sizeof(struct evtchn_alloc_unbound);
		break;
	case EVTCHNOP_bind_ipi:
		len = sizeof(struct evtchn_bind_ipi);
		break;
	case EVTCHNOP_status:
		len = sizeof(struct evtchn_status);
		break;
	case EVTCHNOP_bind_vcpu:
		len = sizeof(struct evtchn_bind_vcpu);
		break;
	case EVTCHNOP_unmask:
		len = sizeof(struct evtchn_unmask);
		break;
	default:
		return -ENOSYS;
	}

	memcpy(&op.u, arg, len);
	rc = _hypercall1(int, event_channel_op_compat, &op);
	memcpy(arg, &op.u, len);

	return rc;
}
EXPORT_SYMBOL_GPL(xen_event_channel_op_compat);

int xen_physdev_op_compat(int cmd, void *arg)
{
	struct physdev_op op = { .cmd = cmd, };
	size_t len;
	int rc;

	switch (cmd) {
	case PHYSDEVOP_IRQ_UNMASK_NOTIFY:
		len = 0;
		break;
	case PHYSDEVOP_irq_status_query:
		len = sizeof(struct physdev_irq_status_query);
		break;
	case PHYSDEVOP_set_iopl:
		len = sizeof(struct physdev_set_iopl);
		break;
	case PHYSDEVOP_set_iobitmap:
		len = sizeof(struct physdev_set_iobitmap);
		break;
	case PHYSDEVOP_apic_read:
	case PHYSDEVOP_apic_write:
		len = sizeof(struct physdev_apic);
		break;
	case PHYSDEVOP_ASSIGN_VECTOR:
		len = sizeof(struct physdev_irq);
		break;
	default:
		return -ENOSYS;
	}

	memcpy(&op.u, arg, len);
	rc = _hypercall1(int, physdev_op_compat, &op);
	memcpy(arg, &op.u, len);

	return rc;
}
EXPORT_SYMBOL_GPL(xen_physdev_op_compat);
