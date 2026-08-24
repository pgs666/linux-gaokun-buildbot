/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HIMAX_SPI_H
#define HIMAX_SPI_H

#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include <drm/drm_panel.h>

#include "hx-algo.h"
#include "hx-frame-status.h"

#define HIMAX_MAX_RX			60U
#define HIMAX_MAX_TX			40U
#define HIMAX_BUS_READ_HEADER_SIZE	3U
#define HIMAX_EVENT_STACK_ADDRESS	0x30
#define HIMAX_EVENT_HEADER_SIZE		128U
#define HIMAX_FULL_EVENT_STACK_SIZE	(HIMAX_EVENT_HEADER_SIZE + \
	(2 + HIMAX_MAX_RX * HIMAX_MAX_TX + HIMAX_MAX_TX + HIMAX_MAX_RX) * 2)
#define HIMAX_HX83121A_FLASH_SIZE	(255 * 1024)
#define HIMAX_SPI_CS_SETUP_TIME		300
#define HIMAX_PANEL_REINIT_RETRIES	3
#define HIMAX_PANEL_REINIT_DELAY_MS	50
#define HIMAX_PANEL_ENABLE_SETTLE_MS	300
#define HIMAX_MAX_CONSECUTIVE_FRAME_ERRORS 3
#define HIMAX_RESUME_QUALITY_ATTEMPTS	30
#define HIMAX_MAX_CONSECUTIVE_NOISE_FRAMES 12

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
#define HIMAX_TRACE_CAPACITY		512U
#define HIMAX_TRACE_MAGIC		0x52545848U
#define HIMAX_TRACE_VERSION		3U

#define HIMAX_TRACE_F_READ_OK		BIT(0)
#define HIMAX_TRACE_F_MASTER_VALID	BIT(1)
#define HIMAX_TRACE_F_RETRY		BIT(2)
#define HIMAX_TRACE_F_HAS_FINGER	BIT(3)
#define HIMAX_TRACE_F_NOISE_HOLD	BIT(4)

struct himax_trace_header {
	__le32 magic;
	__le16 version;
	__le16 header_size;
	__le32 record_size;
	__le32 capacity;
	__le32 count;
	__le32 reserved;
	__le64 snapshot_boottime_ns;
	__le64 reset_generation;
} __packed;

struct himax_trace_peak {
	u8 row;
	u8 col;
	__le16 signal;
	__le16 zone_area;
	u8 zone_index;
	u8 flags;
	u8 id;
	u8 age;
} __packed;

struct himax_trace_track {
	u8 flags;
	u8 age;
	u8 missed;
	u8 debounce;
	u8 source_peak_id;
	u8 source_peak_age;
	__le32 x;
	__le32 y;
	__le32 vx;
	__le32 vy;
	__le32 signal_sum;
	__le32 filtered_x_q8;
	__le32 filtered_y_q8;
	__le32 deriv_x_q8;
	__le32 deriv_y_q8;
} __packed;

struct himax_trace_record {
	__le64 boottime_ns;
	__le32 irq_sequence;
	__le32 reset_generation;
	__le32 flags;
	__le32 read_error;
	__le32 algo_frame_sequence;
	__le32 common_diff;
	__le16 frame_max;
	u8 has_signal;
	u8 zones;
	u8 peaks;
	u8 contacts_pre;
	u8 contacts_post;
	u8 active_tracks;
	u8 reported_tracks;
	u8 reserved;
	struct himax_trace_peak peak[HX_MAX_PEAKS];
	struct himax_trace_track track[HIMAX_MAX_TOUCH];
	__le16 processed_frame[HX_PIXELS];
	u8 event_stack[HIMAX_FULL_EVENT_STACK_SIZE];
} __packed;
#endif

struct himax_ts_data {
	u8 *xfer_buf;
	u8 *event_buf;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	u8 *event_read_buf;
#endif
	u32 spi_xfer_max_sz;
	u32 xfer_buf_sz;
	u32 event_buf_sz;
	spinlock_t irq_lock;
	struct mutex op_lock;
	bool irq_enabled;
	bool panel_prepared;
	bool panel_enabled;
	bool controller_initialized;
	bool retained_suspend;
	bool shutting_down;
	struct gpio_desc *gpiod_rst;
	struct device *dev;
	struct spi_device *spi;
	struct input_dev *input_dev;
	struct touchscreen_properties props;
	struct drm_panel_follower panel_follower;
	struct delayed_work panel_reinit_work;
	struct hx_algo *algo;
	u8 consecutive_frame_errors;
	u8 consecutive_noise_frames;
	u8 afe_command_slot;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	struct himax_trace_record *trace_ring;
	u8 *trace_snapshot;
	size_t trace_snapshot_len;
	u32 trace_write_index;
	u32 trace_count;
	u32 trace_irq_sequence;
	u32 reset_generation;
	u32 cold_init_count;
	u32 warm_resume_count;
	u32 warm_resume_fallback_count;
	u32 resume_stability_failures;
	u32 afe_calibration_count;
	u32 afe_calibration_failures;
#endif
};

bool himax_full_resume_enabled(void);
bool himax_restore_afe_enabled(void);
void himax_int_enable(struct himax_ts_data *ts, bool enable);
void himax_lock(struct himax_ts_data *ts);
void himax_unlock(struct himax_ts_data *ts);
int himax_spi_read(struct himax_ts_data *ts, u8 command, u8 *buf, u32 len);
int himax_restore_afe_runtime(struct himax_ts_data *ts);
int himax_restore_raw_runtime(struct himax_ts_data *ts);
int himax_verify_running_ic(struct himax_ts_data *ts);
int hx83121a_chip_detect(struct himax_ts_data *ts);
int himax_enable_fw_reload(struct himax_ts_data *ts);
int himax_mcu_power_on_init(struct himax_ts_data *ts);
int himax_mcu_check_crc(struct himax_ts_data *ts, u32 start_addr,
			int reload_length, u32 *crc_result);
int himax_manual_reset(struct himax_ts_data *ts);
int himax_sysfs_init(struct himax_ts_data *ts);
void himax_sysfs_remove(struct himax_ts_data *ts);

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
void himax_trace_record_irq(struct himax_ts_data *ts, int read_error,
			    bool master_valid,
			    const struct hx_frame_status *frame_status,
			    bool noise_hold);
#else
static inline void
himax_trace_record_irq(struct himax_ts_data *ts, int read_error,
			bool master_valid,
			const struct hx_frame_status *frame_status,
			bool noise_hold)
{
	(void)ts;
	(void)read_error;
	(void)master_valid;
	(void)frame_status;
	(void)noise_hold;
}
#endif

#endif /* HIMAX_SPI_H */
