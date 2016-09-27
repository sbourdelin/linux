/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Sysfs support functions
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Thomas Richter <tmricht@linux.vnet.ibm.com>
 */

#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/types.h>
#include <linux/ctype.h>

#include <net/sock.h>

#include <rdma/ib_verbs.h>

#include "smc_ib.h"
#include "smc_pnet.h"

#define SMC_MAX_PNET_ID_LEN	16	/* Max. length of PNET id */

/* Sysfs interface for the pnet table
 *
 * Create a directory /sys/kernel/smc/ with these files:
 * /sys/kernel/smc/pnetid_add       --> Create a PNETID
 * /sys/kernel/smc/pnetid_delete    --> Delete a PNETID
 * /sys/kernel/smc/flush            --> Delete all PNETIDs
 * /sys/kernel/smc/pnetids/xxxxx    --> Created PNETIDs
 *
 * Create PNETID PNET1:
 * A new file named PNET1 shows up in /sys/kernel/smc/pnetids/.
 * echo PNET1 > /sys/kernel/smc/pnetid_add
 *
 * Display all created PNETIDs:
 * ls -l /sys/kernel/smc/pnetids
 *
 * Delete PNETID PNET1:
 * File PNET1 is removed from directory /sys/kernel/smc/pnetids/.
 * echo PNET1 > /sys/kernel/smc/pnetid_del
 *
 * Add an ethernet interface to PNETID PNET1:
 * A leading '+' is optional.
 * echo "eth enccw0.0.f5f0" > /sys/kernel/smc/pnetids/PNET1
 *
 * Add an RDMA device to PNETID PNET1:
 * A leading '+' is optional
 * The 3rd field is an optional port. If not specified it defaults to 1.
 * Currently accepted port numbers are 1 and 2. Other numbers generate an
 * error.
 * echo "ib mlx4_0 1" > /sys/kernel/smc/pnetids/PNET1
 * echo "+ib mlx4_1 2" > /sys/kernel/smc/pnetids/PNET1
 *
 * Display all entries belonging to PNETID PNET1:
 * cat /sys/kernel/smc/pnetids/PNET1
 *
 * Delete any entry from PNETID PNET1 with a leading '-':
 * echo "-ib mlx4_1 2" > /sys/kernel/smc/pnetids/PNET1
 *
 * Delete all created PNETIDs at once:
 * echo - > /sys/kernel/smc/flush
 *
 * No load balancing and link fail over is supported.
 * This results a one to one relationship between ethernet interface and
 * RDMA device including port name. Therefore each pnet identifier maps
 * one ethernet interface to one RDMA device.
 */

/**
 * struct smc_pnettable - SMC sysfs anchor
 * @kset: SMC sysfs anchor
 * @pnetids_kobj: Anchor to /sys/kernel/smc/pnetids
 * @lock: Lock for list action
 * @pnetlist: List of PNETIDs
 */
static struct smc_pnettable {
	struct kset *kset;
	struct kobject pnetids_kobj;
	rwlock_t lock;
	struct list_head pnetlist;
} smc_pnettable = {
	.pnetlist = LIST_HEAD_INIT(smc_pnettable.pnetlist),
	.lock = __RW_LOCK_UNLOCKED(smc_pnettable.lock)
};

/**
 * struct smc_pnetentry - pnet identifier name entry
 * @list: List node.
 * @attr: Embedded attribute structure
 * @pnet_name: Pnet identifier name
 * @if_name: Name of the ethernet interface.
 * @ib_name: Name of the RDMA device.
 * @ib_port: RDMA device port number.
 */
struct smc_pnetentry {
	struct list_head list;
	struct kobj_attribute attr;
	char pnet_name[SMC_MAX_PNET_ID_LEN + 1];
	char if_name[IFNAMSIZ];
	char ib_name[IB_DEVICE_NAME_MAX];
	u8 ib_port;
};

#define to_smcpnetentry(a)	container_of((a), struct smc_pnetentry, attr)

/* Release /sys/kernel/smc/pnetids and delete all pnetids. This function
 * is called when the kobject anchor in smc_pnettable.pnetids_kobj is freed.
 */
static void smc_pnetid_release(struct kobject *kobj)
{
	struct smc_pnetentry *e, *tmp_e;

	write_lock(&smc_pnettable.lock);
	list_for_each_entry_safe(e, tmp_e, &smc_pnettable.pnetlist, list) {
		list_del(&e->list);
		kfree(e);
	}
	write_unlock(&smc_pnettable.lock);
}

static struct kobj_type smc_pnet_ktype = {
	.release = smc_pnetid_release,
	.sysfs_ops = &kobj_sysfs_ops
};

/* Remove an ethernet entry from the PNET table */
static int smc_pnet_del_eth(struct smc_pnetentry *pnetelem, char *name)
{
	int rc = -ENOENT;

	write_lock(&smc_pnettable.lock);
	if (!strncmp(pnetelem->if_name, name, sizeof(pnetelem->if_name))) {
		rc = 0;
		pnetelem->if_name[0] = '\0';
	}
	write_unlock(&smc_pnettable.lock);
	return rc;
}

/* Add an ethernet entry to the PNET table. Search the complete pnet table to
 * make sure the same ethernet interface is not listed under different PNET ids.
 */
static int smc_pnet_add_eth(struct smc_pnetentry *pnetelem, char *name)
{
	struct smc_pnetentry *p;
	int rc = -EEXIST;

	write_lock(&smc_pnettable.lock);
	list_for_each_entry(p, &smc_pnettable.pnetlist, list) {
		if (!strncmp(p->if_name, name, sizeof(p->if_name)))
			goto out;
	}
	if (pnetelem->if_name[0] == '\0') {
		strncpy(pnetelem->if_name, name, sizeof(pnetelem->if_name));
		rc = 0;
	}
out:
	write_unlock(&smc_pnettable.lock);
	return rc;
}

/* Create an ethernet interface entry. */
static int smc_pnet_makeeth(struct smc_pnetentry *pnetelem, bool add,
			    char *name)
{
	name = skip_spaces(name);
	if (!dev_valid_name(name))
		return -EINVAL;
	return (add) ? smc_pnet_add_eth(pnetelem, name)
		     : smc_pnet_del_eth(pnetelem, name);
}

/* Check if two RDMA device entries are identical. Use device name and port
 * number for comparison.
 */
static bool smc_pnet_same_ibname(struct smc_pnetentry *a, char *name, u8 ibport)
{
	return a->ib_port == ibport &&
	       !strncmp(a->ib_name, name, sizeof(a->ib_name));
}

/* Add an RDMA device entry to the PNET table */
static int smc_pnet_add_ib(struct smc_pnetentry *pnetelem, char *name,
			   u8 ibport)
{
	struct smc_ib_device *smcibdev = NULL;
	struct smc_ib_device *dev;
	struct smc_pnetentry *p;
	int rc = -EEXIST;

	write_lock(&smc_pnettable.lock);
	list_for_each_entry(p, &smc_pnettable.pnetlist, list) {
		if (smc_pnet_same_ibname(p, name, ibport))
			goto out;
	}
	if (pnetelem->ib_name[0] == '\0') {
		strncpy(pnetelem->ib_name, name, sizeof(pnetelem->ib_name));
		pnetelem->ib_port = ibport;
		spin_lock(&smc_ib_devices.lock);
		/* using string ib_name, search smcibdev in global list */
		list_for_each_entry(dev, &smc_ib_devices.list, list) {
			if (!strncmp(dev->ibdev->name, pnetelem->ib_name,
				     sizeof(pnetelem->ib_name))) {
				smcibdev = dev;
				break;
			}
		}
		spin_unlock(&smc_ib_devices.lock);
		rc = 0;
	}
out:
	write_unlock(&smc_pnettable.lock);
	if (smcibdev && !smcibdev->initialized) {
		/* ib dev already existed [dev coldplug].
		 * Complements: smc_ib_add_dev() [dev hotplug],
		 * smc_ib_global_event_handler() [port hotplug].
		 * Function call chain can sleep so outside of our locks.
		 */
		rc = smc_ib_remember_port_attr(smcibdev,
					       pnetelem->ib_port);
		if (rc)
			return rc;
		rc = smc_ib_setup_per_ibdev(smcibdev);
		if (rc)
			return rc;
		smcibdev->initialized = 1;
	}
	return rc;
}

/* Remove an RDMA device entry from the PNET table */
static int smc_pnet_del_ib(struct smc_pnetentry *pnetelem, char *name,
			   u8 ibport)
{
	int rc = -ENOENT;

	write_lock(&smc_pnettable.lock);
	if (smc_pnet_same_ibname(pnetelem, name, ibport)) {
		rc = 0;
		pnetelem->ib_name[0] = '\0';
		pnetelem->ib_port = 0;
	}
	write_unlock(&smc_pnettable.lock);
	return rc;
}

/* Create an RDMA device entry. Optional port number delimited by blank
 * from name. Missing port number defaults to 1.
 */
static int smc_pnet_makeib(struct smc_pnetentry *pnetelem, bool add, char *name)
{
	unsigned int tmp_port = 1;
	char *portno;
	int rc;

	name = skip_spaces(name);
	portno = strchr(name, ' ');
	if (portno) {		/* Port number specified */
		*portno = '\0';
		portno = skip_spaces(portno + 1);
		rc = kstrtouint(portno, 10, &tmp_port);
		if (rc || tmp_port > SMC_MAX_PORTS || !tmp_port) {
			rc = -EINVAL;
			goto out;
		}
	}
	rc = (add) ? smc_pnet_add_ib(pnetelem, name, (u8)tmp_port)
		   : smc_pnet_del_ib(pnetelem, name, (u8)tmp_port);
out:
	return rc;
}

static ssize_t smc_pnetidfile_attr_store(struct kobject *kobj,
					 struct kobj_attribute *ka,
					 const char *buf, size_t len)
{
	char *text, *buf_copy;
	bool add = true;
	int rc;

	/* Operate on a copy of the buffer, we might modify the string */
	buf_copy = kstrdup(buf, GFP_KERNEL);
	if (!buf_copy)
		return -ENOMEM;
	text = strim(buf_copy);
	switch (*text) {
	case '-':
		add = false;
		/* Fall through intended */
	case '+':
		++text;
		break;
	}
	text = skip_spaces(text);
	rc = -EINVAL;
	if (!strncmp(text, "ib ", 3))
		rc = smc_pnet_makeib(to_smcpnetentry(ka), add, text + 3);
	else if (!strncmp(text, "eth ", 4))
		rc = smc_pnet_makeeth(to_smcpnetentry(ka), add, text + 4);
	kfree(buf_copy);
	return rc ?: len;
}

/* List all entries of a PNETID. List ethernet entries first followed by
 * RDMA device entries. Output limited to PAGE_SIZE bytes.
 */
static ssize_t smc_pnetidfile_attr_show(struct kobject *kobj,
					struct kobj_attribute *ka,
					char *buf)
{
	struct smc_pnetentry *pnetelem = to_smcpnetentry(ka);
	size_t pos = 0;

	read_lock(&smc_pnettable.lock);
	if (strlen(pnetelem->if_name)) {
		snprintf(buf, PAGE_SIZE, "eth %s\n", pnetelem->if_name);
		pos = strlen(buf);
	}
	if (strlen(pnetelem->ib_name))
		snprintf(buf + pos, PAGE_SIZE - pos, "ib %s %u\n",
			 pnetelem->ib_name, pnetelem->ib_port);
	read_unlock(&smc_pnettable.lock);
	return strlen(buf);
}

/* Delete a PNETID attribute file in /sys/kernel/smc/pnetids.
 * Remove the sysfs file first and then remove the node from the list and
 * release memory.
 */
static int smc_pnetid_del_file(char *pnetid)
{
	struct smc_pnetentry *e, *tmp_e, *found = NULL;

	write_lock(&smc_pnettable.lock);
	list_for_each_entry_safe(e, tmp_e, &smc_pnettable.pnetlist, list) {
		if (!strncmp(e->pnet_name, pnetid, sizeof(e->pnet_name))) {
			list_del(&e->list);
			found = e;
			break;
		}
	}
	write_unlock(&smc_pnettable.lock);
	if (!found)
		return -ENOENT;
	sysfs_remove_file(&smc_pnettable.pnetids_kobj, &found->attr.attr);
	kfree(found);
	return 0;
}

/* Append a PNETID to the end of the list if not already on this list. */
static int smc_pnet_append_pnetentry(struct smc_pnetentry *new)
{
	struct smc_pnetentry *pnetelem;
	int rc = 0;

	write_lock(&smc_pnettable.lock);
	list_for_each_entry(pnetelem, &smc_pnettable.pnetlist, list) {
		if (!strncmp(pnetelem->pnet_name, new->pnet_name,
			     sizeof(new->pnet_name))) {
			rc = -EEXIST;
			goto found;
		}
	}
	list_add_tail(&new->list, &smc_pnettable.pnetlist);
found:
	write_unlock(&smc_pnettable.lock);
	return rc;
}

/* Add a PNETID attribute file in /sys/kernel/smc/pnetids. */
static int smc_pnetid_add_file(char *pnetname)
{
	struct smc_pnetentry *pnetelem = kzalloc(sizeof(*pnetelem), GFP_KERNEL);
	struct kobj_attribute *ka;
	int rc;

	if (!pnetelem)
		return -ENOMEM;
	ka = &pnetelem->attr;
	sysfs_attr_init(&ka->attr);
	strncpy(pnetelem->pnet_name, pnetname, sizeof(pnetelem->pnet_name));
	ka->attr.name = pnetelem->pnet_name;
	ka->attr.mode = S_IWUSR | S_IRUGO;
	ka->show = smc_pnetidfile_attr_show;
	ka->store = smc_pnetidfile_attr_store;
	rc = smc_pnet_append_pnetentry(pnetelem);
	if (rc)
		goto outfree;
	rc = sysfs_create_file_ns(&smc_pnettable.pnetids_kobj, &ka->attr, NULL);
	if (!rc)
		return rc;
	/* sysfs failure, remove node from list */
	write_lock(&smc_pnettable.lock);
	list_del(&pnetelem->list);
	write_unlock(&smc_pnettable.lock);
outfree:
	kfree(pnetelem);
	return rc;
}

/* The limit for PNETID is 16 characters.
 * Valid characters should be (single-byte character set) a-z, A-Z, 0-9.
 * Lower case letters are converted to upper case.
 * Interior blanks should not be used.
 */
static bool smc_pnetid_valid(const char *buf, char *pnetid)
{
	char *bf = skip_spaces(buf);
	size_t len = strlen(bf);
	char *end = bf + len;

	if (!len)
		return false;
	while (--end >= bf && isspace(*end))
		;
	if (end - bf >= SMC_MAX_PNET_ID_LEN)
		return false;
	while (bf <= end) {
		if (!isalnum(*bf))
			return false;
		*pnetid++ = islower(*bf) ? toupper(*bf) : *bf;
		bf++;
	}
	*pnetid = '\0';
	return true;
}

static ssize_t smc_pnetid_store(bool add, const char *buf)
{
	char pnetid[SMC_MAX_PNET_ID_LEN + 1];

	if (!smc_pnetid_valid(buf, pnetid))
		return -EINVAL;
	return add ? smc_pnetid_add_file(pnetid) : smc_pnetid_del_file(pnetid);
}

#define SMC_ATTR_WO(_name)	\
	struct kobj_attribute smc_attr_##_name = __ATTR(_name, S_IWUSR, NULL, \
							smc_##_name##_store)

static ssize_t smc_pnetid_del_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	ssize_t rc = smc_pnetid_store(false, buf);

	return rc ?: count;
}
static SMC_ATTR_WO(pnetid_del);

static ssize_t smc_pnetid_add_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	ssize_t rc = smc_pnetid_store(true, buf);

	return rc ?: count;
}
static SMC_ATTR_WO(pnetid_add);

/* Delete all PNETIDs. Any string with leading '-' will do.
 * smc_pnetid_del_file() can not be called directly, because function
 * sysfs_remove_file() can not be called under lock. Get the first entry
 * of the list and remove it. smc_pnetid_del_file() can handle the case
 * when a PNETID already has been deleted in the mean time.
 */
static ssize_t smc_flush_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct smc_pnettable *ptr = &smc_pnettable;
	char pnetname[SMC_MAX_PNET_ID_LEN + 1];
	struct smc_pnetentry *pnetelem;
	char *bf = skip_spaces(buf);

	if (*bf != '-')
		return -EINVAL;
	do {
		read_lock(&ptr->lock);
		pnetelem = list_first_entry_or_null(&ptr->pnetlist,
						    struct smc_pnetentry, list);
		if (pnetelem)
			strncpy(pnetname, pnetelem->pnet_name,
				sizeof(pnetname));
		read_unlock(&ptr->lock);
		if (pnetelem)
			smc_pnetid_del_file(pnetname);
	} while (pnetelem);
	return count;
}
static SMC_ATTR_WO(flush);

static struct attribute *smc_pnetid_attrs[] = { /* Default SMC attributes */
	&smc_attr_pnetid_add.attr,
	&smc_attr_pnetid_del.attr,
	&smc_attr_flush.attr,
	NULL
};

static struct attribute_group smc_attr_group = {
	.attrs = smc_pnetid_attrs
};

/* Remove directory tree created under /sys/kernel/smc/. */
void smc_pnet_exit(void)
{
	kobject_put(&smc_pnettable.pnetids_kobj);
	sysfs_remove_group(&smc_pnettable.kset->kobj, &smc_attr_group);
	kset_unregister(smc_pnettable.kset);
}

/* Create directory tree for SMC under /sys/kernel/smc/. */
int __init smc_pnet_init(void)
{
	int rc = -ENOMEM;

	smc_pnettable.kset = kset_create_and_add("smc", NULL, kernel_kobj);
	if (!smc_pnettable.kset)
		goto bad0;
	rc = sysfs_create_group(&smc_pnettable.kset->kobj, &smc_attr_group);
	if (rc)
		goto bad1;
	rc = kobject_init_and_add(&smc_pnettable.pnetids_kobj, &smc_pnet_ktype,
				  &smc_pnettable.kset->kobj, "pnetids");
	if (rc)
		goto bad2;
	return rc;

bad2:
	sysfs_remove_group(&smc_pnettable.kset->kobj, &smc_attr_group);
bad1:
	kset_unregister(smc_pnettable.kset);
bad0:
	return rc;
}

/* Scan the pnet table and find an IB device given the pnetid entry.
 * Return infiniband device and port number if an active port is found.
 * This function is called under smc_pnettable.lock.
 */
static void smc_pnet_ib_dev_by_pnet(struct smc_pnetentry *pnetelem,
				    struct smc_ib_device **smcibdev, u8 *ibport)
{
	struct smc_ib_device *dev;

	*smcibdev = NULL;
	*ibport = 0;
	spin_lock(&smc_ib_devices.lock);
	/* using string ib->ib_name, search ibdev in global list */
	list_for_each_entry(dev, &smc_ib_devices.list, list) {
		if (!strncmp(dev->ibdev->name, pnetelem->ib_name,
			     sizeof(pnetelem->ib_name)) &&
		    smc_ib_port_active(dev, pnetelem->ib_port)) {
			*smcibdev = dev;
			*ibport = pnetelem->ib_port;
			break;
		}
	}
	spin_unlock(&smc_ib_devices.lock);
}

/* PNET table analysis for a given sock:
 * determine ib_device and port belonging to used internal TCP socket
 * ethernet interface.
 */
void smc_pnet_find_roce_resource(struct sock *sk,
				 struct smc_ib_device **smcibdev, u8 *ibport)
{
	struct dst_entry *dst = sk_dst_get(sk);
	struct smc_pnetentry *pnetelem;

	*smcibdev = NULL;
	*ibport = 0;

	if (!dst)
		return;
	if (!dst->dev)
		goto out_rel;
	read_lock(&smc_pnettable.lock);
	list_for_each_entry(pnetelem, &smc_pnettable.pnetlist, list) {
		if (!strncmp(dst->dev->name, pnetelem->if_name, IFNAMSIZ)) {
			smc_pnet_ib_dev_by_pnet(pnetelem, smcibdev, ibport);
			break;
		}
	}
	read_unlock(&smc_pnettable.lock);
out_rel:
	dst_release(dst);
}

/* Returns true if a specific ib_device and port is in the PNET table. */
bool smc_pnet_exists_in_table(struct smc_ib_device *smcibdev, u8 ibport)
{
	struct smc_pnetentry *pnetelem;
	int rc = false;

	read_lock(&smc_pnettable.lock);
	list_for_each_entry(pnetelem, &smc_pnettable.pnetlist, list) {
		if (!strncmp(smcibdev->ibdev->name, pnetelem->ib_name,
			     IB_DEVICE_NAME_MAX) &&
		    ibport == pnetelem->ib_port) {
			rc = true;
			break;
		}
	}
	read_unlock(&smc_pnettable.lock);
	return rc;
}
