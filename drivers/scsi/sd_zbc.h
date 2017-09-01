#ifndef _SCSI_DISK_ZBC_H
#define _SCSI_DISK_ZBC_H

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include "sd.h"

static inline int sd_is_zoned(struct scsi_disk *sdkp)
{
	return sdkp->zoned == 1 || sdkp->device->type == TYPE_ZBC;
}

#ifdef CONFIG_BLK_DEV_ZONED

static inline sector_t sd_zbc_zone_sectors(struct scsi_disk *sdkp)
{
	return logical_to_sectors(sdkp->device, sdkp->zone_blocks);
}

static inline unsigned int sd_zbc_zone_no(struct scsi_disk *sdkp,
					  sector_t sector)
{
	return sectors_to_logical(sdkp->device, sector) >> sdkp->zone_shift;
}

extern int sd_zbc_read_zones(struct scsi_disk *sdkp, unsigned char *buffer);
extern void sd_zbc_remove(struct scsi_disk *sdkp);
extern void sd_zbc_print_zones(struct scsi_disk *sdkp);

extern int sd_zbc_write_lock_zone(struct scsi_cmnd *cmd);
extern void sd_zbc_write_unlock_zone(struct scsi_cmnd *cmd);

extern int sd_zbc_setup_report_cmnd(struct scsi_cmnd *cmd);
extern int sd_zbc_setup_reset_cmnd(struct scsi_cmnd *cmd);
extern void sd_zbc_complete(struct scsi_cmnd *cmd, unsigned int good_bytes,
			    struct scsi_sense_hdr *sshdr);

#else /* CONFIG_BLK_DEV_ZONED */

static inline int sd_zbc_read_zones(struct scsi_disk *sdkp,
				    unsigned char *buf)
{
	return 0;
}

static inline void sd_zbc_remove(struct scsi_disk *sdkp) {}

static inline void sd_zbc_print_zones(struct scsi_disk *sdkp) {}

static inline int sd_zbc_write_lock_zone(struct scsi_cmnd *cmd)
{
	/* Let the drive fail requests */
	return BLKPREP_OK;
}

static inline void sd_zbc_write_unlock_zone(struct scsi_cmnd *cmd) {}

static inline int sd_zbc_setup_report_cmnd(struct scsi_cmnd *cmd)
{
	return BLKPREP_INVALID;
}

static inline int sd_zbc_setup_reset_cmnd(struct scsi_cmnd *cmd)
{
	return BLKPREP_INVALID;
}

static inline void sd_zbc_complete(struct scsi_cmnd *cmd,
				   unsigned int good_bytes,
				   struct scsi_sense_hdr *sshdr) {}

#endif /* CONFIG_BLK_DEV_ZONED */

#endif /* _SCSI_DISK_ZBC_H */
