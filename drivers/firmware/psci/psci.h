/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __PSCI_H
#define __PSCI_H

struct device_node;

u32 psci_get_domain_state(void);
void psci_set_domain_state(u32 state);

int psci_dt_parse_state_node(struct device_node *np, u32 *state);

#ifdef CONFIG_PM_GENERIC_DOMAINS_OF
int psci_dt_init_pm_domains(struct device_node *np);
#else
static inline int psci_dt_init_pm_domains(struct device_node *np) { return 0; }
#endif

#endif /* __PSCI_H */
