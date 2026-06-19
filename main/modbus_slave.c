/*
 * ESP32 Modbus RTU slave example (ESP-IDF 6.0, esp-modbus v2.1.2)
 *
 * Implements a serial Modbus RTU slave exposing four parameter areas:
 *   - Holding registers  (read / write)   : 10 x uint16
 *   - Input registers    (read only)       : 10 x uint16
 *   - Coils              (read / write)     : 16 bits
 *   - Discrete inputs    (read only)        : 16 bits
 *
 * The slave address, UART port and baud rate are configured via
 * `idf.py menuconfig` -> "Modbus Slave Configuration". The UART pins are
 * configured by the esp-modbus component under
 * "Component config -> Modbus configuration".
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "mbcontroller.h"

static const char *TAG = "mb_slave";

/* ---- Register storage ------------------------------------------------- */

#define MB_HOLDING_CNT   10   /* number of 16-bit holding registers   */
#define MB_INPUT_CNT     10   /* number of 16-bit input registers     */
#define MB_COIL_BYTES     2   /* 2 bytes -> 16 coils                   */
#define MB_DISCRETE_BYTES 2   /* 2 bytes -> 16 discrete inputs         */

static uint16_t s_holding_regs[MB_HOLDING_CNT];
static uint16_t s_input_regs[MB_INPUT_CNT];
static uint8_t  s_coils[MB_COIL_BYTES];
static uint8_t  s_discrete[MB_DISCRETE_BYTES];

/* Modbus slave controller handle (v2.x API). */
static void *s_mb_handle = NULL;

/* ---- Helpers ---------------------------------------------------------- */

static void set_descriptor(mb_param_type_t type, uint16_t offset,
                           void *address, size_t size)
{
    mb_register_area_descriptor_t area = {
        .type = type,
        .start_offset = offset,
        .address = address,
        .size = size,
    };
    ESP_ERROR_CHECK(mbc_slave_set_descriptor(s_mb_handle, area));
}

static const char *event_to_str(mb_event_group_t event)
{
    if (event & MB_EVENT_HOLDING_REG_WR) return "HOLDING WR";
    if (event & MB_EVENT_HOLDING_REG_RD) return "HOLDING RD";
    if (event & MB_EVENT_INPUT_REG_RD)   return "INPUT RD";
    if (event & MB_EVENT_COILS_WR)       return "COILS WR";
    if (event & MB_EVENT_COILS_RD)       return "COILS RD";
    if (event & MB_EVENT_DISCRETE_RD)    return "DISCRETE RD";
    return "OTHER";
}

/* ---- Init ------------------------------------------------------------- */

static void modbus_slave_init(void)
{
    mb_communication_info_t comm_info = {
        .ser_opts = {
            .port      = CONFIG_MB_SLAVE_UART_PORT_NUM,
            .mode      = MB_RTU,
            .baudrate  = CONFIG_MB_SLAVE_UART_BAUD,
            .parity    = MB_PARITY_NONE,
            .uid       = CONFIG_MB_SLAVE_ADDR,
            .data_bits = UART_DATA_8_BITS,
            .stop_bits = UART_STOP_BITS_1,
        },
    };

    ESP_ERROR_CHECK(mbc_slave_create_serial(&comm_info, &s_mb_handle));

    /* Register the four parameter areas. */
    set_descriptor(MB_PARAM_HOLDING,  0, (void *)s_holding_regs, sizeof(s_holding_regs));
    set_descriptor(MB_PARAM_INPUT,    0, (void *)s_input_regs,   sizeof(s_input_regs));
    set_descriptor(MB_PARAM_COIL,     0, (void *)s_coils,        sizeof(s_coils));
    set_descriptor(MB_PARAM_DISCRETE, 0, (void *)s_discrete,     sizeof(s_discrete));

    /* Seed some demo values so a master can read meaningful data. */
    for (int i = 0; i < MB_INPUT_CNT; i++) {
        s_input_regs[i] = 0x1000 + i;
    }
    for (int i = 0; i < MB_HOLDING_CNT; i++) {
        s_holding_regs[i] = i;
    }
    s_discrete[0] = 0xA5;

    ESP_ERROR_CHECK(mbc_slave_start(s_mb_handle));

    ESP_LOGI(TAG, "Modbus RTU slave started: addr=%d, uart=%d, baud=%d",
             CONFIG_MB_SLAVE_ADDR, CONFIG_MB_SLAVE_UART_PORT_NUM,
             CONFIG_MB_SLAVE_UART_BAUD);
}

void app_main(void)
{
    modbus_slave_init();

    /* All read/write events from the master are flagged here; wait for any
     * of them, then report which area was accessed. */
    const mb_event_group_t wait_mask =
        MB_EVENT_HOLDING_REG_WR | MB_EVENT_HOLDING_REG_RD |
        MB_EVENT_INPUT_REG_RD |
        MB_EVENT_COILS_WR | MB_EVENT_COILS_RD |
        MB_EVENT_DISCRETE_RD;

    mb_param_info_t reg_info;

    while (1) {
        mb_event_group_t event = mbc_slave_check_event(s_mb_handle, wait_mask);

        if (mbc_slave_get_param_info(s_mb_handle, &reg_info,
                                     10 / portTICK_PERIOD_MS) == ESP_OK) {
            ESP_LOGI(TAG, "%s: offset=%u, size=%u, addr=%p",
                     event_to_str(event),
                     (unsigned)reg_info.mb_offset,
                     (unsigned)reg_info.size,
                     reg_info.address);
        }
    }
}
