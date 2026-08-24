// SPDX-License-Identifier: GPL-2.0
/*
 * Himax SPI Driver
 * based on Himax hx83102j
 *
 * Copyright (C) 2019,2024 Himax Corporation.
 * Copyright (C) 2026 Pengyu Luo <mitltlatltl@gmail.com>
 */

/* TODO: DT parse: vdd_dig & avdd_analog */

#include<linux/dev_printk.h>

#include <linux/delay.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

static bool disable_pressure = true;
module_param(disable_pressure, bool, 0444);
MODULE_PARM_DESC(disable_pressure, "Disable ABS_MT_TOUCH_MAJOR and ABS_MT_PRESSURE axes (default: true)");

static bool restore_afe_runtime = true;
module_param(restore_afe_runtime, bool, 0644);
MODULE_PARM_DESC(restore_afe_runtime,
		 "Replay calibration and 120 Hz scan-rate commands after hardware initialization");
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/math.h>
#include <linux/math64.h>
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
#include <linux/timekeeping.h>
#endif
#include <linux/workqueue.h>

#include <drm/drm_panel.h>

#include "hx-algo.h"
#include "hx-frame-status.h"

#define HIMAX_BUS_RETRY					3
/* SPI bus read header length */
#define HIMAX_BUS_R_HLEN				3U
/* SPI bus write header length */
#define HIMAX_BUS_W_HLEN				2U
/* TP SRAM address size and data size */
#define HIMAX_REG_SZ					4U
#define HIMAX_MAX_RX					60U
#define HIMAX_MAX_TX					40U
#define HIMAX_HX83121A_SAFE_MODE_PASSWORD		0x9527
/* FIXME: this is for hx83120j, ~4928 for hx83121a 40 * 60 * 2 + 128 */
#define HIMAX_HX83121A_STACK_SIZE			128U
#define HIMAX_HX83121A_FULL_STACK_SZ \
(HIMAX_HX83121A_STACK_SIZE + \
(2 + HIMAX_MAX_RX * HIMAX_MAX_TX + \
HIMAX_MAX_TX + HIMAX_MAX_RX) * 2)

/* Clear 4 bytes data */
#define HIMAX_DATA_CLEAR				0x00000000
/* AHB register addresses */
#define HIMAX_AHB_ADDR_BYTE_0				0x00
#define HIMAX_AHB_ADDR_RDATA_BYTE_0			0x08
#define HIMAX_AHB_ADDR_ACCESS_DIRECTION			0x0c
#define HIMAX_AHB_ADDR_INCR4				0x0d
#define HIMAX_AHB_ADDR_CONTI				0x13
#define HIMAX_AHB_ADDR_EVENT_STACK			0x30
#define HIMAX_AHB_ADDR_PSW_LB				0x31

/* AHB register values/commands */
#define HIMAX_AHB_CMD_ACCESS_DIRECTION_READ		0x00
#define HIMAX_AHB_CMD_CONTI				0x31
#define HIMAX_AHB_CMD_INCR4				0x10
#define HIMAX_AHB_CMD_INCR4_ADD_4_BYTE			0x01
#define HIMAX_AHB_CMD_LEAVE_SAFE_MODE			0x0000

/* DSRAM flag addresses */
#define HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD		0x100072c0
#define HIMAX_DSRAM_ADDR_FLASH_RELOAD			0x10007f00
#define HIMAX_DSRAM_ADDR_SORTING_MODE_EN		0x10007f04
#define HIMAX_DSRAM_ADDR_SET_NFRAME			0x10007294
#define HIMAX_DSRAM_ADDR_RAWDATA_PASSWORD		0x10000000
#define HIMAX_DSRAM_ADDR_AP_NOTIFY_FW_SUSPEND		0x10007fd0
#define HIMAX_DSRAM_ADDR_USB_DETECT			0x10007f38

/* dsram flag data */
#define HIMAX_DSRAM_DATA_FW_RELOAD_DONE			0x000072c0
#define HIMAX_DSRAM_DATA_AP_RESUMED			0x00000000
#define HIMAX_DSRAM_DATA_USB_CONNECTED			0xa55aa55a
/* hx83121a-specific register/dsram flags/data */
#define HIMAX_HX83121A_DSRAM_ADDR_RAW_OUT_SEL		0x100072ec
#define HIMAX_HX83121A_FLASH_SIZE			(255 * 1024)

/* Firmware AFE command queue used by the Windows v1.1.2 runtime. */
#define HIMAX_HX83121A_AFE_CMD_SLOT_BASE		0x10007550
#define HIMAX_HX83121A_AFE_CMD_SLOT_STRIDE		0x10
#define HIMAX_HX83121A_AFE_CMD_SLOT_COUNT		5
#define HIMAX_HX83121A_AFE_CMD_PACKET_SIZE		16
#define HIMAX_HX83121A_AFE_CMD_START_CALIBRATION	0x01
#define HIMAX_HX83121A_AFE_CMD_FORCE_SCAN_RATE		0x0e
#define HIMAX_HX83121A_AFE_SCAN_RATE_120HZ		0x00

/* hardware register addresses */
#define HIMAX_REG_ADDR_RELOAD_STATUS			0x80050000
#define HIMAX_REG_ADDR_RELOAD_CRC32_RESULT		0x80050018
#define HIMAX_REG_ADDR_RELOAD_ADDR_FROM			0x80050020
#define HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT		0x80050028
#define HIMAX_REG_ADDR_CTRL_FW				0x9000005c
#define HIMAX_REG_ADDR_FW_STATUS			0x900000a8
#define HIMAX_REG_ADDR_ICID				0x900000d0

/* hardware reg data/flags */
#define HIMAX_REG_DATA_FW_STATE_RUNNING			0x05
#define HIMAX_REG_DATA_FW_STATE_SAFE_MODE		0x0c
#define HIMAX_REG_DATA_FW_RE_INIT			0x00
#define HIMAX_REG_DATA_FW_GO_SAFEMODE			0xa5
#define HIMAX_REG_DATA_FW_IN_SAFEMODE			0x87
#define HIMAX_REG_DATA_RELOAD_DONE			0x01
#define HIMAX_REG_DATA_RELOAD_PASSWORD			0x99

/* SPI CS setup time */
#define HIMAX_SPI_CS_SETUP_TIME				300
#define HIMAX_PANEL_REINIT_RETRIES			3
#define HIMAX_PANEL_REINIT_DELAY_MS			50
/* Let the panel and display pipeline settle before TDDI touch recovery. */
#define HIMAX_PANEL_ENABLE_SETTLE_MS			300
#define HIMAX_MAX_CONSECUTIVE_FRAME_ERRORS		3
#define HIMAX_RESUME_STABLE_FRAMES			3
#define HIMAX_RESUME_STABLE_ATTEMPTS			12
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
#define HIMAX_TRACE_CAPACITY			512U
#define HIMAX_TRACE_MAGIC			0x52545848U /* "HXTR" */
#define HIMAX_TRACE_VERSION			2U

#define HIMAX_TRACE_F_READ_OK			BIT(0)
#define HIMAX_TRACE_F_MASTER_VALID		BIT(1)
#define HIMAX_TRACE_F_RETRY			BIT(2)
#define HIMAX_TRACE_F_HAS_FINGER		BIT(3)

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

/*
 * One record is generated for every threaded IRQ, including SPI failures and
 * invalid/retry master frames.  Keeping both the raw event stack and the
 * baseline-subtracted matrix lets an offline decoder distinguish electrical
 * input corruption from baseline/detection/tracking faults.
 */
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
	u8 event_stack[HIMAX_HX83121A_FULL_STACK_SZ];
} __packed;
#endif
/* HIMAX SPI function select, 1st byte of any SPI command sequence */
#define HIMAX_SPI_FUNCTION_READ				0xf3
#define HIMAX_SPI_FUNCTION_WRITE			0xf2
/* TODO: f4 for huawei? */

union himax_dword_data {
	u32 dword;
	u16 word[2];
	u8 byte[4];
};

struct himax_ts_data {
	u8 *xfer_buf;
	u8 *event_buf;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	/* Scratch buffer for an in-flight frame.  event_buf is last-good data. */
	u8 *event_read_buf;
#endif
	u32 spi_xfer_max_sz;
	u32 xfer_buf_sz;
	u32 event_buf_sz;
	/* lock for irq_save */
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

static void himax_report_tracked_state(struct himax_ts_data *ts, bool report_on);
static int himax_enable_fw_reload(struct himax_ts_data *ts);
static int himax_mcu_power_on_init(struct himax_ts_data *ts);
static int hx83121a_gaokun_read_event_stack(struct himax_ts_data *ts);
static int himax_wait_for_stable_event_frames(struct himax_ts_data *ts);
static int himax_mcu_check_crc(struct himax_ts_data *ts, u32 start_addr,
			       int reload_length, u32 *crc_result);

/*
 * 1st byte is the spi function select, 2nd byte is the command belong to the
 * spi function and 3rd byte is the dummy byte for IC to process the command.
 */
static int himax_spi_read(struct himax_ts_data *ts, u8 cmd, u8 *buf, u32 len)
{
	int ret;
	int retry_cnt;
	struct spi_message msg;

	memset(ts->xfer_buf, 0, HIMAX_BUS_R_HLEN + len);
	ts->xfer_buf[0] = HIMAX_SPI_FUNCTION_READ;
	ts->xfer_buf[1] = cmd;
	ts->xfer_buf[2] = 0x00;

	struct spi_transfer xfer = {
		.len = HIMAX_BUS_R_HLEN + len,
		.tx_buf = ts->xfer_buf,
		.rx_buf = ts->xfer_buf,
		.cs_change = 0,
	};

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	for (retry_cnt = 0; retry_cnt < HIMAX_BUS_RETRY; retry_cnt++) {
		ret = spi_sync(ts->spi, &msg);
		if (ret < 0)
			// TODO: 确定一般会不会出现重试的情况
			dev_err(&ts->spi->dev, "spi transfer error, %d, retry: %d", ret, retry_cnt);
		else
			break;
	}

	if (ret < 0)
		return ret;

	/* Destination may alias xfer_buf when caller reuses driver buffers. */
	memmove(buf, ts->xfer_buf + HIMAX_BUS_R_HLEN, len);
	return 0;
}

/*
 * 1st byte is the spi function select and 2nd byte is the command belong to the
 * spi function. Else is the data to write.
 */
static int himax_spi_write(struct himax_ts_data *ts, u8 *tx_buf, u32 len)
{
	int ret;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.tx_buf = tx_buf,
		.len = len,
		.cs_change = 0,
	};

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	ret = spi_sync(ts->spi, &msg);
	if (ret < 0) {
		dev_err(&ts->spi->dev, "spi transfer error, %d", ret);
		return ret;
	}

	return 0;
}

/* TODO: merge into spi_write, fix len (=len(data)) */
static int himax_write(struct himax_ts_data *ts, u8 cmd, u8 *addr, const u8 *data, u32 len)
{
	/** @len: len(data) + len(addr) */
	u8 *ptr = ts->xfer_buf;

	memset(ts->xfer_buf, 0, len + HIMAX_BUS_W_HLEN);
	ts->xfer_buf[0] = HIMAX_SPI_FUNCTION_WRITE;
	ts->xfer_buf[1] = cmd;
	ptr += HIMAX_BUS_W_HLEN;
	len += HIMAX_BUS_W_HLEN;

	if (addr) {
		memcpy(ptr, addr, 4);
		ptr += 4;
	}

	if (data)
		memcpy(ptr, data, len - (ptr - ts->xfer_buf));

	return himax_spi_write(ts, ts->xfer_buf, len);
}

/**
 * himax_mcu_set_burst_mode() - Set burst mode for MCU
 * @ts: Himax touch screen data
 * @auto_add_4_byte: Enable auto add 4 byte mode
 *
 * Set burst mode for MCU, which is used for read/write data from/to MCU.
 * HIMAX_AHB_ADDR_CONTI config the IC to take data continuously,
 * HIMAX_AHB_ADDR_INCR4 config the IC to auto increment the address by 4 byte when
 * each 4 bytes read/write.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_set_burst_mode(struct himax_ts_data *ts, bool auto_add_4_byte)
{
	int ret;
	u8 tmp;

	tmp = HIMAX_AHB_CMD_CONTI;
	ret = himax_write(ts, HIMAX_AHB_ADDR_CONTI, NULL, &tmp, 1);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write ahb_addr_conti failed\n", __func__);
		return ret;
	}

	tmp = HIMAX_AHB_CMD_INCR4;
	if (auto_add_4_byte)
		tmp |= HIMAX_AHB_CMD_INCR4_ADD_4_BYTE;

	ret = himax_write(ts, HIMAX_AHB_ADDR_INCR4, NULL, &tmp, 1);
	if (ret < 0)
		dev_err(ts->dev, "%s: write ahb_addr_incr4 failed\n", __func__);

	return ret;
}

/*
 * Read data from himax internal registers and SRAM
 *
 * We write the (SRAM)address to AHB register to tell where to read. Then set
 * the access direction to read, and read the data from AHB register.
 */
static int himax_mcu_register_read(struct himax_ts_data *ts, u32 addr, u8 *buf, u32 len)
{
	int ret;
	u8 direction_switch = HIMAX_AHB_CMD_ACCESS_DIRECTION_READ;
	union himax_dword_data target_addr;

	ret = himax_mcu_set_burst_mode(ts, len > HIMAX_REG_SZ);
	if (ret)
		return ret;

	target_addr.dword = cpu_to_le32(addr);
	ret = himax_write(ts, HIMAX_AHB_ADDR_BYTE_0, target_addr.byte, NULL, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write ahb_addr_byte_0 failed\n", __func__);
		return ret;
	}

	ret = himax_write(ts, HIMAX_AHB_ADDR_ACCESS_DIRECTION, NULL,
				  &direction_switch, 1);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write ahb_addr_access_direction failed\n", __func__);
		return ret;
	}

	ret = himax_spi_read(ts, HIMAX_AHB_ADDR_RDATA_BYTE_0, buf, len);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read ahb_addr_rdata_byte_0 failed\n", __func__);
		return ret;
	}

	return himax_mcu_set_burst_mode(ts, !(len > HIMAX_REG_SZ));
}

/* Write the internal (SRAM)address and data to AHB register */
static int himax_mcu_register_write(struct himax_ts_data *ts, u32 addr, const u8 *buf, u32 len)
{
	int ret;

	union himax_dword_data target_addr;

	ret = himax_mcu_set_burst_mode(ts, len > HIMAX_REG_SZ);
	if (ret)
		return ret;

	target_addr.dword = cpu_to_le32(addr);
	ret = himax_write(ts, HIMAX_AHB_ADDR_BYTE_0,
				target_addr.byte, buf, len + HIMAX_REG_SZ);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write ahb_addr_byte_0 failed\n", __func__);
	}

	return himax_mcu_set_burst_mode(ts, !(len > HIMAX_REG_SZ));
}

/* Xiaomi replays firmware-owned state with write/read retry loops.  Keep the
 * same rule, but require an exact readback (their AP-notify loop accidentally
 * stops on a successful read even when the value differs). */
static int himax_mcu_write_verify_u32(struct himax_ts_data *ts, u32 addr,
				      u32 value, int attempts)
{
	union himax_dword_data expected = { .dword = cpu_to_le32(value) };
	union himax_dword_data actual;
	int attempt;
	int ret = -EIO;

	for (attempt = 0; attempt < attempts; attempt++) {
		ret = himax_mcu_register_write(ts, addr, expected.byte,
					       sizeof(expected.byte));
		if (ret)
			continue;
		usleep_range(1000, 1100);
		ret = himax_mcu_register_read(ts, addr, actual.byte,
					      sizeof(actual.byte));
		if (!ret && actual.dword == expected.dword)
			return 0;
		if (!ret)
			ret = -EIO;
	}

	dev_err(ts->dev, "register 0x%08x failed readback for 0x%08x\n",
		addr, value);
	return ret;
}

static void himax_afe_build_command(u8 command, u8 value, u8 *packet)
{
	u32 sum = 0;
	u16 checksum;
	int i;

	memset(packet, 0, HIMAX_HX83121A_AFE_CMD_PACKET_SIZE);
	packet[0] = 0xa8;
	packet[1] = 0x8a;
	packet[2] = command;
	packet[4] = value;

	for (i = 0; i < HIMAX_HX83121A_AFE_CMD_PACKET_SIZE; i += 2)
		sum += packet[i] | ((u16)packet[i + 1] << 8);
	checksum = (u16)(0U - sum);
	packet[14] = checksum & 0xff;
	packet[15] = checksum >> 8;

	/*
	 * The virtual header participates in the checksum, then the trigger write
	 * supplies it after the full command body has reached the slot.
	 */
	packet[0] = 0;
	packet[1] = 0;
}

static int himax_afe_send_command(struct himax_ts_data *ts, u8 *slot,
				  u8 command, u8 value)
{
	u8 packet[HIMAX_HX83121A_AFE_CMD_PACKET_SIZE];
	u8 trigger[4] = { 0xa8, 0x8a, command, 0x00 };
	u8 response[HIMAX_HX83121A_AFE_CMD_PACKET_SIZE];
	u32 addr;
	int ret;

	addr = HIMAX_HX83121A_AFE_CMD_SLOT_BASE +
		(*slot % HIMAX_HX83121A_AFE_CMD_SLOT_COUNT) *
		HIMAX_HX83121A_AFE_CMD_SLOT_STRIDE;
	himax_afe_build_command(command, value, packet);

	ret = himax_mcu_register_write(ts, addr, packet, sizeof(packet));
	if (ret)
		return ret;
	ret = himax_mcu_register_write(ts, addr, trigger, sizeof(trigger));
	if (ret)
		return ret;
	ret = himax_mcu_register_read(ts, addr, response, sizeof(response));
	if (ret)
		return ret;

	*slot = (*slot + 1) % HIMAX_HX83121A_AFE_CMD_SLOT_COUNT;
	return 0;
}

static int himax_restore_afe_runtime(struct himax_ts_data *ts,
				     bool start_calibration)
{
	u8 slot = 0;
	int first_error = 0;
	int ret;

	if (!restore_afe_runtime)
		return 0;

	if (start_calibration) {
		ret = himax_afe_send_command(ts, &slot,
					     HIMAX_HX83121A_AFE_CMD_START_CALIBRATION,
					     0);
		if (ret) {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			ts->afe_calibration_failures++;
#endif
			dev_warn(ts->dev, "failed to start AFE calibration: %d\n", ret);
			first_error = ret;
		} else {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			ts->afe_calibration_count++;
#endif
			dev_info(ts->dev,
				 "AFE calibration command transport completed\n");
		}
	}

	ret = himax_afe_send_command(ts, &slot,
				     HIMAX_HX83121A_AFE_CMD_FORCE_SCAN_RATE,
				     HIMAX_HX83121A_AFE_SCAN_RATE_120HZ);
	if (ret) {
		dev_warn(ts->dev, "failed to force 120 Hz scan rate: %d\n", ret);
		if (!first_error)
			first_error = ret;
	} else {
		dev_info(ts->dev, "120 Hz scan-rate command transport completed\n");
	}

	return first_error;
}

/*
 * Wakeup IC bus interface. The IC may enter sleep mode and need to wakeup
 * before any operation.
 */
static int himax_mcu_interface_on(struct himax_ts_data *ts)
{
	int ret;
	u8 buf[HIMAX_REG_SZ];
	u8 buf2[HIMAX_REG_SZ];
	u32 retry_cnt;
	const u32 burst_retry_limit = 10;

	/* Read a dummy register to wake up BUS. */
	ret = himax_spi_read(ts, HIMAX_AHB_ADDR_RDATA_BYTE_0, buf, 4);
	if (ret < 0)
		goto err;

	for (retry_cnt = 0; retry_cnt < burst_retry_limit; retry_cnt++) {
		ret = himax_mcu_set_burst_mode(ts, false);
		if (ret < 0)
			goto err;

		/* Check cmd */
		ret = himax_spi_read(ts, HIMAX_AHB_ADDR_CONTI, buf, 1);
		if (ret < 0)
			goto err;

		ret = himax_spi_read(ts, HIMAX_AHB_ADDR_INCR4, buf2, 1);
		if (ret < 0)
			goto err;

		if (buf[0] == HIMAX_AHB_CMD_CONTI && buf2[0] == HIMAX_AHB_CMD_INCR4)
			return 0;

		usleep_range(1000, 1100);
	}

err:
	dev_err(ts->dev, "%s: failed!\n", __func__);

	return -EIO;
}

static void himax_pin_reset(struct himax_ts_data *ts)
{
	/* TODO: reduce to 10ms, 20ms? */
	gpiod_set_value_cansleep(ts->gpiod_rst, 1);
	usleep_range(20000, 20100);
	gpiod_set_value_cansleep(ts->gpiod_rst, 0);
	usleep_range(50000, 50100);
}

static void himax_int_enable(struct himax_ts_data *ts, bool enable)
{
	unsigned long flags;

	spin_lock_irqsave(&ts->irq_lock, flags);
	if (enable && ts->irq_enabled == false)
		enable_irq(ts->spi->irq);
	else if (!enable && ts->irq_enabled == true)
		disable_irq_nosync(ts->spi->irq);
	ts->irq_enabled = enable;
	spin_unlock_irqrestore(&ts->irq_lock, flags);
}

static void himax_quiesce_irq(struct himax_ts_data *ts)
{
	himax_int_enable(ts, false);
	synchronize_irq(ts->spi->irq);
}

static void himax_lock(struct himax_ts_data *ts)
{
	himax_quiesce_irq(ts);
	mutex_lock(&ts->op_lock);
}

static void himax_unlock(struct himax_ts_data *ts)
{
	mutex_unlock(&ts->op_lock);
}

static void himax_mcu_ic_reset(struct himax_ts_data *ts, bool int_off)
{
	if (int_off)
		himax_int_enable(ts, false);

	himax_pin_reset(ts);

	if (int_off)
		himax_int_enable(ts, true);
}

/**
 * sense_off: stop MCU
 * 1. request FW to stop
 * 2. enter safe mode (and reset TCON for some ICs).
 *
 * @check_en: confirm if the FW is stopped
 */
static int himax_sense_off(struct himax_ts_data *ts, bool check_en)
{
	int ret;
	u32 retry_cnt;
	const u32 stop_fw_retry_limit = 35;
	const u32 enter_safe_mode_retry_limit = 5;
	const union himax_dword_data safe_mode = {
		.dword = cpu_to_le32(HIMAX_REG_DATA_FW_GO_SAFEMODE)
	};
	union himax_dword_data data;

	dev_info(ts->dev, "%s: check %s\n", __func__, check_en ? "True" : "False");
	if (!check_en)
		goto without_check;

	for (retry_cnt = 0; retry_cnt < stop_fw_retry_limit; retry_cnt++) {
		if (retry_cnt == 0 ||
		    (data.byte[0] != HIMAX_REG_DATA_FW_GO_SAFEMODE &&
		    data.byte[0] != HIMAX_REG_DATA_FW_RE_INIT &&
		    data.byte[0] != HIMAX_REG_DATA_FW_IN_SAFEMODE)) {
			ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_CTRL_FW,
						       safe_mode.byte, 4);
			if (ret < 0) {
				dev_err(ts->dev, "%s: stop FW failed\n", __func__);
				return ret;
			}
		}
		usleep_range(10000, 11000);

		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_FW_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read central state failed\n", __func__);
			return ret;
		}
		if (data.byte[0] != HIMAX_REG_DATA_FW_STATE_RUNNING) {
			dev_info(ts->dev, "%s: Do not need wait FW, Status = 0x%02X!\n", __func__,
			  data.byte[0]);
			break;
		}

		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_CTRL_FW, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read ctrl FW failed\n", __func__);
			return ret;
		}
		if (data.byte[0] == HIMAX_REG_DATA_FW_IN_SAFEMODE)
			break;
	}

	if (data.byte[0] != HIMAX_REG_DATA_FW_IN_SAFEMODE)
		dev_warn(ts->dev, "%s: Failed to stop FW!\n", __func__);

without_check:
	for (retry_cnt = 0; retry_cnt < enter_safe_mode_retry_limit; retry_cnt++) {
		/* set Enter safe mode : 0x31 ==> 0x9527 */
		data.word[0] = cpu_to_le16(HIMAX_HX83121A_SAFE_MODE_PASSWORD);
		ret = himax_write(ts, HIMAX_AHB_ADDR_PSW_LB, NULL, data.byte, 2);
		if (ret < 0) {
			dev_err(ts->dev, "%s: enter safe mode failed\n", __func__);
			return ret;
		}

		/* Check enter_save_mode */
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_FW_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read central state failed\n", __func__);
			return ret;
		}

		if (data.byte[0] == HIMAX_REG_DATA_FW_STATE_SAFE_MODE) {
			dev_info(ts->dev, "%s: Safe mode entered\n", __func__);
			return 0;
		}

		usleep_range(10000, 10100); // TODO: 5ms for HX83121?
		himax_pin_reset(ts);
	}
	dev_err(ts->dev, "%s: failed!\n", __func__);

	return -EIO;
}

static int hx83102j_sense_off(struct himax_ts_data *ts, bool check_en)
{
	return himax_sense_off(ts, check_en);
}

/**
 * @sw_reset: true for software reset, false for hardware reset
 *     true: write IC to leave safe mode
 *     false: pin reset
 *
 * make MCU restart running the FW
 */
static int himax_sense_on(struct himax_ts_data *ts, bool sw_reset)
{
	int ret;
	const union himax_dword_data re_init = {
		.dword = cpu_to_le32(HIMAX_REG_DATA_FW_RE_INIT)
	};
	union himax_dword_data data;

	ret = himax_mcu_interface_on(ts);
	if (ret < 0)
		return ret;

	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_CTRL_FW, re_init.byte, 4);
	if (ret < 0)
		return ret;
	usleep_range(10000, 11000);
	if (!sw_reset) {
		himax_mcu_ic_reset(ts, false);
	} else {
		data.word[0] = cpu_to_le16(HIMAX_AHB_CMD_LEAVE_SAFE_MODE);
		ret = himax_write(ts, HIMAX_AHB_ADDR_PSW_LB, NULL, data.byte, 2);
		if (ret < 0)
			return ret;
	}
	/* TODO: more operations for other himax ICs */

	return 0;
}

static int hx83121a_chip_detect(struct himax_ts_data *ts)
{
	int ret;
	u32 retry_cnt;
	const u32 read_icid_retry_limit = 5;
	union himax_dword_data data;

	himax_pin_reset(ts);

	ret = himax_mcu_interface_on(ts);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read ahb_addr_conti failed\n", __func__);
		return ret;
	}

	ret = hx83102j_sense_off(ts, false);
	if (ret)
		return ret;

	for (retry_cnt = 0; retry_cnt < read_icid_retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_ICID, data.byte, 4);
		if (ret) {
			dev_err(ts->dev, "%s: Read IC ID Fail\n", __func__);
			return ret;
		}

		/*
		 * For suffix > F, it should be (data.byte[1] & 0xF) + 'A'; // or 'a'
		 * e.g. hx83102j: 0x83102900
		 */
		data.dword = le32_to_cpu(data.dword) >> 8;
		dev_info(ts->dev, "Detected IC HX%06X\n", data.dword);

		if (data.dword == 0x83121a)
			return 0;
	}
	return -ENODEV;
}

/* -------------------------------------------------------------------------- */
/* input 子系统 */
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
	 * during clicks, preventing libinput from treating 10px drifts as swipes. */
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X,
			     0, SZ_64K - 1, 8, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
			     0, SZ_64K - 1, 8, 0);
	// TODO: 根据触点附近数据集建模得到
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
	if (ts->algo) {
		hx_algo_clear_live_state(ts->algo);
	}

	if (ts->input_dev)
		himax_report_tracked_state(ts, false);
}

static int himax_verify_running_ic(struct himax_ts_data *ts)
{
	union himax_dword_data data;
	int ret;

	ret = himax_mcu_interface_on(ts);
	if (ret)
		return ret;
	ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_ICID, data.byte,
				      sizeof(data.byte));
	if (ret)
		return ret;
	if ((le32_to_cpu(data.dword) >> 8) != 0x83121a)
		return -ENODEV;

	return 0;
}

static int himax_restore_raw_runtime(struct himax_ts_data *ts)
{
	union himax_dword_data password = {
		.dword = cpu_to_le32(0x00005aa5),
	};
	int ret;
	int supplied;

	/* These are firmware-owned runtime values.  Xiaomi similarly replays its
	 * USB/gesture/touch-mode values after resume; our raw-matrix transport
	 * additionally needs RawOut and the SRAM producer handshake. */
	ret = himax_mcu_write_verify_u32(ts,
		HIMAX_DSRAM_ADDR_AP_NOTIFY_FW_SUSPEND,
		HIMAX_DSRAM_DATA_AP_RESUMED, 3);
	if (ret)
		return ret;
	ret = himax_mcu_write_verify_u32(ts,
		HIMAX_DSRAM_ADDR_SORTING_MODE_EN, HIMAX_DATA_CLEAR, 3);
	if (ret)
		return ret;
	ret = himax_mcu_write_verify_u32(ts, HIMAX_DSRAM_ADDR_SET_NFRAME,
					 1, 3);
	if (ret)
		return ret;
	ret = himax_mcu_write_verify_u32(ts,
		HIMAX_HX83121A_DSRAM_ADDR_RAW_OUT_SEL, 0xf6, 3);
	if (ret)
		return ret;

	/* dagu replays charger state after every resume to select the firmware's
	 * anti-USB-interference profile.  It is supplementary to our host-side
	 * common-mode filter, not a replacement for it. */
	supplied = power_supply_is_system_supplied();
	if (supplied >= 0) {
		ret = himax_mcu_write_verify_u32(ts,
			HIMAX_DSRAM_ADDR_USB_DETECT,
			supplied ? HIMAX_DSRAM_DATA_USB_CONNECTED : 0, 3);
		if (ret)
			dev_warn(ts->dev,
				 "failed to replay charger interference mode: %d\n",
				 ret);
	}

	/* The password is a producer/consumer handshake, not persistent config;
	 * firmware may consume it immediately, so an equality readback would be
	 * incorrect.  Validate it through fresh master event frames below. */
	return himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_RAWDATA_PASSWORD,
					password.byte, sizeof(password.byte));
}

static int himax_warm_resume(struct himax_ts_data *ts)
{
	int ret;

	himax_release_all_touches(ts);
	ret = himax_verify_running_ic(ts);
	if (ret)
		return ret;
	ret = himax_restore_raw_runtime(ts);
	if (ret)
		return ret;

	/* The retained controller state is valid at the protocol level, but the
	 * display has just restarted and may now impose a different VCOM/scan-phase
	 * environment on the touch AFE.  Windows runs StartCalibration after every
	 * Chip::Init(); do the same here after the panel settle delay while keeping
	 * the already converged host-side per-cell baseline. */
	ret = himax_restore_afe_runtime(ts, true);
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
		afe_ret = himax_restore_afe_runtime(ts, true);
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

	/* dagu's active configuration deliberately omits HX_RESUME_HW_RESET:
	 * preserve the controller across display sleep, replay firmware-owned
	 * runtime state, and hard-reset only when the retained state fails
	 * validation.  Cold boot always takes the complete init path. */
	if (ts->retained_suspend && ts->controller_initialized) {
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
				HIMAX_PANEL_REINIT_DELAY_MS, "panel-cold");
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
	return sysfs_emit(buf, "%u\n", (unsigned)ts->algo->_name);	\
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
	return sysfs_emit(buf, "%u\n", (unsigned)ts->algo->_name);	\
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
HX_ALGO_ATTR_U16_RW(gesture_drag_distance, 0, 4096);
HX_ALGO_ATTR_U16_RW(gesture_long_press_frames, 1, 600);

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
static ssize_t diagnostics_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	struct hx_algo *a = ts->algo;
	ssize_t len;

	mutex_lock(&ts->op_lock);
	len = sysfs_emit(buf,
		"frame=%u common=%d max=%d signal=%u zones=%u peaks=%u "
		"contacts_pre=%u contacts_post=%u active=%u reported=%u "
		"baseline_generation=%u full_resets=%u live_clears=%u "
		"cold_inits=%u warm_resumes=%u warm_fallbacks=%u "
		"resume_stability_failures=%u afe_calibrations=%u "
		"afe_calibration_failures=%u "
		"small_peak_continued=%u weak_peak_continued=%u "
		"small_peak_rejected=%u "
		"split_peak_deferred=%u cross_zone_split_deferred=%u "
		"peak_id_handoffs=%u handoff_residual_deferred=%u "
		"fast_edge_starts=%u\n",
		a->diag_frame_seq, a->diag_common_diff, a->diag_frame_max,
		a->diag_has_signal, a->diag_zones, a->diag_peaks,
		a->diag_contacts_pre_filter, a->diag_contacts_post_filter,
		a->diag_active_tracks, a->diag_reported_tracks,
		a->baseline_generation, a->full_reset_count, a->live_clear_count,
		ts->cold_init_count, ts->warm_resume_count,
		ts->warm_resume_fallback_count, ts->resume_stability_failures,
		ts->afe_calibration_count, ts->afe_calibration_failures,
		a->diag_small_peak_continued, a->diag_weak_peak_continued,
		a->diag_small_peak_rejected,
		a->diag_split_peak_deferred,
		a->diag_cross_zone_split_deferred, a->diag_peak_id_handoffs,
		a->diag_handoff_residual_deferred,
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

static struct bin_attribute event_stack_attr = {
	.attr = {
		.name = "event_stack",
		.mode = 0444,
	},
	.size = HIMAX_HX83121A_FULL_STACK_SZ,
	.read = event_stack_read,
};

static void himax_trace_record_irq(struct himax_ts_data *ts, int read_error,
				   bool master_valid,
				   const struct hx_frame_status *frame_status)
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

static struct bin_attribute event_trace_attr = {
	.attr = {
		.name = "event_trace",
		.mode = 0444,
	},
	.size = sizeof(struct himax_trace_header) +
		HIMAX_TRACE_CAPACITY * sizeof(struct himax_trace_record),
	.read = event_trace_read,
};
#else
static inline void himax_trace_record_irq(struct himax_ts_data *ts,
					  int read_error, bool master_valid,
					  const struct hx_frame_status *frame_status)
{
	(void)ts;
	(void)read_error;
	(void)master_valid;
	(void)frame_status;
}
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
	&dev_attr_gesture_drag_distance.attr,
	&dev_attr_gesture_long_press_frames.attr,
	NULL,
};

static const struct attribute_group hx_algo_attr_group = {
	.name = "algo",
	.attrs = hx_algo_attrs,
};

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
	const u32 max_trunk_sz = ts->spi_xfer_max_sz - HIMAX_BUS_R_HLEN;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	u8 *buf = ts->event_read_buf;

	/* Never destroy the last successful frame while attempting a new one. */
	memset(buf, 0x00, ts->event_buf_sz);
#else
	u8 *buf = ts->event_buf;
#endif
	size_t length = HIMAX_HX83121A_FULL_STACK_SZ; /* FIXME: use actual size. */

	for (i = 0; i < length; i += max_trunk_sz) {
		ret = himax_spi_read(ts, HIMAX_AHB_ADDR_EVENT_STACK, buf + i,
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
 * prove that the producer is running and no retry frame is pending. */
static int himax_wait_for_stable_event_frames(struct himax_ts_data *ts)
{
	struct hx_frame_status status;
	u32 previous_hash = 0;
	bool have_previous = false;
	int consecutive = 0;
	int attempt;
	int ret = -EIO;

	for (attempt = 0; attempt < HIMAX_RESUME_STABLE_ATTEMPTS; attempt++) {
		u32 hash = 2166136261U;
		u32 i;

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
			 * they do not prove that scanning resumed. */
			if (have_previous && hash == previous_hash) {
				consecutive = 0;
				continue;
			}
			previous_hash = hash;
			have_previous = true;
			if (++consecutive >= HIMAX_RESUME_STABLE_FRAMES)
				return 0;
		} else {
			consecutive = 0;
			if (!ret)
				ret = -EAGAIN;
		}
	}

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ts->resume_stability_failures++;
#endif
	dev_err(ts->dev, "raw event producer did not stabilize after resume\n");
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
		himax_trace_record_irq(ts, read_ret, false, &frame_status);
		/* AFE transitions can transiently lose one or two frames.  Resetting
		 * the controller on the first miss creates a long, user-visible UP/
		 * DOWN break.  Match the Windows runtime recovery policy and recover
		 * only after a consecutive failure streak. */
		if (++ts->consecutive_frame_errors >=
		    HIMAX_MAX_CONSECUTIVE_FRAME_ERRORS) {
			int recovery_ret;

			dev_err(ts->dev, "%u consecutive frame reads failed, resetting controller\n",
				ts->consecutive_frame_errors);
			ts->consecutive_frame_errors = 0;
			/* A pin reset alone can leave the controller in a partially
			 * initialized state.  Run the same full reset/detect/FW-init
			 * sequence used by probe and panel recovery.  chip_detect()
			 * starts every attempt with a hardware reset. */
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
		himax_trace_record_irq(ts, 0, master_valid, &frame_status);
		goto report;
	}

	stable_cnt = hx_algo_process_frame_state(algo, ptr,
		frame_status.has_finger ? HX_FINGER_PRESENT : HX_FINGER_ABSENT);
	himax_trace_record_irq(ts, 0, true, &frame_status);

	/* Not final results, touchscreen_report_pos will handle this (x-y swap, y invert) */
	for (int i = 0; i < HIMAX_MAX_TOUCH; ++i) {
		if (!(algo->tracks[i].active && algo->tracks[i].reported))
			continue;
		dev_dbg(ts->dev, "slot %d x-y: %d, %d\n", i,
			algo->tracks[i].x, algo->tracks[i].y);
	}

report:
	/* 这里报告给系统的坐标数据可以通过 evtest 查看 */
	himax_report_tracked_state(ts, stable_cnt > 0);

out_unlock:
	mutex_unlock(&ts->op_lock);
	return irq_ret;
}

static int himax_mcu_assign_sorting_mode(struct himax_ts_data *ts, u8 *tmp_data_in)
{
	int ret;
	u8 rdata[4];
	u32 retry_cnt;
	const u32 retry_limit = 3;

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_SORTING_MODE_EN,
					       tmp_data_in, HIMAX_REG_SZ);
		if (ret < 0) {
			dev_err(ts->dev, "%s: write sorting mode fail\n", __func__);
			return ret;
		}
		usleep_range(1000, 1100);
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_SORTING_MODE_EN,
					      rdata, HIMAX_REG_SZ);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read sorting mode fail\n", __func__);
			return ret;
		}

		if (!memcmp(tmp_data_in, rdata, HIMAX_REG_SZ))
			return 0;
	}
	dev_err(ts->dev, "%s: fail to write sorting mode\n", __func__);

	return -EINVAL;
}

/* Match the Windows Chip::Init() resume path: every hardware reinit must
 * explicitly allow the controller to rebuild its runtime state from flash.
 * Keeping 0x5aa5 ("has flash, but not reload") across a reset can leave a
 * partially retained firmware state after display sleep. */
static int himax_enable_fw_reload(struct himax_ts_data *ts)
{
	union himax_dword_data data = { .dword = cpu_to_le32(0) };

	/* This flag is firmware-owned once sensing restarts, so mirror Windows and
	 * verify success through the subsequent 0x72c0 reload-done handshake. */
	return himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_FLASH_RELOAD,
					data.byte, 4);
}

static int himax_mcu_power_on_init(struct himax_ts_data *ts)
{
	int ret;
	u32 retry_cnt;
	const u32 retry_limit = 30;
	union himax_dword_data data;

	data.dword = cpu_to_le32(HIMAX_DATA_CLEAR);
	/* Initial sorting mode password to normal mode */
	ret = himax_mcu_assign_sorting_mode(ts, data.byte); // 不必要的步骤对于 gaokun3
	if (ret < 0) {
		dev_err(ts->dev, "%s: assign sorting mode fail\n", __func__);
		return ret;
	}

	/* N frame initial */ // 不必要的步骤对于 gaokun3
	/* reset N frame back to default value 1 for normal mode */
	data.dword = cpu_to_le32(1);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_SET_NFRAME, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: set N frame fail\n", __func__);
		return ret;
	}

	/* Initial FW reload status */ // 必要的步骤
	data.dword = cpu_to_le32(HIMAX_DATA_CLEAR);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: initial FW reload status fail\n", __func__);
		return ret;
	}

	ret = himax_sense_on(ts, false); // 必要的步骤
	if (ret < 0) {
		dev_err(ts->dev, "%s: sense on fail\n", __func__);
		return ret;
	}

	dev_info(ts->dev, "%s: waiting for FW reload data\n", __func__);
	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read FW reload status fail\n", __func__);
			return ret;
		}

		// use all 4 bytes to compare
		if (le32_to_cpu(data.dword) == HIMAX_DSRAM_DATA_FW_RELOAD_DONE) {
			dev_info(ts->dev, "%s: FW reload done\n", __func__);
			break;
		}
		dev_info(ts->dev, "%s: wait FW reload %u times\n", __func__, retry_cnt + 1);

		usleep_range(10000, 11000);
	}

	if (retry_cnt == retry_limit) {
		dev_err(ts->dev, "%s: FW reload fail!\n", __func__);
		return -EINVAL;
	}

	/* RawOut select for this panel configuration. */
	data.dword = cpu_to_le32(0xf6);
	ret = himax_mcu_register_write(ts, HIMAX_HX83121A_DSRAM_ADDR_RAW_OUT_SEL, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: set RawOut select fail\n", __func__);
		return ret;
	}

	/* Windows re-arms the raw-data SRAM handshake after every reload.  The
	 * SPI transport has one controller-facing endpoint, so only the master
	 * password is applicable here. */
	data.dword = cpu_to_le32(0x00005aa5);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_RAWDATA_PASSWORD,
				       data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: set raw-data password fail\n", __func__);
		return ret;
	}

	return 0;
}

static int himax_mcu_check_crc(struct himax_ts_data *ts, u32 start_addr,
			       int reload_length, u32 *crc_result)
{
	int ret;
	int length = reload_length / HIMAX_REG_SZ;
	u32 retry_cnt;
	const u32 retry_limit = 100;
	union himax_dword_data data, addr;

	addr.dword = cpu_to_le32(start_addr);
	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_RELOAD_ADDR_FROM, addr.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write reload start address fail\n", __func__);
		return ret;
	}

	data.word[1] = cpu_to_le16(HIMAX_REG_DATA_RELOAD_PASSWORD);
	data.word[0] = cpu_to_le16(length);
	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write reload length and password fail!\n", __func__);
		return ret;
	}

	ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read reload length and password fail!\n", __func__);
		return ret;
	}

	if (le16_to_cpu(data.word[0]) != length) {
		dev_err(ts->dev, "%s: length verify failed!\n", __func__);
		return -EINVAL;
	}

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read reload status fail!\n", __func__);
			return ret;
		}

		data.dword = le32_to_cpu(data.dword);
		if ((data.byte[0] & HIMAX_REG_DATA_RELOAD_DONE) != HIMAX_REG_DATA_RELOAD_DONE) {
			ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_CRC32_RESULT,
						      data.byte, HIMAX_REG_SZ);
			if (ret < 0) {
				dev_err(ts->dev, "%s: read crc32 result fail!\n", __func__);
				return ret;
			}
			*crc_result = le32_to_cpu(data.dword);
			return 0;
		}

		dev_info(ts->dev, "%s: Waiting for HW ready!\n", __func__);
		usleep_range(1000, 1100);
	}

	dev_err(ts->dev, "%s: read FW status fail\n", __func__);
	return -EINVAL;
}

/* -------------------------------------------------------------------------- */
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
	ts->spi_xfer_max_sz = HIMAX_HX83121A_FULL_STACK_SZ;
	ts->xfer_buf_sz = ts->spi_xfer_max_sz;
	ts->event_buf_sz = HIMAX_HX83121A_FULL_STACK_SZ;
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

	ret = device_create_file(ts->dev, &dev_attr_inplace_reset);
	if (ret) {
		dev_err(ts->dev, "failed to create inplace_reset sysfs attribute\n");
		return ret;
	}

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ret = sysfs_create_bin_file(&ts->dev->kobj, &event_stack_attr);
	if (ret) {
		dev_err(ts->dev, "failed to create event_stack sysfs attribute\n");
		device_remove_file(ts->dev, &dev_attr_inplace_reset);
		return ret;
	}
	ret = sysfs_create_bin_file(&ts->dev->kobj, &event_trace_attr);
	if (ret) {
		dev_err(ts->dev, "failed to create event_trace sysfs attribute\n");
		sysfs_remove_bin_file(&ts->dev->kobj, &event_stack_attr);
		device_remove_file(ts->dev, &dev_attr_inplace_reset);
		return ret;
	}
#endif

	ret = devm_device_add_group(ts->dev, &hx_algo_attr_group);
	if (ret) {
		dev_err(ts->dev, "failed to create algo sysfs group\n");
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		sysfs_remove_bin_file(&ts->dev->kobj, &event_trace_attr);
		sysfs_remove_bin_file(&ts->dev->kobj, &event_stack_attr);
#endif
		device_remove_file(ts->dev, &dev_attr_inplace_reset);
		return ret;
	}

	ts->panel_follower.funcs = &himax_panel_follower_funcs;
	ret = devm_drm_panel_add_follower(ts->dev, &ts->panel_follower);
	if (ret) {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		sysfs_remove_bin_file(&ts->dev->kobj, &event_trace_attr);
		sysfs_remove_bin_file(&ts->dev->kobj, &event_stack_attr);
#endif
		device_remove_file(ts->dev, &dev_attr_inplace_reset);
		return dev_err_probe(ts->dev, ret,
				     "failed to register panel follower\n");
	}

	return 0;
}

static void himax_spi_remove(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	cancel_delayed_work_sync(&ts->panel_reinit_work);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	sysfs_remove_bin_file(&ts->dev->kobj, &event_trace_attr);
	sysfs_remove_bin_file(&ts->dev->kobj, &event_stack_attr);
#endif

	himax_lock(ts);
	ts->shutting_down = true;
	ts->panel_enabled = false;
	ts->panel_prepared = false;
	device_remove_file(ts->dev, &dev_attr_inplace_reset);
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
