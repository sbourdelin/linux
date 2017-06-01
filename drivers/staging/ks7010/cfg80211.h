#ifndef _KS7010_CFG80211_H
#define _KS7010_CFG80211_H

#include "common.h"

struct ks7010 *ks7010_cfg80211_create(void);
void ks7010_cfg80211_destroy(struct ks7010 *ks);

#endif	/* _KS7010_CFG80211_H */
