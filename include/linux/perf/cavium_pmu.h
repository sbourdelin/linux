#ifndef _CAVIUM_PMU_H
#define _CAVIUM_PMU_H

#include <linux/io.h>
#include <linux/pci.h>

enum cvm_pmu_type {
	CVM_PMU_LMC,
};

#ifdef CONFIG_CAVIUM_PMU

#include <linux/cpumask.h>
#include <linux/list.h>
#include <linux/perf_event.h>

/* maximum number of parallel hardware counters for all pmu types */
#define CVM_PMU_MAX_COUNTERS 64

/* generic struct to cover the different pmu types */
struct cvm_pmu_dev {
	struct pmu pmu;
	const char *pmu_name;
	bool (*event_valid)(u64);
	void __iomem *map;
	struct pci_dev *pdev;
	int num_counters;
	struct perf_event *events[CVM_PMU_MAX_COUNTERS];
	struct list_head entry;
	struct hlist_node cpuhp_node;
	cpumask_t active_mask;
};


/* PMU interface used by EDAC driver */
void *cvm_pmu_probe(struct pci_dev *pdev, void __iomem *regs, int type);
void cvm_pmu_remove(struct pci_dev *pdev, void *pmu_data, int type);

#else
static inline void *cvm_pmu_probe(struct pci_dev *pdev, void __iomem *regs,
				  int type)
{
	return NULL;
}

static inline void cvm_pmu_remove(struct pci_dev *pdev, void *pmu_data,
				  int type)
{
}

#endif

#endif
