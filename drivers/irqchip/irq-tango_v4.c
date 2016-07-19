/*
 * Copyright (C) 2014 Sebastian Frias <sf84@laposte.net>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>
#include <dt-bindings/interrupt-controller/irq-tango_v4.h>


#define DBGERR(__format, ...)	panic("[%s:%d] %s(): " __format,	\
				      __FILE__, __LINE__,		\
				      __func__, ##__VA_ARGS__)

#define DBGWARN(__format, ...)	pr_err("[%s:%d] %s(): " __format,	\
				       __FILE__, __LINE__,		\
				       __func__, ##__VA_ARGS__)


#if 0
#define DBGLOG(__format, ...)   pr_info("[%s:%d] %s(): " __format,	\
					__FILE__, __LINE__,		\
					__func__, ##__VA_ARGS__)
#else
#define DBGLOG(__format, ...) do {} while (0)
#endif


/*
 * HW description: IRQ router
 *
 * IMPORTANT NOTE: this hw block is not a "full" interrupt controller
 * - it does not support edge detection
 * - it does not latch the inputs (devices are expected to latch their
 * IRQ output by themselves)
 *
 *  ---
 *
 *  CPU block interrupt interface is now 32bits.
 *  The 24 first interrupt bits are generated from the system interrupts
 *  and the 8 msb interrupts are cpu local interrupts :
 *
 *  IRQs [23:0] tango system irqs.
 *  IRQs [27:24] CPU core cross trigger interface interrupt (1 per core).
 *  IRQs [31:28] CPU core PMU (performance unit) interrupt (1 per core).
 *
 *  The 24 lsb interrupts are generated through a new interrupt map module
 *  that maps the tango 128 interrupts to those 24 interrupts.
 *  For each of the 128 input system interrupt, one register is dedicated
 *  to program the destination interrupt among the 24 available.
 *  The mapper is configured as follows, starting at address (0x6f800) :
 *
 *  offset name            description
 *  0x000  irq_in_0_cfg    "en"=bit[31]; "inv"=bit[16]; "dest"=bits[4:0]
 *  0x004  irq_in_1_cfg    "en"=bit[31]; "inv"=bit[16]; "dest"=bits[4:0]
 *  .
 *  .
 *  .
 *  0x1FC  irq_in_127_cfg  "en"=bit[31]; "inv"=bit[16]; "dest"=bits[4:0]
 *  0x400  soft_irq_cfg    "enable"=bits[15:0]
 *  0x404  soft_irq_map0   "map3"=bits[28:24]; "map2"=bits[20:16];
 * "map1"=bits[12:8]; "map0"=bits[4:0]
 *  0x408  soft_irq_map1   "map7"=bits[28:24]; "map6"=bits[20:16];
 * "map5"=bits[12:8]; "map4"=bits[4:0]
 *  0x40C  soft_irq_map2   "map11"=bits[28:24]; "map10"=bits[20:16];
 * "map9"=bits[12:8]; "map8"=bits[4:0]
 *  0x410  soft_irq_map3   "map15"=bits[28:24]; "map14"=bits[20:16];
 * "map13"=bits[12:8]; "map12"=bits[4:0]
 *  0x414  soft_irq_set    "set"=bits[15:0]
 *  0x418  soft_irq_clear  "clear"=bits[15:0]
 *  0x41C  read_cpu_irq    "cpu_block_irq"=bits[23:0]
 *  0x420  read_sys_irq0   "system_irq"=bits[31:0]; (irqs: 0->31)
 *  0x424  read_sys_irq1   "system_irq"=bits[31:0]; (irqs: 32->63)
 *  0x428  read_sys_irq2   "system_irq"=bits[31:0]; (irqs: 64->95)
 *  0x42C  read_sys_irq3   "system_irq"=bits[31:0]; (irqs: 96->127)
 *
 *  - "irq_in_N_cfg"   : input N mapping :
 *     - "dest" bits[4:0]    => set destination interrupt among the 24
 *  output interrupts. (if multiple inputs are mapped to the same output,
 *  result is an OR of the inputs).
 *     - "inv" bit[16]       => if set, inverts input interrupt
 *  polarity (active at 0).
 *     - "en" bit[31]        => enable interrupt. Acts like a mask on the
 *  input interrupt.
 *  - "soft_irq"       : this module supports up to 16 software interrupts.
 *     - "enable" bits[15:0] => enable usage of software IRQs (SIRQ), 1 bit
 *  per SIRQ.
 *  - "soft_irq_mapN"  : For each of the 16 soft IRQ (SIRQ), map them in out
 *  IRQ[23:0] vector.
 *     - "mapN"              => 5 bits to select where to connect the SIRQ
 *  among the 23 bits output IRQ. (if multiple SIRQ are mapped to the same
 *  output IRQ, result is an OR of those signals).
 *  - "soft_irq_set"   : 16bits, write 1 bit at one set the corresponding
 *  SIRQ. Read returns the software SIRQ vector value.
 *  - "soft_irq_clear" : 16bits, write 1 bit at one clear the corresponding
 *  software SIRQ. Read returns the software SIRQ vector value.
 *  - "read_cpu_irq"   : 24bits, returns output IRQ value (IRQs connected to
 *  the ARM cluster).
 *  - "read_sys_irqN"  : 32bits, returns input system IRQ value before mapping.
 */

#define ROUTER_INPUTS  (128)
#define ROUTER_OUTPUTS (24)
#define SWIRQ_COUNT    (16)

#define IRQ_ROUTER_ENABLE_MASK (BIT(31))
#define IRQ_ROUTER_INVERT_MASK (BIT(16))

/* SW irqs */
#define SWIRQ_ENABLE      (0x400)
#define SWIRQ_MAP_GROUP0  (0x404)
#define SWIRQ_MAP_GROUP1  (0x408)
#define SWIRQ_MAP_GROUP2  (0x40C)
#define SWIRQ_MAP_GROUP3  (0x410)
#define READ_SWIRQ_STATUS (0x414)

#define READ_SYS_IRQ_GROUP0 (0x420)
#define READ_SYS_IRQ_GROUP1 (0x424)
#define READ_SYS_IRQ_GROUP2 (0x428)
#define READ_SYS_IRQ_GROUP3 (0x42C)


#if 0
#define SHORT_OR_FULL_NAME full_name
#else
#define SHORT_OR_FULL_NAME name
#endif

#define NODE_NAME(__node__) (__node__ ? __node__->SHORT_OR_FULL_NAME :	\
			     "<no-node>")

#define BITMASK_VECTOR_SIZE(__count__) (__count__ / 32)
#define IRQ_TO_OFFSET(__hwirq__) (__hwirq__ * 4)

struct tango_irqrouter;

/*
 * Maintains the mapping between a Linux virq and a hwirq
 * on the parent controller.
 * It is used by tango_irqdomain_map() or tango_irqdomain_hierarchy_alloc()
 * to setup the route between input IRQ and output IRQ
 */
struct tango_irqrouter_output {
	struct tango_irqrouter *context;

	u32 domain_id;

	u32 hwirq;
	u32 hwirq_level;
	u32 virq;

	int shared_count;
	int *shared_irqs;
};

/*
 * Context for the driver
 */
struct tango_irqrouter {
	raw_spinlock_t lock;
	struct device_node *node;
	void __iomem *base;

	int input_count;
	u32 irq_mask[BITMASK_VECTOR_SIZE(ROUTER_INPUTS)];
	u32 irq_invert_mask[BITMASK_VECTOR_SIZE(ROUTER_INPUTS)];

	int swirq_count;
	u32 swirq_mask;

	int irqgroup_count;
	int implicit_groups;

	int output_count;
	struct tango_irqrouter_output output[ROUTER_OUTPUTS];
};


/******************************************************************************/

/* Register access */
static inline u32 tango_readl(struct tango_irqrouter *irqrouter,
			      int reg);
static inline void tango_writel(struct tango_irqrouter *irqrouter,
				int reg,
				u32 val);
/* IRQ enable */
static inline void tango_set_swirq_enable(struct tango_irqrouter *irqrouter,
					  int swirq,
					  bool enable);
static inline void tango_set_hwirq_enable(struct tango_irqrouter *irqrouter,
					  int hwirq,
					  bool enable);
static inline int tango_set_irq_enable(struct tango_irqrouter *irqrouter,
				       int irq_in,
				       bool enable);
/* IRQ polarity */
static inline void tango_set_swirq_inversion(struct tango_irqrouter *irqrouter,
					     int swirq,
					     bool invert);
static inline void tango_set_hwirq_inversion(struct tango_irqrouter *irqrouter,
					     int hwirq,
					     bool invert);
static inline int tango_set_irq_inversion(struct tango_irqrouter *irqrouter,
					  int irq_in,
					  bool invert);
/* IRQ routing */
static inline void tango_set_swirq_route(struct tango_irqrouter *irqrouter,
					 int swirq_in,
					 int irq_out);
static inline void tango_set_hwirq_route(struct tango_irqrouter *irqrouter,
					 int hwirq_in,
					 int irq_out);
static inline int tango_set_irq_route(struct tango_irqrouter *irqrouter,
				      int irq_in,
				      int irq_out);
/* Misc */
static inline int tango_set_irq_type(struct tango_irqrouter *irqrouter,
				     int hwirq_in,
				     u32 type,
				     u32 parent_type);
static int tango_get_output_for_hwirq(struct tango_irqrouter *irqrouter,
				      int hwirq_in,
				      struct tango_irqrouter_output **out_val);
static inline int tango_parse_fwspec(struct irq_domain *domain,
				     struct irq_fwspec *fwspec,
				     u32 *domain_id_out,
				     irq_hw_number_t *irq_out,
				     u32 *type_out);


/* 'irqchip' handling callbacks
 * Used for 'shared' IRQs, i.e.: IRQs that share a GIC input
 * This driver performs the IRQ dispatch based on the flags
 */
static void tango_irqchip_mask_irq(struct irq_data *data);
static void tango_irqchip_unmask_irq(struct irq_data *data);
static int tango_irqchip_set_irq_type(struct irq_data *data,
				      unsigned int type);
#ifdef CONFIG_SMP
static int tango_irqchip_set_irq_affinity(struct irq_data *data,
					  const struct cpumask *mask_val,
					  bool force);
#endif
static inline u32 tango_dispatch_irqs(struct irq_domain *domain,
				      struct irq_desc *desc,
				      u32 status,
				      int base);
static void tango_irqdomain_handle_cascade_irq(struct irq_desc *desc);

static struct irq_chip tango_irq_chip_shared_ops = {
	.name			= "ROUTER_SHARED_IRQ_HANDLER",
	.irq_mask		= tango_irqchip_mask_irq,
	.irq_unmask		= tango_irqchip_unmask_irq,
	.irq_set_type		= tango_irqchip_set_irq_type,
#ifdef CONFIG_SMP
	.irq_set_affinity	= tango_irqchip_set_irq_affinity,
#endif
};

/* Shared IRQ domain callbacks */
static int tango_irqdomain_map(struct irq_domain *domain,
			       unsigned int virq,
			       irq_hw_number_t hwirq);
static int tango_irqdomain_translate(struct irq_domain *domain,
				     struct irq_fwspec *fwspec,
				     unsigned long *out_hwirq,
				     unsigned int *out_type);
static int tango_irqdomain_select(struct irq_domain *domain,
				  struct irq_fwspec *fwspec,
				  enum irq_domain_bus_token bus_token);

static struct irq_domain_ops tango_irqdomain_ops = {
	.select    = tango_irqdomain_select,
	.translate = tango_irqdomain_translate,
	.map	   = tango_irqdomain_map,
};


/* 'irqrouter' handling callbacks
 * Used for 'direct' IRQs, i.e.: IRQs that are directly routed to the GIC
 * This driver does not dispatch the IRQs, the GIC does.
 */
static void tango_irqrouter_mask_irq(struct irq_data *data);
static void tango_irqrouter_unmask_irq(struct irq_data *data);
static int tango_irqrouter_set_irq_type(struct irq_data *data,
					unsigned int type);

static struct irq_chip tango_irq_chip_direct_ops = {
	.name			= "ROUTER_DIRECT_IRQ_HANDLER",
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_mask		= tango_irqrouter_mask_irq,
	.irq_unmask		= tango_irqrouter_unmask_irq,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	.irq_set_type		= tango_irqrouter_set_irq_type,
	.flags			= IRQCHIP_MASK_ON_SUSPEND |
				  IRQCHIP_SKIP_SET_WAKE,
#ifdef CONFIG_SMP
	.irq_set_affinity	= irq_chip_set_affinity_parent,
#endif
};

/* Direct IRQ domain callbacks */
static int tango_irqdomain_hierarchy_alloc(struct irq_domain *domain,
					   unsigned int virq,
					   unsigned int nr_irqs,
					   void *data);
static void tango_irqdomain_hierarchy_free(struct irq_domain *domain,
					   unsigned int virq,
					   unsigned int nr_irqs);
static int tango_irqdomain_hierarchy_translate(struct irq_domain *domain,
					       struct irq_fwspec *fwspec,
					       unsigned long *out_hwirq,
					       unsigned int *out_type);

static int tango_irqdomain_hierarchy_select(struct irq_domain *domain,
					    struct irq_fwspec *fwspec,
					    enum irq_domain_bus_token bus_tok);

static const struct irq_domain_ops tango_irqdomain_hierarchy_ops = {
	.select    = tango_irqdomain_hierarchy_select,
	.translate = tango_irqdomain_hierarchy_translate,
	.alloc	   = tango_irqdomain_hierarchy_alloc,
	.free	   = tango_irqdomain_hierarchy_free,
};


/******************************************************************************/


static inline u32 tango_readl(struct tango_irqrouter *irqrouter,
			      int reg)
{
	u32 val = readl_relaxed(irqrouter->base + reg);
	/*DBGLOG("r[0x%08x + 0x%08x = 0x%08x] = 0x%08x\n",
	  irqrouter->base, reg, irqrouter->base + reg, val);*/
	return val;
}


static inline void tango_writel(struct tango_irqrouter *irqrouter,
				int reg,
				u32 val)
{
	/*DBGLOG("w[0x%08x + 0x%08x = 0x%08x] = 0x%08x\n",
	  irqrouter->base, reg, irqrouter->base + reg, val);*/
	writel_relaxed(val, irqrouter->base + reg);
}


static inline void tango_set_swirq_enable(struct tango_irqrouter *irqrouter,
					  int swirq,
					  bool enable)
{
	u32 offset = SWIRQ_ENABLE;
	u32 value = tango_readl(irqrouter, offset);
	u32 swirq_bit_index = swirq % SWIRQ_COUNT;

#if 1
	DBGLOG("%smask swirq(in) %d : current regvalue 0x%x\n",
	       enable ? "un":"",
	       swirq, value);
#endif

	if (enable) {
		/* unmask swirq */
		irqrouter->swirq_mask |= (1 << swirq_bit_index);
		value |= (1 << swirq_bit_index);
	} else {
		/* mask swirq */
		irqrouter->swirq_mask &= ~(1 << swirq_bit_index);
		value &= ~(1 << swirq_bit_index);
	}

	tango_writel(irqrouter, offset, value);
}


static inline void tango_set_hwirq_enable(struct tango_irqrouter *irqrouter,
					  int hwirq,
					  bool enable)
{
	u32 offset = IRQ_TO_OFFSET(hwirq);
	u32 value = tango_readl(irqrouter, offset);
	u32 hwirq_reg_index = hwirq / 32;
	u32 hwirq_bit_index = hwirq % 32;
	u32 *enable_mask = &(irqrouter->irq_mask[hwirq_reg_index]);

#if 1
	DBGLOG("%smask hwirq(in) %d : current regvalue 0x%x\n",
	       enable ? "un":"",
	       hwirq, value);
#endif

	if (enable) {
		/* unmask irq */
		*enable_mask |= (1 << hwirq_bit_index);
		value |= IRQ_ROUTER_ENABLE_MASK;
	} else {
		/* mask irq */
		*enable_mask &= ~(1 << hwirq_bit_index);
		value &= ~(IRQ_ROUTER_ENABLE_MASK);
	}

	tango_writel(irqrouter, offset, value);
}


static inline void tango_set_swirq_inversion(struct tango_irqrouter *irqrouter,
					     int swirq,
					     bool invert)
{

	DBGLOG("swirq(in) %d %s inverted\n", swirq, invert ? "":"not");

	if (invert)
		DBGERR("SW IRQs cannot be inverted!\n");
}


static inline void tango_set_hwirq_inversion(struct tango_irqrouter *irqrouter,
					     int hwirq,
					     bool invert)
{
	u32 offset = IRQ_TO_OFFSET(hwirq);
	u32 value = tango_readl(irqrouter, offset);
	u32 hwirq_reg_index = hwirq / 32;
	u32 hwirq_bit_index = hwirq % 32;
	u32 *invert_mask = &(irqrouter->irq_invert_mask[hwirq_reg_index]);

	if (invert) {
		*invert_mask |= (1 << hwirq_bit_index);
		value |= IRQ_ROUTER_INVERT_MASK;
	} else {
		*invert_mask &= ~(1 << hwirq_bit_index);
		value &= ~(IRQ_ROUTER_INVERT_MASK);
	}

	DBGLOG("hwirq(in) %d %s inverted\n", hwirq, invert ? "":"not");

	tango_writel(irqrouter, offset, value);
}


static inline void tango_set_swirq_route(struct tango_irqrouter *irqrouter,
					 int swirq_in,
					 int irq_out)
{
	u32 swirq_reg_index = swirq_in / 4;
	u32 swirq_bit_index = (swirq_in % 4) * 8;
	u32 mask = ~(0x1f << swirq_bit_index);
	u32 offset = SWIRQ_MAP_GROUP0 + (swirq_reg_index * 4);
	u32 value = tango_readl(irqrouter, offset);

	DBGLOG("ri %d, bi %d, mask 0x%x, offset 0x%x, val 0x%x\n",
	       swirq_reg_index,
	       swirq_bit_index,
	       mask,
	       offset,
	       value);

	DBGLOG("route swirq %d => hwirq(out) %d\n", swirq_in, irq_out);

	value &= mask;

	if (irq_out < 0) {
		tango_set_irq_enable(irqrouter,
				     swirq_in + irqrouter->input_count,
				     0);
	} else
		value |= ((irq_out & 0x1f) << swirq_bit_index);

	tango_writel(irqrouter, offset, value);
}


static inline void tango_set_hwirq_route(struct tango_irqrouter *irqrouter,
					 int irq_in,
					 int irq_out)
{
	u32 offset = IRQ_TO_OFFSET(irq_in);
	u32 value;

	DBGLOG("route hwirq(in) %d => hwirq(out) %d\n", irq_in, irq_out);

	if (irq_out < 0) {
		tango_set_irq_enable(irqrouter,
				     irq_in,
				     0);
		value = 0;
	} else
		value = (irq_out & 0x1f);

	tango_writel(irqrouter, offset, value);
}


static inline int tango_set_irq_enable(struct tango_irqrouter *irqrouter,
				       int irq,
				       bool enable)
{
	if (irq >= (irqrouter->input_count + irqrouter->swirq_count))
		return -EINVAL;
	else if (irq >= irqrouter->input_count)
		tango_set_swirq_enable(irqrouter,
				       irq - irqrouter->input_count,
				       enable);
	else
		tango_set_hwirq_enable(irqrouter,
				       irq,
				       enable);
	return 0;
}


static inline int tango_set_irq_inversion(struct tango_irqrouter *irqrouter,
					  int irq_in,
					  bool invert)
{
	if (irq_in >= (irqrouter->input_count + irqrouter->swirq_count))
		return -EINVAL;
	else if (irq_in >= irqrouter->input_count)
		tango_set_swirq_inversion(irqrouter,
					  irq_in - irqrouter->input_count,
					  invert);
	else
		tango_set_hwirq_inversion(irqrouter,
					  irq_in,
					  invert);
	return 0;
}


static inline int tango_set_irq_route(struct tango_irqrouter *irqrouter,
				      int irq_in,
				      int irq_out)
{
	if (irq_in >= (irqrouter->input_count + irqrouter->swirq_count))
		return -EINVAL;
	else if (irq_in >= irqrouter->input_count)
		tango_set_swirq_route(irqrouter,
				      irq_in - irqrouter->input_count,
				      irq_out);
	else
		tango_set_hwirq_route(irqrouter,
				      irq_in,
				      irq_out);
	return 0;
}


static int tango_set_irq_type(struct tango_irqrouter *irqrouter,
			      int hwirq_in,
			      u32 type,
			      u32 parent_type)
{
	int err;

	if (parent_type & (type & IRQ_TYPE_SENSE_MASK))
		/* same polarity */
		err = tango_set_irq_inversion(irqrouter, hwirq_in, 0);
	else
		/* invert polarity */
		err = tango_set_irq_inversion(irqrouter, hwirq_in, 1);

	if (err < 0) {
		DBGWARN("Failed to setup IRQ %d polarity\n", hwirq_in);
		return err;
	}

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		DBGERR("Does not support edge triggers\n");
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		break;
	case IRQ_TYPE_LEVEL_LOW:
		break;
	default:
		DBGWARN("Invalid trigger mode 0x%x for hwirq(in) %d\n",
			type, hwirq_in);
		return -EINVAL;
	}

	return 0;
}


static int tango_get_output_for_hwirq(struct tango_irqrouter *irqrouter,
				      int hwirq_in,
				      struct tango_irqrouter_output **out_val)
{
	struct tango_irqrouter_output *irqrouter_output;
	int i;

	if (!out_val)
		return -EINVAL;

	/* Get the irqrouter_output for the hwirq */
	for (i = 0; i < irqrouter->output_count; i++) {
		int j;

		irqrouter_output = &(irqrouter->output[i]);

		for (j = 0; j < irqrouter_output->shared_count; j++) {
			if (hwirq_in == irqrouter_output->shared_irqs[j])
				goto found_router_output;
		}
	}
	if (i == irqrouter->output_count) {
		DBGWARN("Couldn't find hwirq mapping\n");
		return -ENODEV;
	}

found_router_output:

	*out_val = irqrouter_output;
	return 0;
}

static inline int tango_parse_fwspec(struct irq_domain *domain,
				     struct irq_fwspec *fwspec,
				     u32 *domain_id_out,
				     irq_hw_number_t *irq_out,
				     u32 *type_out)
{
#if 0
	for (i = 0; i < fwspec->param_count; i++)
		DBGLOG("[%d] 0x%x\n", i, fwspec->param[i]);
#endif

	if (!is_of_node(fwspec->fwnode)) {
		DBGWARN("%s:%s(0x%p): Parameter mismatch\n",
			NODE_NAME(irq_domain_get_of_node(domain)),
			domain->name,
			domain);
		return -EINVAL;
	}

	if (fwspec->fwnode != domain->fwnode) {
		DBGLOG("Unknown domain/node\n");
		return -EINVAL;
	}

	if (fwspec->param_count != 3) {
		DBGWARN("We need 3 params\n");
		return -EINVAL;
	}

	if (domain_id_out)
		*domain_id_out = fwspec->param[0];
	if (irq_out)
		*irq_out       = fwspec->param[1];
	if (type_out)
		*type_out      = fwspec->param[2];

	return 0;
}


/* 'irqchip' handling callbacks
 * Used for 'shared' IRQs, i.e.: IRQs that share a GIC input
 * This driver performs the IRQ dispatch based on the flags
 */


static void tango_irqchip_mask_irq(struct irq_data *data)
{
	struct irq_domain *domain = irq_data_get_irq_chip_data(data);
	struct tango_irqrouter_output *irqrouter_output = domain->host_data;
	struct tango_irqrouter *irqrouter = irqrouter_output->context;
	int hwirq_in = (int)data->hwirq;

	tango_set_irq_enable(irqrouter, hwirq_in, 0);
}


static void tango_irqchip_unmask_irq(struct irq_data *data)
{
	struct irq_domain *domain = irq_data_get_irq_chip_data(data);
	struct tango_irqrouter_output *irqrouter_output = domain->host_data;
	struct tango_irqrouter *irqrouter = irqrouter_output->context;
	int hwirq_in = (int)data->hwirq;

	tango_set_irq_enable(irqrouter, hwirq_in, 1);
}


static int tango_irqchip_set_irq_type(struct irq_data *data,
				      unsigned int type)
{
	struct irq_domain *domain = irq_data_get_irq_chip_data(data);
	struct tango_irqrouter_output *irqrouter_output = domain->host_data;
	struct tango_irqrouter *irqrouter = irqrouter_output->context;
	int hwirq_in = (int)data->hwirq;
	u32 parent_type;

	DBGLOG("%s:%s(0x%p) type 0x%x for hwirq(in) %d = virq %d "
	       "(routed to hwirq(out) %d)\n",
	       NODE_NAME(irq_domain_get_of_node(domain)),
	       domain->name,
	       domain,
	       type, hwirq_in, data->irq,
	       irqrouter_output->hwirq);

	parent_type = (irqrouter_output->hwirq_level & IRQ_TYPE_SENSE_MASK);

	return tango_set_irq_type(irqrouter, hwirq_in, type, parent_type);
}


#ifdef CONFIG_SMP
static int tango_irqchip_set_irq_affinity(struct irq_data *data,
					  const struct cpumask *mask_val,
					  bool force)
{
	struct irq_domain *domain = irq_data_get_irq_chip_data(data);
	struct tango_irqrouter_output *irqrouter_output = domain->host_data;
	struct irq_chip *parent_chip = irq_get_chip(irqrouter_output->virq);
	struct irq_data *parent_data = irq_get_irq_data(irqrouter_output->virq);

	DBGLOG("%s:%s(0x%p)\n",
	       NODE_NAME(irq_domain_get_of_node(domain)), domain->name, domain);

	if (parent_chip && parent_chip->irq_set_affinity)
		return parent_chip->irq_set_affinity(parent_data,
						     mask_val,
						     force);
	else
		return -EINVAL;
}
#endif


static inline u32 tango_dispatch_irqs(struct irq_domain *domain,
				      struct irq_desc *desc,
				      u32 status,
				      int base)
{
	u32 hwirq;
	u32 virq;

	while (status) {
		hwirq = __ffs(status);
		virq = irq_find_mapping(domain, base + hwirq);
		if (unlikely(!virq))
			handle_bad_irq(desc);
		else
			generic_handle_irq(virq);

		status &= ~BIT(hwirq);
	}

	return status;
}


static void tango_irqdomain_handle_cascade_irq(struct irq_desc *desc)
{
	struct irq_domain *domain = irq_desc_get_handler_data(desc);
	struct tango_irqrouter_output *irqrouter_output = domain->host_data;
	struct tango_irqrouter *irqrouter = irqrouter_output->context;
	struct irq_chip *host_chip = irq_desc_get_chip(desc);
	u32 i, status;
	u32 swirq_status, irq_status[BITMASK_VECTOR_SIZE(ROUTER_INPUTS)];

#if 0
	DBGLOG("%s:%s(0x%p): irqrouter_output 0x%p, hwirq(out) %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)), domain->name, domain,
	       irqrouter_output, irqrouter_output->hwirq);
#endif

	chained_irq_enter(host_chip, desc);

	raw_spin_lock(&(irqrouter->lock));
	swirq_status = tango_readl(irqrouter, READ_SWIRQ_STATUS);
	for (i = 0; i < BITMASK_VECTOR_SIZE(ROUTER_INPUTS); i++)
		irq_status[i] = tango_readl(irqrouter,
					    READ_SYS_IRQ_GROUP0 + i*4);
	raw_spin_unlock(&(irqrouter->lock));

	/* HW irqs */
	for (i = 0; i < BITMASK_VECTOR_SIZE(ROUTER_INPUTS); i++) {
#if 0
		DBGLOG("%d: 0x%08x (en 0x%08x, inv 0x%08x)\n",
		       i,
		       irq_status[i],
		       irqrouter->irq_mask[0],
		       irqrouter->irq_invert_mask[0]);
#endif

#define HANDLE_INVERTED_LINES(__irqstatus__, __x__) ((((~__irqstatus__) & irqrouter->irq_invert_mask[__x__]) & irqrouter->irq_mask[__x__]) | __irqstatus__)
#define HANDLE_EN_AND_INV_MASKS(__irqstatus__, __y__) (HANDLE_INVERTED_LINES(__irqstatus__, __y__) & irqrouter->irq_mask[__y__])

		irq_status[i] = HANDLE_EN_AND_INV_MASKS(irq_status[i], i);
		status = tango_dispatch_irqs(domain, desc, irq_status[i], i*32);
		if (status & irq_status[i])
			DBGERR("%s: %d unhandled IRQs (as a mask) 0x%x\n",
			       NODE_NAME(irq_domain_get_of_node(domain)),
			       i,
			       status & irq_status[i]);
	}

	/* SW irqs */
	swirq_status &= irqrouter->swirq_mask;
	status = tango_dispatch_irqs(domain, desc, swirq_status, 128);
	if (status & swirq_status)
		DBGERR("%s: Unhandled IRQs (as a mask) 0x%x\n",
		       NODE_NAME(irq_domain_get_of_node(domain)),
		       status & swirq_status);

	chained_irq_exit(host_chip, desc);
}


/**
 * tango_irqdomain_map - route a hwirq(in) to a hwirq(out).
 * NOTE: The hwirq(out) must have been already allocated and enabled on
 * the parent controller.
 * @hwirq: HW IRQ of the device requesting an IRQ
 * if hwirq > inputs it is a SW IRQ
 * @virq: Linux IRQ (associated to the domain) to be given to the device
 * @domain: IRQ domain (from the domain, we get the irqrouter_output
 * in order to know to which output we need to route hwirq to)
 */
static int tango_irqdomain_map(struct irq_domain *domain,
			       unsigned int virq,
			       irq_hw_number_t hwirq)
{
	struct tango_irqrouter_output *irqrouter_output = domain->host_data;
	struct tango_irqrouter *irqrouter = irqrouter_output->context;

	DBGLOG("%s:%s(0x%p): hwirq(in) %d := virq %d, and route "
	       "hwirq(in) %d => hwirq(out) %d (virq %d)\n",
	       NODE_NAME(irq_domain_get_of_node(domain)),
	       domain->name,
	       domain,
	       (u32)hwirq,
	       virq,
	       (u32)hwirq,
	       irqrouter_output->hwirq,
	       irqrouter_output->virq);

	if (hwirq >= (irqrouter->input_count + irqrouter->swirq_count))
		DBGERR("%s: Invalid hwirq(in) %d >= %d + %d\n",
		       NODE_NAME(irq_domain_get_of_node(domain)),
		       (u32)hwirq,
		       irqrouter->input_count,
		       irqrouter->swirq_count);
	else if (hwirq >= irqrouter->input_count)
		DBGLOG("Map swirq %ld\n", hwirq - irqrouter->input_count);

	irq_set_chip_and_handler(virq,
				 &tango_irq_chip_shared_ops,
				 handle_level_irq);
	irq_set_chip_data(virq, domain);
	irq_set_probe(virq);

	tango_set_irq_route(irqrouter, hwirq, irqrouter_output->hwirq);

	return 0;
}


/**
 * tango_irqdomain_translate - used to select the domain for a given
 * irq_fwspec
 * @domain: a registered domain
 * @fwspec: an IRQ specification. This callback is used to translate the
 * parameters given as irq_fwspec into a HW IRQ and Type values.
 * @out_hwirq:
 * @out_type:
 */
static int tango_irqdomain_translate(struct irq_domain *domain,
				     struct irq_fwspec *fwspec,
				     unsigned long *out_hwirq,
				     unsigned int *out_type)
{
	struct tango_irqrouter_output *irqrouter_output = domain->host_data;
	struct tango_irqrouter *irqrouter = irqrouter_output->context;
	irq_hw_number_t irq;
	u32 domain_id, type;
	int err;

	DBGLOG("%s:%s(0x%p): argc %d for hwirq(out) %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)),
	       domain->name,
	       domain,
	       fwspec->param_count,
	       irqrouter_output->hwirq);

	err = tango_parse_fwspec(domain,
				 fwspec,
				 &domain_id,
				 &irq,
				 &type);
	if (err < 0) {
		DBGWARN("Failed to parse fwspec\n");
		return err;
	}

	switch (domain_id) {
	case SIGMA_HWIRQ:
		DBGLOG("Request is for SIGMA_HWIRQ\n");
		break;
	case SIGMA_SWIRQ:
		DBGLOG("Request is for SIGMA_SWIRQ\n");
		irq += irqrouter->input_count;
		break;
	default:
		DBGLOG("Request is for domain ID 0x%x (we are 0x%x)\n",
		       domain_id,
		       irqrouter_output->domain_id);
		break;
	};

	*out_hwirq = irq;
	*out_type  = type & IRQ_TYPE_SENSE_MASK;

	DBGLOG("hwirq %d type 0x%x\n", (u32)*out_hwirq, (u32)*out_type);

	return 0;
}


/**
 * tango_irqdomain_select - used to select the domain for a given irq_fwspec
 * @domain: a registered domain
 * @fwspec: an IRQ specification. This callback should return zero if the
 * irq_fwspec does not belong to the given domain. If it does, it should
 * return non-zero.
 *
 * In practice it will return non-zero if the irq_fwspec matches one of the
 * IRQs shared within the given domain.
 * @bus_token: a bus token
 */
static int tango_irqdomain_select(struct irq_domain *domain,
				  struct irq_fwspec *fwspec,
				  enum irq_domain_bus_token bus_token)
{
	struct tango_irqrouter_output *irqrouter_output = domain->host_data;
	struct tango_irqrouter *irqrouter = irqrouter_output->context;
	irq_hw_number_t irq;
	u32 domain_id, type;
	int err;

	DBGLOG("%s:%s(0x%p): argc %d, 0x%p, bus 0x%x\n",
	       NODE_NAME(irq_domain_get_of_node(domain)), domain->name, domain,
	       fwspec->param_count, fwspec->fwnode, bus_token);

	DBGLOG("router 0x%p, output 0x%p\n", irqrouter, irqrouter_output);

	err = tango_parse_fwspec(domain, fwspec, &domain_id, &irq, &type);
	if (err < 0)
		return 0;

	switch (domain_id) {
	case SIGMA_HWIRQ:
		DBGLOG("Request is for SIGMA_HWIRQ\n");
		break;
	case SIGMA_SWIRQ:
		DBGLOG("Request is for SIGMA_SWIRQ\n");
		break;
	default:
		DBGLOG("Request is for domain ID 0x%x (we are 0x%x)\n",
		       domain_id,
		       irqrouter_output->domain_id);
		break;
	};

	if (!irqrouter->implicit_groups) {
		int i;

		/* Check if the requested IRQ belongs to those listed
		 * to be sharing the output assigned to this domain
		 */
		if (irqrouter_output->shared_count <= 0) {
			DBGLOG("Not shared IRQ line?\n");
			return 0;
		}

		for (i = 0; i < irqrouter_output->shared_count; i++) {
			if (irq == irqrouter_output->shared_irqs[i]) {
				DBGLOG("Match: IRQ %lu\n", irq);
				return 1;
			}
		}
	} else {
		/* Otherwise, check if the domain_id given matches
		 * the one assigned to this output
		 */
		if (domain_id == irqrouter_output->domain_id) {
			DBGLOG("Match: Domain ID %d\n", domain_id);
			return 1;
		}
	}

	return 0;
}


/* 'irqrouter' handling callbacks
 * Used for 'direct' IRQs, i.e.: IRQs that are directly routed to the GIC
 * This driver does not dispatch the IRQs, the GIC does.
 */


static void tango_irqrouter_mask_irq(struct irq_data *data)
{
	struct irq_domain *domain = irq_data_get_irq_chip_data(data);
	struct tango_irqrouter *irqrouter = domain->host_data;
	int hwirq_in = (int)data->hwirq;

	DBGLOG("%s:%s(0x%p) hwirq(in) %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)),
	       domain->name,
	       domain,
	       hwirq_in);

	tango_set_irq_enable(irqrouter, hwirq_in, 0);

	irq_chip_mask_parent(data);
}


static void tango_irqrouter_unmask_irq(struct irq_data *data)
{
	struct irq_domain *domain = irq_data_get_irq_chip_data(data);
	struct tango_irqrouter *irqrouter = domain->host_data;
	int hwirq_in = (int)data->hwirq;

	DBGLOG("%s:%s(0x%p) hwirq(in) %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)),
	       domain->name,
	       domain,
	       hwirq_in);

	tango_set_irq_enable(irqrouter, hwirq_in, 1);

	irq_chip_unmask_parent(data);
}


static int tango_irqrouter_set_irq_type(struct irq_data *data,
					unsigned int type)
{
	struct irq_domain *domain = irq_data_get_irq_chip_data(data);
	struct tango_irqrouter_output *irqrouter_output = NULL;
	struct tango_irqrouter *irqrouter = domain->host_data;
	int hwirq_in = (int)data->hwirq;
	u32 parent_type;

	DBGLOG("%s:%s(0x%p) type 0x%x for hwirq(in) %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)),
	       domain->name,
	       domain,
	       type,
	       hwirq_in);

	tango_get_output_for_hwirq(irqrouter, hwirq_in, &irqrouter_output);
	if (!irqrouter_output)
		goto handle_parent;

	parent_type = (irqrouter_output->hwirq_level & IRQ_TYPE_SENSE_MASK);
	tango_set_irq_type(irqrouter, hwirq_in, type, parent_type);

handle_parent:
	return irq_chip_set_type_parent(data, type);
}


/**
 * tango_irqdomain_hierarchy_alloc - map/reserve a router<->GIC connection
 * @domain: IRQ domain.
 * @virq: Linux IRQ (associated to the domain) to be given to the device.
 * @nr_irqs: number of IRQs to reserve. MUST BE 1.
 * @data: (of type 'struct irq_fwspec *') the HW IRQ requested:
 * if in [0, input_count)
 *    => HW IRQ.
 * if in [input_count, input_count+swirq_count)
 *    => SW IRQ.
 * if in [input_count+swirq_count, input_count+swirq_count+irqgroup_count)
 *    => Fake HW IRQ.
 */
static int tango_irqdomain_hierarchy_alloc(struct irq_domain *domain,
					   unsigned int virq,
					   unsigned int nr_irqs,
					   void *data)
{
	struct tango_irqrouter *irqrouter = domain->host_data;
	struct irq_fwspec *fwspec = data;
	struct irq_fwspec fwspec_out;
	irq_hw_number_t hwirq_in, hwirq_out;
	u32 hwirq_type_in, hwirq_type_out;
	u32 domain_id_in;
	int i, err;

	DBGLOG("%s:%s(0x%p), parent %s:%s(0x%p): virq %d nr_irqs %d, argc %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)), domain->name, domain,
	       NODE_NAME(irq_domain_get_of_node(domain->parent)),
	       domain->parent->name, domain->parent,
	       virq, nr_irqs, fwspec->param_count);

	if (!irq_domain_get_of_node(domain->parent)) {
		DBGWARN("Invalid params\n");
		return -EINVAL;
	}

	if (nr_irqs != 1) {
		DBGWARN("IRQ ranges not handled\n");
		return -EINVAL;
	}

	/* Requested hwirq */
	err = tango_parse_fwspec(domain,
				 fwspec,
				 &domain_id_in,
				 &hwirq_in,
				 &hwirq_type_in);
	if (err < 0) {
		DBGWARN("Failed to parse fwspec\n");
		return err;
	}

	/* Only handle HW IRQ requests.
	 * SW IRQs are all shared and belong to another domain.
	 */
	switch (domain_id_in) {
	case SIGMA_HWIRQ:
		DBGLOG("Request is for SIGMA_HWIRQ\n");
		break;
	case SIGMA_SWIRQ:
		DBGLOG("Request is for SIGMA_SWIRQ\n");
	default:
		DBGWARN("Unhandled domain ID 0x%x\n", domain_id_in);
		return -EINVAL;
	};

	/* Find a route */
	raw_spin_lock(&(irqrouter->lock));
	for (i = irqrouter->output_count - 1; i >= 0; i--) {
		if (irqrouter->output[i].context == NULL)
			break;
	}
	raw_spin_unlock(&(irqrouter->lock));

	if (i < 0) {
		DBGWARN("No more IRQ output lines free\n");
		return -ENODEV;
	}

	/* Request our parent controller (the GIC) an IRQ line for the
	 * chosen route
	 */
	hwirq_out      = i;
	hwirq_type_out = IRQ_TYPE_LEVEL_HIGH;

	fwspec_out.fwnode = domain->parent->fwnode; /* should be the GIC */
	fwspec_out.param_count = 3;
	fwspec_out.param[0] = GIC_SPI;
	fwspec_out.param[1] = hwirq_out;
	fwspec_out.param[2] = hwirq_type_out;

	err = irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec_out);
	if (err) {
		DBGWARN("Failed to allocate irq on parent\n");
		return err;
	}

	/* Setup the route's output context */
	irqrouter->output[hwirq_out].context     = irqrouter;
	irqrouter->output[hwirq_out].hwirq       = hwirq_out;
	irqrouter->output[hwirq_out].hwirq_level = hwirq_type_out;
	irqrouter->output[hwirq_out].virq        = virq;

	if (hwirq_in >= irqrouter->input_count + irqrouter->swirq_count) {
		DBGLOG("Fake hwirq(in) %d for shared IRQ line hwirq(out) %d\n",
		       (int)hwirq_in, (int)hwirq_out);

		/* It is shared but we don't know yet how many IRQ
		 * lines share this output
		 */
		irqrouter->output[hwirq_out].shared_count = -1;
	} else {
		DBGLOG("hwirq(in) %d = virq %d routed to hwirq(out) %d\n",
		       (int)hwirq_in, virq, (int)hwirq_out);

		tango_set_irq_route(irqrouter, hwirq_in, hwirq_out);
		/* Not shared */
		irqrouter->output[hwirq_out].shared_count = 0;
	}

	/* Setup the handler ops for this IRQ line (virq)
	 * Since the IRQ line is allocated and handled by the GIC,
	 * most ops are generic, although we do need to intercept
	 * a few of them.
	 */
	irq_domain_set_hwirq_and_chip(domain,
				      virq,
				      hwirq_in,
				      &tango_irq_chip_direct_ops,
				      domain);

	return 0;
}


/**
 * tango_irqdomain_hierarchy_free - map/reserve a router<->GIC connection
 * @domain: domain of irq to unmap
 * @virq: virq number
 * @nr_irqs: number of irqs to free. MUST BE 1.
 */
static void tango_irqdomain_hierarchy_free(struct irq_domain *domain,
					   unsigned int virq,
					   unsigned int nr_irqs)
{
	struct tango_irqrouter *irqrouter = domain->host_data;
	struct irq_data *irqdata;

	DBGLOG("%s:%s(0x%p): virq %d nr_irqs %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)), domain->name, domain,
	       virq, nr_irqs);

	if (nr_irqs != 1) {
		DBGERR("IRQ ranges not handled\n");
		return;
	}

	raw_spin_lock(&(irqrouter->lock));

	irqdata = irq_domain_get_irq_data(domain, virq);
	if (irqdata) {
		DBGLOG("Freeing virq %d: was routed to hwirq(out) %d\n",
		       (int)virq,
		       (int)irqdata->hwirq);

		tango_set_irq_route(irqrouter, 0x0, irqdata->hwirq);
		irqrouter->output[irqdata->hwirq].context = NULL;
		irq_domain_reset_irq_data(irqdata);
	} else
		DBGERR("Failed to get irq_data for virq %d\n", virq);

	raw_spin_unlock(&(irqrouter->lock));
}


/**
 * tango_irqdomain_hierarchy_translate - used to select the domain for a given
 * irq_fwspec
 * @domain: a registered domain
 * @fwspec: an IRQ specification. This callback is used to translate the
 * parameters given as irq_fwspec into a HW IRQ and Type values.
 * @out_hwirq:
 * @out_type:
 */
static int tango_irqdomain_hierarchy_translate(struct irq_domain *domain,
					       struct irq_fwspec *fwspec,
					       unsigned long *out_hwirq,
					       unsigned int *out_type)
{
	irq_hw_number_t irq;
	u32 domain_id, type;
	int err;

	DBGLOG("%s:%s(0x%p): argc %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)),
	       domain->name,
	       domain,
	       fwspec->param_count);

	err = tango_parse_fwspec(domain,
				 fwspec,
				 &domain_id,
				 &irq,
				 &type);
	if (err < 0)
		return err;

	switch (domain_id) {
	case SIGMA_HWIRQ:
		DBGLOG("Request is for SIGMA_HWIRQ\n");
		break;
	case SIGMA_SWIRQ:
		DBGLOG("Request is for SIGMA_SWIRQ\n");
	default:
		DBGWARN("Request is for domain ID 0x%x\n",
			domain_id);
		break;
	};

	*out_hwirq = irq;
	*out_type  = type & IRQ_TYPE_SENSE_MASK;

	DBGLOG("hwirq %d type 0x%x\n", (u32)*out_hwirq, (u32)*out_type);

	return 0;
}


/**
 * tango_irqdomain_hierarchy_select - used to select the domain for a given
 * irq_fwspec
 * @domain: a registered domain
 * @fwspec: an IRQ specification. This callback should return zero if the
 * irq_fwspec does not belong to the given domain. If it does, it should
 * return non-zero.
 * @bus_token: a bus token
 */
static int tango_irqdomain_hierarchy_select(struct irq_domain *domain,
					    struct irq_fwspec *fwspec,
					    enum irq_domain_bus_token bus_token)
{
	struct tango_irqrouter *irqrouter = domain->host_data;
	irq_hw_number_t irq;
	u32 domain_id, type;
	int err;

	DBGLOG("%s:%s(0x%p): argc %d, 0x%p, bus 0x%x\n",
	       NODE_NAME(irq_domain_get_of_node(domain)), domain->name, domain,
	       fwspec->param_count, fwspec->fwnode, bus_token);

	DBGLOG("router 0x%p\n", irqrouter);

	err = tango_parse_fwspec(domain,
				 fwspec,
				 &domain_id,
				 &irq,
				 &type);
	if (err < 0)
		return 0;

	/* Only handle HW IRQ requests.
	 * SW IRQs are all shared and belong to another domain.
	 */
	switch (domain_id) {
	case SIGMA_HWIRQ:
		DBGLOG("Request is for SIGMA_HWIRQ\n");
		break;
	case SIGMA_SWIRQ:
		DBGLOG("Request is for SIGMA_SWIRQ\n");
	default:
		DBGWARN("Unhandled domain ID 0x%x\n", domain_id);
		return 0;
	};

	if (memcmp(fwspec->fwnode,
		   &(irqrouter->node->fwnode),
		   sizeof(struct fwnode_handle)) == 0) {
		DBGLOG("Match: fwnode\n");
		return 1;
	}

	return 0;
}


static int __init tango_irq_init_domain(struct tango_irqrouter *irqrouter,
					u32 index,
					u32 domain_id,
					struct device_node *parent,
					struct device_node *node)
{
	struct irq_domain *domain;
	struct irq_fwspec fwspec_irq;
	u32 virq, hwirq, hwirq_type, i;
	u32 total_irqs;
	u32 entry_size;
	const __be32 *irqgroup;

	if (index >= irqrouter->irqgroup_count) {
		DBGWARN("%s: Group count mismatch\n", node->name);
		return -EINVAL;
	}

	if (!parent) {
		DBGWARN("%s: Invalid parent\n", node->name);
		return -EINVAL;
	}

	/* The number of IRQs could be dependent on the domain_id but
	 * would require more code and could make it difficult to handle
	 * implicit and explicit domains
	 */
	total_irqs = irqrouter->input_count + irqrouter->swirq_count;

	switch (domain_id) {
	case SIGMA_HWIRQ:
	case SIGMA_SWIRQ:
		break;
	default:
		if (!irqrouter->implicit_groups) {
			DBGWARN("%s: Unhandled domain ID 0x%x\n",
				node->name,
				domain_id);
			return -EINVAL;
		}

		DBGLOG("%s: Domain ID 0x%x\n", node->name, domain_id);
		break;
	};

	/* To request a virq we need a HW IRQ, use a "Fake HW IRQ" */
	hwirq      = index + irqrouter->input_count + irqrouter->swirq_count;
	hwirq_type = IRQ_TYPE_LEVEL_HIGH;

	fwspec_irq.fwnode      = &(parent->fwnode);
	fwspec_irq.param_count = 3;
	fwspec_irq.param[0]    = SIGMA_HWIRQ;
	fwspec_irq.param[1]    = hwirq;
	fwspec_irq.param[2]    = hwirq_type;

	/* Request a virq for the hwirq */
	virq = irq_create_fwspec_mapping(&fwspec_irq);
	if (virq <= 0) {
		DBGWARN("%s: failed to get virq for hwirq(out) %d",
			node->name, hwirq);
		return -ENODEV;
	}

	/* Get the irqrouter_output for the virq */
	for (i = 0; i < irqrouter->output_count; i++) {
		if (virq == irqrouter->output[i].virq)
			break;
	}
	if (i == irqrouter->output_count) {
		DBGWARN("%s: Couldn't find virq<=>hwirq(out) mapping\n",
			node->name);
		return -ENODEV;
	}

	index = i;

	irqrouter->output[index].domain_id = domain_id;

	/* Create a domain for this virq */
	domain = irq_domain_add_linear(parent,
				       total_irqs,
				       &tango_irqdomain_ops,
				       &(irqrouter->output[index]));
	if (!domain) {
		DBGERR("%s: Failed to create irqdomain", node->name);
		return -EINVAL;
	}

	domain->name = kasprintf(GFP_KERNEL,
				 "irqdomain%d@hwirq_out=%d",
				 index,
				 irqrouter->output[index].hwirq);

	DBGLOG("%s:%s(0x%p) [%d], id 0x%x, %d irqs, irqrouter_output 0x%p : "
	       "hwirq(out) %d = virq %d\n",
	       NODE_NAME(irq_domain_get_of_node(domain)),
	       domain->name,
	       domain,
	       index,
	       domain_id,
	       total_irqs,
	       &(irqrouter->output[index]),
	       irqrouter->output[index].hwirq,
	       virq);

	/* Populate list of shared IRQs */

	if (domain_id == SIGMA_SWIRQ) {
		irqrouter->output[index].shared_irqs = kcalloc(total_irqs,
							       sizeof(int),
							       GFP_KERNEL);
		if (!irqrouter->output[index].shared_irqs) {
			DBGERR("%s: Failed to allocate memory for group\n",
			       node->name);
			return -ENOMEM;
		}
		irqrouter->output[index].shared_count = total_irqs;

		for (i = 0; i < total_irqs; i++)
			irqrouter->output[index].shared_irqs[i] = i;
	}

	irqgroup = of_get_property(node, "shared-irqs", &entry_size);
	if (irqgroup) {
		int entry;

		entry_size /= sizeof(__be32);

		irqrouter->output[index].shared_irqs = kcalloc(entry_size,
							       sizeof(int),
							       GFP_KERNEL);
		if (!irqrouter->output[index].shared_irqs) {
			DBGERR("%s: Failed to allocate memory for group\n",
			       node->name);
			return -ENOMEM;
		}

		irqrouter->output[index].shared_count = entry_size;

		for (i = 0; i < entry_size; i++) {
			of_property_read_u32_index(node,
						   "shared-irqs",
						   i,
						   &entry);

			irqrouter->output[index].shared_irqs[i] = entry;

			DBGLOG("%s:%s(0x%p) irq %d sharing hwirq(out) %d\n",
			       NODE_NAME(irq_domain_get_of_node(domain)),
			       domain->name,
			       domain,
			       entry,
			       irqrouter->output[index].hwirq);
		}
	}


	/* Associate the domain with the virq */
	irq_set_chained_handler_and_data(virq,
					 tango_irqdomain_handle_cascade_irq,
					 domain);

	return 0;
}


static int __init tango_of_irq_init(struct device_node *node,
				    struct device_node *parent)
{
	struct irq_domain *parent_domain, *domain;
	struct tango_irqrouter *irqrouter;
	struct device_node *child;
	void __iomem *base;
	u32 i, total_irqs;
	int input_count, swirq_count, output_count;
	int irqgroup_count;
	int implicit_groups = 0;

	if (!parent) {
		DBGERR("%s: Missing parent\n", node->full_name);
		return -ENODEV;
	}

	parent_domain = irq_find_host(parent);
	if (!parent_domain) {
		DBGERR("%s: Cannot get parent domain\n", node->full_name);
		return -ENXIO;
	}

	base = of_iomap(node, 0);
	if (!base) {
		DBGERR("%s: Failed to map registers\n", node->name);
		return -ENXIO;
	}

	if (of_property_read_u32(node, "inputs", &input_count)) {
		DBGWARN("%s: Missing 'inputs' property\n", node->name);
		return -EINVAL;
	}

	if (of_property_read_u32(node, "swirq-count", &swirq_count)) {
		DBGWARN("%s: Missing 'swirq-count' property\n", node->name);
		return -EINVAL;
	}

	if (of_property_read_u32(node, "outputs", &output_count)) {
		DBGWARN("%s: Missing 'outputs' property\n", node->name);
		return -EINVAL;
	}

	if ((input_count != ROUTER_INPUTS)
	    || (swirq_count != SWIRQ_COUNT)
	    || (output_count != ROUTER_OUTPUTS)) {
		DBGERR("%s: input/swirq/output count mismatch\n", node->name);
		return -EINVAL;
	}

	/* Check IRQ group mode */
	if (of_property_read_u32(node, "irq-groups", &irqgroup_count)) {
		DBGLOG("%s: Using explicit IRQ group definition\n", node->name);

		/* count IRQ groups */
		irqgroup_count = 0;
		for_each_child_of_node(node, child)
		irqgroup_count++;

		implicit_groups = 0;
	} else {
		DBGLOG("%s: Using implicit IRQ group definition\n", node->name);
		implicit_groups = irqgroup_count;
	}

	/* SW IRQs are always grouped together */
	if (swirq_count)
		irqgroup_count++;

	if (irqgroup_count > output_count) {
		DBGERR("%s: Too many IRQ groups %d > %d outputs\n",
		       node->name, irqgroup_count, output_count);
		return -EINVAL;
	}

	/* Create the context */
	irqrouter = kzalloc(sizeof(*irqrouter), GFP_KERNEL);
	raw_spin_lock_init(&(irqrouter->lock));
	irqrouter->node = node;
	irqrouter->base = base;
	irqrouter->input_count = input_count;
	irqrouter->swirq_count = swirq_count;
	irqrouter->irqgroup_count = irqgroup_count;
	irqrouter->output_count = output_count;
	irqrouter->implicit_groups = implicit_groups;

	/* We probably don't need to add up swirq_count since SW irqs
	 * have are always muxed together
	 */
	total_irqs = input_count + swirq_count + irqgroup_count;

	domain = irq_domain_add_hierarchy(parent_domain,
					  0,
					  total_irqs,
					  node,
					  &tango_irqdomain_hierarchy_ops,
					  irqrouter);
	if (!domain) {
		DBGERR("%s: Failed to allocate domain hierarchy\n", node->name);
		iounmap(irqrouter->base);
		kfree(irqrouter);
		return -ENOMEM;
	}

	domain->name = node->full_name;

	DBGWARN("%s:%s(0x%p) base 0x%p, %d (+ %d swirq) and %d %s IRQ groups "
		"=> %d router 0x%p, parent %s\n",
		NODE_NAME(irq_domain_get_of_node(domain)),
		domain->name,
		domain,
		base,
		input_count, swirq_count, irqgroup_count,
		implicit_groups ? "implicit" : "explicit",
		output_count,
		irqrouter,
		parent->full_name);

	/* Allocate domains for shared IRQs */

	if (irqrouter->swirq_count) {
		int err;

		/* All SW IRQs are muxed together */
		err = tango_irq_init_domain(irqrouter,
					    0,
					    SIGMA_SWIRQ,
					    node,
					    node);
		if (err < 0) {
			DBGERR("%s: Failed to init SWIRQ domain\n",
			       node->name);
		}
	}

	if (irqrouter->implicit_groups > 0) {
		int err;

		/* NOTE that i starts at 1 because index 0 is reserved
		 * for SW IRQs.
		 */
		i = 1;
		for (; i < irqrouter->implicit_groups; i++) {
			err = tango_irq_init_domain(irqrouter,
						    i,
						    SIGMA_IRQGROUP_KEY + i,
						    node,
						    node);
			if (err < 0) {
				DBGERR("%s: Failed to init domain %d\n",
				       node->name, i);
			}
		}
	} else {
		int err;

		/* NOTE that i starts at 1 because index 0 is reserved
		 * for SW IRQs.
		 */
		i = 1;
		for_each_child_of_node(node, child) {
			err = tango_irq_init_domain(irqrouter,
						    i,
						    SIGMA_HWIRQ,
						    node,
						    child);
			if (err < 0) {
				DBGERR("%s: Failed to init domain %d\n",
				       node->name, i);
			}

			i++;
		}
	}

	/* HW IRQs: clear routing and disable them */
	for (i = 0; i < irqrouter->input_count; i++)
		tango_set_irq_route(irqrouter,
				    i,
				    -1);

	/* SW IRQs: clear routing and disable them */
	for (i = 0; i < irqrouter->swirq_count; i++)
		tango_set_irq_route(irqrouter,
				    irqrouter->input_count + i,
				    -1);

	return 0;
}


IRQCHIP_DECLARE(tango_irqrouter, "sigma,smp,irqrouter", tango_of_irq_init);
