#include <linux/types.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/io.h>
#include <linux/perf_event.h>
#include <linux/pci.h>

#include <asm/cpufeature.h>
#include <asm/cputype.h>

#undef pr_fmt
#define pr_fmt(fmt)     "thunderx_uncore: " fmt

enum uncore_type {
	L2C_TAD_TYPE,
	L2C_CBC_TYPE,
	LMC_TYPE,
	OCX_LNE_TYPE,
};

extern int thunder_uncore_version;

#define MAX_NR_UNCORE_PDEVS		16

/* maximum number of parallel hardware counters for all uncore parts */
#define MAX_COUNTERS			64

/* generic uncore struct for different pmu types */
struct thunder_uncore {
	int num_counters;
	int nr_units;
	int type;
	struct pmu *pmu;
	int (*event_valid)(u64);
	struct {
		unsigned long base;
		void __iomem *map;
		struct pci_dev *pdev;
	} pdevs[MAX_NR_UNCORE_PDEVS];
	struct perf_event *events[MAX_COUNTERS];
};

#define EVENT_PTR(_id) (&event_attr_##_id.attr.attr)

#define EVENT_ATTR(_name, _val)						   \
static struct perf_pmu_events_attr event_attr_##_name = {		   \
	.attr	   = __ATTR(_name, 0444, thunder_events_sysfs_show, NULL), \
	.event_str = "event=" __stringify(_val),			   \
};

#define EVENT_ATTR_STR(_name, _str)					   \
static struct perf_pmu_events_attr event_attr_##_name = {		   \
	.attr	   = __ATTR(_name, 0444, thunder_events_sysfs_show, NULL), \
	.event_str = _str,						   \
};

static inline void __iomem *map_offset(unsigned long addr,
				struct thunder_uncore *uncore, int unit)
{
	return (void __iomem *) (addr + uncore->pdevs[unit].map);
}

extern struct attribute_group thunder_uncore_attr_group;
extern struct thunder_uncore *thunder_uncore_l2c_tad;
extern struct thunder_uncore *thunder_uncore_l2c_cbc;
extern struct thunder_uncore *thunder_uncore_lmc;
extern struct thunder_uncore *thunder_uncore_ocx_lne;
extern struct pmu thunder_l2c_tad_pmu;
extern struct pmu thunder_l2c_cbc_pmu;
extern struct pmu thunder_lmc_pmu;
extern struct pmu thunder_ocx_lne_pmu;

/* Prototypes */
struct thunder_uncore *event_to_thunder_uncore(struct perf_event *event);
void thunder_uncore_del(struct perf_event *event, int flags);
int thunder_uncore_event_init(struct perf_event *event);
void thunder_uncore_read(struct perf_event *event);
int thunder_uncore_setup(struct thunder_uncore *uncore, int id,
			 unsigned long offset, unsigned long size,
			 struct pmu *pmu);
ssize_t thunder_events_sysfs_show(struct device *dev,
				  struct device_attribute *attr,
				  char *page);

int thunder_uncore_l2c_tad_setup(void);
int thunder_uncore_l2c_cbc_setup(void);
int thunder_uncore_lmc_setup(void);
int thunder_uncore_ocx_lne_setup(void);
