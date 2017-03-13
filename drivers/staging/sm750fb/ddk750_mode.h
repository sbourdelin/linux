#ifndef DDK750_MODE_H__
#define DDK750_MODE_H__

#include "ddk750_chip.h"

enum _spolarity_t {
	POS = 0, /* positive */
	NEG, /* negative */
};

struct _mode_parameter_t {
	/* Horizontal timing. */
	unsigned long horizontal_total;
	unsigned long horizontal_display_end;
	unsigned long horizontal_sync_start;
	unsigned long horizontal_sync_width;
	enum _spolarity_t horizontal_sync_polarity;

	/* Vertical timing. */
	unsigned long vertical_total;
	unsigned long vertical_display_end;
	unsigned long vertical_sync_start;
	unsigned long vertical_sync_height;
	enum _spolarity_t vertical_sync_polarity;

	/* Refresh timing. */
	unsigned long pixel_clock;
	unsigned long horizontal_frequency;
	enum _spolarity_t vertical_frequency;

	/* Clock Phase. This clock phase only applies to Panel. */
	enum _spolarity_t clock_phase_polarity;
};

int ddk750_setModeTiming(struct _mode_parameter_t *parm, clock_type_t clock);

#endif
