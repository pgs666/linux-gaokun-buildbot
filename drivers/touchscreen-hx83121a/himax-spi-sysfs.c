// SPDX-License-Identifier: GPL-2.0
/* Sysfs controls and optional diagnostic tracing for HX83121A. */

#include <linux/slab.h>
#include <linux/sysfs.h>
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
#include <linux/timekeeping.h>
#endif

#include "himax-spi.h"

static ssize_t inplace_reset_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	bool do_reset;
	int ret;

	ret = kstrtobool(buf, &do_reset);
	if (ret)
		return ret;

	if (!do_reset)
		return count;

	ret = himax_manual_reset(ts);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_WO(inplace_reset);

/* ---- sysfs: algo parameter group ---- */

#define HX_ALGO_ATTR_BOOL_RW(_name)					\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%d\n", ts->algo->_name ? 1 : 0);	\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	bool val;							\
	int ret = kstrtobool(buf, &val);				\
	if (ret)							\
		return ret;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

#define HX_ALGO_ATTR_S16_RW(_name, _min, _max)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%d\n", (int)ts->algo->_name);		\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	s16 val;							\
	int ret = kstrtos16(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	if (val < (_min) || val > (_max))				\
		return -ERANGE;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

#define HX_ALGO_ATTR_U16_RW(_name, _min, _max)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%u\n", (unsigned int)ts->algo->_name);	\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	u16 val;							\
	int ret = kstrtou16(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	if (val < (_min) || val > (_max))				\
		return -ERANGE;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

#define HX_ALGO_ATTR_U8_RW(_name, _min, _max)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%u\n", (unsigned int)ts->algo->_name);	\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	u8 val;							\
	int ret = kstrtou8(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	if (val < (_min) || val > (_max))				\
		return -ERANGE;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

#define HX_ALGO_ATTR_S32_RW(_name, _min, _max)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%d\n", ts->algo->_name);		\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	s32 val;							\
	int ret = kstrtos32(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	if (val < (_min) || val > (_max))				\
		return -ERANGE;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

/* Preprocessing */
HX_ALGO_ATTR_BOOL_RW(baseline_enabled);
HX_ALGO_ATTR_S16_RW(baseline_noise_deadband, 0, 200);
HX_ALGO_ATTR_S16_RW(baseline_peak_threshold, 1, 2000);
HX_ALGO_ATTR_U8_RW(baseline_release_hold_frames, 0, 255);
HX_ALGO_ATTR_U8_RW(baseline_background_alpha_shift, 0, 15);
HX_ALGO_ATTR_U8_RW(baseline_no_finger_alpha_shift, 0, 15);
HX_ALGO_ATTR_U8_RW(baseline_recovery_alpha_shift, 0, 15);
HX_ALGO_ATTR_S16_RW(baseline_background_max_step, 1, 2048);
HX_ALGO_ATTR_S16_RW(baseline_no_finger_max_step, 1, 2048);
HX_ALGO_ATTR_S16_RW(baseline_recovery_max_step, 1, 2048);
HX_ALGO_ATTR_U8_RW(baseline_recovery_max_frames, 1, 120);
HX_ALGO_ATTR_BOOL_RW(baseline_noise_tracking);
HX_ALGO_ATTR_U8_RW(wake_stable_frames, 2, 30);
HX_ALGO_ATTR_U8_RW(wake_finger_safe_frames, 1, 15);
HX_ALGO_ATTR_S16_RW(wake_raw_jump_threshold, 1, 4095);
HX_ALGO_ATTR_U16_RW(wake_max_unstable_nodes, 0, HX_PIXELS);
HX_ALGO_ATTR_U8_RW(wake_max_unstable_line_nodes, 1, HX_ROWS);
HX_ALGO_ATTR_U8_RW(safe_commit_no_finger_frames, 1, 240);
HX_ALGO_ATTR_S16_RW(runtime_noise_threshold, 1, 8192);
HX_ALGO_ATTR_U8_RW(runtime_noise_line_nodes, 1, HX_COLS);
HX_ALGO_ATTR_U16_RW(runtime_noise_total_nodes, 1, HX_PIXELS);
HX_ALGO_ATTR_BOOL_RW(cmf_enabled);
HX_ALGO_ATTR_S16_RW(cmf_exclusion, 0, 32767);
HX_ALGO_ATTR_S16_RW(cmf_max_correction, 0, 32767);
HX_ALGO_ATTR_BOOL_RW(iir_enabled);
HX_ALGO_ATTR_U16_RW(iir_decay_weight, 0, 256);
HX_ALGO_ATTR_U16_RW(iir_decay_step, 0, 4095);
HX_ALGO_ATTR_S16_RW(iir_noise_floor, 0, 4095);
HX_ALGO_ATTR_S16_RW(iir_gate_floor, 0, 4095);
HX_ALGO_ATTR_U8_RW(iir_gate_ratio_q8, 0, 255);
/* Detection */
HX_ALGO_ATTR_S16_RW(macro_threshold, 1, 4095);
HX_ALGO_ATTR_S16_RW(peak_threshold, 1, 4095);
HX_ALGO_ATTR_U8_RW(peak_local_radius, 1, 5);
HX_ALGO_ATTR_BOOL_RW(peak_z8_enabled);
HX_ALGO_ATTR_BOOL_RW(peak_saddle_enabled);
HX_ALGO_ATTR_U8_RW(peak_saddle_radius, 1, 8);
HX_ALGO_ATTR_S16_RW(peak_saddle_drop, 0, 4095);
HX_ALGO_ATTR_S16_RW(peak_signal_threshold_limit, 1, 4095);
HX_ALGO_ATTR_S16_RW(peak_edge_threshold, 0, 4095);
HX_ALGO_ATTR_U8_RW(peak_macro_min_area, 1, 64);
HX_ALGO_ATTR_U8_RW(peak_continue_min_area, 1, 2);
HX_ALGO_ATTR_S16_RW(peak_continue_min_signal, 1, 4095);
HX_ALGO_ATTR_S16_RW(peak_single_track_continue_min_signal, 1, 4095);
HX_ALGO_ATTR_S32_RW(peak_continue_dist2, 1, 1000000);
HX_ALGO_ATTR_S16_RW(peak_fast_start_min_signal, 1, 4095);
HX_ALGO_ATTR_U8_RW(peak_fast_start_edge_cells, 1, 16);
HX_ALGO_ATTR_BOOL_RW(palm_enabled);
HX_ALGO_ATTR_U8_RW(palm_area_threshold, 0, 250);
HX_ALGO_ATTR_S32_RW(palm_signal_threshold, 0, 1000000);
HX_ALGO_ATTR_S16_RW(palm_density_low, 0, 4095);
HX_ALGO_ATTR_BOOL_RW(palm_box_enabled);
HX_ALGO_ATTR_U8_RW(palm_box_expand_rows, 0, 10);
HX_ALGO_ATTR_U8_RW(palm_box_expand_cols, 0, 10);
HX_ALGO_ATTR_U8_RW(palm_box_match_distance, 0, 30);
HX_ALGO_ATTR_U16_RW(palm_box_max_hold, 0, 300);
HX_ALGO_ATTR_BOOL_RW(zone_cleanup_enabled);
HX_ALGO_ATTR_U8_RW(zone_max_radius, 0, 16);
HX_ALGO_ATTR_U8_RW(zone_threshold_numer, 0, 255);
HX_ALGO_ATTR_U8_RW(zone_threshold_shift, 0, 15);
/* Pressure / touch-major reporting */
HX_ALGO_ATTR_BOOL_RW(pressure_enabled);
/* Edge compensation */
HX_ALGO_ATTR_BOOL_RW(edge_comp_enabled);
HX_ALGO_ATTR_S16_RW(edge_boost_pct, 0, 200);
HX_ALGO_ATTR_S16_RW(edge_push_q8, 0, 1280);
HX_ALGO_ATTR_S16_RW(edge_blend_q8, 1, 1280);
HX_ALGO_ATTR_BOOL_RW(edge_reject_enabled);
HX_ALGO_ATTR_U16_RW(edge_reject_margin, 0, 256);
HX_ALGO_ATTR_S32_RW(edge_reject_min_signal, 0, 1000000);
/* Tracking */
HX_ALGO_ATTR_S32_RW(track_dist2_max, 1, 16777216);
HX_ALGO_ATTR_U8_RW(track_lost_frames, 1, 16);
HX_ALGO_ATTR_U8_RW(debounce_base, 0, 16);
HX_ALGO_ATTR_BOOL_RW(track_smoothing);
HX_ALGO_ATTR_BOOL_RW(track_active_guard);
HX_ALGO_ATTR_U8_RW(track_start_debounce, 0, 16);
HX_ALGO_ATTR_S32_RW(track_jump_dist2, 0, 16777216);
HX_ALGO_ATTR_BOOL_RW(hungarian_enabled);
HX_ALGO_ATTR_U8_RW(debounce_weak_extra, 0, 16);
HX_ALGO_ATTR_U8_RW(debounce_edge_extra, 0, 16);
HX_ALGO_ATTR_S32_RW(debounce_strong_signal, 0, 1000000);
HX_ALGO_ATTR_BOOL_RW(firmware_edge_fast_start);
HX_ALGO_ATTR_U8_RW(split_peak_confirm_frames, 1, 16);
HX_ALGO_ATTR_S32_RW(split_peak_dist2, 1, 1000000);
HX_ALGO_ATTR_U8_RW(split_cross_zone_confirm_frames, 1, 16);
HX_ALGO_ATTR_S32_RW(split_cross_zone_dist2, 1, 1000000);
HX_ALGO_ATTR_S32_RW(track_peak_id_penalty, 0, 16777216);
HX_ALGO_ATTR_BOOL_RW(ghost_enabled);
HX_ALGO_ATTR_U16_RW(ghost_row_distance, 0, 512);
HX_ALGO_ATTR_U8_RW(ghost_weak_ratio_q8, 0, 255);
HX_ALGO_ATTR_U16_RW(ghost_min_col_distance, 0, 4096);
HX_ALGO_ATTR_BOOL_RW(euro_enabled);
HX_ALGO_ATTR_U8_RW(euro_alpha_min_q8, 1, 255);
HX_ALGO_ATTR_U8_RW(euro_alpha_max_q8, 1, 255);
HX_ALGO_ATTR_U16_RW(euro_speed_threshold, 1, 4096);

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
static ssize_t diagnostics_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	struct hx_algo *a = ts->algo;
	ssize_t len = 0;

	mutex_lock(&ts->op_lock);
	len += sysfs_emit_at(buf, len, "frame=%u common=%d max=%d signal=%u ",
			     a->diag_frame_seq, a->diag_common_diff,
			     a->diag_frame_max, a->diag_has_signal);
	len += sysfs_emit_at(buf, len, "zones=%u peaks=%u contacts_pre=%u ",
			     a->diag_zones, a->diag_peaks,
			     a->diag_contacts_pre_filter);
	len += sysfs_emit_at(buf, len, "contacts_post=%u active=%u reported=%u ",
			     a->diag_contacts_post_filter,
			     a->diag_active_tracks, a->diag_reported_tracks);
	len += sysfs_emit_at(buf, len,
			     "baseline_generation=%u full_resets=%u live_clears=%u ",
			     a->baseline_generation, a->full_reset_count,
			     a->live_clear_count);
	len += sysfs_emit_at(buf, len,
			     "cold_inits=%u warm_resumes=%u warm_fallbacks=%u ",
			     ts->cold_init_count, ts->warm_resume_count,
			     ts->warm_resume_fallback_count);
	len += sysfs_emit_at(buf, len,
			     "resume_stability_failures=%u afe_calibrations=%u ",
			     ts->resume_stability_failures,
			     ts->afe_calibration_count);
	len += sysfs_emit_at(buf, len,
			     "afe_calibration_failures=%u full_resume=%u ",
			     ts->afe_calibration_failures,
			     himax_full_resume_enabled());
	len += sysfs_emit_at(buf, len,
			     "wake_qualifications=%u wake_candidate_rejects=%u ",
			     a->wake_qualification_count,
			     a->wake_candidate_reject_count);
	len += sysfs_emit_at(buf, len,
			     "wake_safe_fallbacks=%u wake_baseline_commits=%u ",
			     a->wake_safe_fallback_count,
			     a->wake_baseline_commit_count);
	len += sysfs_emit_at(buf, len,
			     "wake_safe_divergences=%u safe_baseline_commits=%u ",
			     a->wake_safe_divergence_count,
			     a->baseline_safe_commit_count);
	len += sysfs_emit_at(buf, len,
			     "noise_frame_holds=%u small_peak_continued=%u ",
			     a->noise_frame_hold_count,
			     a->diag_small_peak_continued);
	len += sysfs_emit_at(buf, len,
			     "weak_peak_continued=%u small_peak_rejected=%u ",
			     a->diag_weak_peak_continued,
			     a->diag_small_peak_rejected);
	len += sysfs_emit_at(buf, len,
			     "split_peak_deferred=%u cross_zone_split_deferred=%u ",
			     a->diag_split_peak_deferred,
			     a->diag_cross_zone_split_deferred);
	len += sysfs_emit_at(buf, len,
			     "peak_id_handoffs=%u handoff_residual_deferred=%u ",
			     a->diag_peak_id_handoffs,
			     a->diag_handoff_residual_deferred);
	len += sysfs_emit_at(buf, len, "fast_edge_starts=%u\n",
			     a->diag_fast_edge_starts);
	mutex_unlock(&ts->op_lock);
	return len;
}
static DEVICE_ATTR_RO(diagnostics);

/*
 * A bounded, read-only snapshot of the most recently received event stack.
 * This is intentionally exposed as hex rather than as a binary sysfs file so
 * an unprivileged diagnostic helper can preserve it without a custom ioctl.
 * The buffer is protected by op_lock, just like diagnostics_show().
 */
static ssize_t event_stack_hex_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	ssize_t len = 0;
	u32 i;

	mutex_lock(&ts->op_lock);
	for (i = 0; i < ts->event_buf_sz && len < PAGE_SIZE - 3; i++)
		len += sysfs_emit_at(buf, len, "%02x%s", ts->event_buf[i],
					(i + 1) % 32 ? "" : "\n");
	if (len && buf[len - 1] != '\n' && len < PAGE_SIZE - 1)
		buf[len++] = '\n';
	mutex_unlock(&ts->op_lock);
	return len;
}
static DEVICE_ATTR_RO(event_stack_hex);

/* Full last-good event stack.  Reads are snapshots only; no SPI I/O occurs. */
static ssize_t event_stack_read(struct file *file, struct kobject *kobj,
				const struct bin_attribute *attr, char *buf,
				loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	size_t available;

	if (offset < 0 || offset >= ts->event_buf_sz)
		return 0;

	available = ts->event_buf_sz - offset;
	count = min(count, available);

	mutex_lock(&ts->op_lock);
	memcpy(buf, ts->event_buf + offset, count);
	mutex_unlock(&ts->op_lock);

	return count;
}

static const struct bin_attribute event_stack_attr = {
	.attr = {
		.name = "event_stack",
		.mode = 0444,
	},
	.size = HIMAX_FULL_EVENT_STACK_SIZE,
	.read = event_stack_read,
};

void himax_trace_record_irq(struct himax_ts_data *ts, int read_error,
			    bool master_valid,
			    const struct hx_frame_status *frame_status,
			    bool noise_hold)
{
	struct himax_trace_record *record;
	struct hx_algo *algo = ts->algo;
	u32 flags = 0;
	int i;

	if (!ts->trace_ring || !algo)
		return;

	record = &ts->trace_ring[ts->trace_write_index];
	memset(record, 0, sizeof(*record));
	record->boottime_ns = cpu_to_le64(ktime_get_boottime_ns());
	record->irq_sequence = cpu_to_le32(++ts->trace_irq_sequence);
	record->reset_generation = cpu_to_le32(ts->reset_generation);
	record->read_error = cpu_to_le32((u32)read_error);

	if (!read_error)
		flags |= HIMAX_TRACE_F_READ_OK;
	if (master_valid)
		flags |= HIMAX_TRACE_F_MASTER_VALID;
	if (master_valid && frame_status->retry)
		flags |= HIMAX_TRACE_F_RETRY;
	if (master_valid && frame_status->has_finger)
		flags |= HIMAX_TRACE_F_HAS_FINGER;
	if (noise_hold)
		flags |= HIMAX_TRACE_F_NOISE_HOLD;
	record->flags = cpu_to_le32(flags);

	record->algo_frame_sequence = cpu_to_le32(algo->diag_frame_seq);
	record->common_diff = cpu_to_le32((u32)algo->diag_common_diff);
	record->frame_max = cpu_to_le16((u16)algo->diag_frame_max);
	record->has_signal = algo->diag_has_signal;
	record->zones = algo->diag_zones;
	record->peaks = algo->diag_peaks;
	record->contacts_pre = algo->diag_contacts_pre_filter;
	record->contacts_post = algo->diag_contacts_post_filter;
	record->active_tracks = algo->diag_active_tracks;
	record->reported_tracks = algo->diag_reported_tracks;

	for (i = 0; i < min_t(int, algo->peak_count, HX_MAX_PEAKS); i++) {
		record->peak[i].row = algo->peaks[i].r;
		record->peak[i].col = algo->peaks[i].c;
		record->peak[i].signal = cpu_to_le16((u16)algo->peaks[i].z);
		record->peak[i].zone_area = cpu_to_le16(algo->peaks[i].zone_area);
		record->peak[i].zone_index = algo->peaks[i].zone_index;
		record->peak[i].flags = algo->peaks[i].on_edge ? BIT(0) : 0;
		record->peak[i].id = algo->peaks[i].id;
		record->peak[i].age = algo->peaks[i].age;
	}

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		const struct hx_track *track = &algo->tracks[i];

		record->track[i].flags = (track->active ? BIT(0) : 0) |
					 (track->reported ? BIT(1) : 0);
		record->track[i].age = track->age;
		record->track[i].missed = track->missed;
		record->track[i].debounce = track->debounce;
		record->track[i].source_peak_id = track->source_peak_id;
		record->track[i].source_peak_age = track->source_peak_age;
		record->track[i].x = cpu_to_le32((u32)track->x);
		record->track[i].y = cpu_to_le32((u32)track->y);
		record->track[i].vx = cpu_to_le32((u32)track->vx);
		record->track[i].vy = cpu_to_le32((u32)track->vy);
		record->track[i].signal_sum = cpu_to_le32((u32)track->signal_sum);
		record->track[i].filtered_x_q8 =
			cpu_to_le32((u32)track->filtered_x_q8);
		record->track[i].filtered_y_q8 =
			cpu_to_le32((u32)track->filtered_y_q8);
		record->track[i].deriv_x_q8 = cpu_to_le32((u32)track->deriv_x_q8);
		record->track[i].deriv_y_q8 = cpu_to_le32((u32)track->deriv_y_q8);
	}

	if (master_valid && !frame_status->retry) {
		for (i = 0; i < HX_PIXELS; i++)
			record->processed_frame[i] =
				cpu_to_le16((u16)((s16 *)algo->frame)[i]);
	}
	if (!read_error)
		memcpy(record->event_stack, ts->event_buf, ts->event_buf_sz);

	ts->trace_write_index =
		(ts->trace_write_index + 1) % HIMAX_TRACE_CAPACITY;
	if (ts->trace_count < HIMAX_TRACE_CAPACITY)
		ts->trace_count++;
}

static void himax_trace_build_snapshot(struct himax_ts_data *ts)
{
	struct himax_trace_header *header;
	u32 count = ts->trace_count;
	u32 start;
	u32 i;

	header = (struct himax_trace_header *)ts->trace_snapshot;
	memset(header, 0, sizeof(*header));
	header->magic = cpu_to_le32(HIMAX_TRACE_MAGIC);
	header->version = cpu_to_le16(HIMAX_TRACE_VERSION);
	header->header_size = cpu_to_le16(sizeof(*header));
	header->record_size = cpu_to_le32(sizeof(struct himax_trace_record));
	header->capacity = cpu_to_le32(HIMAX_TRACE_CAPACITY);
	header->count = cpu_to_le32(count);
	header->snapshot_boottime_ns = cpu_to_le64(ktime_get_boottime_ns());
	header->reset_generation = cpu_to_le64(ts->reset_generation);

	start = (ts->trace_write_index + HIMAX_TRACE_CAPACITY - count) %
		HIMAX_TRACE_CAPACITY;
	for (i = 0; i < count; i++) {
		void *dst = ts->trace_snapshot + sizeof(*header) +
			i * sizeof(struct himax_trace_record);
		u32 src = (start + i) % HIMAX_TRACE_CAPACITY;

		memcpy(dst, &ts->trace_ring[src], sizeof(struct himax_trace_record));
	}
	ts->trace_snapshot_len = sizeof(*header) +
		count * sizeof(struct himax_trace_record);
}

static ssize_t event_trace_read(struct file *file, struct kobject *kobj,
				const struct bin_attribute *attr, char *buf,
				loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	size_t available;

	mutex_lock(&ts->op_lock);
	if (offset == 0)
		himax_trace_build_snapshot(ts);
	if (offset < 0 || offset >= ts->trace_snapshot_len) {
		count = 0;
		goto out_unlock;
	}

	available = ts->trace_snapshot_len - offset;
	count = min(count, available);
	memcpy(buf, ts->trace_snapshot + offset, count);

out_unlock:
	mutex_unlock(&ts->op_lock);
	return count;
}

static const struct bin_attribute event_trace_attr = {
	.attr = {
		.name = "event_trace",
		.mode = 0444,
	},
	.size = sizeof(struct himax_trace_header) +
		HIMAX_TRACE_CAPACITY * sizeof(struct himax_trace_record),
	.read = event_trace_read,
};
#endif

static struct attribute *hx_algo_attrs[] = {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	&dev_attr_diagnostics.attr,
	&dev_attr_event_stack_hex.attr,
#endif
	&dev_attr_baseline_enabled.attr,
	&dev_attr_baseline_noise_deadband.attr,
	&dev_attr_baseline_peak_threshold.attr,
	&dev_attr_baseline_release_hold_frames.attr,
	&dev_attr_baseline_background_alpha_shift.attr,
	&dev_attr_baseline_no_finger_alpha_shift.attr,
	&dev_attr_baseline_recovery_alpha_shift.attr,
	&dev_attr_baseline_background_max_step.attr,
	&dev_attr_baseline_no_finger_max_step.attr,
	&dev_attr_baseline_recovery_max_step.attr,
	&dev_attr_baseline_recovery_max_frames.attr,
	&dev_attr_baseline_noise_tracking.attr,
	&dev_attr_wake_stable_frames.attr,
	&dev_attr_wake_finger_safe_frames.attr,
	&dev_attr_wake_raw_jump_threshold.attr,
	&dev_attr_wake_max_unstable_nodes.attr,
	&dev_attr_wake_max_unstable_line_nodes.attr,
	&dev_attr_safe_commit_no_finger_frames.attr,
	&dev_attr_runtime_noise_threshold.attr,
	&dev_attr_runtime_noise_line_nodes.attr,
	&dev_attr_runtime_noise_total_nodes.attr,
	&dev_attr_cmf_enabled.attr,
	&dev_attr_cmf_exclusion.attr,
	&dev_attr_cmf_max_correction.attr,
	&dev_attr_iir_enabled.attr,
	&dev_attr_iir_decay_weight.attr,
	&dev_attr_iir_decay_step.attr,
	&dev_attr_iir_noise_floor.attr,
	&dev_attr_iir_gate_floor.attr,
	&dev_attr_iir_gate_ratio_q8.attr,
	&dev_attr_macro_threshold.attr,
	&dev_attr_peak_threshold.attr,
	&dev_attr_peak_local_radius.attr,
	&dev_attr_peak_z8_enabled.attr,
	&dev_attr_peak_saddle_enabled.attr,
	&dev_attr_peak_saddle_radius.attr,
	&dev_attr_peak_saddle_drop.attr,
	&dev_attr_peak_signal_threshold_limit.attr,
	&dev_attr_peak_edge_threshold.attr,
	&dev_attr_peak_macro_min_area.attr,
	&dev_attr_peak_continue_min_area.attr,
	&dev_attr_peak_continue_min_signal.attr,
	&dev_attr_peak_single_track_continue_min_signal.attr,
	&dev_attr_peak_continue_dist2.attr,
	&dev_attr_peak_fast_start_min_signal.attr,
	&dev_attr_peak_fast_start_edge_cells.attr,
	&dev_attr_palm_enabled.attr,
	&dev_attr_palm_area_threshold.attr,
	&dev_attr_palm_signal_threshold.attr,
	&dev_attr_palm_density_low.attr,
	&dev_attr_palm_box_enabled.attr,
	&dev_attr_palm_box_expand_rows.attr,
	&dev_attr_palm_box_expand_cols.attr,
	&dev_attr_palm_box_match_distance.attr,
	&dev_attr_palm_box_max_hold.attr,
	&dev_attr_zone_cleanup_enabled.attr,
	&dev_attr_zone_max_radius.attr,
	&dev_attr_zone_threshold_numer.attr,
	&dev_attr_zone_threshold_shift.attr,
	&dev_attr_pressure_enabled.attr,
	&dev_attr_edge_comp_enabled.attr,
	&dev_attr_edge_boost_pct.attr,
	&dev_attr_edge_push_q8.attr,
	&dev_attr_edge_blend_q8.attr,
	&dev_attr_edge_reject_enabled.attr,
	&dev_attr_edge_reject_margin.attr,
	&dev_attr_edge_reject_min_signal.attr,
	&dev_attr_track_dist2_max.attr,
	&dev_attr_track_lost_frames.attr,
	&dev_attr_debounce_base.attr,
	&dev_attr_track_smoothing.attr,
	&dev_attr_track_active_guard.attr,
	&dev_attr_track_start_debounce.attr,
	&dev_attr_track_jump_dist2.attr,
	&dev_attr_hungarian_enabled.attr,
	&dev_attr_debounce_weak_extra.attr,
	&dev_attr_debounce_edge_extra.attr,
	&dev_attr_debounce_strong_signal.attr,
	&dev_attr_firmware_edge_fast_start.attr,
	&dev_attr_split_peak_confirm_frames.attr,
	&dev_attr_split_peak_dist2.attr,
	&dev_attr_split_cross_zone_confirm_frames.attr,
	&dev_attr_split_cross_zone_dist2.attr,
	&dev_attr_track_peak_id_penalty.attr,
	&dev_attr_ghost_enabled.attr,
	&dev_attr_ghost_row_distance.attr,
	&dev_attr_ghost_weak_ratio_q8.attr,
	&dev_attr_ghost_min_col_distance.attr,
	&dev_attr_euro_enabled.attr,
	&dev_attr_euro_alpha_min_q8.attr,
	&dev_attr_euro_alpha_max_q8.attr,
	&dev_attr_euro_speed_threshold.attr,
	NULL,
};

static const struct attribute_group hx_algo_attr_group = {
	.name = "algo",
	.attrs = hx_algo_attrs,
};

int himax_sysfs_init(struct himax_ts_data *ts)
{
	int ret;

	ret = device_create_file(ts->dev, &dev_attr_inplace_reset);
	if (ret)
		return ret;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ret = sysfs_create_bin_file(&ts->dev->kobj, &event_stack_attr);
	if (ret)
		goto remove_reset;
	ret = sysfs_create_bin_file(&ts->dev->kobj, &event_trace_attr);
	if (ret)
		goto remove_event_stack;
#endif

	ret = devm_device_add_group(ts->dev, &hx_algo_attr_group);
	if (!ret)
		return 0;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	sysfs_remove_bin_file(&ts->dev->kobj, &event_trace_attr);
remove_event_stack:
	sysfs_remove_bin_file(&ts->dev->kobj, &event_stack_attr);
remove_reset:
#endif
	device_remove_file(ts->dev, &dev_attr_inplace_reset);
	return ret;
}

void himax_sysfs_remove(struct himax_ts_data *ts)
{
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	sysfs_remove_bin_file(&ts->dev->kobj, &event_trace_attr);
	sysfs_remove_bin_file(&ts->dev->kobj, &event_stack_attr);
#endif
	device_remove_file(ts->dev, &dev_attr_inplace_reset);
}
