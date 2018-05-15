struct btuart_vnd {
	const struct h4_recv_pkt *recv_pkts;
	int recv_pkts_cnt;
	unsigned int manufacturer;
	void *(*init)(struct device *dev);

	int (*open)(struct hci_dev *hdev);
	int (*close)(struct hci_dev *hdev);
	int (*setup)(struct hci_dev *hdev);
	int (*shutdown)(struct hci_dev *hdev);
	int (*send)(struct hci_dev *hdev, struct sk_buff *skb);
	int (*recv)(struct hci_dev *hdev, const u8 *data, size_t count);
};

struct btuart_dev {
	struct hci_dev *hdev;
	struct serdev_device *serdev;

	struct work_struct tx_work;
	unsigned long tx_state;
	struct sk_buff_head txq;

	struct sk_buff *rx_skb;

	const struct btuart_vnd *vnd;
	void *data;
};

#define BTUART_TX_STATE_ACTIVE	1
#define BTUART_TX_STATE_WAKEUP	2
