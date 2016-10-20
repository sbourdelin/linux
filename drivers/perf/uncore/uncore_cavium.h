#include <linux/io.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/perf_event.h>

#undef pr_fmt
#define pr_fmt(fmt)     "thunderx_uncore: " fmt

#define to_uncore(x) container_of((x), struct thunder_uncore, pmu)

#define UNCORE_EVENT_ID_MASK		0xffff
#define UNCORE_EVENT_ID_SHIFT		16

/* maximum number of parallel hardware counters for all uncore parts */
#define MAX_COUNTERS			64

struct thunder_uncore_unit {
	struct list_head entry;
	void __iomem *map;
	struct pci_dev *pdev;
};

struct thunder_uncore_node {
	int nr_units;
	int num_counters;
	struct list_head unit_list;
	struct perf_event *events[MAX_COUNTERS];
};

/* generic uncore struct for different pmu types */
struct thunder_uncore {
	struct pmu pmu;
	bool (*event_valid)(u64);
	struct hlist_node node;
	struct thunder_uncore_node *nodes[MAX_NUMNODES];
	cpumask_t active_mask;
};

#define UC_EVENT_ENTRY(_name, _id)							\
	&((struct perf_pmu_events_attr[]) {						\
		{									\
			__ATTR(_name, S_IRUGO, thunder_events_sysfs_show, NULL),	\
			0,								\
			"event=" __stringify(_id),					\
		}									\
	})[0].attr.attr

static inline struct thunder_uncore_node *get_node(u64 config,
				   struct thunder_uncore *uncore)
{
	return uncore->nodes[config >> UNCORE_EVENT_ID_SHIFT];
}

#define get_id(config) (config & UNCORE_EVENT_ID_MASK)

extern struct attribute_group thunder_uncore_attr_group;
extern struct device_attribute format_attr_node;

/* Prototypes */
void thunder_uncore_read(struct perf_event *event);
int thunder_uncore_add(struct perf_event *event, int flags, u64 config_base,
		       u64 event_base);
void thunder_uncore_del(struct perf_event *event, int flags);
void thunder_uncore_start(struct perf_event *event, int flags);
void thunder_uncore_stop(struct perf_event *event, int flags);
int thunder_uncore_event_init(struct perf_event *event);
int thunder_uncore_setup(struct thunder_uncore *uncore, int id,
			 struct pmu *pmu, int counters);
ssize_t thunder_events_sysfs_show(struct device *dev,
				  struct device_attribute *attr,
				  char *page);
int thunder_uncore_l2c_tad_setup(void);
int thunder_uncore_l2c_cbc_setup(void);
