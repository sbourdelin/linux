
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

struct tcr_entry {
	unsigned int tcrid;

	unsigned long *tcrlist_bmap;

	u64 user_kbytes;
	u64 rounded_kbytes;
	unsigned int cbm_bits;

	u32 type;

	cpumask_var_t *cpumask;
};

#define CBM_LEN 64
#define MAX_LAYOUTS 10

struct tcr_list_per_socket {
	int cbm_start_bit, cbm_end_bit;
};

struct tcr_list {
	/* cache allocation */
	struct tcr_list_per_socket psd[MAX_LAYOUTS];

	/* bitmap indicating whether cap_bitmask is synced to a given socket */
	unsigned long *synced_to_socket;

	/* TCRlist id */
	unsigned int id;

	// One bit per tcrentry.
	DECLARE_BITMAP(tcrentry_bmap, CBM_LEN);
	
	// link in global tcrlist list
	struct list_head global_link; 
	// list of tasks referencing this tcr_list 
	struct list_head tasks;
	// nr of tasks referencing this tcr_list
	unsigned int nr_tasks;
};

