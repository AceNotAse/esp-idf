/*
 * Home Assistant rotary knob - Elecrow CrowPanel 1.28" ESP32-S3 Rotary Display
 *
 * Start-up order:
 *   1. NVS (Wi-Fi driver stores calibration data there)
 *   2. Board hardware: power pins, LCD, touch, LED ring, LVGL
 *   3. Knob (rotary encoder + button)
 *   4. UI, then backlight on
 *   5. Wi-Fi + Home Assistant client (runs in the background, the UI shows its progress)
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "board.h"
#include "encoder.h"
#include "ha_client.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board_init());
    QueueHandle_t knob_events = encoder_start();

    board_lvgl_lock();
    ui_start(knob_events);
    board_lvgl_unlock();
    board_backlight_set(100);

    ha_client_start();
    ESP_LOGI(TAG, "up and running");
}
