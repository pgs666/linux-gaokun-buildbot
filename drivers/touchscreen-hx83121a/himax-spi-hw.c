// SPDX-License-Identifier: GPL-2.0
/* HX83121A SPI transport, register access and firmware control. */

#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/interrupt.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

#include "himax-spi.h"

#define HIMAX_BUS_RETRY					3
/* SPI bus read header length */
#define HIMAX_BUS_R_HLEN				3U
/* SPI bus write header length */
#define HIMAX_BUS_W_HLEN				2U
/* TP SRAM address size and data size */
#define HIMAX_REG_SZ					4U
#define HIMAX_HX83121A_SAFE_MODE_PASSWORD		0x9527

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
/* HIMAX SPI function select, 1st byte of any SPI command sequence */
#define HIMAX_SPI_FUNCTION_READ				0xf3
#define HIMAX_SPI_FUNCTION_WRITE			0xf2

union himax_dword_data {
	u32 dword;
	u16 word[2];
	u8 byte[4];
};

int himax_spi_read(struct himax_ts_data *ts, u8 cmd, u8 *buf, u32 len)
{
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = HIMAX_BUS_R_HLEN + len,
		.tx_buf = ts->xfer_buf,
		.rx_buf = ts->xfer_buf,
		.cs_change = 0,
	};
	int retry_cnt;
	int ret;

	memset(ts->xfer_buf, 0, HIMAX_BUS_R_HLEN + len);
	ts->xfer_buf[0] = HIMAX_SPI_FUNCTION_READ;
	ts->xfer_buf[1] = cmd;
	ts->xfer_buf[2] = 0x00;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	for (retry_cnt = 0; retry_cnt < HIMAX_BUS_RETRY; retry_cnt++) {
		ret = spi_sync(ts->spi, &msg);
		if (ret < 0)
			dev_err_ratelimited(ts->dev,
					    "SPI read failed: %d (attempt %d/%d)\n",
					    ret, retry_cnt + 1, HIMAX_BUS_RETRY);
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
		dev_err(ts->dev, "SPI write failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int himax_write(struct himax_ts_data *ts, u8 cmd, u8 *addr,
		       const u8 *data, u32 len)
{
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
	if (ret < 0)
		dev_err(ts->dev, "%s: write ahb_addr_byte_0 failed\n", __func__);

	return himax_mcu_set_burst_mode(ts, !(len > HIMAX_REG_SZ));
}

/* Xiaomi replays firmware-owned state with write/read retry loops.  Keep the
 * same rule, but require an exact readback (their AP-notify loop accidentally
 * stops on a successful read even when the value differs).
 */
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

int himax_restore_afe_runtime(struct himax_ts_data *ts)
{
	int ret;

	if (!himax_restore_afe_enabled())
		return 0;

	/* StartCalibration is intentionally absent here.  The command transport
	 * has no calibration-complete acknowledgement in the recovered screen-on
	 * path; issuing it and immediately consuming raw frames races the AFE.
	 */
	ret = himax_afe_send_command(ts, &ts->afe_command_slot,
				     HIMAX_HX83121A_AFE_CMD_FORCE_SCAN_RATE,
				     HIMAX_HX83121A_AFE_SCAN_RATE_120HZ);
	if (ret)
		dev_warn(ts->dev, "failed to force 120 Hz scan rate: %d\n", ret);
	else
		dev_info(ts->dev, "120 Hz scan-rate command transport completed\n");

	return ret;
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
	gpiod_set_value_cansleep(ts->gpiod_rst, 1);
	usleep_range(20000, 20100);
	gpiod_set_value_cansleep(ts->gpiod_rst, 0);
	usleep_range(50000, 50100);
}

void himax_int_enable(struct himax_ts_data *ts, bool enable)
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

void himax_lock(struct himax_ts_data *ts)
{
	himax_quiesce_irq(ts);
	mutex_lock(&ts->op_lock);
}

void himax_unlock(struct himax_ts_data *ts)
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

		usleep_range(10000, 10100);
		himax_pin_reset(ts);
	}
	dev_err(ts->dev, "%s: failed!\n", __func__);

	return -EIO;
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
	return 0;
}

int hx83121a_chip_detect(struct himax_ts_data *ts)
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

	ret = himax_sense_off(ts, false);
	if (ret)
		return ret;

	for (retry_cnt = 0; retry_cnt < read_icid_retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_ICID, data.byte, 4);
		if (ret) {
			dev_err(ts->dev, "%s: Read IC ID Fail\n", __func__);
			return ret;
		}

		data.dword = le32_to_cpu(data.dword) >> 8;
		dev_info(ts->dev, "Detected IC HX%06X\n", data.dword);

		if (data.dword == 0x83121a)
			return 0;
	}
	return -ENODEV;
}

int himax_verify_running_ic(struct himax_ts_data *ts)
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

int himax_restore_raw_runtime(struct himax_ts_data *ts)
{
	union himax_dword_data password = {
		.dword = cpu_to_le32(0x00005aa5),
	};
	int ret;
	int supplied;

	/* These are firmware-owned runtime values.  Xiaomi similarly replays its
	 * USB/gesture/touch-mode values after resume; our raw-matrix transport
	 * additionally needs RawOut and the SRAM producer handshake.
	 */
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
	 * common-mode filter, not a replacement for it.
	 */
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
	 * incorrect.  Validate it through fresh master event frames below.
	 */
	return himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_RAWDATA_PASSWORD,
					password.byte, sizeof(password.byte));
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
 * partially retained firmware state after display sleep.
 */
int himax_enable_fw_reload(struct himax_ts_data *ts)
{
	union himax_dword_data data = { .dword = cpu_to_le32(0) };

	/* This flag is firmware-owned once sensing restarts, so mirror Windows and
	 * verify success through the subsequent 0x72c0 reload-done handshake.
	 */
	return himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_FLASH_RELOAD,
					data.byte, 4);
}

int himax_mcu_power_on_init(struct himax_ts_data *ts)
{
	int ret;
	u32 retry_cnt;
	const u32 retry_limit = 30;
	union himax_dword_data data;

	data.dword = cpu_to_le32(HIMAX_DATA_CLEAR);
	/* Initial sorting mode password to normal mode */
	ret = himax_mcu_assign_sorting_mode(ts, data.byte);
	if (ret < 0) {
		dev_err(ts->dev, "%s: assign sorting mode fail\n", __func__);
		return ret;
	}

	/* Restore the firmware normal-mode N-frame value. */
	data.dword = cpu_to_le32(1);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_SET_NFRAME, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: set N frame fail\n", __func__);
		return ret;
	}

	/* Clear firmware reload status before leaving safe mode. */
	data.dword = cpu_to_le32(HIMAX_DATA_CLEAR);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: initial FW reload status fail\n", __func__);
		return ret;
	}

	ret = himax_sense_on(ts, false);
	if (ret < 0) {
		dev_err(ts->dev, "%s: sense on fail\n", __func__);
		return ret;
	}

	/* Windows waits for central-state A8 == 0x05 before trusting reload
	 * state.  A responsive bus alone does not mean firmware boot completed.
	 */
	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_FW_STATUS,
					      data.byte, 4);
		if (ret < 0)
			return ret;
		if (data.byte[0] == HIMAX_REG_DATA_FW_STATE_RUNNING)
			break;
		usleep_range(10000, 11000);
	}
	if (retry_cnt == retry_limit) {
		dev_err(ts->dev, "%s: FW did not enter running state\n", __func__);
		return -ETIMEDOUT;
	}

	dev_info(ts->dev, "%s: waiting for FW reload data\n", __func__);
	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_DSRAM_ADDR_2ND_FLASH_RELOAD, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read FW reload status fail\n", __func__);
			return ret;
		}

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
	 * password is applicable here.
	 */
	data.dword = cpu_to_le32(0x00005aa5);
	ret = himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_RAWDATA_PASSWORD,
				       data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: set raw-data password fail\n", __func__);
		return ret;
	}

	return 0;
}

int himax_mcu_check_crc(struct himax_ts_data *ts, u32 start_addr,
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
