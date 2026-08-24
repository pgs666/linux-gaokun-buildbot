// SPDX-License-Identifier: GPL-2.0
/* Himax HX83121A zone, palm and peak detection. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

/* ======================================================================== */
/* Phase 2A — macro-zone detection (8-connected BFS)                        */
/* ======================================================================== */

void hx_detect_macro_zones(struct hx_algo *algo)
{
	static const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
	static const int dc[] = {-1,  0,  1, -1, 1, -1, 0, 1};
	u16 head, tail;
	int r, c, d;

	memset(algo->visited, 0, sizeof(algo->visited));
	algo->zone_count = 0;
	algo->zone_arena_used = 0;

	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			struct hx_macro_zone candidate;
			struct hx_macro_zone *zone = &candidate;

			if (algo->visited[idx])
				continue;
			if (algo->frame[r][c] < algo->macro_threshold)
				continue;
			zone->arena_start = algo->zone_arena_used;
			zone->area       = 0;
			zone->signal_sum = 0;
			zone->min_r = r;  zone->max_r = r;
			zone->min_c = c;  zone->max_c = c;

			/* Ring-buffer BFS using the pre-allocated queue. */
			head = 0;
			tail = 0;
			algo->bfs_queue[tail++] = idx;
			algo->visited[idx] = 1;

			while (head != tail) {
				int ci = algo->bfs_queue[head++];
				int cr = ci / HX_COLS;
				int cc = ci % HX_COLS;
				s16 sig = algo->frame[cr][cc];

				if (algo->zone_arena_used < HX_PIXELS)
					algo->zone_arena[algo->zone_arena_used++] = ci;
				zone->area++;
				if (sig > 0)
					zone->signal_sum += sig;

				if (cr < zone->min_r)
					zone->min_r = cr;
				if (cr > zone->max_r)
					zone->max_r = cr;
				if (cc < zone->min_c)
					zone->min_c = cc;
				if (cc > zone->max_c)
					zone->max_c = cc;

				for (d = 0; d < 8; d++) {
					int nr = cr + dr[d];
					int nc = cc + dc[d];
					int ni;

					if (nr < 0 || nr >= HX_ROWS ||
					    nc < 0 || nc >= HX_COLS)
						continue;
					ni = nr * HX_COLS + nc;
					if (algo->visited[ni])
						continue;
					if (algo->frame[nr][nc] < algo->macro_threshold)
						continue;
					algo->visited[ni] = 1;
					algo->bfs_queue[tail++] = ni;
				}
			}

			if (algo->zone_count < HX_MAX_ZONES) {
				algo->zones[algo->zone_count++] = candidate;
			} else {
				u8 weakest = 0;
				u8 zi;

				for (zi = 1; zi < HX_MAX_ZONES; zi++)
					if (algo->zones[zi].signal_sum <
					    algo->zones[weakest].signal_sum)
						weakest = zi;
				if (candidate.signal_sum >
				    algo->zones[weakest].signal_sum)
					algo->zones[weakest] = candidate;
			}
		}
	}

	/* Windows exposes macro zones strongest-first.  Besides matching its
	 * behavior, deterministic ordering prevents later fixed-capacity stages
	 * from depending on row-major scan position.
	 */
	for (r = 0; r + 1 < algo->zone_count; r++) {
		int strongest = r;

		for (c = r + 1; c < algo->zone_count; c++)
			if (algo->zones[c].signal_sum >
			    algo->zones[strongest].signal_sum)
				strongest = c;
		if (strongest != r)
			swap(algo->zones[r], algo->zones[strongest]);
	}
}

/* ======================================================================== */
/* Phase 2B — palm rejection                                                 */
/*                                                                           */
/* Combine footprint, integrated signal, density and bounding-box evidence. */
/* A compact, sharp peak is preserved even when its total signal is high.   */
/* ======================================================================== */

static bool hx_box_has_domain(const struct hx_algo *algo,
			      const struct hx_palm_box *box)
{
	int r, c;

	for (r = box->min_r; r <= box->max_r; r++)
		for (c = box->min_c; c <= box->max_c; c++)
			if (hx_frame_at(algo, r, c) >= algo->macro_threshold)
				return true;
	return false;
}

static void hx_age_palm_boxes(struct hx_algo *algo)
{
	u8 i, dst = 0;

	for (i = 0; i < algo->palm_box_count; i++) {
		struct hx_palm_box box = algo->palm_boxes[i];

		if (hx_box_has_domain(algo, &box))
			box.missed = 0;
		else
			box.missed++;
		if (box.missed && (!algo->palm_box_max_hold ||
				   box.missed > algo->palm_box_max_hold))
			continue;
		algo->palm_boxes[dst++] = box;
	}
	algo->palm_box_count = dst;
}

static void hx_update_palm_box(struct hx_algo *algo,
			       const struct hx_macro_zone *z)
{
	struct hx_palm_box candidate;
	int center_r, center_c;
	u8 i;

	if (!algo->palm_box_enabled)
		return;
	candidate.min_r = max_t(int, 0, z->min_r - algo->palm_box_expand_rows);
	candidate.max_r = min_t(int, HX_ROWS - 1,
				z->max_r + algo->palm_box_expand_rows);
	candidate.min_c = max_t(int, 0, z->min_c - algo->palm_box_expand_cols);
	candidate.max_c = min_t(int, HX_COLS - 1,
				z->max_c + algo->palm_box_expand_cols);
	candidate.missed = 0;
	center_r = (candidate.min_r + candidate.max_r) / 2;
	center_c = (candidate.min_c + candidate.max_c) / 2;

	for (i = 0; i < algo->palm_box_count; i++) {
		struct hx_palm_box *old = &algo->palm_boxes[i];
		int old_r = (old->min_r + old->max_r) / 2;
		int old_c = (old->min_c + old->max_c) / 2;

		if (abs(center_r - old_r) <= algo->palm_box_match_distance &&
		    abs(center_c - old_c) <= algo->palm_box_match_distance) {
			*old = candidate;
			return;
		}
	}
	if (algo->palm_box_count < HX_MAX_PALM_BOXES)
		algo->palm_boxes[algo->palm_box_count++] = candidate;
}

void hx_reject_palms(struct hx_algo *algo)
{
	u8 dst = 0;
	u8 i;

	hx_age_palm_boxes(algo);
	if (!algo->palm_enabled)
		return;

	for (i = 0; i < algo->zone_count; i++) {
		struct hx_macro_zone *z = &algo->zones[i];
		u16 bbox_w, bbox_h, max_side, min_side;
		u16 pi;
		s32 max_signal = 0;
		s32 mean_signal;
		int palm_score = 0;
		bool strong_finger_shape;
		bool reject = false;

		/* The Windows solver treats these as evidence, not four independent
		 * kill switches.  The old Linux port dropped an entire normal finger
		 * as soon as its integrated signal crossed 80000.
		 */
		if (z->area >= algo->palm_area_threshold)
			palm_score += 35;
		if (z->signal_sum >= algo->palm_signal_threshold)
			palm_score += 25;
		if (z->area >= 20 &&
		    z->signal_sum < (s32)algo->palm_density_low * z->area)
			palm_score += 15;

		bbox_w   = z->max_c - z->min_c + 1;
		bbox_h   = z->max_r - z->min_r + 1;
		max_side = max(bbox_w, bbox_h);
		min_side = min(bbox_w, bbox_h);
		if (z->area >= 10 && min_side > 0 &&
		    (u32)max_side * 256 >= 1024u * min_side)
			palm_score += 15;
		if (z->area >= 35 &&
		    (u32)z->area * 100 >= (u32)bbox_w * bbox_h * 40)
			palm_score += 15;

		for (pi = 0; pi < z->area; pi++) {
			int idx = algo->zone_arena[z->arena_start + pi];

			max_signal = max_t(s32, max_signal,
					   hx_frame_at(algo, idx / HX_COLS,
						       idx % HX_COLS));
		}
		mean_signal = z->area ? z->signal_sum / z->area : 0;
		strong_finger_shape = max_signal >= mean_signal + 100 &&
				      max_signal * 100 >= mean_signal * 335;
		if (strong_finger_shape)
			palm_score -= 20;

		/* Mirror PalmLikely: a large region needs several independent palm
		 * signals before it may suppress contacts.
		 */
		reject = z->area >= 55 && palm_score >= 55;
		if (reject)
			hx_update_palm_box(algo, z);

		if (!reject) {
			if (dst != i)
				algo->zones[dst] = *z;
			dst++;
		}
	}

	algo->zone_count = dst;
}

/* ======================================================================== */
/* Phase 2C — peak detection within surviving zones                         */
/* ======================================================================== */

/*
 * Asymmetric local-maximum test.
 *
 * "Before" neighbours (up + left in scan order) must be strictly less;
 * "after" neighbours (down + right) may be equal.  This breaks ties on
 * flat ridges so exactly one peak is produced per finger plateau.
 */
static bool hx_is_asymmetric_peak(const struct hx_algo *algo, int r, int c)
{
	s16 v = algo->frame[r][c];
	int radius = max_t(int, 1, algo->peak_local_radius);
	int dr, dc;

	for (dr = -radius; dr <= radius; dr++) {
		for (dc = -radius; dc <= radius; dc++) {
			int nr, nc;
			s16 nv;
			bool after;

			if (dr == 0 && dc == 0)
				continue;
			nr = r + dr;
			nc = c + dc;
			if (nr < 0 || nr >= HX_ROWS || nc < 0 || nc >= HX_COLS)
				continue;
			nv    = algo->frame[nr][nc];
			after = (dr > 0) || (dr == 0 && dc > 0);
			if (after) {
				if (nv > v)
					return false;
			} else {
				if (nv >= v)
					return false;
			}
		}
	}
	return true;
}

/*
 * Pressure-drift detector.
 *
 * A flat palm press produces a nearly-uniform row of elevated pixels with
 * low cross-row gradient.  Returns true when the peak signal falls in the
 * drift range [3/8, 3/4] of the independent signal limit, the row gradient
 * is low, and the row signal sum is high relative to the peak.
 */
static bool hx_detect_pressure_drift(const struct hx_algo *algo, int r, int c)
{
	s16 peak_sig  = algo->frame[r][c];
	s16 limit3_4  = (algo->peak_signal_threshold_limit * 3) >> 2;
	s16 limit3_8  = (algo->peak_signal_threshold_limit * 3) >> 3;
	int grad_sum  = 0;
	int row_sum   = 0;
	int col;

	if (peak_sig > limit3_4 || peak_sig < limit3_8)
		return false;

	for (col = 1; col < HX_COLS - 1; col++) {
		int grad = abs((int)hx_frame_at(algo, r, col + 1) -
			       (int)hx_frame_at(algo, r, col - 1));

		if (grad > algo->peak_signal_threshold_limit / 3)
			return false;   /* sharp spike → not drift */
		grad_sum += grad;
		if (algo->frame[r][col] > 0)
			row_sum += algo->frame[r][col];
	}

	return (row_sum >= peak_sig * 9 / 2) &&
	       (peak_sig * 6 >= grad_sum);
}

/*
 * Insert a peak into the fixed-size peak array.  When the array is full,
 * replace the weakest existing entry if the new peak is stronger.
 */
static void hx_insert_peak(struct hx_algo *algo, const struct hx_peak *p)
{
	int k, weakest;

	if (algo->peak_count < HX_MAX_PEAKS) {
		algo->peaks[algo->peak_count++] = *p;
		return;
	}

	weakest = 0;
	for (k = 1; k < HX_MAX_PEAKS; k++) {
		if (algo->peaks[k].z < algo->peaks[weakest].z)
			weakest = k;
	}
	if (p->z > algo->peaks[weakest].z)
		algo->peaks[weakest] = *p;
}

static u8 hx_allocate_peak_id(struct hx_algo *algo)
{
	int attempt;

	for (attempt = 0; attempt < U8_MAX; attempt++) {
		u8 id = algo->next_peak_id++;
		bool used = false;
		int i;

		if (!algo->next_peak_id)
			algo->next_peak_id = 1;
		if (!id)
			continue;
		for (i = 0; i < algo->peak_count; i++)
			if (algo->peaks[i].id == id) {
				used = true;
				break;
			}
		if (!used)
			return id;
	}
	return 1;
}

/* Preserve peak identity before contacts are expanded.  A pure adjacent-cell
 * match loses identity during a fast swipe (the captured failure moved more
 * than three cells per frame).  Predict from the last grid velocity, while
 * retaining a wider raw-distance fallback for acceleration and first motion.
 * This is deliberately separate from the output-slot tracker: peak identity
 * is supporting evidence, never an exclusive ownership lock.
 */
static void hx_track_peak_ids(struct hx_algo *algo)
{
	bool current_used[HX_MAX_PEAKS] = { false };
	bool previous_used[HX_MAX_PEAKS] = { false };
	int i;

	for (i = 0; i < algo->peak_count; i++) {
		algo->peaks[i].id = 0;
		algo->peaks[i].age = 0;
	}
	for (;;) {
		int best_score = INT_MAX;
		int best_current = -1;
		int best_previous = -1;
		int j;

		for (i = 0; i < algo->peak_count; i++) {
			if (current_used[i])
				continue;
			for (j = 0; j < algo->prev_peak_count; j++) {
				const struct hx_peak *cur = &algo->peaks[i];
				const struct hx_peak *prev = &algo->prev_peaks[j];
				int dr, dc, raw_distance, predicted_distance;
				int score;

				if (previous_used[j])
					continue;
				dr = (int)cur->r - (int)prev->r;
				dc = (int)cur->c - (int)prev->c;
				raw_distance = abs(dr) + abs(dc);
				predicted_distance =
					abs((int)cur->r - ((int)prev->r + prev->vr)) +
					abs((int)cur->c - ((int)prev->c + prev->vc));
				if (raw_distance > 10 && predicted_distance > 6)
					continue;
				/* Prediction dominates.  Raw displacement and signal
				 * continuity only break ambiguous split-lobe matches.
				 */
				score = predicted_distance * 16 + raw_distance * 2;
				score += min(abs((int)cur->z - (int)prev->z) / 64, 32);
				if ((prev->vr || prev->vc) &&
				    dr * prev->vr + dc * prev->vc <= 0 &&
				    raw_distance > 1)
					score += 48;
				if (score < best_score) {
					best_score = score;
					best_current = i;
					best_previous = j;
				}
			}
		}
		if (best_current < 0)
			break;
		algo->peaks[best_current].id =
			algo->prev_peaks[best_previous].id;
		algo->peaks[best_current].age =
			algo->prev_peaks[best_previous].age < U8_MAX ?
			algo->prev_peaks[best_previous].age + 1 : U8_MAX;
		algo->peaks[best_current].vr = clamp_t(int,
			(int)algo->peaks[best_current].r -
			(int)algo->prev_peaks[best_previous].r, -128, 127);
		algo->peaks[best_current].vc = clamp_t(int,
			(int)algo->peaks[best_current].c -
			(int)algo->prev_peaks[best_previous].c, -128, 127);
		current_used[best_current] = true;
		previous_used[best_previous] = true;
	}
	for (i = 0; i < algo->peak_count; i++)
		if (!algo->peaks[i].id)
			algo->peaks[i].id = hx_allocate_peak_id(algo);

	memcpy(algo->prev_peaks, algo->peaks,
	       algo->peak_count * sizeof(algo->peaks[0]));
	algo->prev_peak_count = algo->peak_count;
}

void hx_detect_peaks(struct hx_algo *algo)
{
	u8 zi;

	algo->peak_count = 0;

	/* --- Asymmetric local-max scan within each surviving zone --- */
	for (zi = 0; zi < algo->zone_count; zi++) {
		struct hx_macro_zone *zone = &algo->zones[zi];
		u16 pi;

		for (pi = 0; pi < zone->area; pi++) {
			int idx = algo->zone_arena[zone->arena_start + pi];
			int r = idx / HX_COLS;
			int c = idx % HX_COLS;
			s16 v = algo->frame[r][c];
			struct hx_peak peak;
			bool on_edge = (r == 0 || r == HX_ROWS - 1 ||
					c == 0 || c == HX_COLS - 1);
			bool edge_threshold_cell = (c == 1 || c == HX_COLS - 2 ||
						    r == HX_ROWS - 1);
			int dr, dc;
			s32 nbr_sum = 0;

			if (v < (edge_threshold_cell ? algo->peak_edge_threshold :
						algo->peak_threshold))
				continue;
			if (!hx_is_asymmetric_peak(algo, r, c))
				continue;
			if (hx_detect_pressure_drift(algo, r, c))
				continue;

			for (dr = -1; dr <= 1; dr++)
				for (dc = -1; dc <= 1; dc++) {
					if (dr == 0 && dc == 0)
						continue;
					nbr_sum += hx_frame_at(algo, r + dr, c + dc);
				}

			peak = (struct hx_peak){
				.r         = r,
				.c         = c,
				.z         = v,
				.nbr_sum   = nbr_sum,
				.zone_area = zone->area,
				.zone_index = zi,
				.on_edge = on_edge,
				.continuation_track_slot = -1,
			};
			hx_insert_peak(algo, &peak);
		}
	}

	/* Close maxima without a real saddle are one broad peak, not two
	 * fingers.  Integer midpoint sampling mirrors the Windows saddle gate.
	 */
	if (algo->peak_saddle_enabled) {
		u8 i, j;

		for (i = 0; i < algo->peak_count; i++) {
			for (j = i + 1; j < algo->peak_count; j++) {
				struct hx_peak *a = &algo->peaks[i];
				struct hx_peak *b = &algo->peaks[j];
				int dr = (int)b->r - a->r;
				int dc = (int)b->c - a->c;
				int steps = max(abs(dr), abs(dc));
				int saddle = 0;
				int weaker;
				int required_drop;
				int s;

				if (a->zone_index != b->zone_index)
					continue;
				if (steps == 0 || steps > algo->peak_saddle_radius)
					continue;
				for (s = 1; s < steps; s++) {
					int rr_num = dr * s;
					int cc_num = dc * s;
					int rr = a->r + (rr_num >= 0 ?
						(rr_num + steps / 2) / steps :
						(rr_num - steps / 2) / steps);
					int cc = a->c + (cc_num >= 0 ?
						(cc_num + steps / 2) / steps :
						(cc_num - steps / 2) / steps);

					saddle = max_t(int, saddle,
						       hx_frame_at(algo, rr, cc));
				}
				weaker = min(a->z, b->z);
				required_drop = max((int)algo->peak_saddle_drop,
						    weaker * 8 / 100);
				if (weaker - saddle < required_drop) {
					struct hx_peak *weak = a->z <= b->z ? a : b;

					weak->z = -1;
				}
			}
		}
		{
			u8 dst = 0;

			for (i = 0; i < algo->peak_count; i++)
				if (algo->peaks[i].z >= 0)
					algo->peaks[dst++] = algo->peaks[i];
			algo->peak_count = dst;
		}
	}

	/* --- Z8 isolation filter: (z >> 5) > nbr_sum → isolated spike --- */
	{
		u8 dst = 0, i;

		for (i = 0; i < algo->peak_count; i++) {
			if (!algo->peak_z8_enabled ||
			    (algo->peaks[i].z >> 5) <= algo->peaks[i].nbr_sum)
				algo->peaks[dst++] = algo->peaks[i];
		}
		algo->peak_count = dst;
	}

	/* Small peaks normally may only continue an existing reported track.  A
	 * single strong compact peak close to a physical edge is the one exception:
	 * on a trustworthy firmware finger-on transition it may bootstrap the first
	 * slot.  This covers fast edge entries whose footprint has not grown to the
	 * normal macro-area threshold yet.
	 */
	{
		u8 dst = 0, i;
		int reported_tracks = 0;
		int ti;

		for (ti = 0; ti < HIMAX_MAX_TOUCH; ti++)
			if (algo->tracks[ti].active && algo->tracks[ti].reported)
				reported_tracks++;

		for (i = 0; i < algo->peak_count; i++) {
			struct hx_peak *pk = &algo->peaks[i];
			bool near_edge =
				pk->r < algo->peak_fast_start_edge_cells ||
				pk->r >= HX_ROWS - algo->peak_fast_start_edge_cells ||
				pk->c < algo->peak_fast_start_edge_cells ||
				pk->c >= HX_COLS - algo->peak_fast_start_edge_cells;
			bool fast_start = algo->fast_edge_start_pending &&
				algo->zone_count == 1 && algo->peak_count == 1 &&
				pk->zone_area >= algo->peak_continue_min_area &&
				pk->zone_area < algo->peak_macro_min_area && near_edge &&
				pk->z >= algo->peak_fast_start_min_signal;
			bool weak_single_track = reported_tracks == 1 &&
				algo->zone_count == 1 && algo->firmware_finger_present &&
				pk->z >= algo->peak_single_track_continue_min_signal;
			bool continuation = false;
			int matches = 0;
			s8 continuation_slot = -1;

			pk->fast_start_candidate = fast_start;
			pk->continuation_only = false;
			pk->continuation_track_slot = -1;
			if (pk->zone_area < algo->peak_macro_min_area && !pk->on_edge &&
			    pk->zone_area >= algo->peak_continue_min_area &&
			    (pk->z >= algo->peak_continue_min_signal ||
			     weak_single_track)) {
				struct input_mt_pos approx = {
					.x = (pk->c * 256 + 128) / 6,
					.y = 5 * (pk->r * 256 + 128) / 32,
				};

				for (ti = 0; ti < HIMAX_MAX_TOUCH; ti++) {
					struct hx_track *trk = &algo->tracks[ti];

					if (!trk->active || !trk->reported)
						continue;
					if (hx_dist2_predicted(&approx, trk) <=
					    max_t(s32, algo->peak_continue_dist2, 1)) {
						matches++;
						continuation_slot = ti;
					}
				}
				continuation = matches == 1;
			}
			if (pk->zone_area >= algo->peak_macro_min_area || pk->on_edge ||
			    fast_start || continuation) {
				pk->continuation_only = continuation;
				pk->continuation_track_slot = continuation ?
					continuation_slot : -1;
				algo->peaks[dst++] = *pk;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
				if (continuation)
					algo->diag_small_peak_continued++;
				if (continuation &&
				    pk->z < algo->peak_continue_min_signal)
					algo->diag_weak_peak_continued++;
#endif
			} else {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
				algo->diag_small_peak_rejected++;
#endif
			}
		}
		algo->peak_count = dst;
	}

	/* Persistent PalmBox suppression catches strong finger-like peaks that
	 * appear inside a palm domain on later frames.
	 */
	if (algo->palm_box_enabled && algo->palm_box_count) {
		u8 i, bi, dst = 0;

		for (i = 0; i < algo->peak_count; i++) {
			bool inside = false;

			for (bi = 0; bi < algo->palm_box_count; bi++) {
				struct hx_palm_box *b = &algo->palm_boxes[bi];

				if (algo->peaks[i].r >= b->min_r &&
				    algo->peaks[i].r <= b->max_r &&
				    algo->peaks[i].c >= b->min_c &&
				    algo->peaks[i].c <= b->max_c) {
					inside = true;
					break;
				}
			}
			if (!inside)
				algo->peaks[dst++] = algo->peaks[i];
		}
		algo->peak_count = dst;
	}

	/* --- Edge peak filter: weak edge peaks < max_sig * 5/8 --- */
	{
		int edge;

		for (edge = 0; edge < 2; edge++) {
			s16 max_sig = 0, cutoff;
			u8 dst = 0, i;

			for (i = 0; i < algo->peak_count; i++) {
				bool on_edge;

				on_edge = algo->peaks[i].c ==
					(edge == 0 ? 0 : HX_COLS - 1);
				if (on_edge && algo->peaks[i].z > max_sig)
					max_sig = algo->peaks[i].z;
			}
			if (max_sig == 0)
				continue;

			cutoff = (max_sig >> 3) * 5;
			for (i = 0; i < algo->peak_count; i++) {
				bool on_edge;

				on_edge = algo->peaks[i].c ==
					(edge == 0 ? 0 : HX_COLS - 1);
				if (!(on_edge && algo->peaks[i].z < cutoff))
					algo->peaks[dst++] = algo->peaks[i];
			}
			algo->peak_count = dst;
		}
	}

	/* --- Sort ascending by signal (selection sort, ≤20 elements) --- */
	{
		u8 i, j;

		for (i = 0; i + 1 < algo->peak_count; i++) {
			u8 min_idx = i;

			for (j = i + 1; j < algo->peak_count; j++) {
				if (algo->peaks[j].z < algo->peaks[min_idx].z)
					min_idx = j;
			}
			if (min_idx != i)
				swap(algo->peaks[i], algo->peaks[min_idx]);
		}
	}

	hx_track_peak_ids(algo);
}
