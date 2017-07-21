#ifndef _CS_CHANGES_H
#define _CS_CHANGES_H

#include <linux/notifier.h>

/*
 * The clocksource changes notifier is called when the system
 * clocksource is changed or some properties of the current
 * system clocksource is changed that can affect other parts of the system,
 * for example KVM guests
 */

extern void clocksource_changes_notify(void);
extern int clocksource_changes_register_notifier(struct notifier_block *nb);
extern int clocksource_changes_unregister_notifier(struct notifier_block *nb);

#endif /* _CS_CHANGES_H */
