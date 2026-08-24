// SPDX-License-Identifier: GPL-2.0
/* Himax HX83121A baseline and frame preprocessing. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

static s32 hx_baseline_step_q8(s32 delta, u8 shift, s16 max_step)
{
	s32 limit_q8 = max_t(s32, max_step, 0) *
			(1 << HX_BASELINE_FRACTION_BITS);
	s32 step_q8;

	if (!delta)
		return 0;
	step_q8 = delta * (1 << HX_BASELINE_FRACTION_BITS);
	step_q8 >>= min_t(u8, shift, 30);
	return clamp_t(s32, step_q8, -limit_q8, limit_q8);
}

static void hx_prepare_frame_baseline(struct hx_algo *algo, const u16 *raw,
				      enum hx_finger_state finger_state)
{
	s64 common_sum = 0;
	s32 common_diff;
	s32 max_delta = INT_MIN;
	int common_bin = 0;
	int common_count = 0;
	int cumulative = 0;
	bool has_signal = false;
	bool recovery;
	bool found_freeze = false;
	int r, c;

	if (!algo->baseline_initialized) {
		/* Match Windows BLIIR_Reset: use a real no-finger raw snapshot.  If
		 * the first usable frame already carries a finger, retain the neutral
		 * fallback so the contact cannot disappear into the baseline.
		 */
		if (finger_state != HX_FINGER_ABSENT) {
			for (r = 0; r < HX_PIXELS; r++)
				algo->baseline_q8[r] =
					(s32)algo->baseline_initial <<
					HX_BASELINE_FRACTION_BITS;
		} else {
			hx_copy_raw_to_baseline(algo->baseline_q8, raw);
		}
		memset(algo->baseline_release_hold, 0,
		       sizeof(algo->baseline_release_hold));
		algo->baseline_initialized = true;
	}
	memset(algo->baseline_hist, 0, sizeof(algo->baseline_hist));

	/* Estimate panel-wide VCOM/temperature drift before classifying cells. */
	for (r = 1; r < HX_PIXELS; r++) {
		s32 sample = (s32)le16_to_cpup(raw + r);
		s32 baseline = algo->baseline_q8[r] >>
				HX_BASELINE_FRACTION_BITS;
		s32 delta = sample - baseline;
		int bin = (delta + 65536) >> 6;

		algo->baseline_hist[clamp_t(int, bin, 0,
			HX_BASELINE_HIST_BINS - 1)]++;
	}
	for (common_bin = 0; common_bin < HX_BASELINE_HIST_BINS;
	     common_bin++) {
		cumulative += algo->baseline_hist[common_bin];
		if (cumulative >= HX_PIXELS / 2)
			break;
	}
	common_bin = min(common_bin, HX_BASELINE_HIST_BINS - 1);
	for (r = 1; r < HX_PIXELS; r++) {
		s32 sample = (s32)le16_to_cpup(raw + r);
		s32 baseline = algo->baseline_q8[r] >>
				HX_BASELINE_FRACTION_BITS;
		s32 delta = sample - baseline;
		int bin = clamp_t(int, (delta + 65536) >> 6, 0,
				      HX_BASELINE_HIST_BINS - 1);

		max_delta = max(max_delta, delta);
		if (bin == common_bin) {
			common_sum += delta;
			common_count++;
		}
	}
	common_diff = common_count ? (s32)(common_sum / common_count) : 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_common_diff = common_diff;
#endif
	if (finger_state == HX_FINGER_PRESENT)
		has_signal = true;
	/* Even a validated firmware "no finger" bit may be wrong during display
	 * interference.  Never let a contradictory raw peak be learned quickly.
	 */
	if (!has_signal) {
		/* max_delta is collected while locating the common-mode bin.  This
		 * avoids a third full-grid pass on every firmware no-finger frame.
		 */
		has_signal = max_delta - common_diff >=
			algo->baseline_peak_threshold;
		for (r = 0; r < HIMAX_MAX_TOUCH; r++)
			if (algo->tracks[r].active) {
				has_signal = true;
				break;
			}
	}

	recovery = has_signal && (!algo->baseline_prev_had_signal ||
				    !algo->baseline_had_freeze) &&
		   algo->baseline_recovery_frames <
				algo->baseline_recovery_max_frames;
	if (recovery)
		algo->baseline_recovery_frames++;
	else if (!has_signal || algo->baseline_had_freeze)
		algo->baseline_recovery_frames = 0;
	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			s32 sample = (s32)le16_to_cpup(raw + idx);
			s32 baseline = algo->baseline_q8[idx] >>
					HX_BASELINE_FRACTION_BITS;
			s32 delta = sample - baseline;
			s32 local = delta - common_diff;
			s32 step_q8 = 0;

			if (!algo->baseline_enabled) {
				delta = sample - algo->baseline_initial;
				goto store;
			}

			/* Freeze cells carrying a finger-sized positive signal.  A hold
			 * after release prevents the negative rebound from being absorbed
			 * into the baseline and creating a later false lift.
			 */
			if (has_signal && local >= algo->baseline_peak_threshold) {
				found_freeze = true;
				algo->baseline_release_hold[idx] =
					algo->baseline_release_hold_frames;
				/* Frozen cells still follow global VCOM drift. */
				step_q8 = hx_baseline_step_q8(common_diff,
					algo->baseline_background_alpha_shift,
					algo->baseline_background_max_step);
				algo->baseline_q8[idx] = clamp_t(s32,
					algo->baseline_q8[idx] + step_q8, 0,
					0xffff << HX_BASELINE_FRACTION_BITS);
				delta = local;
				goto store;
			}
			if (algo->baseline_release_hold[idx]) {
				algo->baseline_release_hold[idx]--;
				if (local < -algo->baseline_negative_deadband) {
					delta = local;
					goto store;
				}
				delta = 0;
				goto store;
			}

			if (!has_signal) {
				if (abs(delta) <= algo->baseline_noise_deadband &&
				    algo->baseline_noise_tracking)
					step_q8 = hx_baseline_step_q8(delta,
						algo->baseline_noise_alpha_shift, 1);
				else
					step_q8 = hx_baseline_step_q8(delta,
						algo->baseline_no_finger_alpha_shift,
						algo->baseline_no_finger_max_step);
			} else if (recovery) {
				step_q8 = hx_baseline_step_q8(delta,
					algo->baseline_recovery_alpha_shift,
					algo->baseline_recovery_max_step);
			} else if (abs(delta) <= algo->baseline_noise_deadband &&
				   algo->baseline_noise_tracking)
				step_q8 = hx_baseline_step_q8(delta,
					algo->baseline_noise_alpha_shift, 1);
			else if (delta > algo->baseline_positive_deadband)
				step_q8 = hx_baseline_step_q8(delta,
					algo->baseline_positive_alpha_shift,
					algo->baseline_positive_max_step);
			else if (delta < -algo->baseline_negative_deadband)
				step_q8 = hx_baseline_step_q8(delta,
					algo->baseline_negative_alpha_shift,
					algo->baseline_negative_max_step);

			algo->baseline_q8[idx] = clamp_t(s32,
				algo->baseline_q8[idx] + step_q8, 0,
				0xffff << HX_BASELINE_FRACTION_BITS);

			/* Match v1.1.2 ProcessNoFinger/ProcessFinger background
			 * semantics: only frozen candidate cells reach the solver.
			 * Passing every local residual fills the fixed zone arena with
			 * background islands and can evict a real finger by scan order.
			 */
			delta = 0;
store:
			algo->frame[r][c] = clamp_t(s32, delta, SHRT_MIN, SHRT_MAX);
		}
	}
	algo->baseline_prev_had_signal = has_signal;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_has_signal = has_signal;
#endif
	algo->baseline_had_freeze = found_freeze;
	if (finger_state == HX_FINGER_ABSENT && !has_signal && !found_freeze) {
		u8 row_unstable[HX_ROWS] = { 0 };
		u8 col_unstable[HX_COLS] = { 0 };
		u16 unstable = 0;

		if (!algo->safe_no_finger_frames) {
			hx_copy_raw_to_baseline(algo->wake_candidate_q8, raw);
			algo->safe_no_finger_frames = 1;
		} else {
			for (r = 1; r < HX_PIXELS; r++) {
				s32 candidate = algo->wake_candidate_q8[r] >>
						HX_BASELINE_FRACTION_BITS;
				s32 sample = (s32)le16_to_cpup(raw + r);

				if (abs(sample - candidate) >
				    algo->wake_raw_jump_threshold) {
					row_unstable[r / HX_COLS]++;
					col_unstable[r % HX_COLS]++;
					unstable++;
				}
			}
			for (r = 0; r < HX_ROWS; r++)
				if (row_unstable[r] >=
				    algo->wake_max_unstable_line_nodes)
					unstable = algo->wake_max_unstable_nodes + 1;
			for (r = 0; r < HX_COLS; r++)
				if (col_unstable[r] >=
				    algo->wake_max_unstable_line_nodes)
					unstable = algo->wake_max_unstable_nodes + 1;
			if (unstable > algo->wake_max_unstable_nodes) {
				hx_copy_raw_to_baseline(algo->wake_candidate_q8,
							raw);
				algo->safe_no_finger_frames = 1;
			} else if (algo->safe_no_finger_frames < U8_MAX) {
				algo->safe_no_finger_frames++;
			}
		}
		if (algo->safe_no_finger_frames >=
		    algo->safe_commit_no_finger_frames) {
			memcpy(algo->safe_baseline_q8, algo->wake_candidate_q8,
			       sizeof(algo->safe_baseline_q8));
			algo->safe_baseline_valid = true;
			algo->safe_no_finger_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->baseline_safe_commit_count++;
#endif
		}
	} else {
		algo->safe_no_finger_frames = 0;
	}

	/* pixel [0][0] is always invalid on this panel layout */
	algo->frame[0][0] = 0;
}

/* ======================================================================== */
/* Phase 1A½ — edge signal boost                                            */
/*                                                                           */
/* Compensate reduced capacitive sensitivity at sensor borders by scaling   */
/* border pixels upward.  Row 0/last and col 0/last get the full boost;    */
/* row 1/last-1 and col 1/last-1 get half.  Corner pixels (on two borders) */
/* are boosted once from each axis (multiplicative).                         */
/* ======================================================================== */

static void hx_edge_boost(struct hx_algo *algo)
{
	int r, c;
	s32 pct = algo->edge_boost_pct;
	s32 half_pct = pct / 2;

	if (!algo->edge_comp_enabled || pct <= 0)
		return;

	/* Boost border rows: row 0 and row HX_ROWS-1 (full), row 1 and HX_ROWS-2 (half) */
	for (c = 0; c < HX_COLS; c++) {
		s32 v;

		/* Top edge */
		v = algo->frame[0][c];
		if (v > 0)
			algo->frame[0][c] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[1][c];
		if (v > 0)
			algo->frame[1][c] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);

		/* Bottom edge */
		v = algo->frame[HX_ROWS - 1][c];
		if (v > 0)
			algo->frame[HX_ROWS - 1][c] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[HX_ROWS - 2][c];
		if (v > 0)
			algo->frame[HX_ROWS - 2][c] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);
	}

	/* Boost border columns: col 0 and col HX_COLS-1 (full), col 1 and HX_COLS-2 (half) */
	for (r = 0; r < HX_ROWS; r++) {
		s32 v;

		/* Left edge */
		v = algo->frame[r][0];
		if (v > 0)
			algo->frame[r][0] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[r][1];
		if (v > 0)
			algo->frame[r][1] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);

		/* Right edge */
		v = algo->frame[r][HX_COLS - 1];
		if (v > 0)
			algo->frame[r][HX_COLS - 1] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[r][HX_COLS - 2];
		if (v > 0)
			algo->frame[r][HX_COLS - 2] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);
	}
}

/* ======================================================================== */
/* Phase 1B — CMF (Common Mode Filter)                                      */
/*                                                                           */
/* Removes charger-induced common-mode noise by subtracting per-row and     */
/* per-column offsets computed from "quiet" pixels (|val| < exclusion).     */
/* DualDim mode: rows first, then columns.                                   */
/* ======================================================================== */

static void hx_apply_cmf(struct hx_algo *algo)
{
	int r, c;

	/* Row pass */
	for (r = 0; r < HX_ROWS; r++) {
		s32 sum = 0, count = 0, offset;

		for (c = 0; c < HX_COLS; c++) {
			s16 v = algo->frame[r][c];

			if (abs((int)v) < algo->cmf_exclusion) {
				sum += v;
				count++;
			}
		}
		if (!count)
			continue;

		offset = clamp_t(s32, sum / count,
				 -algo->cmf_max_correction,
				  algo->cmf_max_correction);
		for (c = 0; c < HX_COLS; c++) {
			s32 corrected = (s32)algo->frame[r][c] - offset;

			algo->frame[r][c] = clamp_t(s32, corrected, SHRT_MIN, SHRT_MAX);
		}
	}

	/* Column pass */
	for (c = 0; c < HX_COLS; c++) {
		s32 sum = 0, count = 0, offset;

		for (r = 0; r < HX_ROWS; r++) {
			s16 v = algo->frame[r][c];

			if (abs((int)v) < algo->cmf_exclusion) {
				sum += v;
				count++;
			}
		}
		if (!count)
			continue;

		offset = clamp_t(s32, sum / count,
				 -algo->cmf_max_correction,
				  algo->cmf_max_correction);
		for (r = 0; r < HX_ROWS; r++) {
			s32 corrected = (s32)algo->frame[r][c] - offset;

			algo->frame[r][c] = clamp_t(s32, corrected, SHRT_MIN, SHRT_MAX);
		}
	}
}

/* ======================================================================== */
/* Phase 1C — GridIIR temporal filter                                       */
/*                                                                           */
/* Per-pixel exponential decay for noise suppression.  Pixels above a       */
/* dynamic threshold (proportional to the frame maximum) bypass the filter  */
/* so real touch signals are never attenuated.                               */
/* ======================================================================== */

static void hx_apply_iir(struct hx_algo *algo)
{
	int r, c;
	s32 frame_max = 0;
	s32 dyn_threshold;
	u16 decay_weight, decay_step;

	if (!algo->iir_enabled) {
		/* Do not maintain an unused 2400-cell history at frame rate.  Marking
		 * it invalid makes a later runtime enable seed from its first frame.
		 */
		algo->iir_initialized = false;
		return;
	}

	if (!algo->iir_initialized) {
		memcpy(algo->iir_history, algo->frame, sizeof(algo->frame));
		algo->iir_initialized = true;
		return;
	}

	for (r = 0; r < HX_ROWS; r++)
		for (c = 0; c < HX_COLS; c++)
			frame_max = max(frame_max, abs((int)algo->frame[r][c]));

	dyn_threshold = max((frame_max * algo->iir_gate_ratio_q8) >> 8,
			    (s32)algo->iir_gate_floor);
	decay_weight  = min_t(u16, algo->iir_decay_weight, 256);
	decay_step    = algo->iir_decay_step;

	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			s32 cur = algo->frame[r][c];
			s32 output;

			if (cur >= dyn_threshold) {
				output = cur;
			} else {
				s32 hist  = algo->iir_history[r][c];
				s32 mixed = decay_weight * cur +
					    (256 - decay_weight) * hist;

				output = mixed >> 8;
				output = max(0, output - (s32)decay_step);
				if (output < algo->iir_noise_floor)
					output = 0;
			}

			algo->frame[r][c]       = clamp_t(s32, output, SHRT_MIN, SHRT_MAX);
			algo->iir_history[r][c] = algo->frame[r][c];
		}
	}
}

/* ======================================================================== */
/* Phase 1 entry point                                                       */
/* ======================================================================== */

#ifdef HX_ALGO_HOST_TEST
void hx_preprocess_frame(struct hx_algo *algo, const u16 *raw)
{
	hx_preprocess_frame_state(algo, raw, HX_FINGER_UNKNOWN);
}
#endif

void hx_preprocess_frame_state(struct hx_algo *algo, const u16 *raw,
			       enum hx_finger_state finger_state)
{
	hx_prepare_frame_baseline(algo, raw, finger_state);

	if (algo->cmf_enabled)
		hx_apply_cmf(algo);

	hx_edge_boost(algo);

	hx_apply_iir(algo);
}

/*
 * Clamp-to-zero accessor: returns 0 for out-of-bounds or negative values so
 * neighbour lookups near the grid edge never need special-casing.
 */
