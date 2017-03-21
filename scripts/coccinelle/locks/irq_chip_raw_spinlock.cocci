// Copyright: (C) 2017 National Instruments Corp. GPLv2.
// Author: Julia Cartwright <julia@ni.com>
//
// Identify callers of non-raw spinlock_t functions in hardirq irq_chip
// callbacks.
//
// spin_lock{_irq,_irqsave}(), w/ PREEMPT_RT are "sleeping" spinlocks, and so
// therefore "sleep" under contention; identify (and potentially patch) callers
// to use raw_spinlock_t instead.
//
// Confidence: Moderate

virtual report
virtual patch

@match@
identifier __irqchip;
identifier __irq_mask;
@@
	static struct irq_chip __irqchip = {
		.irq_mask	= __irq_mask,
	};

@match2 depends on match exists@
identifier match.__irq_mask;
identifier data;
identifier x;
identifier l;
type T;
position j0;
expression flags;
@@
	static void __irq_mask(struct irq_data *data)
	{
		... when any
		T *x;
		... when any
(
		spin_lock_irqsave(&x->l@j0, flags);
|
		spin_lock_irq(&x->l@j0);
|
		spin_lock(&x->l@j0);
)
		... when any
	}

@match3 depends on match2 && patch@
type match2.T;
identifier match2.l;
@@
	T {
		...
-		spinlock_t	l;
+		raw_spinlock_t	l;
		...
	};

@match4 depends on match2 && patch@
type match2.T;
identifier match2.l;
expression flags;
T *x;
@@

(
-spin_lock(&x->l)
+raw_spin_lock(&x->l)
|
-spin_lock_irqsave(&x->l, flags)
+raw_spin_lock_irqsave(&x->l, flags)
|
-spin_lock_irq(&x->l)
+raw_spin_lock_irq(&x->l)
|
-spin_unlock(&x->l)
+raw_spin_unlock(&x->l)
|
-spin_unlock_irq(&x->l)
+raw_spin_unlock_irq(&x->l)
|
-spin_unlock_irqrestore(&x->l, flags)
+raw_spin_unlock_irqrestore(&x->l, flags)
|
-spin_lock_init(&x->l)
+raw_spin_lock_init(&x->l)
)

@script:python wat depends on match2 && report@
j0 << match2.j0;
t << match2.T;
l << match2.l;
@@

msg = "Use of non-raw spinlock is illegal in this context (%s::%s)" % (t, l)
coccilib.report.print_report(j0[0], msg)
