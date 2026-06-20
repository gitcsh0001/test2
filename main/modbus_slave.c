/*
 * ESP32 Modbus slave (ESP-IDF 6.0, esp-modbus v2.1.2)
 *
 * A single firmware that runs a Modbus slave on EITHER RTU (serial) or TCP
 * (network), reconfigurable at runtime from a built-in web UI. The web page
 * lets you change the transport and all RTU/TCP parameters and apply them
 * with a live hot-switch (stop -> rebuild -> start). Settings are persisted
 * in NVS so they survive a reboot.
 *
 * Wi-Fi is always brought up at boot (it serves the web UI); the Modbus
 * slave transport is independent of it.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "mbcontroller.h"
#include "mb_types.h"          /* mb_err_enum_t, mb_reg_mode_enum_t, exceptions */
#include "protocol_examples_common.h"

#include "modbus_slave.h"
#include "web_server.h"

static const char *TAG = "mb_slave";

#define NVS_NAMESPACE "mbcfg"
#define NVS_KEY       "cfg"

static const char *mode_str(slave_mode_t m) { return m == MODE_RTU ? "RTU" : "TCP"; }

/* ---- Holding register map (anti-backflow meter node) ------------------
 *
 * Holding registers (16-bit each). 32-bit values span two registers; their
 * word order follows CONFIG_MB_REG_WORD_SWAP (big-word-first by default).
 *
 *   [0,1] Active power P (W)       int32   READ   measured, signed
 *                                                  (P > 0 import, P < 0 export)
 *   [2,3] Export limit (W)         int32   R/W    anti-backflow threshold
 *                                                  (max allowed export, >= 0)
 *   [4]   Command / acknowledge     uint16  R/W    write 0xAC5A to clear a
 *                                                  latched fail-safe (when no
 *                                                  fault is active). Any access
 *                                                  also feeds the watchdog.
 *   [5]   Status flags             uint16  READ   bit0 fail-safe, bit1 reverse
 *
 * NOTE: power is exposed in watts as a signed integer. Apply any scaling on
 * the master side as agreed for your deployment.
 */
#define REG_P_HI        0
#define REG_P_LO        1
#define REG_LIMIT_HI    2
#define REG_LIMIT_LO    3
#define REG_HEARTBEAT   4
#define REG_STATUS      5
#define MB_HOLDING_CNT  6

#define STATUS_FAILSAFE (1u << 0)   /* fail-safe output asserted (trip)        */
#define STATUS_REVERSE  (1u << 1)   /* reverse power flow over the limit       */

#define MB_FAILSAFE_ACK_CMD  0xAC5A /* master writes this to [4] to clear latch */

static uint16_t s_holding_regs[MB_HOLDING_CNT];

/* Short critical sections protect the register array against torn reads of
 * multi-register (32-bit) values shared with the measurement/watchdog tasks. */
static portMUX_TYPE s_reg_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- State ------------------------------------------------------------ */

static void           *s_mb_handle = NULL;
static slave_cfg_t      s_cfg;
static SemaphoreHandle_t s_lock;
static volatile bool    s_net_up = false;
static int64_t          s_last_access_us = 0;    /* last master access (s_reg_mux) */
static bool             s_ever_polled = false;    /* master polled at least once    */

/* Slave response timeout for the TCP transport (ms). */
#define MB_TCP_RESPONSE_TOUT_MS  1000

/* ---- 32-bit register helpers (honor configured word order) ------------ */

static void reg_set_i32(int idx_hi, int32_t v)
{
    uint16_t hi = (uint16_t)((uint32_t)v >> 16);
    uint16_t lo = (uint16_t)((uint32_t)v & 0xFFFF);
#if CONFIG_MB_REG_WORD_SWAP
    s_holding_regs[idx_hi]     = lo;
    s_holding_regs[idx_hi + 1] = hi;
#else
    s_holding_regs[idx_hi]     = hi;
    s_holding_regs[idx_hi + 1] = lo;
#endif
}

static int32_t reg_get_i32(int idx_hi)
{
    uint16_t a = s_holding_regs[idx_hi];
    uint16_t b = s_holding_regs[idx_hi + 1];
#if CONFIG_MB_REG_WORD_SWAP
    return (int32_t)(((uint32_t)b << 16) | a);
#else
    return (int32_t)(((uint32_t)a << 16) | b);
#endif
}

/* Telemetry accessors (used by the web UI). */
int32_t modbus_power_w(void)
{
    int32_t v;
    taskENTER_CRITICAL(&s_reg_mux);
    v = reg_get_i32(REG_P_HI);
    taskEXIT_CRITICAL(&s_reg_mux);
    return v;
}

int32_t modbus_limit_w(void)
{
    int32_t v;
    taskENTER_CRITICAL(&s_reg_mux);
    v = reg_get_i32(REG_LIMIT_HI);
    taskEXIT_CRITICAL(&s_reg_mux);
    return v;
}

uint16_t modbus_status(void)
{
    uint16_t v;
    taskENTER_CRITICAL(&s_reg_mux);
    v = s_holding_regs[REG_STATUS];
    taskEXIT_CRITICAL(&s_reg_mux);
    return v;
}

bool modbus_net_is_up(void)
{
    return s_net_up;
}

/* ---- Config defaults / persistence ------------------------------------ */

static void cfg_defaults(slave_cfg_t *c)
{
    c->mode =
#if CONFIG_MB_SLAVE_INIT_TCP
        MODE_TCP;
#else
        MODE_RTU;
#endif
    c->slave_addr = CONFIG_MB_SLAVE_ADDR;
    c->uart_port  = CONFIG_MB_SLAVE_UART_PORT_NUM;
    c->baudrate   = CONFIG_MB_SLAVE_UART_BAUD;
    c->parity     = 0;   /* none */
    c->data_bits  = 8;
    c->stop_bits  = 1;
    c->tcp_port   = CONFIG_MB_SLAVE_TCP_PORT;
}

static void cfg_load(slave_cfg_t *c)
{
    cfg_defaults(c);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t size = sizeof(*c);
    slave_cfg_t stored;
    if (nvs_get_blob(h, NVS_KEY, &stored, &size) == ESP_OK && size == sizeof(*c)) {
        *c = stored;
        ESP_LOGI(TAG, "Loaded saved config from NVS");
    }
    nvs_close(h);
}

static void cfg_save(const slave_cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, config not persisted");
        return;
    }
    if (nvs_set_blob(h, NVS_KEY, c, sizeof(*c)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ---- Parameter mapping ------------------------------------------------ */

static uart_parity_t map_parity(uint8_t p)
{
    return p == 1 ? UART_PARITY_ODD : p == 2 ? UART_PARITY_EVEN : UART_PARITY_DISABLE;
}

static uart_word_length_t map_databits(uint8_t d)
{
    return d == 7 ? UART_DATA_7_BITS : UART_DATA_8_BITS;
}

static uart_stop_bits_t map_stopbits(uint8_t s)
{
    return s == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

/* ---- Modbus controller (descriptors / create / start / stop) ---------- */

static void set_descriptor(mb_param_type_t type, uint16_t offset,
                           void *address, size_t size)
{
    mb_register_area_descriptor_t area = {
        .type = type,
        .start_offset = offset,
        .address = address,
        .size = size,
        .access = MB_ACCESS_RW,
    };
    ESP_ERROR_CHECK(mbc_slave_set_descriptor(s_mb_handle, area));
}

static void register_descriptors(void)
{
    set_descriptor(MB_PARAM_HOLDING, 0, (void *)s_holding_regs, sizeof(s_holding_regs));
}

static void seed_values(void)
{
    taskENTER_CRITICAL(&s_reg_mux);
    reg_set_i32(REG_P_HI, 0);                       /* measured power = 0 W   */
    reg_set_i32(REG_LIMIT_HI, 0);                   /* strict: 0 W export     */
    s_holding_regs[REG_HEARTBEAT] = 0;
    s_holding_regs[REG_STATUS]    = 0;
    taskEXIT_CRITICAL(&s_reg_mux);
}

/* ---- Custom holding-register handling ---------------------------------
 *
 * Two complementary customization mechanisms are used:
 *
 * 1) WEAK callback override (mbc_reg_holding_slave_cb): esp-modbus declares
 *    the default register callback as a weak symbol, so the strong definition
 *    below replaces it at link time. It performs the actual memory transfer
 *    and a bounds check (-> 0x02 Illegal Data Address). Note that a register
 *    callback can only return mb_err_enum_t, which esp-modbus v2.1.2 maps to
 *    a limited set of exceptions (no 0x03 Illegal Data Value).
 *
 * 2) Function-code handler override (mbc_set_handler, a public API): for the
 *    write function codes 0x06 and 0x10 we install wrapper handlers that
 *    return mb_exception_t DIRECTLY, so they can emit ANY exception code,
 *    including 0x03 Illegal Data Value. They validate the request, and on
 *    success delegate to the saved default handler (which performs the write
 *    through the callback above).
 */

#define MB_FC_WRITE_SINGLE_HOLDING  0x06
#define MB_FC_WRITE_MULTI_HOLDING   0x10

/* PDU layout (Modbus standard): function code at offset 0, data at offset 1. */
#define MB_PDU_DATA_OFF             1

/* Metering write policy: measurement and status registers are read-only to
 * the master; only the export-limit setpoint and the heartbeat are writable.
 * Single source of truth for both write handlers. */
static mb_exception_t validate_holding_write(uint16_t idx, uint16_t val)
{
    (void)val;
    if (idx >= MB_HOLDING_CNT) {
        return MB_EX_ILLEGAL_DATA_ADDRESS;        /* 0x02 out of range        */
    }
    switch (idx) {
    case REG_P_HI:
    case REG_P_LO:
    case REG_STATUS:
        return MB_EX_ILLEGAL_DATA_ADDRESS;        /* 0x02 read-only to master  */
    case REG_LIMIT_HI:
    case REG_LIMIT_LO:
    case REG_HEARTBEAT:
        return MB_EX_NONE;                        /* writable                  */
    default:
        return MB_EX_ILLEGAL_DATA_ADDRESS;
    }
}

/* Saved default write handlers, delegated to after validation passes. */
static mb_fn_handler_fp s_def_write_single = NULL;
static mb_fn_handler_fp s_def_write_multi  = NULL;

/* Handler for 0x06 Write Single Holding Register. */
static mb_exception_t fn_write_single_holding(void *inst, uint8_t *frame, uint16_t *len)
{
    uint16_t addr = ((uint16_t)frame[MB_PDU_DATA_OFF] << 8) | frame[MB_PDU_DATA_OFF + 1];
    uint16_t val  = ((uint16_t)frame[MB_PDU_DATA_OFF + 2] << 8) | frame[MB_PDU_DATA_OFF + 3];

    mb_exception_t ex = validate_holding_write(addr, val);
    if (ex != MB_EX_NONE) {
        ESP_LOGW(TAG, "0x06 reject reg %u val %u -> exception 0x%02x", addr, val, ex);
        return ex;
    }
    return s_def_write_single ? s_def_write_single(inst, frame, len) : MB_EX_SLAVE_DEVICE_FAILURE;
}

/* Handler for 0x10 Write Multiple Holding Registers. */
static mb_exception_t fn_write_multi_holding(void *inst, uint8_t *frame, uint16_t *len)
{
    uint16_t addr = ((uint16_t)frame[MB_PDU_DATA_OFF] << 8) | frame[MB_PDU_DATA_OFF + 1];
    uint16_t cnt  = ((uint16_t)frame[MB_PDU_DATA_OFF + 2] << 8) | frame[MB_PDU_DATA_OFF + 3];
    const uint8_t *values = &frame[MB_PDU_DATA_OFF + 5];   /* after byte-count field */

    /* Address-range check first so the value loop stays bounded. */
    if (addr >= MB_HOLDING_CNT || (uint32_t)addr + cnt > MB_HOLDING_CNT) {
        ESP_LOGW(TAG, "0x10 reject range [%u..%u) -> 0x02", addr, addr + cnt);
        return MB_EX_ILLEGAL_DATA_ADDRESS;
    }
    for (uint16_t i = 0; i < cnt; i++) {
        uint16_t val = ((uint16_t)values[i * 2] << 8) | values[i * 2 + 1];
        mb_exception_t ex = validate_holding_write((uint16_t)(addr + i), val);
        if (ex != MB_EX_NONE) {
            ESP_LOGW(TAG, "0x10 reject reg %u val %u -> exception 0x%02x", addr + i, val, ex);
            return ex;
        }
    }
    return s_def_write_multi ? s_def_write_multi(inst, frame, len) : MB_EX_SLAVE_DEVICE_FAILURE;
}

/* Install the custom write handlers on the current controller. Must run after
 * the controller is (re)created, i.e. on every start / hot-switch. */
static void install_custom_handlers(void)
{
    mbc_get_handler(s_mb_handle, MB_FC_WRITE_SINGLE_HOLDING, &s_def_write_single);
    mbc_get_handler(s_mb_handle, MB_FC_WRITE_MULTI_HOLDING,  &s_def_write_multi);
    ESP_ERROR_CHECK(mbc_set_handler(s_mb_handle, MB_FC_WRITE_SINGLE_HOLDING, fn_write_single_holding));
    ESP_ERROR_CHECK(mbc_set_handler(s_mb_handle, MB_FC_WRITE_MULTI_HOLDING,  fn_write_multi_holding));
}

mb_err_enum_t mbc_reg_holding_slave_cb(mb_base_t *inst, uint8_t *reg_buffer,
                                       uint16_t address, uint16_t n_regs,
                                       mb_reg_mode_enum_t mode)
{
    (void)inst;
    if (reg_buffer == NULL) {
        return MB_EINVAL;
    }

    /* The stack passes a 1-based address; convert to a 0-based index. */
    uint16_t base = (uint16_t)(address - 1);

    /* Bounds check against our holding area -> 0x02 Illegal Data Address. */
    if ((uint32_t)base + n_regs > MB_HOLDING_CNT) {
        ESP_LOGW(TAG, "Holding out of range: [%u..%u)", base, base + n_regs);
        return MB_ENOREG;
    }

    /* Record master activity for the communication watchdog. */
    int64_t access_now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_reg_mux);
    s_last_access_us = access_now;
    s_ever_polled = true;
    taskEXIT_CRITICAL(&s_reg_mux);

    if (mode == MB_REG_WRITE) {
        /* Value/permission policy is enforced earlier by the 0x06/0x10
         * handlers; here we just commit (big-endian frame -> host storage). */
        taskENTER_CRITICAL(&s_reg_mux);
        for (uint16_t i = 0; i < n_regs; i++) {
            s_holding_regs[base + i] =
                ((uint16_t)reg_buffer[i * 2] << 8) | reg_buffer[i * 2 + 1];
        }
        taskEXIT_CRITICAL(&s_reg_mux);
        ESP_LOGI(TAG, "Holding WR ok: base=%u n=%u", base, n_regs);
    } else { /* MB_REG_READ */
        taskENTER_CRITICAL(&s_reg_mux);
        for (uint16_t i = 0; i < n_regs; i++) {
            uint16_t val = s_holding_regs[base + i];
            reg_buffer[i * 2]     = (uint8_t)(val >> 8);
            reg_buffer[i * 2 + 1] = (uint8_t)(val & 0xFF);
        }
        taskEXIT_CRITICAL(&s_reg_mux);
        ESP_LOGI(TAG, "Holding RD: base=%u n=%u", base, n_regs);
    }
    return MB_ENOERR;
}

static void slave_create(const slave_cfg_t *c)
{
    if (c->mode == MODE_RTU) {
        mb_communication_info_t info = {
            .ser_opts = {
                .port      = c->uart_port,
                .mode      = MB_RTU,
                .baudrate  = c->baudrate,
                .parity    = map_parity(c->parity),
                .uid       = c->slave_addr,
                .data_bits = map_databits(c->data_bits),
                .stop_bits = map_stopbits(c->stop_bits),
            },
        };
        ESP_ERROR_CHECK(mbc_slave_create_serial(&info, &s_mb_handle));
    } else {
        mb_communication_info_t info = {
            .tcp_opts = {
                .mode             = MB_TCP,
                .port             = c->tcp_port,
                .addr_type        = MB_IPV4,
                .ip_addr_table    = NULL,
                .ip_netif_ptr     = (void *)get_example_netif(),
                .uid              = c->slave_addr,
                .response_tout_ms = MB_TCP_RESPONSE_TOUT_MS,
            },
        };
        ESP_ERROR_CHECK(mbc_slave_create_tcp(&info, &s_mb_handle));
    }
}

/* Caller must hold s_lock. */
static void slave_start_locked(void)
{
    slave_create(&s_cfg);
    register_descriptors();
    install_custom_handlers();
    ESP_ERROR_CHECK(mbc_slave_start(s_mb_handle));
    ESP_LOGI(TAG, "Slave running: %s addr=%d", mode_str(s_cfg.mode), s_cfg.slave_addr);
}

/* Caller must hold s_lock. */
static void slave_stop_locked(void)
{
    if (s_mb_handle) {
        ESP_ERROR_CHECK(mbc_slave_delete(s_mb_handle));
        s_mb_handle = NULL;
    }
}

/* ---- Public API (used by the web server) ------------------------------ */

void modbus_get_config(slave_cfg_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t modbus_apply_config(const slave_cfg_t *cfg)
{
    /* Light validation / clamping. */
    slave_cfg_t c = *cfg;
    if (c.slave_addr < 1 || c.slave_addr > 247) c.slave_addr = 1;
    if (c.tcp_port == 0) c.tcp_port = 502;
    if (c.mode != MODE_TCP) c.mode = MODE_RTU;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    ESP_LOGW(TAG, "Applying config -> %s (hot switch)", mode_str(c.mode));
    slave_stop_locked();
    s_cfg = c;
    slave_start_locked();
    cfg_save(&s_cfg);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* ---- Boot ------------------------------------------------------------- */

/* ---- Measurement + anti-backflow watchdog / fail-safe ----------------- */

/* Read the meter's active power in watts (P > 0 import, P < 0 export).
 *
 * DEMO: a slow oscillation that crosses zero so reverse flow is observable.
 * REPLACE this with a real reading from your meter (e.g. a metering chip,
 * an analog front-end, or another Modbus device). */
static int32_t read_meter_active_power(void)
{
    int64_t t_ms = esp_timer_get_time() / 1000;
    /* +/-2000 W triangle-ish wave, ~20 s period. */
    int32_t phase = (int32_t)(t_ms % 20000) - 10000;     /* -10000..+10000 */
    return phase / 5;                                    /* -2000..+2000 W */
}

/* Drive the fail-safe output (e.g. relay / inverter enable). Asserted = trip
 * to the safe state (curtail / disable export). */
static void failsafe_output(bool trip)
{
#if CONFIG_MB_FAILSAFE_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_MB_FAILSAFE_GPIO, trip ? 1 : 0);
#else
    (void)trip;
#endif
}

static void failsafe_gpio_init(void)
{
#if CONFIG_MB_FAILSAFE_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_MB_FAILSAFE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    failsafe_output(true);   /* default to the SAFE state until proven OK */
    ESP_LOGI(TAG, "Fail-safe output on GPIO%d (asserted at boot)", CONFIG_MB_FAILSAFE_GPIO);
#endif
}

/* Periodic control loop: refresh measured power, evaluate reverse flow and
 * comms health, and drive the fail-safe output. Runs regardless of transport. */
static void control_task(void *arg)
{
    (void)arg;
    const int64_t wd_us  = (int64_t)CONFIG_MB_WATCHDOG_TIMEOUT_MS * 1000;
    const int32_t hyst   = CONFIG_MB_REVERSE_HYSTERESIS_W;
    bool reverse = false;      /* hysteresis state                          */
    bool latched = false;      /* fail-safe latch state                     */
    bool prev_trip = true;     /* boot state is SAFE (asserted)             */

    while (1) {
        int32_t p = read_meter_active_power();

        /* Snapshot shared registers / supervision under the lock. */
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_reg_mux);
        reg_set_i32(REG_P_HI, p);
        int32_t limit = reg_get_i32(REG_LIMIT_HI);
        int64_t last  = s_last_access_us;
        bool ever     = s_ever_polled;
        uint16_t cmd  = s_holding_regs[REG_HEARTBEAT];
        taskEXIT_CRITICAL(&s_reg_mux);
        if (limit < 0) {
            limit = 0;
        }

        /* Reverse flow with hysteresis: trip at >limit, clear below limit-hyst. */
        if (!reverse) {
            reverse = (p < 0) && (-p > limit);
        } else if (p >= 0 || -p <= (limit - hyst)) {
            reverse = false;
        }

        /* Comms lost if offline, never polled yet, or polled too long ago.
         * (No supervision == not safe, so untrip only after the first poll.) */
        bool comms_lost = !s_net_up || !ever || ((now - last) > wd_us);

        bool fault = reverse || comms_lost;
        bool trip;
#if CONFIG_MB_FAILSAFE_LATCH
        if (fault) {
            latched = true;
        } else if (cmd == MB_FAILSAFE_ACK_CMD) {
            latched = false;            /* master acknowledged, no active fault */
        }
        trip = latched;
#else
        (void)latched; (void)cmd;
        trip = fault;
#endif

        uint16_t st = 0;
        if (trip)    st |= STATUS_FAILSAFE;
        if (reverse) st |= STATUS_REVERSE;

        taskENTER_CRITICAL(&s_reg_mux);
        s_holding_regs[REG_STATUS] = st;
#if CONFIG_MB_FAILSAFE_LATCH
        if (cmd == MB_FAILSAFE_ACK_CMD) {
            s_holding_regs[REG_HEARTBEAT] = 0;   /* consume the ack command */
        }
#endif
        taskEXIT_CRITICAL(&s_reg_mux);

        failsafe_output(trip);

        if (trip != prev_trip) {
            ESP_LOGW(TAG, "Fail-safe %s (P=%d W, limit=%d W, reverse=%d, comms_lost=%d)",
                     trip ? "TRIP" : "clear", p, limit, reverse, comms_lost);
            prev_trip = trip;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* Track link state so the TCP transport health is visible (web UI / logs).
 * example_connect() already drives Wi-Fi auto-reconnect; we only observe it. */
static void net_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)) {
        s_net_up = true;
        ESP_LOGI(TAG, "Network up (got IP)");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_net_up = false;
        ESP_LOGW(TAG, "Network down (Wi-Fi disconnected), reconnecting...");
    }
}

static void network_connect(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Observe link state (registered before connecting so we catch events). */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               net_event_handler, NULL));

    /* Wi-Fi / Ethernet via "Example Connection Configuration".
     * Blocks until connected; auto-reconnects forever afterwards. */
    ESP_ERROR_CHECK(example_connect());
    s_net_up = true;
}

void app_main(void)
{
    s_lock = xSemaphoreCreateMutex();

    failsafe_gpio_init();       /* assert safe state before anything else  */

    network_connect();          /* Wi-Fi up first (web UI + optional TCP) */

    seed_values();
    cfg_load(&s_cfg);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    slave_start_locked();
    xSemaphoreGive(s_lock);

    /* Anti-backflow control / fail-safe loop. */
    xTaskCreate(control_task, "mb_control", 3072, NULL, 6, NULL);

    web_server_start();         /* serves UI + /api/status + /api/config  */

    ESP_LOGI(TAG, "Ready. Open the device IP in a browser to configure.");
}
