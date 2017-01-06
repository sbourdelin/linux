#ifndef _LINUX_IF_TAP_H_
#define _LINUX_IF_TAP_H_

rx_handler_result_t tap_handle_frame(struct sk_buff **pskb);
void tap_del_queues(struct net_device *dev);
int tap_get_minor(struct macvlan_dev *vlan);
void tap_free_minor(struct macvlan_dev *vlan);
int tap_queue_resize(struct macvlan_dev *vlan);
int tap_create_cdev(struct cdev *tap_cdev,
		     dev_t *tap_major, const char *device_name);
void tap_destroy_cdev(dev_t major, struct cdev *tap_cdev);
struct socket *tap_get_socket(struct file *file);

#endif /*_LINUX_IF_TAP_H_*/
