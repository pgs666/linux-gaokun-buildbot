// SPDX-License-Identifier: GPL-2.0
/* Himax HX83121A contact-region expansion and coordinate solving. */

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
/* Phase 2D — zone expansion + weighted centroid                            */
/*                                                                           */
/* For each peak, BFS-expand outward while signal >= 50% of the peak       */
/* value.  Accumulate weighted centroid (Q8.8 fixed-point grid coords)     */
/* using s64 intermediate products.  When the BFS meets pixels already     */
/* owned by another peak, fall back to a 3x3 local centroid.               */
/*                                                                           */
/* Result: contacts[] filled, then converted to output coords [0, 65535].  */
/* ======================================================================== */

/*
 * Compute the zone expansion threshold: ~50% of min(peak_threshold, peak_z).
 * Uses integer multiply + shift: base * 0x40 >> 7 ≈ base * 0.5.
 */
static inline s16 hx_zone_thold(const struct hx_algo *algo, s16 peak_z)
{
	int base = min_t(int, algo->peak_threshold, peak_z);
	int shift = min_t(int, algo->zone_threshold_shift, 15);
	int result = (base * algo->zone_threshold_numer) >> shift;

	return (s16)max(result, 1);
}

static int hx_neighbor_support(const struct hx_algo *algo, int r, int c,
			       s16 threshold)
{
	int dr, dc;
	int support = 0;

	for (dr = -1; dr <= 1; dr++)
		for (dc = -1; dc <= 1; dc++)
			if ((dr || dc) &&
			    hx_frame_at(algo, r + dr, c + dc) >= threshold)
				support++;
	return support;
}

/*
 * Single-peak zone: BFS flood-fill weighted centroid.
 * Returns true if the expansion was clean (no overlap with other zones).
 */
static bool hx_expand_single_peak(struct hx_algo *algo, int pi,
				    struct hx_contact *ct)
{
	struct hx_peak *pk = &algo->peaks[pi];
	s16 thold = hx_zone_thold(algo, pk->z);
	u8 zone_id = (u8)(pi + 1);
	u16 head = 0, tail = 0;
	int seed = pk->r * HX_COLS + pk->c;
	bool clean = true;
	s64 w_col = 0, w_row = 0;
	s32 w_total = 0;
	u16 area = 0;
	s32 sig_sum = 0;

	static const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
	static const int dc[] = {-1,  0,  1, -1, 1, -1, 0, 1};

	algo->zone_map[seed] = zone_id;
	algo->bfs_queue[tail++] = seed;

	while (head != tail) {
		int idx = algo->bfs_queue[head++];
		int r = idx / HX_COLS;
		int c = idx % HX_COLS;
		s16 sig = hx_frame_at(algo, r, c);
		int d;

		area++;
		sig_sum += sig;
		w_col += (s64)c * 128 * sig;
		w_row += (s64)r * 128 * sig;
		w_total += sig;

		for (d = 0; d < 8; d++) {
			int nr = r + dr[d];
			int nc = c + dc[d];
			int ni;

			if (nr < 0 || nr >= HX_ROWS || nc < 0 || nc >= HX_COLS)
				continue;
			ni = nr * HX_COLS + nc;
			if (algo->zone_map[ni]) {
				if (algo->zone_map[ni] != zone_id)
					clean = false;
				continue;
			}
			if (hx_frame_at(algo, nr, nc) < thold)
				continue;
			if (algo->zone_max_radius &&
			    max(abs(nr - (int)pk->r), abs(nc - (int)pk->c)) >
				algo->zone_max_radius)
				continue;
			if (algo->zone_cleanup_enabled &&
			    hx_neighbor_support(algo, nr, nc, thold) < 2)
				continue;
			algo->zone_map[ni] = zone_id;
			algo->bfs_queue[tail++] = ni;
		}
	}

	if (w_total > 0) {
		ct->x = (s32)(w_col * 2 / w_total) + 0x80;
		ct->y = (s32)(w_row * 2 / w_total) + 0x80;
	} else {
		ct->x = pk->c * 256 + 128;
		ct->y = pk->r * 256 + 128;
	}
	ct->area = area;
	ct->signal_sum = sig_sum;
	ct->is_edge = (pk->r == 0 || pk->r == HX_ROWS - 1 ||
		       pk->c == 0 || pk->c == HX_COLS - 1);
	ct->fast_start_candidate = pk->fast_start_candidate;
	ct->peak_index = pi;
	ct->source_peak_id = pk->id;
	ct->source_peak_age = pk->age;
	ct->source_zone_index = pk->zone_index;
	ct->continuation_only = pk->continuation_only;
	ct->continuation_track_slot = pk->continuation_track_slot;

	return clean;
}

/*
 * Multi-peak fallback: 3x3 local weighted centroid around the peak.
 */
static void hx_local_centroid(struct hx_algo *algo, int pi,
			       struct hx_contact *ct)
{
	struct hx_peak *pk = &algo->peaks[pi];
	s64 w_col = 0, w_row = 0;
	s32 w_total = 0;
	u16 area = 0;
	s32 sig_sum = 0;
	int dr, dc;

	for (dr = -1; dr <= 1; dr++) {
		for (dc = -1; dc <= 1; dc++) {
			int nr = pk->r + dr;
			int nc = pk->c + dc;
			s16 sig;

			if (nr < 0 || nr >= HX_ROWS || nc < 0 || nc >= HX_COLS)
				continue;
			sig = hx_frame_at(algo, nr, nc);
			if (sig <= 0)
				continue;
			w_col += (s64)nc * 128 * sig;
			w_row += (s64)nr * 128 * sig;
			w_total += sig;
			area++;
			sig_sum += sig;
		}
	}

	if (w_total > 0) {
		ct->x = (s32)(w_col * 2 / w_total) + 0x80;
		ct->y = (s32)(w_row * 2 / w_total) + 0x80;
	} else {
		ct->x = pk->c * 256 + 128;
		ct->y = pk->r * 256 + 128;
	}
	ct->area = area;
	ct->signal_sum = sig_sum;
	ct->is_edge = (pk->r == 0 || pk->r == HX_ROWS - 1 ||
		       pk->c == 0 || pk->c == HX_COLS - 1);
	ct->fast_start_candidate = pk->fast_start_candidate;
	ct->peak_index = pi;
	ct->source_peak_id = pk->id;
	ct->source_peak_age = pk->age;
	ct->source_zone_index = pk->zone_index;
	ct->continuation_only = pk->continuation_only;
	ct->continuation_track_slot = pk->continuation_track_slot;
}

/*
 * Edge compensation: push centroid outward toward the physical sensor
 * boundary.  The sensor extends ~0.5 cells beyond the last grid node,
 * but the weighted centroid is biased inward because there's no data
 * outside the grid.  This function linearly pushes edge contacts
 * outward, with maximum push at the boundary itself, fading to zero
 * at edge_blend_q8 distance from the edge.
 */
static void hx_edge_compensate(struct hx_algo *algo, struct hx_contact *ct)
{
	s32 push_max = algo->edge_push_q8;
	s32 blend    = algo->edge_blend_q8;
	s32 dist, push;

	if (!algo->edge_comp_enabled || push_max <= 0 || blend <= 0)
		return;

	/* Left boundary: distance = ct->x (Q8.8, 0 = grid col 0 center) */
	dist = ct->x;
	if (dist < blend) {
		push = push_max * (blend - dist) / blend;
		ct->x = max_t(s32, ct->x - push, 0);
	}

	/* Right boundary: distance from last col center */
	dist = (HX_COLS - 1) * 256 + 128 - ct->x;
	if (dist < blend) {
		push = push_max * (blend - dist) / blend;
		ct->x = min_t(s32, ct->x + push, (HX_COLS - 1) * 256 + 256);
	}

	/* Top boundary */
	dist = ct->y;
	if (dist < blend) {
		push = push_max * (blend - dist) / blend;
		ct->y = max_t(s32, ct->y - push, 0);
	}

	/* Bottom boundary */
	dist = (HX_ROWS - 1) * 256 + 128 - ct->y;
	if (dist < blend) {
		push = push_max * (blend - dist) / blend;
		ct->y = min_t(s32, ct->y + push, (HX_ROWS - 1) * 256 + 256);
	}
}

void hx_expand_and_resolve(struct hx_algo *algo,
			    struct input_mt_pos *pos, int *cnt)
{
	int i, n;

	memset(algo->zone_map, 0, sizeof(algo->zone_map));
	algo->contact_count = 0;

	n = min_t(int, algo->peak_count, HX_MAX_PEAKS);

	for (i = 0; i < n; i++) {
		struct hx_contact *ct = &algo->contacts[algo->contact_count];
		bool clean;

		clean = hx_expand_single_peak(algo, i, ct);
		if (!clean)
			hx_local_centroid(algo, i, ct);

		/* Push edge centroids outward toward physical sensor boundary */
		if (ct->is_edge)
			hx_edge_compensate(algo, ct);

		algo->contact_count++;
	}

	/* If more peaks than slots, keep the strongest signal_sum contacts.
	 * Do this after resolving every stored peak: peaks are intentionally
	 * processed weakest-first for deterministic zone ownership.
	 */
	if (algo->contact_count > HIMAX_MAX_TOUCH) {
		/* Selection-sort descending by signal_sum, keep first MAX */
		u8 ci, cj;

		for (ci = 0; ci + 1 < algo->contact_count; ci++) {
			u8 best = ci;

			for (cj = ci + 1; cj < algo->contact_count; cj++) {
				if (algo->contacts[cj].signal_sum >
				    algo->contacts[best].signal_sum)
					best = cj;
			}
			if (best != ci)
				swap(algo->contacts[ci], algo->contacts[best]);
		}
		algo->contact_count = HIMAX_MAX_TOUCH;
	}

	/* Convert Q8.8 grid coordinates to output space matching rxtx2xy:
	 *   x = ct->x / 6       (maps to [~21, ~2539])
	 *   y = 5 * ct->y / 32  (maps to [~20, ~1580])
	 * This matches the coordinate range the DT/touchscreen_properties
	 * are calibrated for.
	 */
	*cnt = 0;
	for (i = 0; i < algo->contact_count; i++) {
		struct hx_contact *ct = &algo->contacts[i];
		s32 x = clamp_val((s32)(ct->x / 6), 0, SZ_64K - 1);
		s32 y = clamp_val((s32)(5 * ct->y / 32), 0, SZ_64K - 1);
		bool near_edge = x < algo->edge_reject_margin ||
			x > (HX_COLS * 256 / 6) - algo->edge_reject_margin ||
			y < algo->edge_reject_margin ||
			y > (HX_ROWS * 40) - algo->edge_reject_margin;

		if (algo->edge_reject_enabled && near_edge &&
		    ct->signal_sum < algo->edge_reject_min_signal)
			continue;
		if (*cnt != i)
			algo->contacts[*cnt] = *ct;
		pos[*cnt].x = x;
		pos[*cnt].y = y;
		(*cnt)++;
	}
	algo->contact_count = *cnt;
}
