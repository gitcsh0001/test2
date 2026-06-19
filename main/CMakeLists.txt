idf_component_register(SRCS "modbus_slave.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp-modbus esp_netif esp_event
                                nvs_flash protocol_examples_common)
