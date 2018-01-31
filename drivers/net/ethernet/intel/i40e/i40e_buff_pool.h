#ifndef I40E_BUFF_POOL_H_
#define I40E_BUFF_POOL_H_

#include <linux/types.h>

struct buff_pool;
struct device;

struct buff_pool *i40e_buff_pool_create(struct device *dev);

struct buff_pool *i40e_buff_pool_recycle_create(unsigned int mtu,
						bool reserve_headroom,
						struct device *dev,
						unsigned int pool_size);
#endif /* I40E_BUFF_POOL_H_ */
