#include <linux/blk_dim.h>

static inline int blk_dim_step(struct dim *dim)
{
	switch (dim->tune_state) {
	case DIM_PARKING_ON_TOP:
	case DIM_PARKING_TIRED:
		break;
	case DIM_GOING_RIGHT:
		if (dim->profile_ix == (BLK_DIM_PARAMS_NUM_PROFILES - 1))
			return DIM_ON_EDGE;
		dim->profile_ix++;
		dim->steps_right++;
		break;
	case DIM_GOING_LEFT:
		if (dim->profile_ix == 0)
			return DIM_ON_EDGE;
		dim->profile_ix--;
		dim->steps_left++;
		break;
	}

	return DIM_STEPPED;
}

static inline int blk_dim_stats_compare(struct dim_stats *curr, struct dim_stats *prev)
{
	/* first stat */
	if (!prev->cpms)
		return DIM_STATS_SAME;

	if (IS_SIGNIFICANT_DIFF(curr->cpms, prev->cpms))
		return (curr->cpms > prev->cpms) ? DIM_STATS_BETTER :
						DIM_STATS_WORSE;

	if (IS_SIGNIFICANT_DIFF(curr->cpe_ratio, prev->cpe_ratio))
		return (curr->cpe_ratio > prev->cpe_ratio) ? DIM_STATS_BETTER :
						DIM_STATS_WORSE;

	return DIM_STATS_SAME;
}

static inline bool blk_dim_decision(struct dim_stats *curr_stats, struct dim *dim)
{
	int prev_ix = dim->profile_ix;
	int stats_res;
	int step_res;

	switch (dim->tune_state) {
	case DIM_PARKING_ON_TOP:
		break;
	case DIM_PARKING_TIRED:
		break;

	case DIM_GOING_RIGHT:
	case DIM_GOING_LEFT:
		stats_res = blk_dim_stats_compare(curr_stats, &dim->prev_stats);

		switch (stats_res) {
		case DIM_STATS_SAME:
			if (curr_stats->cpe_ratio <= 50*prev_ix)
				dim->profile_ix = 0;
			break;
		case DIM_STATS_WORSE:
			dim_turn(dim);
		default:
		case DIM_STATS_BETTER:
			/* fall through */
			step_res = blk_dim_step(dim);
			if (step_res == DIM_ON_EDGE)
				dim_turn(dim);
			break;
		}
		break;
	}

	dim->prev_stats = *curr_stats;

	return dim->profile_ix != prev_ix;
}

void blk_dim(struct dim *dim, struct dim_sample end_sample)
{
	struct dim_stats curr_stats;
	u16 nevents;

	switch (dim->state) {
	case DIM_MEASURE_IN_PROGRESS:
		nevents = end_sample.event_ctr - dim->start_sample.event_ctr;
		if (nevents < DIM_NEVENTS) {
			dim_create_sample(end_sample.event_ctr, end_sample.pkt_ctr,
				end_sample.byte_ctr, end_sample.comp_ctr, &dim->measuring_sample);
			break;
		}
		dim_calc_stats(&dim->start_sample, &end_sample,
				   &curr_stats);
		if (blk_dim_decision(&curr_stats, dim)) {
			dim->state = DIM_APPLY_NEW_PROFILE;
			schedule_work(&dim->work);
			break;
		}
		/* fall through */
	case DIM_START_MEASURE:
		dim->state = DIM_MEASURE_IN_PROGRESS;
		dim_create_sample(end_sample.event_ctr, end_sample.pkt_ctr, end_sample.byte_ctr,
				end_sample.comp_ctr, &dim->start_sample);
		dim_create_sample(end_sample.event_ctr, end_sample.pkt_ctr, end_sample.byte_ctr,
				end_sample.comp_ctr, &dim->measuring_sample);
		break;
	case DIM_APPLY_NEW_PROFILE:
		break;
	}
}
EXPORT_SYMBOL(blk_dim);
