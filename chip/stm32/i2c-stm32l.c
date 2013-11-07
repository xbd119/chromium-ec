/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "chipset.h"
#include "clock.h"
#include "common.h"
#include "console.h"
#include "dma.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
#include "i2c_arbitration.h"
#include "registers.h"
#include "task.h"
#include "timer.h"
#include "util.h"

/* Console output macros */
#define CPUTS(outstr) cputs(CC_I2C, outstr)
#define CPRINTF(format, args...) cprintf(CC_I2C, format, ## args)

#define I2C1      STM32_I2C1_PORT
#define I2C2      STM32_I2C2_PORT

/* Maximum transfer of a SMBUS block transfer */
#define SMBUS_MAX_BLOCK 32

#define I2C_ERROR_FAILED_START EC_ERROR_INTERNAL_FIRST

/*
 * Transmit timeout in microseconds
 *
 * In theory we shouldn't have a timeout here (at least when we're in slave
 * mode).  The slave is supposed to wait forever for the master to read bytes.
 * ...but we're going to keep the timeout to make sure we're robust.  It may in
 * fact be needed if the host resets itself mid-read.
 */
#define I2C_TX_TIMEOUT_MASTER	(10 * MSEC)

/*
 * Delay 5us in bitbang mode.  That gives us roughly 5us low and 5us high or
 * a frequency of 100kHz.
 */
#define I2C_BITBANG_HALF_CYCLE_US    5

#ifdef CONFIG_I2C_DEBUG
static void dump_i2c_reg(int port, const char *what)
{
	CPRINTF("[%T i2c CR1=%04x CR2=%04x SR1=%04x SR2=%04x %s]\n",
		STM32_I2C_CR1(port),
		STM32_I2C_CR2(port),
		STM32_I2C_SR1(port),
		STM32_I2C_SR2(port),
		what);
}
#else
static inline void dump_i2c_reg(int port, const char *what)
{
}
#endif

/**
 * Wait for SR1 register to contain the specified mask.
 *
 * Returns EC_SUCCESS, EC_ERROR_TIMEOUT if timed out waiting, or
 * EC_ERROR_UNKNOWN if an error bit appeared in the status register.
 */
static int wait_sr1(int port, int mask)
{
	uint64_t timeout = get_time().val + I2C_TX_TIMEOUT_MASTER;

	while (get_time().val < timeout) {
		int sr1 = STM32_I2C_SR1(port);

		/* Check for desired mask */
		if ((sr1 & mask) == mask)
			return EC_SUCCESS;

		/* Check for errors */
		if (sr1 & (STM32_I2C_SR1_ARLO | STM32_I2C_SR1_BERR |
			   STM32_I2C_SR1_AF)) {
			dump_i2c_reg(port, "wait_sr1 failed");
			return EC_ERROR_UNKNOWN;
		}

		/* I2C is slow, so let other things run while we wait */
		usleep(100);
	}

	return EC_ERROR_TIMEOUT;
}

/**
 * Send a start condition and slave address on the specified port.
 *
 * @param port		I2C port
 * @param slave_addr	Slave address, with LSB set for receive-mode
 *
 * @return Non-zero if error.
 */
static int send_start(int port, int slave_addr)
{
	int rv;

	/* Send start bit */
	STM32_I2C_CR1(port) |= STM32_I2C_CR1_START;
	dump_i2c_reg(port, "sent start");
	rv = wait_sr1(port, STM32_I2C_SR1_SB);
	if (rv)
		return I2C_ERROR_FAILED_START;

	/* Write slave address */
	STM32_I2C_DR(port) = slave_addr & 0xff;
	rv = wait_sr1(port, STM32_I2C_SR1_ADDR);
	if (rv)
		return rv;

	/* Read SR2 to clear ADDR bit */
	rv = STM32_I2C_SR2(port);

	dump_i2c_reg(port, "wrote addr");

	return EC_SUCCESS;
}

static void i2c_set_freq_port(const struct i2c_port_t *p)
{
	int port = p->port;
	int freq = clock_get_freq();

	/* Force peripheral reset and disable port */
	STM32_I2C_CR1(port) = STM32_I2C_CR1_SWRST;
	STM32_I2C_CR1(port) = 0;

	/* Set clock frequency */
	STM32_I2C_CCR(port) = freq / (2 * MSEC * p->kbps);
	STM32_I2C_CR2(port) = freq / SECOND;
	STM32_I2C_TRISE(port) = freq / SECOND + 1;

	/* Enable port */
	STM32_I2C_CR1(port) |= STM32_I2C_CR1_PE;
}

/*
 * Try to pull up SCL. If clock is stretched, we will wait for a few cycles
 * for the slave to get ready.
 *
 * @param scl		the SCL gpio pin
 * @return 0 when success; -1 if SCL is still low
 */
static int try_pull_up_scl(enum gpio_signal scl)
{
	int i;
	for (i = 0; i < 3; ++i) {
		gpio_set_level(scl, 1);
		if (gpio_get_level(scl))
			return 0;
		udelay(I2C_BITBANG_HALF_CYCLE_US);
	}
	CPRINTF("[%T I2C clock stretched too long?]\n");
	return -1;
}

/*
 * Try to unwedge the bus.
 *
 * The implementation is based on unwedge_i2c_bus() in i2c-stm32f.c.
 * Or refer to https://chromium-review.googlesource.com/#/c/32168 for details.
 *
 * @param port		I2C port
 * @param force_unwedge	perform unwedge without checking if wedged
 */
static void i2c_try_unwedge(int port, int force_unwedge)
{
	enum gpio_signal scl, sda;
	int i;

	/*
	 * TODO(crosbug.com/p/23802): This requires defining GPIOs for both
	 * ports even if the board only supports one port.
	 */
	if (port == I2C1) {
		sda = GPIO_I2C1_SDA;
		scl = GPIO_I2C1_SCL;
	} else {
		sda = GPIO_I2C2_SDA;
		scl = GPIO_I2C2_SCL;
	}

	if (!force_unwedge) {
		if (gpio_get_level(scl) && gpio_get_level(sda))
			/* Everything seems ok; no need to unwedge */
			return;
		CPRINTF("[%T I2C wedge detected; fixing]\n");
	}

	gpio_set_flags(scl, GPIO_ODR_HIGH);
	gpio_set_flags(sda, GPIO_ODR_HIGH);

	if (!gpio_get_level(scl)) {
		/*
		 * Clock is low, wait for a while in case of clock stretched
		 * by a slave.
		 */
		if (try_pull_up_scl(scl))
			return;
	}

	/*
	 * SCL is high. No matter whether SDA is 0 or 1, we generate at most
	 * 9 clocks with SDA released and then send a STOP. If a slave is in the
	 * middle of writing, one of the cycles should be a NACK.
	 * If it's in reading, then this should finish the transaction.
	 */
	udelay(I2C_BITBANG_HALF_CYCLE_US);
	for (i = 0; i < 9; ++i) {
		if (try_pull_up_scl(scl))
			return;
		udelay(I2C_BITBANG_HALF_CYCLE_US);
		gpio_set_level(scl, 0);
		udelay(I2C_BITBANG_HALF_CYCLE_US);
		if (gpio_get_level(sda))
			break;
	}

	/* Issue a STOP */
	gpio_set_level(sda, 0);
	udelay(I2C_BITBANG_HALF_CYCLE_US);
	if (try_pull_up_scl(scl))
		return;
	udelay(I2C_BITBANG_HALF_CYCLE_US);
	gpio_set_level(sda, 1);
	if (gpio_get_level(sda) == 0)
		CPRINTF("[%T sda is still low]\n");
	udelay(I2C_BITBANG_HALF_CYCLE_US);
}

/**
 * Initialize on the specified I2C port.
 *
 * @param p		the I2c port
 * @param force_unwedge	perform unwedge without checking if wedged
 */
static void i2c_init_port(const struct i2c_port_t *p, int force_unwedge)
{
	int port = p->port;

	/* Unwedge the bus if it seems wedged */
	i2c_try_unwedge(port, force_unwedge);

	/* Enable clocks to I2C modules if necessary */
	if (!(STM32_RCC_APB1ENR & (1 << (21 + port))))
		STM32_RCC_APB1ENR |= 1 << (21 + port);

	/* Configure GPIOs */
	gpio_config_module(MODULE_I2C, 1);

	/* Set up initial bus frequencies */
	i2c_set_freq_port(p);
}

/*****************************************************************************/
/* Interface */

int i2c_xfer(int port, int slave_addr, const uint8_t *out, int out_bytes,
	     uint8_t *in, int in_bytes, int flags)
{
	int started = (flags & I2C_XFER_START) ? 0 : 1;
	int rv = EC_SUCCESS;
	int i;

	ASSERT(out || !out_bytes);
	ASSERT(in || !in_bytes);

	dump_i2c_reg(port, "xfer start");

	/*
	 * Clear status
	 *
	 * TODO(crosbug.com/p/29314): should check for any leftover error
	 * status, and reset the port if present.
	 */
	STM32_I2C_SR1(port) = 0;

	/* Clear start, stop, POS, ACK bits to get us in a known state */
	STM32_I2C_CR1(port) &= ~(STM32_I2C_CR1_START |
				 STM32_I2C_CR1_STOP |
				 STM32_I2C_CR1_POS |
				 STM32_I2C_CR1_ACK);

	/* No out bytes and no in bytes means just check for active */
	if (out_bytes || !in_bytes) {
		if (!started) {
			rv = send_start(port, slave_addr);
			if (rv)
				goto xfer_exit;
		}

		/* Write data, if any */
		for (i = 0; i < out_bytes; i++) {
			/* Write next data byte */
			STM32_I2C_DR(port) = out[i];
			dump_i2c_reg(port, "wrote data");

			rv = wait_sr1(port, STM32_I2C_SR1_BTF);
			if (rv)
				goto xfer_exit;
		}

		/* Need repeated start condition before reading */
		started = 0;

		/* If no input bytes, queue stop condition */
		if (!in_bytes && (flags & I2C_XFER_STOP))
			STM32_I2C_CR1(port) |= STM32_I2C_CR1_STOP;
	}

	if (in_bytes) {
		/* Setup ACK/POS before sending start as per user manual */
		if (in_bytes == 2)
			STM32_I2C_CR1(port) |= STM32_I2C_CR1_POS;
		else if (in_bytes != 1)
			STM32_I2C_CR1(port) |= STM32_I2C_CR1_ACK;

		if (!started) {
			rv = send_start(port, slave_addr | 0x01);
			if (rv)
				goto xfer_exit;
		}

		if (in_bytes == 1) {
			/* Set stop immediately after ADDR cleared */
			if (flags & I2C_XFER_STOP)
				STM32_I2C_CR1(port) |= STM32_I2C_CR1_STOP;

			rv = wait_sr1(port, STM32_I2C_SR1_RXNE);
			if (rv)
				goto xfer_exit;

			in[0] = STM32_I2C_DR(port);
		} else if (in_bytes == 2) {
			/* Wait till the shift register is full */
			rv = wait_sr1(port, STM32_I2C_SR1_BTF);
			if (rv)
				goto xfer_exit;

			if (flags & I2C_XFER_STOP)
				STM32_I2C_CR1(port) |= STM32_I2C_CR1_STOP;

			in[0] = STM32_I2C_DR(port);
			in[1] = STM32_I2C_DR(port);
		} else {
			/* Read all but last three */
			for (i = 0; i < in_bytes - 3; i++) {
				/* Wait for receive buffer not empty */
				rv = wait_sr1(port, STM32_I2C_SR1_RXNE);
				if (rv)
					goto xfer_exit;

				dump_i2c_reg(port, "read data");
				in[i] = STM32_I2C_DR(port);
				dump_i2c_reg(port, "post read data");
			}

			/* Wait for BTF (data N-2 in DR, N-1 in shift) */
			rv = wait_sr1(port, STM32_I2C_SR1_BTF);
			if (rv)
				goto xfer_exit;

			/* No more acking */
			STM32_I2C_CR1(port) &= ~STM32_I2C_CR1_ACK;
			in[i++] = STM32_I2C_DR(port);

			/* Wait for BTF (data N-1 in DR, N in shift) */
			rv = wait_sr1(port, STM32_I2C_SR1_BTF);
			if (rv)
				goto xfer_exit;

			/* If this is the last byte, queue stop condition */
			if (flags & I2C_XFER_STOP)
				STM32_I2C_CR1(port) |= STM32_I2C_CR1_STOP;

			/* Read the last two bytes */
			in[i++] = STM32_I2C_DR(port);
			in[i++] = STM32_I2C_DR(port);
		}
	}

 xfer_exit:
	/* On error, queue a stop condition */
	if (rv) {
		flags |= I2C_XFER_STOP;
		STM32_I2C_CR1(port) |= STM32_I2C_CR1_STOP;
		dump_i2c_reg(port, "stop after error");

		/*
		 * If failed at sending start, try resetting the port
		 * to unwedge the bus.
		 */
		if (rv == I2C_ERROR_FAILED_START) {
			const struct i2c_port_t *p = i2c_ports;
			CPRINTF("[%T i2c_xfer start error; "
				"try resetting i2c%d to unwedge.\n", port);
			for (i = 0; i < i2c_ports_used; i++, p++) {
				if (p->port == port) {
					i2c_init_port(p, 1); /* force unwedge */
					break;
				}
			}
			CPRINTF("[%T I2C done resetting.\n");
		}
	}

	/* If a stop condition is queued, wait for it to take effect */
	if (flags & I2C_XFER_STOP) {
		/* Wait up to 100 us for bus idle */
		for (i = 0; i < 10; i++) {
			if (!(STM32_I2C_SR2(port) & STM32_I2C_SR2_BUSY))
				break;
			udelay(10);
		}

		/*
		 * Allow bus to idle for at least one 100KHz clock = 10 us.
		 * This allows slaves on the bus to detect bus-idle before
		 * the next start condition.
		 */
		udelay(10);
	}

	return rv;
}

int i2c_get_line_levels(int port)
{
	enum gpio_signal sda, scl;

	ASSERT(port == I2C1 || port == I2C2);

	if (port == I2C1) {
		sda = GPIO_I2C1_SDA;
		scl = GPIO_I2C1_SCL;
	} else {
		sda = GPIO_I2C2_SDA;
		scl = GPIO_I2C2_SCL;
	}

	return (gpio_get_level(sda) ? I2C_LINE_SDA_HIGH : 0) |
		(gpio_get_level(scl) ? I2C_LINE_SCL_HIGH : 0);
}

int i2c_read_string(int port, int slave_addr, int offset, uint8_t *data,
	int len)
{
	int rv;
	uint8_t reg, block_length;

	/*
	 * TODO(crosbug.com/p/23569): when i2c_xfer() supports start/stop bits,
	 * merge this with the LM4 implementation and move to i2c_common.c.
	 */

	if ((len <= 0) || (len > SMBUS_MAX_BLOCK))
		return EC_ERROR_INVAL;

	i2c_lock(port, 1);

	/* Read the counted string into the output buffer */
	reg = offset;
	rv = i2c_xfer(port, slave_addr, &reg, 1, data, len, I2C_XFER_SINGLE);
	if (rv == EC_SUCCESS) {
		/* Block length is the first byte of the returned buffer */
		block_length = MIN(data[0], len - 1);

		/* Move data down, then null-terminate it */
		memmove(data, data + 1, block_length);
		data[block_length] = 0;
	}

	i2c_lock(port, 0);
	return rv;
}

/*****************************************************************************/
/* Hooks */

/* Handle CPU clock changing frequency */
static void i2c_freq_change(void)
{
	const struct i2c_port_t *p = i2c_ports;
	int i;

	for (i = 0; i < i2c_ports_used; i++, p++)
		i2c_set_freq_port(p);
}

static void i2c_pre_freq_change_hook(void)
{
	const struct i2c_port_t *p = i2c_ports;
	int i;

	/* Lock I2C ports so freq change can't interrupt an I2C transaction */
	for (i = 0; i < i2c_ports_used; i++, p++)
		i2c_lock(p->port, 1);
}
DECLARE_HOOK(HOOK_PRE_FREQ_CHANGE, i2c_pre_freq_change_hook, HOOK_PRIO_DEFAULT);
static void i2c_freq_change_hook(void)
{
	const struct i2c_port_t *p = i2c_ports;
	int i;

	i2c_freq_change();

	/* Unlock I2C ports we locked in pre-freq change hook */
	for (i = 0; i < i2c_ports_used; i++, p++)
		i2c_lock(p->port, 0);
}
DECLARE_HOOK(HOOK_FREQ_CHANGE, i2c_freq_change_hook, HOOK_PRIO_DEFAULT);

static void i2c_init(void)
{
	const struct i2c_port_t *p = i2c_ports;
	int i;

	for (i = 0; i < i2c_ports_used; i++, p++)
		i2c_init_port(p, 0); /* do not force unwedged */
}
DECLARE_HOOK(HOOK_INIT, i2c_init, HOOK_PRIO_DEFAULT);

/*****************************************************************************/
/* Console commands */

static int command_i2cdump(int argc, char **argv)
{
	dump_i2c_reg(I2C_PORT_MASTER, "dump");
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(i2cdump, command_i2cdump,
			NULL,
			"Dump I2C regs",
			NULL);
