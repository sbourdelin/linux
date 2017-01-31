/*
 * Applied Micro X-Gene SoC Ethernet v2 Driver
 *
 * Copyright (c) 2017, Applied Micro Circuits Corporation
 * Author(s): Iyappan Subramanian <isubramanian@apm.com>
 *	      Keyur Chudgar <kchudgar@apm.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __XGENE_ENET_V2_RING_H__
#define __XGENE_ENET_V2_RING_H__

struct xge_desc_ring;

#define XGENE_ENET_DESC_SIZE	64
#define XGENE_ENET_NUM_DESC	256
#define NUM_BUFS		8

#define E_POS			63
#define E_LEN			1
#define PKT_ADDRL_POS		0
#define PKT_ADDRL_LEN		32
#define PKT_ADDRH_POS		32
#define PKT_ADDRH_LEN		10
#define PKT_SIZE_POS		32
#define PKT_SIZE_LEN		12
#define NEXT_DESC_ADDRL_POS	0
#define NEXT_DESC_ADDRL_LEN	32
#define NEXT_DESC_ADDRH_POS	48
#define NEXT_DESC_ADDRH_LEN	10

struct xge_raw_desc {
	__le64 m0;
	__le64 m1;
	__le64 m2;
	__le64 m3;
	__le64 m4;
	__le64 m5;
	__le64 m6;
	__le64 m7;
};

static inline u64 xge_set_desc_bits(int pos, int len, u64 val)
{
	return (val & ((1ULL << len) - 1)) << pos;
}

static inline u64 xge_get_desc_bits(int pos, int len, u64 src)
{
	return (src >> pos) & ((1ULL << len) - 1);
}

#define SET_BITS(field, val) \
		xge_set_desc_bits(field ## _POS, field ## _LEN, val)

#define GET_BITS(field, src) \
		xge_get_desc_bits(field ## _POS, field ## _LEN, src)

void xge_setup_desc(struct xge_desc_ring *ring);
void xge_update_tx_desc_addr(struct xge_pdata *pdata);
void xge_update_rx_desc_addr(struct xge_pdata *pdata);
void xge_intr_enable(struct xge_pdata *pdata);
void xge_intr_disable(struct xge_pdata *pdata);

#endif  /* __XGENE_ENET_V2_RING_H__ */
