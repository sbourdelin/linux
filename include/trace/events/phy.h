#undef TRACE_SYSTEM
#define TRACE_SYSTEM phy

#if !defined(_TRACE_PHY_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PHY_H

#include <linux/tracepoint.h>

#define PHY_STATE_ENUMS \
        EM( DOWN )      \
        EM( STARTING )  \
        EM( READY )     \
        EM( PENDING )   \
        EM( UP )        \
        EM( AN )        \
        EM( RUNNING )   \
        EM( NOLINK )    \
        EM( FORCING )   \
        EM( CHANGELINK )\
        EM( HALTED )    \
        EMe(RESUMING)

#undef EM
#undef EMe

#define EM(a) TRACE_DEFINE_ENUM( PHY_##a );
#define EMe(a) TRACE_DEFINE_ENUM( PHY_##a );

PHY_STATE_ENUMS

#undef EM
#undef EMe

#define EM(a) { PHY_##a, #a },
#define EMe(a) { PHY_##a, #a }

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
	    TP_printk("phy-%d-irq irq=%d ifname=%-.16s state=%s",
		      __entry->addr,
		      __entry->irq,
		      __entry->ifname,
		      __print_symbolic(__entry->state, PHY_STATE_ENUMS)
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
	    TP_printk("phy-%d-change ifname=%-.16s old_state=%s state=%s",
		      __entry->addr,
		      __entry->ifname,
		      __print_symbolic(__entry->old_state, PHY_STATE_ENUMS),
		      __print_symbolic(__entry->state, PHY_STATE_ENUMS)
		    )
	);

#undef EM
#undef EMe
#undef PHY_STATE_ENUMS

#endif /* _TRACE_PHY_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
