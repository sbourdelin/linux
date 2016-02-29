#ifndef _UAPI_LINUX_SIGNAL_H
#define _UAPI_LINUX_SIGNAL_H

#include <asm/signal.h>
#include <asm/siginfo.h>

#define SS_ONSTACK	1
#define SS_DISABLE	2
#define SS_VALMASK	0xf
/* bit-flags */
#define SS_AUTODISARM	(1 << 4)	/* disable sas during sighandling */

#endif /* _UAPI_LINUX_SIGNAL_H */
