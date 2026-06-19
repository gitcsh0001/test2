/*
 * ESP32 LED Blink example (ESP-IDF)
 *
 * Toggles an LED connected to a configurable GPIO at a configurable
 * interval. The GPIO number and blink period can be changed via
 * `idf.py menuconfig` -> "Blink Configuration".
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "blink";

#define BLINK_GPIO       CONFIG_BLINK_GPIO
#define BLINK_PERIOD_MS  CONFIG_BLINK_PERIOD_MS

static uint8_t s_led_state = 0;

static void configure_led(void)
{
    ESP_LOGI(TAG, "Configuring LED on GPIO%d", BLINK_GPIO);
    gpio_reset_pin(BLINK_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
}

void app_main(void)
{
    configure_led();

    ESP_LOGI(TAG, "Starting blink loop (period %d ms)", BLINK_PERIOD_MS);

    while (1) {
        s_led_state = !s_led_state;
        ESP_LOGI(TAG, "Turning the LED %s", s_led_state ? "ON" : "OFF");
        gpio_set_level(BLINK_GPIO, s_led_state);
        vTaskDelay(BLINK_PERIOD_MS / portTICK_PERIOD_MS);
    }
}
