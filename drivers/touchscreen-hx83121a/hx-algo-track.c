// SPDX-License-Identifier: GPL-2.0
/* Himax HX83121A contact assignment and slot tracking. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

s64 hx_dist2_predicted(const struct input_mt_pos *a,
			const struct hx_track *b)
{
	s32 frames_ahead = max_t(s32, 1, (s32)b->missed + 1);
	s32 pred_x = b->x + b->vx * frames_ahead;
	s32 pred_y = b->y + b->vy * frames_ahead;
	s32 dx = a->x - pred_x;
	s32 dy = a->y - pred_y;

	return (s64)dx * dx + (s64)dy * dy;
}

/* A fast swipe can cover several times the immediately preceding step when a
 * weak corner frame briefly pins the centroid.  For a competing peak-ID
 * handoff, measure distance to a short forward motion ray rather than only to
 * the one-step prediction.  This is never used to enlarge the match gate.
 */
static s64 hx_dist2_motion_ray(const struct input_mt_pos *a,
				const struct hx_track *b)
{
	s64 best = hx_dist2_predicted(a, b);
	int step;

	if (!b->vx && !b->vy)
		return best;
	for (step = 2; step <= 4; step++) {
		s32 dx = a->x - (b->x + b->vx * step);
		s32 dy = a->y - (b->y + b->vy * step);
		s64 d2 = (s64)dx * dx + (s64)dy * dy;

		if (d2 < best)
			best = d2;
	}
	return best;
}

static void hx_reset_track(struct hx_track *trk)
{
	memset(trk, 0, sizeof(*trk));
}

static void hx_filter_track(struct hx_algo *algo, struct hx_track *trk,
			    const struct input_mt_pos *det)
{
	s32 dx = det->x - trk->x;
	s32 dy = det->y - trk->y;
	s32 speed = max(abs(dx), abs(dy));
	s32 alpha;

	trk->vx = dx;
	trk->vy = dy;
	if (!algo->track_smoothing || !algo->euro_enabled) {
		trk->x = det->x;
		trk->y = det->y;
		trk->filtered_x_q8 = det->x << 8;
		trk->filtered_y_q8 = det->y << 8;
		return;
	}

	alpha = min(algo->euro_alpha_min_q8, algo->euro_alpha_max_q8);
	if (algo->euro_speed_threshold)
		alpha += (s32)(max(algo->euro_alpha_min_q8,
				 algo->euro_alpha_max_q8) - alpha) *
			 min_t(s32, speed, algo->euro_speed_threshold) /
			 algo->euro_speed_threshold;
	alpha = clamp_t(s32, alpha, 1, 255);
	trk->deriv_x_q8 += (alpha * ((dx << 8) - trk->deriv_x_q8)) >> 8;
	trk->deriv_y_q8 += (alpha * ((dy << 8) - trk->deriv_y_q8)) >> 8;
	trk->filtered_x_q8 +=
		(alpha * ((det->x << 8) - trk->filtered_x_q8)) >> 8;
	trk->filtered_y_q8 +=
		(alpha * ((det->y << 8) - trk->filtered_y_q8)) >> 8;
	trk->x = trk->filtered_x_q8 >> 8;
	trk->y = trk->filtered_y_q8 >> 8;
}

static void hx_hungarian_assign(struct hx_algo *algo, const u8 *active,
				int active_cnt, int det_cnt, s8 *match)
{
	const s64 inf = (s64)1 << 55;
	int cols = det_cnt + active_cnt;
	int i, j;

	memset(algo->assign_u, 0, (active_cnt + 1) * sizeof(algo->assign_u[0]));
	memset(algo->assign_v, 0, (cols + 1) * sizeof(algo->assign_v[0]));
	memset(algo->assign_p, 0, (cols + 1) * sizeof(algo->assign_p[0]));
	memset(algo->assign_way, 0, (cols + 1) * sizeof(algo->assign_way[0]));
	for (i = 1; i <= active_cnt; i++) {
		s64 minv[HX_ASSIGN_COLS + 1];
		bool used[HX_ASSIGN_COLS + 1] = { false };
		int j0 = 0;

		for (j = 0; j <= cols; j++)
			minv[j] = inf;
		algo->assign_p[0] = i;
		do {
			int i0, j1 = 0;
			s64 delta = inf;

			used[j0] = true;
			i0 = algo->assign_p[j0];
			for (j = 1; j <= cols; j++) {
				s64 cur;

				if (used[j])
					continue;
				cur = algo->assign_cost[i0 - 1][j - 1] -
					algo->assign_u[i0] - algo->assign_v[j];
				if (cur < minv[j]) {
					minv[j] = cur;
					algo->assign_way[j] = j0;
				}
				if (minv[j] < delta) {
					delta = minv[j];
					j1 = j;
				}
			}
			for (j = 0; j <= cols; j++) {
				if (used[j]) {
					algo->assign_u[algo->assign_p[j]] += delta;
					algo->assign_v[j] -= delta;
				} else {
					minv[j] -= delta;
				}
			}
			j0 = j1;
		} while (algo->assign_p[j0]);
		do {
			int j1 = algo->assign_way[j0];

			algo->assign_p[j0] = algo->assign_p[j1];
			j0 = j1;
		} while (j0);
	}

	for (i = 0; i < HIMAX_MAX_TOUCH; i++)
		match[i] = -1;
	for (j = 1; j <= det_cnt; j++) {
		int row = algo->assign_p[j];

		if (row && algo->assign_cost[row - 1][j - 1] < inf / 2)
			match[active[row - 1]] = j - 1;
	}
}

static bool hx_is_unconfirmed_split(struct hx_algo *algo,
				    const struct input_mt_pos *det, int candidate,
				    const u8 *active, int active_cnt,
				    const s8 *match, const u8 *prior_peak_id)
{
	const struct hx_contact *contact;
	int reported_tracks = 0;
	int i;

	if (candidate >= algo->contact_count)
		return false;
	contact = &algo->contacts[candidate];
	for (i = 0; i < active_cnt; i++)
		if (algo->tracks[active[i]].reported)
			reported_tracks++;

	for (i = 0; i < active_cnt; i++) {
		int ti = active[i];
		int di = match[ti];
		bool same_zone;
		bool handoff_residual;
		bool continuing_residual = false;
		u8 confirm_frames;
		s32 confirm_dist2;
		s32 dx, dy;

		if (di < 0 || di >= algo->contact_count ||
		    !algo->tracks[ti].reported || di == candidate)
			continue;
		same_zone = contact->source_zone_index ==
			algo->contacts[di].source_zone_index;
		handoff_residual = prior_peak_id[ti] &&
			contact->source_peak_id == prior_peak_id[ti] &&
			algo->contacts[di].source_peak_id &&
			algo->contacts[di].source_peak_id != prior_peak_id[ti];
		for (int k = 0; k < HX_MAX_PEAKS; k++)
			if (contact->source_peak_id &&
			    algo->peak_competition[k].peak_id ==
				contact->source_peak_id) {
				continuing_residual =
					algo->peak_competition[k].handoff_residual;
				break;
			}
		/* Separate macro zones can still be two lobes of one moving finger.
		 * Apply the shorter cross-zone guard only to a single reported touch;
		 * established multi-touch must not acquire extra latency.
		 */
		if (!handoff_residual && !continuing_residual && !same_zone &&
		    reported_tracks != 1)
			continue;
		confirm_frames = (same_zone || handoff_residual ||
			continuing_residual) ?
			algo->split_peak_confirm_frames :
			algo->split_cross_zone_confirm_frames;
		confirm_dist2 = same_zone ? algo->split_peak_dist2 :
			algo->split_cross_zone_dist2;
		if (confirm_frames <= 1)
			continue;
		dx = det[candidate].x - det[di].x;
		dy = det[candidate].y - det[di].y;
		if (handoff_residual || continuing_residual ||
		    (s64)dx * dx + (s64)dy * dy <=
		    max_t(s32, confirm_dist2, 1)) {
			u8 competition_age = 1;
			int k, free_slot = -1;

			/* Count from the first frame this peak is an unmatched
			 * competitor, not from its absolute peak lifetime.
			 */
			for (k = 0; k < HX_MAX_PEAKS; k++) {
				struct hx_peak_competition *pc =
					&algo->peak_competition[k];

				if (pc->peak_id == contact->source_peak_id &&
				    contact->source_peak_id) {
					if (!pc->seen && pc->age < U8_MAX)
						pc->age++;
					pc->seen = true;
					pc->handoff_residual |= handoff_residual;
					competition_age = pc->age;
					break;
				}
				if (free_slot < 0 && (!pc->peak_id || !pc->seen))
					free_slot = k;
			}
			if (k == HX_MAX_PEAKS && contact->source_peak_id &&
			    free_slot >= 0) {
				struct hx_peak_competition *pc =
					&algo->peak_competition[free_slot];

				pc->peak_id = contact->source_peak_id;
				pc->age = 1;
				pc->seen = true;
				pc->handoff_residual = handoff_residual;
			}
			if (competition_age >= confirm_frames)
				continue;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->diag_split_peak_deferred++;
			if (!same_zone)
				algo->diag_cross_zone_split_deferred++;
			if (handoff_residual || continuing_residual)
				algo->diag_handoff_residual_deferred++;
#endif
			return true;
		}
	}
	return false;
}

void hx_track_contacts(struct hx_algo *algo,
		       struct input_mt_pos *det, int det_cnt)
{
	bool det_used[HIMAX_MAX_TOUCH] = { false };
	bool target_has_regular[HIMAX_MAX_TOUCH] = { false };
	bool target_has_same_peak[HIMAX_MAX_TOUCH] = { false };
	u8 prior_peak_id[HIMAX_MAX_TOUCH] = { 0 };
	u8 active[HIMAX_MAX_TOUCH];
	s8 match[HIMAX_MAX_TOUCH];
	u16  jump_released = 0;  /* bitmask: slots freed by jump detection */
	int active_cnt = 0;
	int i, j;
	const s64 inf = (s64)1 << 55;

	for (i = 0; i < HX_MAX_PEAKS; i++)
		algo->peak_competition[i].seen = false;

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo->tracks[i].active) {
			prior_peak_id[i] = algo->tracks[i].source_peak_id;
			active[active_cnt++] = i;
		}
	}
	/* A regular detection always wins over a continuation-only fallback for
	 * the same slot.  Otherwise Hungarian could consume the tiny split peak
	 * for the old slot and let the regular peak bootstrap a duplicate slot.
	 */
	for (i = 0; i < active_cnt; i++) {
		int ti = active[i];
		struct hx_track *trk = &algo->tracks[ti];
		s64 gate2 = max_t(s64, algo->track_dist2_max, 1);

		if (trk->missed)
			gate2 *= min_t(s32, (s32)trk->missed + 1, 4);
		for (j = 0; j < det_cnt; j++) {
			struct hx_contact *ct = j < algo->contact_count ?
				&algo->contacts[j] : NULL;

			if (j < algo->contact_count &&
			    algo->contacts[j].continuation_only)
				continue;
			if (hx_dist2_predicted(&det[j], trk) <= gate2) {
				target_has_regular[ti] = true;
				if (ct && trk->source_peak_id &&
				    ct->source_peak_id == trk->source_peak_id)
					target_has_same_peak[ti] = true;
			}
		}
	}
	for (i = 0; i < active_cnt; i++) {
		int ti = active[i];
		struct hx_track *trk = &algo->tracks[ti];
		s64 unmatched = max_t(s64, algo->track_dist2_max, 1) * 16;

		for (j = 0; j < det_cnt; j++) {
			s64 d2 = hx_dist2_predicted(&det[j], trk);
			s64 gate2 = max_t(s64, algo->track_dist2_max, 1);
			s64 cost = d2;
			struct hx_contact *ct = j < algo->contact_count ?
				&algo->contacts[j] : NULL;

			if (trk->missed)
				gate2 *= min_t(s32, (s32)trk->missed + 1, 4);
			if (ct && ct->continuation_only &&
			    (ct->continuation_track_slot != ti ||
			     target_has_regular[ti]))
				algo->assign_cost[i][j] = inf;
			else if (d2 <= gate2) {
				/* Peak IDs are continuity hints, not hard ownership.  A
				 * hard lock strands a fast-moving finger on its old residual
				 * lobe and turns the real moving lobe into a second slot.
				 */
				if (ct && trk->source_peak_id && ct->source_peak_id &&
				    trk->source_peak_id != ct->source_peak_id) {
					s64 age_weight = 1 +
						min_t(u8, trk->source_peak_age, 20) / 10;

					if (target_has_same_peak[ti])
						cost = hx_dist2_motion_ray(&det[j], trk);

					cost += (s64)algo->track_peak_id_penalty *
						age_weight;
				}
				algo->assign_cost[i][j] = cost;
			} else {
				algo->assign_cost[i][j] = inf;
			}
		}
		for (j = det_cnt; j < det_cnt + active_cnt; j++)
			algo->assign_cost[i][j] = unmatched;
	}
	if (algo->hungarian_enabled) {
		hx_hungarian_assign(algo, active, active_cnt, det_cnt, match);
	} else {
		memset(match, -1, sizeof(match));
		for (i = 0; i < active_cnt; i++) {
			s64 best = inf;

			for (j = 0; j < det_cnt; j++)
				if (!det_used[j] && algo->assign_cost[i][j] < best) {
					best = algo->assign_cost[i][j];
					match[active[i]] = j;
				}
			if (match[active[i]] >= 0)
				det_used[match[active[i]]] = true;
		}
		memset(det_used, 0, sizeof(det_used));
	}

	for (i = 0; i < active_cnt; i++) {
		int ti = active[i];
		int di = match[ti];
		struct hx_track *trk = &algo->tracks[ti];

		if (di < 0)
			continue;

		/* Jump detection: if the actual (non-predicted) displacement
		 * exceeds the jump threshold, this is a finger swap, not a
		 * slide.  Release the old slot and let the detection spawn
		 * a new track at a *different* slot so that lift + press
		 * both appear in the same SYN_REPORT (zero added latency).
		 */
		if (algo->track_jump_dist2 > 0 && trk->age >= 2) {
			s32 dx = det[di].x - trk->x;
			s32 dy = det[di].y - trk->y;
			s64 actual_d2 = (s64)dx * dx + (s64)dy * dy;

			if (actual_d2 > algo->track_jump_dist2) {
				hx_reset_track(trk);
				jump_released |= (1u << ti);
				/* det stays unused → picked up by new-slot logic */
				continue;
			}
		}

		hx_filter_track(algo, trk, &det[di]);
		trk->missed = 0;
		if (di < algo->contact_count)
			trk->signal_sum = algo->contacts[di].signal_sum;
		if (di < algo->contact_count &&
		    algo->contacts[di].source_peak_id) {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			if (target_has_same_peak[ti] && trk->source_peak_id &&
			    trk->source_peak_id !=
				algo->contacts[di].source_peak_id)
				algo->diag_peak_id_handoffs++;
#endif
			trk->source_peak_id = algo->contacts[di].source_peak_id;
			trk->source_peak_age = algo->contacts[di].source_peak_age;
		}
		if (trk->age < U8_MAX)
			trk->age++;
		if (trk->debounce > 0)
			trk->debounce--;
		det_used[di] = true;
	}

	/* Age or release unmatched tracks. */
	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		struct hx_track *trk = &algo->tracks[i];

		if (!trk->active || match[i] >= 0)
			continue;

		/*
		 * Before the first stable touch is established, drop stray
		 * tracks immediately to prevent noise from being reported.
		 */
		if (!trk->reported ||
		    (algo->track_active_guard && !algo->touch_active)) {
			hx_reset_track(trk);
			continue;
		}

		trk->missed++;
		if (trk->missed > algo->track_lost_frames)
			hx_reset_track(trk);
	}

	/* Create new slots for unmatched detections. */
	for (j = 0; j < det_cnt; j++) {
		struct hx_track *trk = NULL;

		if (det_used[j])
			continue;
		if (j < algo->contact_count &&
		    algo->contacts[j].continuation_only)
			continue;
		if (hx_is_unconfirmed_split(algo, det, j, active, active_cnt,
					    match, prior_peak_id))
			continue;

		for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
			if (!algo->tracks[i].active &&
			    !(jump_released & (1u << i))) {
				trk = &algo->tracks[i];
				break;
			}
		}
		if (!trk)
			continue;

		trk->active   = true;
		trk->age      = 1;
		trk->missed   = 0;
		trk->debounce = algo->debounce_base;
		if (j < algo->contact_count) {
			if (algo->contacts[j].signal_sum < algo->debounce_strong_signal)
				trk->debounce += algo->debounce_weak_extra;
			if (algo->contacts[j].is_edge)
				trk->debounce += algo->debounce_edge_extra;
			trk->source_peak_id = algo->contacts[j].source_peak_id;
			trk->source_peak_age = algo->contacts[j].source_peak_age;
		}
		if (algo->fast_edge_start_pending && active_cnt == 0 &&
		    det_cnt == 1 && j == 0 && j < algo->contact_count &&
		    (algo->contacts[j].is_edge ||
		     algo->contacts[j].fast_start_candidate)) {
			trk->debounce = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->diag_fast_edge_starts++;
#endif
		}
		trk->x        = det[j].x;
		trk->y        = det[j].y;
		trk->vx       = 0;
		trk->vy       = 0;
		trk->filtered_x_q8 = det[j].x << 8;
		trk->filtered_y_q8 = det[j].y << 8;
		if (j < algo->contact_count)
			trk->signal_sum = algo->contacts[j].signal_sum;
		trk->reported = trk->debounce == 0 && algo->touch_active;
	}

	for (i = 0; i < HX_MAX_PEAKS; i++)
		if (!algo->peak_competition[i].seen)
			memset(&algo->peak_competition[i], 0,
			       sizeof(algo->peak_competition[i]));
}

int hx_count_stable_tracks(struct hx_algo *algo)
{
	int i, cnt = 0;

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo->tracks[i].active && algo->tracks[i].reported)
			cnt++;
	}
	return cnt;
}

static void hx_suppress_rx_ghosts(struct hx_algo *algo,
				  struct input_mt_pos *det, int *det_cnt)
{
	bool drop[HIMAX_MAX_TOUCH] = { false };
	int i, j, dst;

	if (!algo->ghost_enabled)
		return;
	for (i = 0; i < *det_cnt; i++) {
		for (j = i + 1; j < *det_cnt; j++) {
			int weak, strong, ti;
			bool linked = false;

			if (abs(det[i].y - det[j].y) > algo->ghost_row_distance ||
			    abs(det[i].x - det[j].x) < algo->ghost_min_col_distance)
				continue;
			strong = algo->contacts[i].signal_sum >=
				 algo->contacts[j].signal_sum ? i : j;
			weak = strong == i ? j : i;
			if ((s64)algo->contacts[weak].signal_sum * 256 >
			    (s64)algo->contacts[strong].signal_sum *
				algo->ghost_weak_ratio_q8)
				continue;
			for (ti = 0; ti < HIMAX_MAX_TOUCH; ti++)
				if (algo->tracks[ti].active &&
				    hx_dist2_predicted(&det[weak], &algo->tracks[ti]) <=
					algo->track_dist2_max) {
					linked = true;
					break;
				}
			if (!linked)
				drop[weak] = true;
		}
	}
	for (i = 0, dst = 0; i < *det_cnt; i++) {
		if (drop[i])
			continue;
		if (dst != i) {
			det[dst] = det[i];
			algo->contacts[dst] = algo->contacts[i];
		}
		dst++;
	}
	*det_cnt = dst;
	algo->contact_count = dst;
}

#ifdef HX_ALGO_HOST_TEST
int hx_algo_process_frame(struct hx_algo *algo, const u16 *raw)
{
	return hx_algo_process_frame_state(algo, raw, HX_FINGER_UNKNOWN);
}
#endif

int hx_algo_process_frame_state(struct hx_algo *algo, const u16 *raw,
				enum hx_finger_state finger_state)
{
	struct input_mt_pos det[HIMAX_MAX_TOUCH];
	bool firmware_finger_rising = false;
	bool has_active_track = false;
	int det_cnt = 0;
	int reported = 0;
	int stable = 0;
	int i;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	s16 frame_max = 0;

	algo->diag_frame_seq++;
#endif

	if (finger_state == HX_FINGER_PRESENT) {
		firmware_finger_rising = !algo->firmware_finger_present;
		algo->firmware_finger_present = true;
	} else if (finger_state == HX_FINGER_ABSENT) {
		algo->firmware_finger_present = false;
	}
	for (i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo->tracks[i].active) {
			has_active_track = true;
			break;
		}
	algo->fast_edge_start_pending = algo->firmware_edge_fast_start &&
		firmware_finger_rising && !has_active_track;
	hx_preprocess_frame_state(algo, raw, finger_state);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	for (i = 0; i < HX_PIXELS; i++)
		frame_max = max(frame_max, ((s16 *)algo->frame)[i]);
	algo->diag_frame_max = frame_max;
#endif

	/* The controller's master-frame status is authoritative for lift-off.
	 * Do not let matrix rebound or the tracker's silent-gap window extend an
	 * already confirmed UP event.  The gap window remains useful for UNKNOWN
	 * status (host tests/legacy callers) and invalid frames, which never enter
	 * this function from the IRQ path.
	 */
	if (finger_state == HX_FINGER_ABSENT) {
		algo->zone_count = 0;
		algo->peak_count = 0;
		algo->prev_peak_count = 0;
		algo->contact_count = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->diag_zones = 0;
		algo->diag_peaks = 0;
		algo->diag_contacts_pre_filter = 0;
		algo->diag_contacts_post_filter = 0;
#endif
		for (i = 0; i < HIMAX_MAX_TOUCH; i++)
			hx_reset_track(&algo->tracks[i]);
		goto update_state;
	}

	hx_detect_macro_zones(algo);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_zones = algo->zone_count;
#endif
	hx_reject_palms(algo);
	hx_detect_peaks(algo);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_peaks = algo->peak_count;
#endif
	hx_expand_and_resolve(algo, det, &det_cnt);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_contacts_pre_filter = det_cnt;
#endif
	hx_suppress_rx_ghosts(algo, det, &det_cnt);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_contacts_post_filter = det_cnt;
#endif
	/* Himax coordinate-reporting drivers trust a valid firmware finger-on
	 * transition and report its first coordinate immediately.  Our raw-matrix
	 * path still requires the candidate to survive palm, edge and ghost
	 * filtering, and only grants that confidence to one initial edge contact.
	 * A sustained firmware-present state cannot validate later edge noise.
	 */
	algo->fast_edge_start_pending = algo->firmware_edge_fast_start &&
		firmware_finger_rising && !has_active_track && det_cnt == 1 &&
		algo->contact_count == 1 &&
		(algo->contacts[0].is_edge ||
		 algo->contacts[0].fast_start_candidate) &&
		!algo->contacts[0].continuation_only;
	hx_track_contacts(algo, det, det_cnt);
	algo->fast_edge_start_pending = false;

update_state:
	for (i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo->tracks[i].active && algo->tracks[i].debounce == 0)
			stable++;
	if (stable) {
		if (!algo->touch_active &&
		    ++algo->touch_start_frames >= algo->track_start_debounce)
			algo->touch_active = true;
	} else {
		algo->touch_start_frames = 0;
		algo->touch_active = false;
	}
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_active_tracks = 0;
	algo->diag_reported_tracks = 0;
#endif
	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		if (algo->tracks[i].active)
			algo->diag_active_tracks++;
#endif
		if (algo->tracks[i].active && algo->tracks[i].debounce == 0 &&
		    algo->touch_active)
			algo->tracks[i].reported = true;
		if (algo->tracks[i].active && algo->tracks[i].reported)
			reported++;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		if (algo->tracks[i].reported)
			algo->diag_reported_tracks++;
#endif
	}

	return reported;
}
