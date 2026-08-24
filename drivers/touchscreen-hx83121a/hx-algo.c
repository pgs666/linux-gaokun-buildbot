// SPDX-License-Identifier: GPL-2.0
/* Himax HX83121A algorithm state and wake handling. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

#define HIMAX_TRACK_MATCH_DIST2   (420 * 420)
#define HIMAX_TRACK_LOST_FRAMES   4
#define HIMAX_NEW_TOUCH_DEBOUNCE  1

void hx_algo_init_defaults(struct hx_algo *algo)
{
	/* Values track the current Windows solver defaults.  The baseline is
	 * adaptive per cell; using one immutable 0x7ffe value was a major source
	 * of weak-frame loss after temperature/VCOM/common-mode drift.
	 */
	algo->baseline_enabled              = true;
	algo->baseline_initial              = 0x7fee;
	algo->baseline_noise_deadband       = 90;
	algo->baseline_positive_deadband    = 14;
	algo->baseline_negative_deadband    = 13;
	algo->baseline_peak_threshold       = 305;
	algo->baseline_release_hold_frames  = 60;
	algo->baseline_positive_alpha_shift = 7;
	algo->baseline_negative_alpha_shift = 5;
	algo->baseline_noise_alpha_shift    = 6;
	algo->baseline_positive_max_step    = 20;
	algo->baseline_negative_max_step    = 20;
	algo->baseline_background_alpha_shift = 3;
	algo->baseline_no_finger_alpha_shift = 3;
	algo->baseline_recovery_alpha_shift = 2;
	algo->baseline_background_max_step = 512;
	algo->baseline_no_finger_max_step = 512;
	algo->baseline_recovery_max_step = 256;
	algo->baseline_recovery_max_frames = 30;
	algo->baseline_noise_tracking = true;
	algo->wake_stable_frames = 6;
	algo->wake_finger_safe_frames = 3;
	algo->wake_raw_jump_threshold = 160;
	algo->wake_max_unstable_nodes = 24;
	algo->wake_max_unstable_line_nodes = 12;
	algo->safe_commit_no_finger_frames = 30;
	algo->runtime_noise_threshold = 1800;
	algo->runtime_noise_line_nodes = 20;
	algo->runtime_noise_total_nodes = 180;
	algo->cmf_enabled        = true;
	algo->cmf_exclusion      = 2000;
	algo->cmf_max_correction = 2000;
	/* v1.1.2 removed GridIIR from the active pipeline.  Keep the compatible
	 * sysfs implementation available, but do not enable it by default.
	 */
	algo->iir_enabled        = false;
	algo->iir_decay_weight   = 200;
	algo->iir_decay_step     = 80;
	algo->iir_noise_floor    = 5;
	algo->iir_gate_floor     = 200;
	algo->iir_gate_ratio_q8  = 26;
	algo->macro_threshold    = 280;
	algo->peak_threshold     = 280;
	algo->peak_local_radius = 1;
	algo->peak_z8_enabled = true;
	algo->peak_saddle_enabled = true;
	algo->peak_saddle_radius = 2;
	algo->peak_saddle_drop = 80;
	algo->peak_signal_threshold_limit = 1000;
	algo->peak_edge_threshold = 300;
	algo->peak_macro_min_area = 3;
	algo->peak_continue_min_area = 1;
	algo->peak_continue_min_signal = 900;
	algo->peak_single_track_continue_min_signal = 650;
	algo->peak_continue_dist2 = 220 * 220;
	algo->peak_fast_start_min_signal = 1500;
	algo->peak_fast_start_edge_cells = 4;
	algo->palm_enabled       = true;
	algo->palm_area_threshold    = 50;
	algo->palm_signal_threshold  = 80000;
	algo->palm_density_low       = 400;
	algo->palm_box_enabled = true;
	algo->palm_box_expand_rows = 9;
	algo->palm_box_expand_cols = 10;
	algo->palm_box_match_distance = 6;
	algo->palm_box_max_hold = 0;
	algo->zone_cleanup_enabled = true;
	algo->zone_max_radius = 3;
	algo->zone_threshold_numer = 0x40;
	algo->zone_threshold_shift = 7;
	algo->pressure_enabled   = false;
	algo->edge_comp_enabled = true;
	algo->edge_boost_pct   = 50;   /* 50% signal boost on border pixels  */
	algo->edge_push_q8     = 128;  /* push up to 0.5 grid cells outward  */
	algo->edge_blend_q8    = 512;  /* blend over 2 grid cells from edge  */
	algo->edge_reject_enabled = true;
	algo->edge_reject_margin = 24;
	algo->edge_reject_min_signal = 500;
	algo->track_dist2_max   = HIMAX_TRACK_MATCH_DIST2;
	algo->track_lost_frames = HIMAX_TRACK_LOST_FRAMES;
	algo->debounce_base     = HIMAX_NEW_TOUCH_DEBOUNCE;
	algo->track_smoothing   = true;
	algo->track_active_guard   = true;
	algo->track_start_debounce = 1;
	algo->track_jump_dist2     = 0;  /* disabled by default */
	algo->hungarian_enabled = true;
	algo->debounce_weak_extra = 1;
	algo->debounce_edge_extra = 1;
	algo->debounce_strong_signal = 3000;
	algo->firmware_edge_fast_start = true;
	algo->split_peak_confirm_frames = 8;
	algo->split_peak_dist2 = 300 * 300;
	algo->split_cross_zone_confirm_frames = 4;
	algo->split_cross_zone_dist2 = 180 * 180;
	algo->track_peak_id_penalty = 40 * 40;
	algo->ghost_enabled = true;
	algo->ghost_row_distance = 32;
	algo->ghost_weak_ratio_q8 = 96;
	algo->ghost_min_col_distance = 300;
	algo->euro_enabled = true;
	algo->euro_alpha_min_q8 = 64;
	algo->euro_alpha_max_q8 = 224;
	algo->euro_speed_threshold = 24;
}

static void hx_algo_clear_transient_state(struct hx_algo *algo)
{
	/* Scratch/result arrays are guarded by their counts or cleared by the
	 * pipeline stage that consumes them.  Reset only persistent state here;
	 * bulk-clearing every backing array added latency without changing what
	 * the next frame can observe.
	 */
	memset(algo->frame, 0, sizeof(algo->frame));
	memset(algo->peak_competition, 0, sizeof(algo->peak_competition));
	memset(algo->baseline_release_hold, 0,
	       sizeof(algo->baseline_release_hold));
	memset(algo->tracks, 0, sizeof(algo->tracks));
	/* An invalid history makes the first frame after a runtime IIR enable
	 * seed the complete buffer before it is read.
	 */
	algo->iir_initialized = false;
	/* Preserve the converged per-cell baseline across display/lid/idle and
	 * hardware reinitialisation.  Force the next valid frame to re-evaluate
	 * recovery instead of inheriting a stale touch/freeze transition.
	 */
	algo->baseline_prev_had_signal = false;
	algo->baseline_had_freeze = algo->baseline_initialized;
	algo->baseline_recovery_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_frame_seq = 0;
	algo->diag_common_diff = 0;
	algo->diag_frame_max = 0;
	algo->diag_has_signal = 0;
	algo->diag_zones = 0;
	algo->diag_peaks = 0;
	algo->diag_contacts_pre_filter = 0;
	algo->diag_contacts_post_filter = 0;
	algo->diag_active_tracks = 0;
	algo->diag_reported_tracks = 0;
#endif
	algo->zone_arena_used = 0;
	algo->zone_count = 0;
	algo->peak_count = 0;
	algo->prev_peak_count = 0;
	algo->next_peak_id = 1;
	algo->contact_count = 0;
	algo->palm_box_count = 0;
	algo->touch_active = false;
	algo->touch_start_frames = 0;
	algo->firmware_finger_present = false;
	algo->fast_edge_start_pending = false;
}

void hx_algo_clear_live_state(struct hx_algo *algo)
{
	hx_algo_clear_transient_state(algo);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->live_clear_count++;
#endif
}

void hx_algo_full_reset(struct hx_algo *algo)
{
	hx_algo_clear_transient_state(algo);
	memset(algo->baseline_q8, 0, sizeof(algo->baseline_q8));
	memset(algo->safe_baseline_q8, 0, sizeof(algo->safe_baseline_q8));
	memset(algo->wake_candidate_q8, 0,
	       sizeof(algo->wake_candidate_q8));
	algo->baseline_initialized = false;
	algo->safe_baseline_valid = false;
	algo->wake_qualifying = false;
	algo->wake_candidate_valid = false;
	algo->wake_needs_double_confirm = false;
	algo->wake_candidate_frames = 0;
	algo->wake_finger_frames = 0;
	algo->safe_no_finger_frames = 0;
	algo->baseline_prev_had_signal = false;
	algo->baseline_had_freeze = false;
	algo->baseline_recovery_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->baseline_generation++;
	algo->full_reset_count++;
#endif
}

void hx_copy_raw_to_baseline(s32 *dst, const u16 *raw)
{
	int i;

	for (i = 0; i < HX_PIXELS; i++)
		dst[i] = (s32)le16_to_cpup(raw + i) <<
			 HX_BASELINE_FRACTION_BITS;
}

void hx_algo_begin_wake(struct hx_algo *algo)
{
	hx_algo_clear_transient_state(algo);
	algo->wake_qualifying = true;
	algo->wake_candidate_valid = false;
	algo->wake_needs_double_confirm = false;
	algo->wake_candidate_frames = 0;
	algo->wake_finger_frames = 0;
	algo->safe_no_finger_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->live_clear_count++;
#endif
}

int hx_algo_qualify_wake_frame(struct hx_algo *algo, const u16 *raw,
			       enum hx_finger_state finger_state)
{
	u8 row_unstable[HX_ROWS] = { 0 };
	u8 col_unstable[HX_COLS] = { 0 };
	u16 out_of_range = 0;
	u16 unstable = 0;
	u8 required_frames;
	int i;

	if (!algo->wake_qualifying)
		return HX_WAKE_QUALITY_READY;
	for (i = 1; i < HX_PIXELS; i++) {
		u16 sample = le16_to_cpup(raw + i);

		if (sample < 0x1000 || sample > 0xf000)
			out_of_range++;
	}
	if (out_of_range > algo->wake_max_unstable_nodes) {
		algo->wake_candidate_valid = false;
		algo->wake_candidate_frames = 0;
		algo->wake_needs_double_confirm = false;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->wake_candidate_reject_count++;
#endif
		return HX_WAKE_QUALITY_REJECTED;
	}

	/* A finger already present at screen-on must never be learned into a new
	 * baseline.  Use the last independently committed no-finger snapshot.
	 */
	if (finger_state == HX_FINGER_PRESENT) {
		algo->wake_candidate_valid = false;
		algo->wake_candidate_frames = 0;
		algo->wake_needs_double_confirm = false;
		if (algo->wake_finger_frames < U8_MAX)
			algo->wake_finger_frames++;
		if (algo->wake_finger_frames >= algo->wake_finger_safe_frames) {
			if (algo->safe_baseline_valid) {
				memcpy(algo->baseline_q8, algo->safe_baseline_q8,
				       sizeof(algo->baseline_q8));
			} else if (!algo->baseline_initialized) {
				for (i = 0; i < HX_PIXELS; i++)
					algo->baseline_q8[i] =
						(s32)algo->baseline_initial <<
						HX_BASELINE_FRACTION_BITS;
			}
			algo->baseline_initialized = true;
			algo->wake_qualifying = false;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->wake_safe_fallback_count++;
			algo->wake_qualification_count++;
#endif
			return algo->safe_baseline_valid ?
				HX_WAKE_QUALITY_USING_SAFE :
				HX_WAKE_QUALITY_PROTECTED;
		}
		return HX_WAKE_QUALITY_PENDING;
	}
	algo->wake_finger_frames = 0;

	if (!algo->wake_candidate_valid) {
		hx_copy_raw_to_baseline(algo->wake_candidate_q8, raw);
		algo->wake_candidate_valid = true;
		algo->wake_candidate_frames = 1;
		return HX_WAKE_QUALITY_PENDING;
	}

	/* A candidate is only committed after several complete raw grids agree.
	 * Count spatially-local changes; a panel-wide DC shift is naturally
	 * represented by the first candidate and is not confused with activity.
	 */
	for (i = 1; i < HX_PIXELS; i++) {
		s32 sample_q8 = (s32)le16_to_cpup(raw + i) <<
				HX_BASELINE_FRACTION_BITS;
		s32 delta = (sample_q8 - algo->wake_candidate_q8[i]) >>
			    HX_BASELINE_FRACTION_BITS;

		if (abs(delta) > algo->wake_raw_jump_threshold) {
			row_unstable[i / HX_COLS]++;
			col_unstable[i % HX_COLS]++;
			unstable++;
		}
	}
	for (i = 0; i < HX_ROWS; i++)
		if (row_unstable[i] >= algo->wake_max_unstable_line_nodes)
			unstable = algo->wake_max_unstable_nodes + 1;
	for (i = 0; i < HX_COLS; i++)
		if (col_unstable[i] >= algo->wake_max_unstable_line_nodes)
			unstable = algo->wake_max_unstable_nodes + 1;
	if (unstable > algo->wake_max_unstable_nodes) {
		/* Start the next qualification window from the newest complete frame.
		 * Do not poison either the working or last-safe baseline.
		 */
		hx_copy_raw_to_baseline(algo->wake_candidate_q8, raw);
		algo->wake_candidate_frames = 1;
		algo->wake_needs_double_confirm = false;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->wake_candidate_reject_count++;
#endif
		return HX_WAKE_QUALITY_REJECTED;
	}

	if (algo->wake_candidate_frames < U8_MAX)
		algo->wake_candidate_frames++;
	if (algo->wake_candidate_frames < algo->wake_stable_frames)
		return HX_WAKE_QUALITY_PENDING;

	/* A candidate that differs spatially from last-safe is not rejected just
	 * because the display environment genuinely changed.  It must, however,
	 * survive a second complete stability window before replacing last-safe.
	 */
	if (algo->safe_baseline_valid && !algo->wake_needs_double_confirm) {
		s64 sum = 0;
		s32 common;
		u16 divergent = 0;

		for (i = 1; i < HX_PIXELS; i++)
			sum += (algo->wake_candidate_q8[i] -
				algo->safe_baseline_q8[i]) >>
				HX_BASELINE_FRACTION_BITS;
		common = (s32)(sum / (HX_PIXELS - 1));
		for (i = 1; i < HX_PIXELS; i++) {
			s32 delta = ((algo->wake_candidate_q8[i] -
				algo->safe_baseline_q8[i]) >>
				HX_BASELINE_FRACTION_BITS) - common;

			if (abs(delta) > algo->wake_raw_jump_threshold * 2)
				divergent++;
		}
		if (divergent > algo->wake_max_unstable_nodes) {
			algo->wake_needs_double_confirm = true;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->wake_safe_divergence_count++;
#endif
		}
	}
	required_frames = algo->wake_stable_frames;
	if (algo->wake_needs_double_confirm)
		required_frames = min_t(u8, algo->wake_stable_frames * 2,
					 U8_MAX);
	if (algo->wake_candidate_frames < required_frames)
		return HX_WAKE_QUALITY_PENDING;

	memcpy(algo->baseline_q8, algo->wake_candidate_q8,
	       sizeof(algo->baseline_q8));
	memcpy(algo->safe_baseline_q8, algo->wake_candidate_q8,
	       sizeof(algo->safe_baseline_q8));
	algo->baseline_initialized = true;
	algo->safe_baseline_valid = true;
	algo->wake_qualifying = false;
	algo->wake_candidate_valid = false;
	algo->wake_needs_double_confirm = false;
	algo->baseline_prev_had_signal = false;
	algo->baseline_had_freeze = false;
	algo->baseline_recovery_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->baseline_generation++;
	algo->wake_baseline_commit_count++;
	algo->wake_qualification_count++;
#endif
	return HX_WAKE_QUALITY_READY;
}

bool hx_algo_is_exception_frame(struct hx_algo *algo, const u16 *raw)
{
	u8 col_bad[HX_COLS] = { 0 };
	s64 sum = 0;
	s32 common;
	u16 total = 0;
	int r, c;

	if (!algo->baseline_initialized || algo->wake_qualifying)
		return false;
	for (r = 1; r < HX_PIXELS; r++)
		sum += (s32)le16_to_cpup(raw + r) -
		       (algo->baseline_q8[r] >> HX_BASELINE_FRACTION_BITS);
	common = (s32)(sum / (HX_PIXELS - 1));

	for (r = 0; r < HX_ROWS; r++) {
		u8 row_bad = 0;

		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			s32 local;

			if (!idx)
				continue;
			local = (s32)le16_to_cpup(raw + idx) -
				(algo->baseline_q8[idx] >>
				 HX_BASELINE_FRACTION_BITS) - common;
			if (abs(local) >= algo->runtime_noise_threshold) {
				row_bad++;
				col_bad[c]++;
				total++;
			}
		}
		if (row_bad >= algo->runtime_noise_line_nodes)
			goto exception;
	}
	for (c = 0; c < HX_COLS; c++)
		if (col_bad[c] >= algo->runtime_noise_line_nodes)
			goto exception;
	if (total < algo->runtime_noise_total_nodes)
		return false;

exception:
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->noise_frame_hold_count++;
#endif
	return true;
}

#ifdef HX_ALGO_HOST_TEST
void hx_algo_reset_runtime(struct hx_algo *algo)
{
	hx_algo_clear_live_state(algo);
}
#endif

/* ======================================================================== */
/* Phase 1A — baseline subtraction                                          */
/* ======================================================================== */
