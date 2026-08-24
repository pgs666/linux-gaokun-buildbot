// SPDX-License-Identifier: GPL-2.0
/*
 * Himax SPI Driver
 * based on Himax hx83102j
 *
 * Copyright (C) 2019,2024 Himax Corporation.
 * Copyright (C) 2026 Pengyu Luo <mitltlatltl@gmail.com>
 */

#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
#include <linux/timekeeping.h>
#endif
#include <linux/workqueue.h>

#include <drm/drm_panel.h>

#include "himax-spi.h"

static bool disable_pressure = true;
module_param(disable_pressure, bool, 0444);
MODULE_PARM_DESC(disable_pressure,
		 "Disable ABS_MT_TOUCH_MAJOR and ABS_MT_PRESSURE axes");

static bool restore_afe_runtime = true;
module_param(restore_afe_runtime, bool, 0644);
MODULE_PARM_DESC(restore_afe_runtime,
		 "Replay the 120 Hz scan-rate command after hardware initialization");

static bool full_resume_on_panel_enable = true;
module_param(full_resume_on_panel_enable, bool, 0644);
MODULE_PARM_DESC(full_resume_on_panel_enable,
		 "Run complete reset/reload/wake qualification on panel enable");

bool himax_full_resume_enabled(void)
{
	return full_resume_on_panel_enable;
}

bool himax_restore_afe_enabled(void)
{
	return restore_afe_runtime;
}

static void himax_report_tracked_state(struct himax_ts_data *ts, bool report_on);
static int hx83121a_gaokun_read_event_stack(struct himax_ts_data *ts);
static int himax_wait_for_stable_event_frames(struct himax_ts_data *ts);
static int himax_input_dev_config(struct himax_ts_data *ts)
{
	struct input_dev *input_dev;
	int ret;

	input_dev = devm_input_allocate_device(ts->dev);
	if (!input_dev)
		return -ENOMEM;

	ts->input_dev = input_dev;
	input_set_drvdata(input_dev, ts);

	input_dev->name = "Himax Capacitive TouchScreen";
	input_dev->phys = "input/ts";
	input_dev->id.bustype = BUS_SPI;

	/* Standard capacitive touchscreen fuzz (8) to absorb static finger deformation
	 * during clicks, preventing libinput from treating 10px drifts as swipes.
	 */
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X,
			     0, SZ_64K - 1, 8, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
			     0, SZ_64K - 1, 8, 0);
	if (!disable_pressure) {
		input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
		input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, 4095, 0, 0);
		dev_info(ts->dev, "pressure/touch_major axes enabled\n");
	} else {
		dev_info(ts->dev, "pressure/touch_major axes disabled by module parameter\n");
	}
	touchscreen_parse_properties(ts->input_dev, true, &ts->props);

	ret = input_mt_init_slots(ts->input_dev, HIMAX_MAX_TOUCH,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(ts->input_dev);
	if (ret)
		return ret;

	return 0;
}

static void himax_release_all_touches(struct himax_ts_data *ts)
{
	if (ts->algo)
		hx_algo_clear_live_state(ts->algo);

	if (ts->input_dev)
		himax_report_tracked_state(ts, false);
}

static int himax_warm_resume(struct himax_ts_data *ts)
{
	int ret;

	himax_release_all_touches(ts);
	hx_algo_begin_wake(ts->algo);
	ret = himax_verify_running_ic(ts);
	if (ret)
		return ret;
	ret = himax_restore_raw_runtime(ts);
	if (ret)
		return ret;

	ret = himax_restore_afe_runtime(ts);
	if (ret)
		dev_warn(ts->dev,
			 "warm-resume AFE mode replay incomplete: %d\n", ret);

	ret = himax_wait_for_stable_event_frames(ts);
	if (ret)
		return ret;

	ts->retained_suspend = false;
	ts->controller_initialized = true;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ts->warm_resume_count++;
#endif
	himax_int_enable(ts, true);
	dev_info(ts->dev, "retained-state resume completed\n");
	return 0;
}

static int himax_hw_reinit(struct himax_ts_data *ts, bool check_crc)
{
	u32 crc_hw;
	int afe_ret;
	int ret;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ts->reset_generation++;
#endif
	ts->controller_initialized = false;
	ts->retained_suspend = false;
	himax_release_all_touches(ts);
	hx_algo_begin_wake(ts->algo);

	ret = hx83121a_chip_detect(ts);
	if (ret) {
		dev_err(ts->dev, "%s: IC detect failed\n", __func__);
		goto out_enable_irq;
	}

	if (check_crc) {
		ret = himax_mcu_check_crc(ts, 0, HIMAX_HX83121A_FLASH_SIZE, &crc_hw);
		if (ret || crc_hw) {
			if (!ret && crc_hw)
				ret = -EINVAL;
			dev_err(ts->dev, "hw crc failed, fw broken, fix it on windows\n");
			goto out_enable_irq;
		}
	}

	ret = himax_enable_fw_reload(ts);
	if (ret < 0) {
		dev_err(ts->dev, "%s: enable FW reload fail\n", __func__);
		goto out_enable_irq;
	}

	ret = himax_mcu_power_on_init(ts);
	if (ret < 0) {
		dev_err(ts->dev, "%s: power-on init failed\n", __func__);
	} else {
		/*
		 * Windows restores these AFE settings after every Chip::Init().
		 * They are best-effort because older firmware may not expose the
		 * command queue even though touch reporting itself is usable.
		 */
		afe_ret = himax_restore_afe_runtime(ts);
		if (afe_ret)
			dev_warn(ts->dev,
				 "AFE runtime restore incomplete; continuing with firmware defaults\n");
		ret = himax_restore_raw_runtime(ts);
		if (ret)
			dev_err(ts->dev, "cold-init runtime replay failed: %d\n",
				ret);
		if (!ret)
			ret = himax_wait_for_stable_event_frames(ts);
	}

out_enable_irq:
	if (!ret) {
		ts->controller_initialized = true;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		ts->cold_init_count++;
#endif
		himax_int_enable(ts, true);
	}
	return ret;
}

static int himax_hw_reinit_retry(struct himax_ts_data *ts, bool check_crc,
				 int retries, unsigned int delay_ms,
				 const char *reason)
{
	int ret;
	int attempt;

	for (attempt = 1; attempt <= retries; attempt++) {
		ret = himax_hw_reinit(ts, check_crc);
		if (!ret)
			return 0;

		if (attempt < retries) {
			dev_warn(ts->dev,
				 "%s reinit attempt %d/%d failed, retrying in %u ms\n",
				 reason, attempt, retries, delay_ms);
			msleep(delay_ms);
		}
	}

	dev_err(ts->dev, "%s reinit failed after %d attempts\n", reason, retries);
	return ret;
}

int himax_manual_reset(struct himax_ts_data *ts)
{
	int ret;

	cancel_delayed_work_sync(&ts->panel_reinit_work);
	himax_lock(ts);
	if (ts->shutting_down) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!ts->panel_prepared || !ts->panel_enabled) {
		ret = -EHOSTDOWN;
		goto out_unlock;
	}

	ret = himax_hw_reinit_retry(ts, false,
				    HIMAX_PANEL_REINIT_RETRIES,
				    HIMAX_PANEL_REINIT_DELAY_MS,
				    "manual");
out_unlock:
	himax_unlock(ts);
	return ret;
}

static void himax_power_down(struct himax_ts_data *ts)
{
	himax_int_enable(ts, false);
	himax_release_all_touches(ts);
	gpiod_set_value_cansleep(ts->gpiod_rst, 1);
	ts->controller_initialized = false;
	ts->retained_suspend = false;
}

static void himax_retain_for_suspend(struct himax_ts_data *ts)
{
	himax_int_enable(ts, false);
	himax_release_all_touches(ts);
	ts->consecutive_frame_errors = 0;
	ts->retained_suspend = ts->controller_initialized;
}

static int himax_panel_prepared(struct drm_panel_follower *follower)
{
	struct himax_ts_data *ts = container_of(follower, struct himax_ts_data,
						panel_follower);

	himax_lock(ts);
	if (ts->shutting_down) {
		himax_unlock(ts);
		return 0;
	}

	ts->panel_prepared = true;
	himax_unlock(ts);

	return 0;
}

static void himax_panel_reinit_work(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(to_delayed_work(work),
						struct himax_ts_data,
						panel_reinit_work);
	int ret;

	himax_lock(ts);
	if (ts->shutting_down || !ts->panel_prepared || !ts->panel_enabled) {
		himax_unlock(ts);
		return;
	}

	/* Gaokun Windows performs the complete reset/reload path on screen-on.
	 * Keep retained resume as an explicit A/B escape hatch, but default to the
	 * deterministic full path while the electrical wake issue is evaluated.
	 */
	if (!full_resume_on_panel_enable && ts->retained_suspend &&
	    ts->controller_initialized) {
		ret = himax_warm_resume(ts);
		if (ret) {
			dev_warn(ts->dev,
				 "retained-state resume failed (%d), falling back to hardware init\n",
				 ret);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			ts->warm_resume_fallback_count++;
#endif
			ret = himax_hw_reinit_retry(ts, false,
				HIMAX_PANEL_REINIT_RETRIES,
				HIMAX_PANEL_REINIT_DELAY_MS, "panel-fallback");
		}
	} else {
		ret = himax_hw_reinit_retry(ts, false,
				HIMAX_PANEL_REINIT_RETRIES,
				HIMAX_PANEL_REINIT_DELAY_MS,
				full_resume_on_panel_enable ? "panel-full" :
				"panel-cold");
	}
	if (ret)
		himax_power_down(ts);
	himax_unlock(ts);
}

static int himax_panel_enabled(struct drm_panel_follower *follower)
{
	struct himax_ts_data *ts = container_of(follower, struct himax_ts_data,
						panel_follower);

	himax_lock(ts);
	if (ts->shutting_down) {
		himax_unlock(ts);
		return 0;
	}

	ts->panel_enabled = true;
	himax_unlock(ts);

	mod_delayed_work(system_wq, &ts->panel_reinit_work,
			 msecs_to_jiffies(HIMAX_PANEL_ENABLE_SETTLE_MS));
	return 0;
}

static int himax_panel_disabling(struct drm_panel_follower *follower)
{
	struct himax_ts_data *ts = container_of(follower, struct himax_ts_data,
						panel_follower);

	cancel_delayed_work_sync(&ts->panel_reinit_work);

	himax_lock(ts);
	ts->panel_enabled = false;
	if (ts->panel_prepared)
		himax_retain_for_suspend(ts);
	himax_unlock(ts);

	return 0;
}

static int himax_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct himax_ts_data *ts = container_of(follower, struct himax_ts_data,
						panel_follower);

	cancel_delayed_work_sync(&ts->panel_reinit_work);

	himax_lock(ts);
	ts->panel_enabled = false;
	ts->panel_prepared = false;
	himax_retain_for_suspend(ts);
	himax_unlock(ts);

	return 0;
}

static const struct drm_panel_follower_funcs himax_panel_follower_funcs = {
	.panel_prepared = himax_panel_prepared,
	.panel_unpreparing = himax_panel_unpreparing,
	.panel_enabled = himax_panel_enabled,
	.panel_disabling = himax_panel_disabling,
};

/* -------------------------------------------------------------------------- */
/* 中断处理 */

static int hx83121a_gaokun_read_event_stack(struct himax_ts_data *ts)
{
	u32 i;
	int ret;
	const u32 max_trunk_sz = ts->spi_xfer_max_sz -
		HIMAX_BUS_READ_HEADER_SIZE;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	u8 *buf = ts->event_read_buf;

	/* Never destroy the last successful frame while attempting a new one. */
	memset(buf, 0x00, ts->event_buf_sz);
#else
	u8 *buf = ts->event_buf;
#endif
	size_t length = HIMAX_FULL_EVENT_STACK_SIZE;

	for (i = 0; i < length; i += max_trunk_sz) {
		ret = himax_spi_read(ts, HIMAX_EVENT_STACK_ADDRESS, buf + i,
					 min(length - i, max_trunk_sz));
		if (ret) {
			dev_err(ts->dev, "%s: read event stack error!\n", __func__);
			return ret;
		}
	}
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	memcpy(ts->event_buf, buf, length);
#endif

	return 0;
}

/* The Xiaomi coordinate path can trust a checksum-protected firmware packet
 * immediately after resume.  Our 0xf6 raw producer has a separate SRAM
 * handshake, so do not expose IRQs until several newly sampled master frames
 * prove that the producer is running and no retry frame is pending.
 */
static int himax_wait_for_stable_event_frames(struct himax_ts_data *ts)
{
	struct hx_frame_status status;
	u32 previous_hash = 0;
	bool have_previous = false;
	int attempt;
	int ret = -EIO;

	for (attempt = 0; attempt < HIMAX_RESUME_QUALITY_ATTEMPTS; attempt++) {
		u32 hash = 2166136261U;
		u32 i;
		int quality;

		memset(&status, 0, sizeof(status));
		usleep_range(8000, 9000);
		ret = hx83121a_gaokun_read_event_stack(ts);
		if (!ret &&
		    hx_parse_master_frame_status(ts->event_buf, ts->event_buf_sz,
						 &status) &&
		    !status.retry) {
			for (i = 0; i < ts->event_buf_sz; i++)
				hash = (hash ^ ts->event_buf[i]) * 16777619U;
			/* Valid but byte-identical reads can be a stale SRAM image;
			 * they do not prove that scanning resumed.
			 */
			if (have_previous && hash == previous_hash)
				continue;
			previous_hash = hash;
			have_previous = true;
			quality = hx_algo_qualify_wake_frame(ts->algo,
				(const u16 *)(ts->event_buf + HX_FRAME_HEADER_BYTES),
				status.has_finger ? HX_FINGER_PRESENT :
				HX_FINGER_ABSENT);
			if (quality > HX_WAKE_QUALITY_PENDING) {
				const char *baseline_source = "new baseline";

				if (quality == HX_WAKE_QUALITY_USING_SAFE)
					baseline_source = "last-safe baseline";
				else if (quality == HX_WAKE_QUALITY_PROTECTED)
					baseline_source = "protected neutral baseline";
				dev_info(ts->dev,
					 "raw producer passed wake quality (%s)\n",
					 baseline_source);
				return 0;
			}
			if (quality == HX_WAKE_QUALITY_REJECTED)
				ret = -EUCLEAN;
		} else {
			if (!ret)
				ret = -EAGAIN;
		}
	}

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ts->resume_stability_failures++;
#endif
	dev_err(ts->dev,
		"raw event producer did not pass wake quality after resume\n");
	return ret;
}


static void himax_report_tracked_state(struct himax_ts_data *ts, bool report_on)
{
	int i;

	if (!ts->algo || !ts->input_dev)
		return;

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		bool on = report_on && ts->algo->tracks[i].active &&
			  ts->algo->tracks[i].reported;

		input_mt_slot(ts->input_dev, i);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, on);
		if (!on)
			continue;

		touchscreen_report_pos(ts->input_dev, &ts->props,
				       ts->algo->tracks[i].x, ts->algo->tracks[i].y, true);
		if (!disable_pressure) {
			if (ts->algo->pressure_enabled) {
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
						 min_t(s32, int_sqrt(ts->algo->tracks[i].signal_sum >> 4) * 4, 255));
				input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
						 clamp_t(s32, ts->algo->tracks[i].signal_sum >> 2, 0, 4095));
			} else {
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 1);
				input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 4095);
			}
		}
	}

	/*
	 * Also emit single-touch pointer emulation so compositors that still
	 * key parts of their touchscreen handling off ABS_X/ABS_Y or BTN_TOUCH
	 * observe a coherent state across suspend/resume.
	 */
	input_mt_report_pointer_emulation(ts->input_dev, true);
	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);
}

/* Byte offset into the SPI event buffer where the raw capacitance grid starts. */
#define OFST 4

static irqreturn_t himax_ts_thread(int irq, void *data)
{
	struct himax_ts_data *ts = data;
	u16 *ptr = (u16 *)(ts->event_buf + OFST);
	struct hx_algo *algo = ts->algo;
	struct hx_frame_status frame_status = { 0 };
	bool master_valid;
	int read_ret;
	int stable_cnt;
	irqreturn_t irq_ret = IRQ_HANDLED;

	mutex_lock(&ts->op_lock);
	if (!algo) {
		irq_ret = IRQ_NONE;
		goto out_unlock;
	}

	read_ret = hx83121a_gaokun_read_event_stack(ts);
	if (read_ret) {
		himax_trace_record_irq(ts, read_ret, false, &frame_status, false);
		/* AFE transitions can transiently lose one or two frames.  Resetting
		 * the controller on the first miss creates a long, user-visible UP/
		 * DOWN break.  Match the Windows runtime recovery policy and recover
		 * only after a consecutive failure streak.
		 */
		if (++ts->consecutive_frame_errors >=
		    HIMAX_MAX_CONSECUTIVE_FRAME_ERRORS) {
			int recovery_ret;

			dev_err(ts->dev, "%u consecutive frame reads failed, resetting controller\n",
				ts->consecutive_frame_errors);
			ts->consecutive_frame_errors = 0;
			/* A pin reset alone can leave the controller in a partially
			 * initialized state.  Run the same full reset/detect/FW-init
			 * sequence used by probe and panel recovery.  chip_detect()
			 * starts every attempt with a hardware reset.
			 */
			himax_int_enable(ts, false);
			recovery_ret = himax_hw_reinit_retry(ts, false,
							HIMAX_PANEL_REINIT_RETRIES,
							HIMAX_PANEL_REINIT_DELAY_MS,
							"frame");
			if (recovery_ret)
				dev_err(ts->dev,
					"frame recovery failed; touch IRQ remains disabled\n");
		} else {
			dev_warn_ratelimited(ts->dev,
				"touch frame read failed (%u/%u), retaining tracks\n",
				ts->consecutive_frame_errors,
				HIMAX_MAX_CONSECUTIVE_FRAME_ERRORS);
		}
		goto out_unlock;
	}
	ts->consecutive_frame_errors = 0;
	master_valid = hx_parse_master_frame_status(ts->event_buf,
						   ts->event_buf_sz,
						   &frame_status);
	if (!master_valid || frame_status.retry) {
		/* A shifted, non-master, or retry frame is not a missing touch frame:
		 * retain and re-report current tracks without feeding garbage into the
		 * baseline or advancing tracker miss counters.
		 */
		dev_warn_ratelimited(ts->dev,
			"discarding invalid/retry master event-stack frame\n");
		stable_cnt = hx_count_stable_tracks(algo);
		himax_trace_record_irq(ts, 0, master_valid, &frame_status, false);
		goto report;
	}
	if (hx_algo_is_exception_frame(algo, ptr)) {
		int recovery_ret;

		stable_cnt = hx_count_stable_tracks(algo);
		himax_trace_record_irq(ts, 0, true, &frame_status, true);
		if (++ts->consecutive_noise_frames <
		    HIMAX_MAX_CONSECUTIVE_NOISE_FRAMES) {
			dev_warn_ratelimited(ts->dev,
				"holding tracks across electrical-noise frame (%u/%u)\n",
				ts->consecutive_noise_frames,
				HIMAX_MAX_CONSECUTIVE_NOISE_FRAMES);
			goto report;
		}
		ts->consecutive_noise_frames = 0;
		dev_err(ts->dev,
			"persistent electrical-noise frames, reinitializing controller\n");
		himax_int_enable(ts, false);
		recovery_ret = himax_hw_reinit_retry(ts, false,
			HIMAX_PANEL_REINIT_RETRIES, HIMAX_PANEL_REINIT_DELAY_MS,
			"noise");
		if (recovery_ret)
			dev_err(ts->dev,
				"noise recovery failed; touch IRQ remains disabled\n");
		goto out_unlock;
	}
	ts->consecutive_noise_frames = 0;

	stable_cnt = hx_algo_process_frame_state(algo, ptr,
		frame_status.has_finger ? HX_FINGER_PRESENT : HX_FINGER_ABSENT);
	himax_trace_record_irq(ts, 0, true, &frame_status, false);

report:
	himax_report_tracked_state(ts, stable_cnt > 0);

out_unlock:
	mutex_unlock(&ts->op_lock);
	return irq_ret;
}

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
static void himax_kvfree_action(void *data)
{
	kvfree(data);
}
#endif

static int himax_spi_probe(struct spi_device *spi)
{
	int ret;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	size_t trace_ring_size;
	size_t trace_snapshot_size;
#endif
	struct himax_ts_data *ts;

	ts = devm_kzalloc(&spi->dev, sizeof(struct himax_ts_data), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->dev = &spi->dev;

	ts->gpiod_rst = devm_gpiod_get_optional(ts->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->gpiod_rst)) {
		dev_err(ts->dev, "%s: gpio-rst value is not valid\n", __func__);
		return -EIO;
	}

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_3;
	spi->cs_setup.value = HIMAX_SPI_CS_SETUP_TIME;

	ts->spi = spi;
	ts->spi_xfer_max_sz = HIMAX_FULL_EVENT_STACK_SIZE;
	ts->xfer_buf_sz = ts->spi_xfer_max_sz;
	ts->event_buf_sz = HIMAX_FULL_EVENT_STACK_SIZE;
	ts->xfer_buf = devm_kzalloc(ts->dev, ts->xfer_buf_sz, GFP_KERNEL);
	if (!ts->xfer_buf)
		return -ENOMEM;

	ts->event_buf = devm_kzalloc(ts->dev, ts->event_buf_sz, GFP_KERNEL);
	if (!ts->event_buf)
		return -ENOMEM;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ts->event_read_buf = devm_kzalloc(ts->dev, ts->event_buf_sz, GFP_KERNEL);
	if (!ts->event_read_buf)
		return -ENOMEM;
	trace_ring_size = HIMAX_TRACE_CAPACITY *
		sizeof(struct himax_trace_record);
	trace_snapshot_size = sizeof(struct himax_trace_header) + trace_ring_size;
	ts->trace_ring = kvzalloc(trace_ring_size, GFP_KERNEL);
	if (!ts->trace_ring)
		return -ENOMEM;
	ret = devm_add_action_or_reset(ts->dev, himax_kvfree_action,
				       ts->trace_ring);
	if (ret)
		return ret;
	ts->trace_snapshot = kvzalloc(trace_snapshot_size, GFP_KERNEL);
	if (!ts->trace_snapshot)
		return -ENOMEM;
	ret = devm_add_action_or_reset(ts->dev, himax_kvfree_action,
				       ts->trace_snapshot);
	if (ret)
		return ret;
#endif

	ts->algo = devm_kzalloc(ts->dev, sizeof(*ts->algo), GFP_KERNEL);
	if (!ts->algo)
		return -ENOMEM;
	hx_algo_init_defaults(ts->algo);
	hx_algo_full_reset(ts->algo);

	spin_lock_init(&ts->irq_lock);
	mutex_init(&ts->op_lock);
	INIT_DELAYED_WORK(&ts->panel_reinit_work, himax_panel_reinit_work);
	dev_set_drvdata(&spi->dev, ts);
	spi_set_drvdata(spi, ts);

	ret = himax_input_dev_config(ts);
	if (ret) {
		dev_err(ts->dev, "input device set failed\n");
		return ret;
	}

	ret = devm_request_threaded_irq(ts->dev, ts->spi->irq, NULL,
					himax_ts_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"himax-spi-ts", ts);
	if (ret) {
		dev_err(ts->dev, "request irq failed. ret=%d\n", ret);
		return ret;
	}

	ret = himax_sysfs_init(ts);
	if (ret) {
		dev_err(ts->dev, "failed to create sysfs controls: %d\n", ret);
		return ret;
	}

	ts->panel_follower.funcs = &himax_panel_follower_funcs;
	ret = devm_drm_panel_add_follower(ts->dev, &ts->panel_follower);
	if (ret) {
		himax_sysfs_remove(ts);
		return dev_err_probe(ts->dev, ret,
				     "failed to register panel follower\n");
	}

	return 0;
}

static void himax_spi_remove(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	cancel_delayed_work_sync(&ts->panel_reinit_work);
	himax_sysfs_remove(ts);

	himax_lock(ts);
	ts->shutting_down = true;
	ts->panel_enabled = false;
	ts->panel_prepared = false;
	himax_power_down(ts);
	himax_unlock(ts);
}

static const struct spi_device_id himax_spi_ids[] = {
	{ .name = "hx83121a-ts" },
	{ .name = "hx83121a" },
	{ },
};
MODULE_DEVICE_TABLE(spi, himax_spi_ids);

static const struct of_device_id himax_spi_of_match[] = {
	{ .compatible = "himax,hx83121a-ts" },
	{ .compatible = "himax,hx83121a" },
	{ }
};
MODULE_DEVICE_TABLE(of, himax_spi_of_match);

static struct spi_driver himax_spi_driver = {
	.driver = {
		.name = "himax-spi",
		.of_match_table = himax_spi_of_match,
	},
	.probe = himax_spi_probe,
	.remove = himax_spi_remove,
	.id_table = himax_spi_ids,
};
module_spi_driver(himax_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pengyu Luo <mitltlatltl@gmail.com>");
MODULE_DESCRIPTION("Himax HX83121A SPI touchscreen driver");
