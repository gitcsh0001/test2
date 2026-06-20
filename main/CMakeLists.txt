idf_component_register(SRCS "modbus_slave.c" "web_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_gpio esp-modbus esp_netif
                                esp_event esp_wifi esp_timer nvs_flash
                                esp_http_server protocol_examples_common)
