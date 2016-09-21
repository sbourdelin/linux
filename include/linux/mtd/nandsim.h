#ifndef __LINUX_NANDSIM_H__
#define __LINUX_NANDSIM_H__

#include <linux/mtd/mtd.h>
#include <mtd/nandsim-user.h>

struct nandsim_params {
	unsigned int access_delay;
	unsigned int program_delay;
	unsigned int erase_delay;
	unsigned int output_cycle;
	unsigned int input_cycle;
	unsigned int bus_width;
	unsigned int do_delays;
	unsigned long *parts;
	unsigned int parts_num;
	char *badblocks;
	char *weakblocks;
	char *weakpages;
	unsigned int bitflips;
	char *gravepages;
	unsigned int overridesize;
	char *cache_file;
	unsigned int bbt;
	unsigned int bch;
	unsigned char id_bytes[8];
	unsigned int file_fd;
	struct ns_backend_ops *bops;
};

/* NAND flash "geometry" */
struct nandsim_geom {
	uint64_t totsz;     /* total flash size, bytes */
	uint32_t secsz;     /* flash sector (erase block) size, bytes */
	uint pgsz;          /* NAND flash page size, bytes */
	uint oobsz;         /* page OOB area size, bytes */
	uint64_t totszoob;  /* total flash size including OOB, bytes */
	uint pgszoob;       /* page size including OOB , bytes*/
	uint secszoob;      /* sector size including OOB, bytes */
	uint pgnum;         /* total number of pages */
	uint pgsec;         /* number of pages per sector */
	uint secshift;      /* bits number in sector size */
	uint pgshift;       /* bits number in page size */
	uint pgaddrbytes;   /* bytes per page address */
	uint secaddrbytes;  /* bytes per sector address */
	uint idbytes;       /* the number ID bytes that this chip outputs */
};

/* NAND flash internal registers */
struct nandsim_regs {
	unsigned command; /* the command register */
	u_char   status;  /* the status register */
	uint     row;     /* the page number */
	uint     column;  /* the offset within page */
	uint     count;   /* internal counter */
	uint     num;     /* number of bytes which must be processed */
	uint     off;     /* fixed page offset */
};

/*
 * A union to represent flash memory contents and flash buffer.
 */
union ns_mem {
	u_char *byte;    /* for byte access */
	uint16_t *word;  /* for 16-bit word access */
};

struct nandsim;
struct ns_backend_ops {
	void (*erase_sector)(struct nandsim *ns);
	int (*prog_page)(struct nandsim *ns, int num);
	void (*read_page)(struct nandsim *ns, int num);
	int (*init)(struct nandsim *ns, struct nandsim_params *nsparam);
	void (*destroy)(struct nandsim *ns);
};

struct mtd_info *ns_new_instance(struct nandsim_params *nsparam);
void ns_destroy_instance(struct mtd_info *nsmtd);
struct nandsim_geom *nandsim_get_geom(struct nandsim *ns);
struct nandsim_regs *nandsim_get_regs(struct nandsim *ns);
void nandsim_set_backend_data(struct nandsim *ns, void *data);
void *nandsim_get_backend_data(struct nandsim *ns);
union ns_mem *nandsim_get_buf(struct nandsim *ns);

#endif /* __LINUX_NANDSIM_H__ */
