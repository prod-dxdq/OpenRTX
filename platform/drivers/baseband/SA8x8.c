/***************************************************************************
 *   Copyright (C) 2023 by Silvano Seva IU2KWO                             *
 *                     and Niccolò Izzo IU2KIN                             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <interfaces/delays.h>
#include <string.h>
#include <stdio.h>
#include "SA8x8.h"

/*
 * Minimum required version of sa868-fw
 */
#define SA868FW_MAJOR    1
#define SA868FW_MINOR    1
#define SA868FW_PATCH    0
#define SA868FW_RELEASE  20


#if DT_NODE_HAS_STATUS(DT_ALIAS(radio), okay)
#define UART_RADIO_DEV_NODE DT_ALIAS(radio)
#else
#error "Please select the correct radio UART device"
#endif

#define RADIO_PDN_NODE DT_ALIAS(radio_pdn)

#define SA8X8_MSG_SIZE 32U

static const struct device *const uart_dev = DEVICE_DT_GET(UART_RADIO_DEV_NODE);
static const struct gpio_dt_spec radio_pdn = GPIO_DT_SPEC_GET(RADIO_PDN_NODE, gpios);

K_MSGQ_DEFINE(uart_msgq, SA8X8_MSG_SIZE, 10, 4);

static uint16_t rx_buf_pos;
static char rx_buf[SA8X8_MSG_SIZE];


static void uartRxCallback(const struct device *dev, void *user_data)
{
    uint8_t c;

    if (uart_irq_update(uart_dev) == false)
        return;

    if (uart_irq_rx_ready(uart_dev) == false)
        return;

    // read until FIFO empty
    while (uart_fifo_read(uart_dev, &c, 1) == 1)
    {
        if ((c == '\n') && (rx_buf_pos > 0))
        {
            rx_buf[rx_buf_pos] = '\0';

            // if queue is full, message is silently dropped
            k_msgq_put(&uart_msgq, &rx_buf, K_NO_WAIT);
            rx_buf_pos = 0;
        }
        else if (rx_buf_pos < (sizeof(rx_buf) - 1))
        {
            rx_buf[rx_buf_pos++] = c;
        }
    }
}

static void uartPrint(const char *fmt, ...)
{
    char buf[SA8X8_MSG_SIZE];

    va_list args;
    va_start(args, fmt);
    int len = vsnprintk(buf, SA8X8_MSG_SIZE, fmt, args);
    va_end(args);

    for(int i = 0; i < len; i++)
        uart_poll_out(uart_dev, buf[i]);
}

static inline void waitUntilReady()
{
    char buf[SA8X8_MSG_SIZE] = { 0 };

    while(true)
    {
        uartPrint("AT\r\n");
        int ret = k_msgq_get(&uart_msgq, buf, K_MSEC(250));
        if(ret != 0)
            printk("SA8x8: baseband is not ready!\n");

        if(strncmp(buf, "OK\r", SA8X8_MSG_SIZE) == 0)
            break;
    }
}

static inline bool checkFwVersion()
{
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t release;

    char *fwVersionStr = sa8x8_getFwVersion();
    sscanf(fwVersionStr, "sa8x8-fw/v%hhu.%hhu.%hhu.r%hhu", &major, &minor,
           &patch, &release);

    if((major >= SA868FW_MAJOR) && (minor >= SA868FW_MINOR) &&
       (patch >= SA868FW_PATCH) && (release >= SA868FW_RELEASE))
    {
        return true;
    }

    // Major, minor, patch or release not matching.
    printk("SA8x8: error, unsupported baseband firmware, please update!\n");
    return false;
}


int sa8x8_init()
{
    // Initialize GPIO for SA868S power down
    if(gpio_is_ready_dt(&radio_pdn) == false)
    {
        printk("SA8x8: error, radio device %s is not ready\n", radio_pdn.port->name);
        return -1;
    }

    int ret = gpio_pin_configure_dt(&radio_pdn, GPIO_OUTPUT);
    if (ret != 0)
    {
        printk("SA8x8: error %d, failed to configure %s pin %d\n", ret,
               radio_pdn.port->name, radio_pdn.pin);
        return ret;
    }

    // Reset the SA868S baseband
    gpio_pin_set_dt(&radio_pdn, 1);
    delayMs(100);
    gpio_pin_set_dt(&radio_pdn, 0);

    // Setup UART for communication
    if (device_is_ready(uart_dev) == false)
    {
        printk("SA8x8: error, UART device not found!\n");
        return -1;
    }

    ret = uart_irq_callback_user_data_set(uart_dev, uartRxCallback, NULL);
    if (ret < 0)
    {
        switch(ret)
        {
            case -ENOTSUP:
                printk("SA8x8: error, interrupt-driven UART support not enabled\n");
                break;

            case -ENOSYS:
                printk("SA8x8: error, UART device does not support interrupt-driven API\n");
                break;

            default:
                printk("SA8x8: error, cannot set UART callback: %d\n", ret);
                break;
        }

        return ret;
    }

    uart_irq_rx_enable(uart_dev);

    waitUntilReady();
    bool ok = checkFwVersion();
    if(ok)
        return 0;

    return -1;
}

const char *sa8x8_getModel()
{
    static char model[SA8X8_MSG_SIZE] = { 0 };

    if(model[0] == 0)
    {
        uartPrint("AT+MODEL\r\n");
        int ret = k_msgq_get(&uart_msgq, model, K_MSEC(100));
        if(ret != 0)
            printk("SA8x8: error while reading radio model\n");
    }

    return model;
}

const char *sa8x8_getFwVersion()
{
    static char fw_version[SA8X8_MSG_SIZE] = { 0 };

    if(fw_version[0] == 0)
    {
        uartPrint("AT+VERSION\r\n");
        int ret = k_msgq_get(&uart_msgq, fw_version, K_MSEC(100));
        if(ret != 0)
            printk("SA8x8: error while reading FW version\n");
    }

    return fw_version;
}

int sa8x8_enableHSMode()
{
    char buf[SA8X8_MSG_SIZE] = { 0 };
    struct uart_config uart_config;

    int ret = uart_config_get(uart_dev, &uart_config);
    if(ret != 0)
    {
        printk("SA8x8: error while retrieving UART configuration!\n");
        return ret;
    }

    uartPrint("AT+TURBO\r\n");
    ret = k_msgq_get(&uart_msgq, buf, K_MSEC(100));
    if(ret != 0)
    {
        printk("SA8x8: error while retrieving turbo response!\n");
        return ret;
    }

    uart_config.baudrate = 115200;

    ret = uart_configure(uart_dev, &uart_config);
    if(ret != 0)
    {
        printk("c error while setting UART configuration!\n");
        return ret;
    }

    return 0;
}

void sa8x8_writeAT1846Sreg(uint8_t reg, uint16_t value)
{
    char buf[SA8X8_MSG_SIZE];

    uartPrint("AT+POKE=%d,%d\r\n", reg, value);
    k_msgq_get(&uart_msgq, buf, K_MSEC(100));

    // Check that response is "OK\r"
    if(strncmp(buf, "OK\r", 3U) != 0)
        printk("SA8x8 Error: %d <- %d\n", reg, value);
}

uint16_t sa8x8_readAT1846Sreg(uint8_t reg)
{
    char buf[SA8X8_MSG_SIZE];
    uint16_t value = 0;

    uartPrint("AT+PEEK=%d\r\n", reg);
    k_msgq_get(&uart_msgq, buf, K_MSEC(100));

    int ret = sscanf(buf, "%hd\r", &value);
    if(ret != 1)
        printk("SA8x8 Error: %d ->\n", reg);

    return value;
}