/*
 * ESP32 WS2812 RGB LED Blink example (ESP-IDF)
 *
 * Drives a WS2812 (NeoPixel) addressable LED using the official
 * `led_strip` component with the RMT backend. The LED blinks on/off,
 * and each "on" phase shows the next color in a small palette so the
 * RGB capability is visible.
 *
 * The GPIO number, blink period and number of LEDs can be changed via
 * `idf.py menuconfig` -> "Blink Configuration".
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "blink";

#define BLINK_GPIO       CONFIG_BLINK_GPIO
#define BLINK_PERIOD_MS  CONFIG_BLINK_PERIOD_MS
#define BLINK_LED_COUNT  CONFIG_BLINK_LED_COUNT

static led_strip_handle_t s_led_strip;
static uint8_t s_led_state = 0;

/* A small RGB palette to cycle through on each "on" phase. */
static const uint8_t s_palette[][3] = {
    {32,  0,  0},   /* red   */
    { 0, 32,  0},   /* green */
    { 0,  0, 32},   /* blue  */
    {32, 32,  0},   /* yellow */
    { 0, 32, 32},   /* cyan  */
    {32,  0, 32},   /* magenta */
};
#define PALETTE_LEN (sizeof(s_palette) / sizeof(s_palette[0]))

static void configure_led(void)
{
    ESP_LOGI(TAG, "Configuring WS2812 strip on GPIO%d (%d LED(s))",
             BLINK_GPIO, BLINK_LED_COUNT);

    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = BLINK_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags = {
            .with_dma = false,
        },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip));
    led_strip_clear(s_led_strip);
}

void app_main(void)
{
    size_t color_idx = 0;

    configure_led();

    ESP_LOGI(TAG, "Starting blink loop (period %d ms)", BLINK_PERIOD_MS);

    while (1) {
        s_led_state = !s_led_state;

        if (s_led_state) {
            const uint8_t *c = s_palette[color_idx];
            ESP_LOGI(TAG, "LED ON  (R=%d G=%d B=%d)", c[0], c[1], c[2]);
            for (int i = 0; i < BLINK_LED_COUNT; i++) {
                led_strip_set_pixel(s_led_strip, i, c[0], c[1], c[2]);
            }
            led_strip_refresh(s_led_strip);
            color_idx = (color_idx + 1) % PALETTE_LEN;
        } else {
            ESP_LOGI(TAG, "LED OFF");
            led_strip_clear(s_led_strip);
        }

        vTaskDelay(BLINK_PERIOD_MS / portTICK_PERIOD_MS);
    }
}
