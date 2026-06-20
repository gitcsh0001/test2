idf_component_register(SRCS "modbus_slave.c" "web_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp-modbus esp_netif esp_event esp_wifi
                                nvs_flash esp_http_server
                                protocol_examples_common)
