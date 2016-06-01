#include <linux/fsnotify_backend.h>
#include <linux/inotify.h>
#include <linux/slab.h> /* struct kmem_cache */
#include <linux/hashtable.h>

struct inotify_event_info {
	struct fsnotify_event fse;
	int wd;
	u32 sync_cookie;
	int name_len;
	char name[];
};

struct inotify_inode_mark {
	struct fsnotify_mark fsn_mark;
	int wd;
};

struct inotify_state {
	struct hlist_node node;
	void *key; /* user_namespace ptr */
	u32 inotify_watches; /* How many inotify watches does this user have? */
	u32 inotify_devs;  /* How many inotify devs does this user have opened? */
};

static inline struct inotify_event_info *INOTIFY_E(struct fsnotify_event *fse)
{
	return container_of(fse, struct inotify_event_info, fse);
}

extern void inotify_ignored_and_remove_idr(struct fsnotify_mark *fsn_mark,
					   struct fsnotify_group *group);
extern int inotify_handle_event(struct fsnotify_group *group,
				struct inode *inode,
				struct fsnotify_mark *inode_mark,
				struct fsnotify_mark *vfsmount_mark,
				u32 mask, void *data, int data_type,
				const unsigned char *file_name, u32 cookie);

extern const struct fsnotify_ops inotify_fsnotify_ops;

/* Helpers for manipulating various inotify state, stored in user_struct */
static inline struct inotify_state *__find_inotify_state(struct user_struct *user,
							  void *key)
{
	struct inotify_state *state;

	hash_for_each_possible(user->inotify_tbl, state, node, (unsigned long)key)
		if (state->key == key)
			return state;

	return NULL;
}

static inline void inotify_inc_watches(struct user_struct *user, void *key)
{
	struct inotify_state *state;

	spin_lock(&user->inotify_lock);
	state = __find_inotify_state(user, key);
	state->inotify_watches++;
	spin_unlock(&user->inotify_lock);
}


static inline void inotify_dec_watches(struct user_struct *user, void *key)
{
	struct inotify_state *state;

	spin_lock(&user->inotify_lock);
	state = __find_inotify_state(user, key);
	state->inotify_watches--;
	spin_unlock(&user->inotify_lock);
}

static inline int inotify_read_watches(struct user_struct *user, void *key)
{
	struct inotify_state *state;
	int ret;

	spin_lock(&user->inotify_lock);
	state = __find_inotify_state(user, key);
	ret = state->inotify_watches;
	spin_unlock(&user->inotify_lock);
	return ret;
}

static inline unsigned long inotify_dec_return_dev(struct user_struct *user,
						   void *key)
{
	struct inotify_state *state;
	unsigned long ret;

	spin_lock(&user->inotify_lock);
	state = __find_inotify_state(user, key);
	ret = --state->inotify_devs;
	spin_unlock(&user->inotify_lock);

	return ret;
}
