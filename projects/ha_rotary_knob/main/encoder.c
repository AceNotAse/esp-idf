/*
 * encoder.c - rotary knob + push button.
 *
 * Rotation is counted in hardware by the PCNT (pulse counter) peripheral, so no clicks are lost
 * even when the CPU is busy drawing. A small task samples the count and the button every 10 ms
 * and turns them into knob_evt_t events.
 */
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "encoder.h"

static const char *TAG = "encoder";

#define SAMPLE_PERIOD_MS   10
#define DEBOUNCE_MS        30
#define PCNT_LIMIT         10000

static pcnt_unit_handle_t s_pcnt;

static void encoder_pcnt_init(void)
{
    pcnt_unit_config_t unit_cfg = {
        .high_limit = PCNT_LIMIT,
        .low_limit = -PCNT_LIMIT,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_pcnt));

    /* Ignore contact bounce shorter than 1 us */
    pcnt_glitch_filter_config_t filter_cfg = {
        .max_glitch_ns = 1000,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &filter_cfg));

    /* Standard quadrature decoding: two channels, each watches one pin's edges and the
     * other pin's level to decide the direction. Same setup as the IDF rotary_encoder example. */
    pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num = CONFIG_KNOB_PIN_ENC_A,
        .level_gpio_num = CONFIG_KNOB_PIN_ENC_B,
    };
    pcnt_channel_handle_t chan_a = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &chan_a_cfg, &chan_a));
    pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num = CONFIG_KNOB_PIN_ENC_B,
        .level_gpio_num = CONFIG_KNOB_PIN_ENC_A,
    };
    pcnt_channel_handle_t chan_b = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &chan_b_cfg, &chan_b));

    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    /* The encoder pins float without pull-ups */
    gpio_pullup_en(CONFIG_KNOB_PIN_ENC_A);
    gpio_pullup_en(CONFIG_KNOB_PIN_ENC_B);

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));
}

static void encoder_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_KNOB_PIN_ENC_SW,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  /* button pulls the pin to ground when pressed */
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void encoder_task(void *arg)
{
    QueueHandle_t queue = arg;
    int last_count = 0;
    int raw_accum = 0;          /* counts not yet turned into whole clicks */

    bool btn_stable = false;    /* debounced button state, true = pressed */
    bool btn_last_raw = false;
    int64_t btn_raw_since = 0;
    int64_t btn_down_at = 0;
    bool long_sent = false;

    while (1) {
        /* ---- rotation ---- */
        int count = 0;
        pcnt_unit_get_count(s_pcnt, &count);
        raw_accum += count - last_count;
        last_count = count;
        if (abs(count) > PCNT_LIMIT / 2) {
            /* Recentre well before the hardware limit so the counter never wraps */
            pcnt_unit_clear_count(s_pcnt);
            last_count = 0;
        }

        int steps = raw_accum / CONFIG_KNOB_ENCODER_COUNTS_PER_STEP;
        if (steps != 0) {
            raw_accum -= steps * CONFIG_KNOB_ENCODER_COUNTS_PER_STEP;
#if CONFIG_KNOB_ENCODER_REVERSE
            steps = -steps;
#endif
            knob_evt_t evt = { .type = KNOB_EVT_ROTATE, .steps = steps };
            xQueueSend(queue, &evt, 0);
        }

        /* ---- button ---- */
        int64_t now = esp_timer_get_time() / 1000;
        bool raw = gpio_get_level(CONFIG_KNOB_PIN_ENC_SW) == 0;
        if (raw != btn_last_raw) {
            btn_last_raw = raw;
            btn_raw_since = now;
        }
        if (raw != btn_stable && now - btn_raw_since >= DEBOUNCE_MS) {
            btn_stable = raw;
            if (btn_stable) {
                btn_down_at = now;
                long_sent = false;
            } else if (!long_sent) {
                knob_evt_t evt = { .type = KNOB_EVT_CLICK };
                xQueueSend(queue, &evt, 0);
            }
        }
        if (btn_stable && !long_sent && now - btn_down_at >= CONFIG_KNOB_LONG_PRESS_MS) {
            long_sent = true;
            knob_evt_t evt = { .type = KNOB_EVT_LONG_PRESS };
            xQueueSend(queue, &evt, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

QueueHandle_t encoder_start(void)
{
    QueueHandle_t queue = xQueueCreate(16, sizeof(knob_evt_t));
    encoder_pcnt_init();
    encoder_button_init();
    xTaskCreate(encoder_task, "encoder", 3072, queue, 5, NULL);
    ESP_LOGI(TAG, "knob ready (A=%d B=%d SW=%d)", CONFIG_KNOB_PIN_ENC_A, CONFIG_KNOB_PIN_ENC_B, CONFIG_KNOB_PIN_ENC_SW);
    return queue;
}
