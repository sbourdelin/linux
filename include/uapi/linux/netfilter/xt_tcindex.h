#ifndef _XT_TCINDEX_H
#define _XT_TCINDEX_H

#include <linux/types.h>

struct xt_tcindex_tginfo1 {
	__u16 mark, mask;
};

struct xt_tcindex_mtinfo1 {
	__u16 mark, mask;
	__u8 invert;
};

#endif /*_XT_TCINDEX_H*/
