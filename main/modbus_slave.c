/*
 * ESP32 Modbus slave example (ESP-IDF 6.0, esp-modbus v2.1.2)
 *
 * Supports BOTH Modbus RTU (serial) and Modbus TCP (network) in a single
 * firmware, with RUNTIME HOT-SWITCHING between them: a button press tears
 * down the active slave and rebuilds it on the other transport, without a
 * reboot or reflash.
 *
 * The boot mode and switch button GPIO are set via
 * `idf.py menuconfig` -> "Modbus Slave Configuration".
 *
 * Both modes expose four parameter areas:
 *   - Holding registers  (read / write)   : 10 x uint16
 *   - Input registers    (read only)       : 10 x uint16
 *   - Coils              (read / write)     : 16 bits
 *   - Discrete inputs    (read only)        : 16 bits
 */

#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "mbcontroller.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

static const char *TAG = "mb_slave";

#define SWITCH_GPIO ((gpio_num_t)CONFIG_MB_SWITCH_GPIO)

typedef enum {
    MODE_RTU = 0,
    MODE_TCP,
} slave_mode_t;

static const char *mode_str(slave_mode_t m) { return m == MODE_RTU ? "RTU" : "TCP"; }

/* ---- Register storage ------------------------------------------------- */

#define MB_HOLDING_CNT   10   /* number of 16-bit holding registers   */
#define MB_INPUT_CNT     10   /* number of 16-bit input registers     */
#define MB_COIL_BYTES     2   /* 2 bytes -> 16 coils                   */
#define MB_DISCRETE_BYTES 2   /* 2 bytes -> 16 discrete inputs         */

static uint16_t s_holding_regs[MB_HOLDING_CNT];
static uint16_t s_input_regs[MB_INPUT_CNT];
static uint8_t  s_coils[MB_COIL_BYTES];
static uint8_t  s_discrete[MB_DISCRETE_BYTES];

/* Modbus slave controller handle (v2.x API) and current transport. */
static void *s_mb_handle = NULL;
static slave_mode_t s_current_mode;
static bool s_net_ready = false;

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

static void register_areas_and_seed(void)
{
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
}

/* ---- Network (lazy, only needed for TCP) ------------------------------ */

static void ensure_network(void)
{
    if (s_net_ready) {
        return;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* Brings up Wi-Fi / Ethernet using "Example Connection Configuration". */
    ESP_ERROR_CHECK(example_connect());

    s_net_ready = true;
}

/* ---- Transport-specific controller creation --------------------------- */

static void slave_create_rtu(void)
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
}

static void slave_create_tcp(void)
{
    ensure_network();

    mb_communication_info_t comm_info = {
        .tcp_opts = {
            .mode          = MB_TCP,
            .port          = CONFIG_MB_SLAVE_TCP_PORT,
            .addr_type     = MB_IPV4,
            .ip_addr_table = NULL,                 /* accept any master */
            .ip_netif_ptr  = (void *)get_example_netif(),
            .uid           = CONFIG_MB_SLAVE_ADDR,
        },
    };
    ESP_ERROR_CHECK(mbc_slave_create_tcp(&comm_info, &s_mb_handle));
}

/* ---- Start / stop / hot-switch ---------------------------------------- */

static void slave_start(slave_mode_t mode)
{
    if (mode == MODE_RTU) {
        slave_create_rtu();
    } else {
        slave_create_tcp();
    }

    register_areas_and_seed();
    ESP_ERROR_CHECK(mbc_slave_start(s_mb_handle));

    s_current_mode = mode;
    ESP_LOGI(TAG, "Modbus slave running in %s mode (addr=%d)",
             mode_str(mode), CONFIG_MB_SLAVE_ADDR);
}

static void slave_stop(void)
{
    if (s_mb_handle) {
        ESP_ERROR_CHECK(mbc_slave_delete(s_mb_handle));
        s_mb_handle = NULL;
    }
}

static void slave_switch(slave_mode_t mode)
{
    if (mode == s_current_mode) {
        return;
    }
    ESP_LOGW(TAG, "Hot-switching %s -> %s ...",
             mode_str(s_current_mode), mode_str(mode));
    slave_stop();
    slave_start(mode);
}

/* ---- Mode-switch button ----------------------------------------------- */

static void switch_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_MB_SWITCH_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "Mode switch button on GPIO%d (press to toggle RTU/TCP)",
             CONFIG_MB_SWITCH_GPIO);
}

/* ---- Application ------------------------------------------------------ */

void app_main(void)
{
    switch_button_init();

    slave_mode_t init_mode =
#if CONFIG_MB_SLAVE_INIT_TCP
        MODE_TCP;
#else
        MODE_RTU;
#endif
    slave_start(init_mode);

    /* Supervisor loop: debounce the button (active low) and hot-switch on a
     * falling edge. The esp-modbus stack services master requests in the
     * background, so no blocking event loop is required here. */
    int last_level = 1;
    while (1) {
        int level = gpio_get_level(SWITCH_GPIO);
        if (last_level == 1 && level == 0) {
            slave_switch(s_current_mode == MODE_RTU ? MODE_TCP : MODE_RTU);
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
