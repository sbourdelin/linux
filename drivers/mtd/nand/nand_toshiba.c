/*
 * Copyright (C) 2017 Free Electrons
 * Copyright (C) 2017 NextThing Co
 *
 * Author: Boris Brezillon <boris.brezillon@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/mtd/nand.h>

/* ECC Status Read Command for BENAND */
#define NAND_CMD_ECC_STATUS	0x7A

/* Recommended to rewrite for BENAND */
#define NAND_STATUS_RECOM_REWRT	0x08


static int toshiba_nand_benand_status_chk(struct mtd_info *mtd,
			struct nand_chip *chip)
{
	unsigned int max_bitflips = 0;
	u8 status;

	/* Check Read Status */
	chip->cmdfunc(mtd, NAND_CMD_STATUS, -1, -1);
	status = chip->read_byte(mtd);

	/* timeout */
	if (!(status & NAND_STATUS_READY)) {
		pr_debug("BENAND : Time Out!\n");
		return -EIO;
	}

	/* uncorrectable */
	else if (status & NAND_STATUS_FAIL)
		mtd->ecc_stats.failed++;

	/* correctable */
	else if (status & NAND_STATUS_RECOM_REWRT) {
		if (chip->cmd_ctrl &&
			IS_ENABLED(CONFIG_MTD_NAND_BENAND_ECC_STATUS)) {

			int i;
			u8 ecc_status;
			unsigned int bitflips;

			/* Check Read ECC Status */
			chip->cmd_ctrl(mtd, NAND_CMD_ECC_STATUS,
				NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE);
			/* Get bitflips info per 512Byte */
			for (i = 0; i < mtd->writesize >> 9; i++) {
				ecc_status = chip->read_byte(mtd);
				bitflips = ecc_status & 0x0f;
				max_bitflips = max_t(unsigned int,
						max_bitflips, bitflips);
			}
			mtd->ecc_stats.corrected += max_bitflips;
		} else {
			/*
			 * If can't use chip->cmd_ctrl,
			 * we can't get real number of bitflips.
			 * So, we set max_bitflips mtd->bitflip_threshold.
			 */
			max_bitflips = mtd->bitflip_threshold;
			mtd->ecc_stats.corrected += max_bitflips;
		}
	}

	return max_bitflips;
}

static int toshiba_nand_read_page_benand(struct mtd_info *mtd,
			struct nand_chip *chip, uint8_t *buf,
			int oob_required, int page)
{
	unsigned int max_bitflips = 0;

	chip->ecc.read_page_raw(mtd, chip, buf, oob_required, page);
	max_bitflips = toshiba_nand_benand_status_chk(mtd, chip);

	return max_bitflips;
}

static int toshiba_nand_read_subpage_benand(struct mtd_info *mtd,
			struct nand_chip *chip, uint32_t data_offs,
			uint32_t readlen, uint8_t *bufpoi, int page)
{
	uint8_t *p;
	unsigned int max_bitflips = 0;

	if (data_offs != 0)
		chip->cmdfunc(mtd, NAND_CMD_RNDOUT, data_offs, -1);

	p = bufpoi + data_offs;
	chip->read_buf(mtd, p, readlen);

	max_bitflips = toshiba_nand_benand_status_chk(mtd, chip);

	return max_bitflips;
}

static void toshiba_nand_decode_id(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);

	nand_decode_ext_id(chip);

	/*
	 * Toshiba 24nm raw SLC (i.e., not BENAND) have 32B OOB per
	 * 512B page. For Toshiba SLC, we decode the 5th/6th byte as
	 * follows:
	 * - ID byte 6, bits[2:0]: 100b -> 43nm, 101b -> 32nm,
	 *                         110b -> 24nm
	 * - ID byte 5, bit[7]:    1 -> BENAND, 0 -> raw SLC
	 */
	if (chip->id.len >= 6 && nand_is_slc(chip) &&
	    (chip->id.data[5] & 0x7) == 0x6 /* 24nm */ &&
	    (chip->id.data[4] & 0x80) /* BENAND */) {
		if (IS_ENABLED(CONFIG_MTD_NAND_BENAND))
			chip->ecc.mode = NAND_ECC_BENAND;
	} else {
		mtd->oobsize = 32 * mtd->writesize >> 9;	/* !BENAND */
	}
}

static int toshiba_nand_init(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);

	if (nand_is_slc(chip))
		chip->bbt_options |= NAND_BBT_SCAN2NDPAGE;

	if (chip->ecc.mode == NAND_ECC_BENAND) {
		chip->ecc.options = NAND_ECC_CUSTOM_PAGE_ACCESS;
		chip->ecc.bytes = 0;
		chip->ecc.strength = 8;
		chip->ecc.total = 0;
		chip->ecc.read_page = toshiba_nand_read_page_benand;
		chip->ecc.read_subpage = toshiba_nand_read_subpage_benand;

		mtd_set_ooblayout(mtd, &nand_ooblayout_lp_ops);
	}

	return 0;
}

const struct nand_manufacturer_ops toshiba_nand_manuf_ops = {
	.detect = toshiba_nand_decode_id,
	.init = toshiba_nand_init,
};
