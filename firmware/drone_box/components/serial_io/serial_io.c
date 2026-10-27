#include "serial_io.h"

#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#define CONSOLE_UART    ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#define CONSOLE_BAUD    CONFIG_ESP_CONSOLE_UART_BAUDRATE
#define CONSOLE_RX_BUF  256
#endif

void serial_io_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
#else
    const uart_config_t cfg = {
        .baud_rate  = CONSOLE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CONSOLE_UART, &cfg));
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF, 0, 0, NULL, 0));
#endif
}

bool serial_io_read_byte(uint8_t *c)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(c, 1, portMAX_DELAY) == 1;
#else
    return uart_read_bytes(CONSOLE_UART, c, 1, portMAX_DELAY) == 1;
#endif
}

int serial_io_read_line(char *buf, size_t buf_len, bool secret)
{
    size_t n = 0;
    if (buf_len == 0) {
        return -1;
    }

    for (;;) {
        uint8_t c;
        if (!serial_io_read_byte(&c)) {
            return -1;
        }

        if (c == '\r' || c == '\n') {
            buf[n] = '\0';
            printf("\n");
            fflush(stdout);
            return (int)n;
        }
        if ((c == 0x7f || c == '\b') && n > 0) {   // backspace
            n--;
            printf("\b \b");
            fflush(stdout);
            continue;
        }
        if (c < 0x20 || n + 1 >= buf_len) {
            continue;                               // ignore control chars, do not overflow
        }

        buf[n++] = (char)c;
        putchar(secret ? '*' : (char)c);
        fflush(stdout);
    }
}

const char *serial_io_transport_name(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return "USB-Serial-JTAG";
#else
    return "UART0";
#endif
}
