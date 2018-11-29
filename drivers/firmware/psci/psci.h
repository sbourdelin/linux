/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __PSCI_H
#define __PSCI_H

struct device_node;

bool psci_set_osi_mode(void);
u32 psci_get_domain_state(void);
void psci_set_domain_state(u32 state);
bool psci_has_osi_support(void);
int psci_dt_parse_state_node(struct device_node *np, u32 *state);

#ifdef CONFIG_CPU_IDLE
int psci_dt_init_pm_domains(struct device_node *np);
int psci_dt_pm_domains_parse_states(struct cpuidle_driver *drv,
			struct device_node *cpu_node, u32 *psci_states);
int psci_dt_attach_cpu(int cpu);
#else
static inline int psci_dt_init_pm_domains(struct device_node *np) { return 0; }
#endif

#endif /* __PSCI_H */
