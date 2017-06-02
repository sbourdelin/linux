#ifndef __MVEBU_GICP_H__
#define __MVEBU_GICP_H__

struct mvebu_gicp;

int mvebu_gicp_alloc(struct mvebu_gicp *gicp);
void mvebu_gicp_free(struct mvebu_gicp *gicp, int idx);
int mvebu_gicp_idx_to_spi(struct mvebu_gicp *gicp, int idx);
int mvebu_gicp_spi_to_idx(struct mvebu_gicp *gicp, int spi);
phys_addr_t mvebu_gicp_setspi_phys_addr(struct mvebu_gicp *gicp);
phys_addr_t mvebu_gicp_clrspi_phys_addr(struct mvebu_gicp *gicp);
int mvebu_gicp_spi_count(struct mvebu_gicp *gicp);

#endif /* __MVEBU_GICP_H__ */

