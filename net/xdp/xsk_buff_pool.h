#ifndef XSK_BUFF_POOL_H_
#define XSK_BUFF_POOL_H_

struct xsk_buff;
struct xsk_buff_info;
struct xsk_queue;

struct xsk_buff_pool {
	struct xsk_buff *free_list;
	struct xsk_buff_info *bi;
	struct xsk_queue *q;
};

struct buff_pool *xsk_buff_pool_create(struct xsk_buff_info *buff_info,
				       struct xsk_queue *queue);

#endif /* XSK_BUFF_POOL_H_ */
