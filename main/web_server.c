/*
 * Built-in web UI for the Modbus slave.
 *
 *   GET  /             -> single-page configuration UI
 *   GET  /api/status   -> current config as JSON
 *   POST /api/config   -> apply new config (url-encoded form), hot-switch
 *
 * The page lets the user pick RTU or TCP and edit every RTU/TCP parameter,
 * then applies it live via modbus_apply_config().
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"

#include "modbus_slave.h"
#include "web_server.h"

static const char *TAG = "web";

/* Single-page UI. HTML attributes use single quotes so the whole document
 * can live in a C string literal without escaping. */
static const char HTML_PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 Modbus Slave</title><style>"
"body{font-family:sans-serif;max-width:480px;margin:18px auto;padding:0 12px}"
"label{display:block;margin:8px 0 2px}fieldset{margin:12px 0}"
"input,select{width:100%;padding:6px;box-sizing:border-box}"
"button{margin-top:14px;padding:10px;width:100%;font-size:16px}"
"#cur{font-weight:bold}#msg{margin-top:10px;color:#0a0}"
"</style></head><body>"
"<h2>ESP32 Modbus Slave</h2>"
"<div>current: <span id='cur'>...</span></div>"
"<form id='f'>"
"<label>Mode</label>"
"<select name='mode' id='mode'><option value='rtu'>RTU (serial)</option>"
"<option value='tcp'>TCP (network)</option></select>"
"<label>Slave address (1-247)</label>"
"<input name='addr' id='addr' type='number' min='1' max='247'>"
"<fieldset id='rtu'><legend>RTU parameters</legend>"
"<label>UART port</label><input name='uart' id='uart' type='number' min='0' max='2'>"
"<label>Baud rate</label><input name='baud' id='baud' type='number'>"
"<label>Parity</label><select name='parity' id='parity'>"
"<option value='0'>None</option><option value='1'>Odd</option>"
"<option value='2'>Even</option></select>"
"<label>Data bits</label><select name='databits' id='databits'>"
"<option>8</option><option>7</option></select>"
"<label>Stop bits</label><select name='stopbits' id='stopbits'>"
"<option>1</option><option>2</option></select></fieldset>"
"<fieldset id='tcp'><legend>TCP parameters</legend>"
"<label>Listen port</label>"
"<input name='tcpport' id='tcpport' type='number' min='1' max='65535'></fieldset>"
"<button type='submit'>Apply &amp; hot-switch</button></form>"
"<div id='msg'></div>"
"<script>"
"function tg(){rtu.style.display=mode.value=='rtu'?'':'none';"
"tcp.style.display=mode.value=='tcp'?'':'none';}"
"function load(){fetch('/api/status').then(r=>r.json()).then(s=>{"
"cur.textContent=s.mode.toUpperCase()+' addr='+s.addr;"
"mode.value=s.mode;addr.value=s.addr;uart.value=s.uart;baud.value=s.baud;"
"parity.value=s.parity;databits.value=s.databits;stopbits.value=s.stopbits;"
"tcpport.value=s.tcpport;tg();});}"
"mode.addEventListener('change',tg);"
"f.addEventListener('submit',e=>{e.preventDefault();"
"var b=new URLSearchParams(new FormData(f)).toString();"
"msg.textContent='applying...';"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':"
"'application/x-www-form-urlencoded'},body:b}).then(r=>r.text())"
".then(t=>{msg.textContent=t;setTimeout(load,600);});});"
"load();"
"</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    slave_cfg_t c;
    modbus_get_config(&c);

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"mode\":\"%s\",\"addr\":%u,\"uart\":%u,\"baud\":%u,"
        "\"parity\":%u,\"databits\":%u,\"stopbits\":%u,\"tcpport\":%u}",
        c.mode == MODE_RTU ? "rtu" : "tcp",
        c.slave_addr, c.uart_port, (unsigned)c.baudrate,
        c.parity, c.data_bits, c.stop_bits, c.tcp_port);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t config_post(httpd_req_t *req)
{
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) {
        return ESP_FAIL;
    }
    body[r] = '\0';

    slave_cfg_t c;
    modbus_get_config(&c);   /* start from current, override provided keys */

    char v[16];
    if (httpd_query_key_value(body, "mode", v, sizeof(v)) == ESP_OK)
        c.mode = strcmp(v, "tcp") == 0 ? MODE_TCP : MODE_RTU;
    if (httpd_query_key_value(body, "addr", v, sizeof(v)) == ESP_OK)
        c.slave_addr = atoi(v);
    if (httpd_query_key_value(body, "uart", v, sizeof(v)) == ESP_OK)
        c.uart_port = atoi(v);
    if (httpd_query_key_value(body, "baud", v, sizeof(v)) == ESP_OK)
        c.baudrate = strtoul(v, NULL, 10);
    if (httpd_query_key_value(body, "parity", v, sizeof(v)) == ESP_OK)
        c.parity = atoi(v);
    if (httpd_query_key_value(body, "databits", v, sizeof(v)) == ESP_OK)
        c.data_bits = atoi(v);
    if (httpd_query_key_value(body, "stopbits", v, sizeof(v)) == ESP_OK)
        c.stop_bits = atoi(v);
    if (httpd_query_key_value(body, "tcpport", v, sizeof(v)) == ESP_OK)
        c.tcp_port = atoi(v);

    ESP_LOGI(TAG, "Config request: mode=%s addr=%u",
             c.mode == MODE_RTU ? "rtu" : "tcp", c.slave_addr);

    modbus_apply_config(&c);

    char msg[64];
    int n = snprintf(msg, sizeof(msg), "Applied: %s, addr=%u",
                     c.mode == MODE_RTU ? "RTU" : "TCP", c.slave_addr);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, n);
}

void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root   = { .uri = "/",           .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t config_uri = { .uri = "/api/config", .method = HTTP_POST, .handler = config_post };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &config_uri);

    ESP_LOGI(TAG, "Web UI started on port 80");
}
