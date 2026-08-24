/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_ALGO_INTERNAL_H
#define HX_ALGO_INTERNAL_H

#include "hx-algo.h"

#define HX_BASELINE_FRACTION_BITS 8

void hx_copy_raw_to_baseline(s32 *baseline, const u16 *raw);
s64 hx_dist2_predicted(const struct input_mt_pos *position,
			const struct hx_track *track);

static inline s16 hx_frame_at(const struct hx_algo *algo, int row, int col)
{
	s16 value;

	if (row < 0 || row >= HX_ROWS || col < 0 || col >= HX_COLS)
		return 0;
	value = algo->frame[row][col];
	return value > 0 ? value : 0;
}

#endif /* HX_ALGO_INTERNAL_H */
