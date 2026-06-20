#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODE_RTU = 0,
    MODE_TCP,
} slave_mode_t;

/* Runtime-configurable slave settings (editable from the web UI). */
typedef struct {
    slave_mode_t mode;       /* MODE_RTU or MODE_TCP                    */
    uint8_t  slave_addr;     /* Modbus unit id, 1..247                 */
    /* RTU parameters */
    uint8_t  uart_port;      /* UART port number                       */
    uint32_t baudrate;       /* e.g. 9600, 115200                      */
    uint8_t  parity;         /* 0 = none, 1 = odd, 2 = even            */
    uint8_t  data_bits;      /* 7 or 8                                 */
    uint8_t  stop_bits;      /* 1 or 2                                 */
    /* TCP parameters */
    uint16_t tcp_port;       /* listen port, e.g. 502                  */
} slave_cfg_t;

/* Copy the current configuration into *out (thread-safe). */
void modbus_get_config(slave_cfg_t *out);

/* Atomically stop the running slave, adopt *cfg, restart on the selected
 * transport and persist to NVS. Safe to call from the HTTP task. */
esp_err_t modbus_apply_config(const slave_cfg_t *cfg);

/* True while the device has network connectivity (has an IP address).
 * Reflects Wi-Fi link state for the TCP transport and web UI. */
bool modbus_net_is_up(void);

/* Anti-backflow telemetry (thread-safe snapshots). */
int32_t  modbus_power_w(void);   /* measured active power, W (>0 import, <0 export) */
int32_t  modbus_limit_w(void);   /* configured export limit, W                     */
uint16_t modbus_status(void);    /* status flags: bit0 fail-safe, bit1 reverse     */

#ifdef __cplusplus
}
#endif
