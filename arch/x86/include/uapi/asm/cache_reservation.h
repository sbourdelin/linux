enum cache_rsvt_flags {
	CACHE_RSVT_ROUND_DOWN	=	(1 << 0),  /* round kbytes down */
};

enum cache_rsvt_type {
	CACHE_RSVT_TYPE_CODE = 0, /* cache reservation is for code */
	CACHE_RSVT_TYPE_DATA,     /* cache reservation is for data */
	CACHE_RSVT_TYPE_BOTH,     /* cache reservation is for both */
};

struct cat_reservation {
	__u64 kbytes;
	__u32 type;
	__u32 flags;
	__u32 tcrid;
	__u32 pad[11];
};

struct cat_reservation_cpumask {
	size_t cpusetsize;
	cpu_set_t *mask;
	struct cat_reservation res;
};

struct pid_cat_reservation {
	__u32 tcrid;
	__s32 pid; 
	__u32 pad[8];
};

struct cat_tcrid {
	__u32 tcrid;
	__u32 pad[7];
};

struct cat_reservation_list {
	/* -- input -- */
	struct cat_reservation *res;
	/* how many bytes allocated for list */
	size_t cat_res_size;
	cpu_set_t *mask;
	/* how many bytes allocated for mask */
	size_t cpusetsize;

	/* -- output -- */
	/* size of each cpu_set_t entry copied to 
         * cpu_set_t *mask
         */
	size_t cpumask_size;
	__u32 pad[11];
};

struct cat_tcrid_tasks {
	__u32 tcrid;
	size_t nr_entries;
	struct pid_t *list;
};

#define CAT_CREATE_RESERVATION	_IOW(CATIO, 0x00, struct cat_reservation_cpumask)
#define CAT_DELETE_RESERVATION	_IOR(CATIO,  0x01, struct cat_tcrid)
#define CAT_ATTACH_RESERVATION	_IOW(CATIO, 0x02, struct pid_cat_reservation)
#define CAT_DETACH_RESERVATION	_IOW(CATIO, 0x03, struct pid_cat_reservation)
#define CAT_GET_RESERVATIONS	_IOW(CATIO, 0x04, struct cat_reservation_list)
#define CAT_GET_TCRID_TASKS     _IOW(CATIO, 0x05, struct)
