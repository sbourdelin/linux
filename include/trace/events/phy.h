#undef TRACE_SYSTEM
#define TRACE_SYSTEM phy

#if !defined(_TRACE_PHY_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PHY_H

#include <linux/tracepoint.h>

TRACE_EVENT(phy_interrupt,
	    TP_PROTO(int irq, struct phy_device *phydev),
	    TP_ARGS(irq, phydev),
	    TP_STRUCT__entry(
		    __field(int, irq)
		    __field(int, addr)
		    __field(int, state)
		    __array(char, ifname, IFNAMSIZ)
		    ),
	    TP_fast_assign(
		    __entry->irq = irq;
		    __entry->addr = phydev->mdio.addr;
		    __entry->state = phydev->state;
		    if (phydev->attached_dev)
			    memcpy(__entry->ifname,
				   netdev_name(phydev->attached_dev),
				   IFNAMSIZ);
		    else
			    memset(__entry->ifname, 0, IFNAMSIZ);
		    ),
	    TP_printk("phy-%d-irq irq=%d ifname=%16s state=%d",
		      __entry->addr,
		      __entry->irq,
		      __entry->ifname,
		      __entry->state
		    )
	);

TRACE_EVENT(phy_state_change,
	    TP_PROTO(struct phy_device *phydev, enum phy_state old_state),
	    TP_ARGS(phydev, old_state),
	    TP_STRUCT__entry(
		    __field(int, addr)
		    __field(int, state)
		    __field(int, old_state)
		    __array(char, ifname, IFNAMSIZ)
		    ),
	    TP_fast_assign(
		    __entry->addr = phydev->mdio.addr;
		    __entry->state = phydev->state;
		    __entry->old_state = old_state;
		    if (phydev->attached_dev)
			    memcpy(__entry->ifname,
				   netdev_name(phydev->attached_dev),
				   IFNAMSIZ);
		    else
			    memset(__entry->ifname, 0, IFNAMSIZ);
		    ),
	    TP_printk("phy-%d-change ifname=%16s old_state=%d state=%d",
		      __entry->addr,
		      __entry->ifname,
		      __entry->old_state,
		      __entry->state
		    )
	);

#endif /* _TRACE_PHY_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
