/*
 * board.c - hardware bring-up for the Elecrow CrowPanel 1.28" ESP32-S3 Rotary Display.
 *
 *   GC9A01 240x240 round LCD  -> SPI2 (SCLK 10, MOSI 11, CS 9, DC 3, RST 14), backlight on 46
 *   CST816D capacitive touch  -> I2C  (SDA 6, SCL 7, INT 5, RST 13), address 0x15
 *   5x WS2812 LED ring        -> GPIO 48 (driven by the RMT peripheral)
 *   GPIO 1 / GPIO 2           -> must be driven high to power the screen and LEDs
 *
 * All pins can be changed in menuconfig ("HA Rotary Knob" -> "Board pins").
 */
#include <stdbool.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "board.h"

/* Kconfig leaves "n" booleans undefined, so give them a 0 default */
#ifndef CONFIG_KNOB_LCD_MIRROR_X
#define CONFIG_KNOB_LCD_MIRROR_X 0
#endif
#ifndef CONFIG_KNOB_LCD_MIRROR_Y
#define CONFIG_KNOB_LCD_MIRROR_Y 0
#endif
#ifndef CONFIG_KNOB_TP_MIRROR_X
#define CONFIG_KNOB_TP_MIRROR_X 0
#endif
#ifndef CONFIG_KNOB_TP_MIRROR_Y
#define CONFIG_KNOB_TP_MIRROR_Y 0
#endif

static const char *TAG = "board";

#define LCD_HOST              SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ    (40 * 1000 * 1000)
#define LCD_DRAW_BUF_LINES    40      /* LVGL renders 40 lines at a time, double buffered */

#define BL_LEDC_TIMER         LEDC_TIMER_0
#define BL_LEDC_CHANNEL       LEDC_CHANNEL_0

#define LVGL_TICK_PERIOD_MS   2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_TASK_STACK_SIZE  (8 * 1024)
#define LVGL_TASK_PRIORITY    2

static _lock_t s_lvgl_lock;
static led_strip_handle_t s_leds;
static esp_lcd_touch_handle_t s_touch;
static volatile bool s_touch_irq;  /* set from the touch interrupt, cleared when we read the chip */

/* ---------------------------------------------------------------------------------------------
 * Power and backlight
 * ------------------------------------------------------------------------------------------- */

static void board_power_on(void)
{
    const int high_pins[] = { CONFIG_KNOB_PIN_PWR_1, CONFIG_KNOB_PIN_PWR_2 };
    for (size_t i = 0; i < sizeof(high_pins) / sizeof(high_pins[0]); i++) {
        if (high_pins[i] >= 0) {
            gpio_reset_pin(high_pins[i]);
            gpio_set_direction(high_pins[i], GPIO_MODE_OUTPUT);
            gpio_set_level(high_pins[i], 1);
        }
    }
    if (CONFIG_KNOB_PIN_PWR_LED >= 0) {
        gpio_reset_pin(CONFIG_KNOB_PIN_PWR_LED);
        gpio_set_direction(CONFIG_KNOB_PIN_PWR_LED, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_KNOB_PIN_PWR_LED, 0);
    }
    /* Give the screen's power rail a moment to settle before talking to it */
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t board_backlight_init(void)
{
    /* PWM on the backlight pin lets us dim the screen instead of just on/off */
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "backlight timer");
    ledc_channel_config_t chan_cfg = {
        .gpio_num = CONFIG_KNOB_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,  /* start dark so the user doesn't see garbage while the LCD initialises */
    };
    return ledc_channel_config(&chan_cfg);
}

void board_backlight_set(uint8_t percent)
{
    uint32_t duty = (MIN(percent, 100) * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

/* ---------------------------------------------------------------------------------------------
 * LED ring
 * ------------------------------------------------------------------------------------------- */

static esp_err_t board_leds_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_KNOB_PIN_LED,
        .max_leds = CONFIG_KNOB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_leds), TAG, "LED ring");
    return led_strip_clear(s_leds);
}

void board_leds_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_leds) {
        return;
    }
    for (int i = 0; i < CONFIG_KNOB_LED_COUNT; i++) {
        led_strip_set_pixel(s_leds, i, r, g, b);
    }
    led_strip_refresh(s_leds);
}

/* ---------------------------------------------------------------------------------------------
 * LCD
 * ------------------------------------------------------------------------------------------- */

static bool lcd_flush_ready_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    /* The SPI DMA finished sending a chunk of pixels: LVGL may reuse that buffer */
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    /* The LCD expects big-endian RGB565, LVGL renders little-endian */
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static esp_err_t board_lcd_init(lv_display_t **out_disp)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = CONFIG_KNOB_PIN_LCD_SCLK,
        .mosi_io_num = CONFIG_KNOB_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "SPI bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_KNOB_PIN_LCD_DC,
        .cs_gpio_num = CONFIG_KNOB_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io), TAG, "LCD IO");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_KNOB_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &panel), TAG, "GC9A01");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "LCD init");
    /* This panel shows inverted colours unless told otherwise (black would look white) */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "LCD invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, CONFIG_KNOB_LCD_MIRROR_X, CONFIG_KNOB_LCD_MIRROR_Y),
                        TAG, "LCD mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "LCD on");

    lv_display_t *disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    size_t buf_sz = BOARD_LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "LVGL draw buffers");
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(disp, panel);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_flush_ready_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp), TAG, "LCD callbacks");

    *out_disp = disp;
    return ESP_OK;
}

/* ---------------------------------------------------------------------------------------------
 * Touch
 *
 * The CST816D goes to sleep when nobody is touching it and then stops answering on I2C, so we
 * only read it after its interrupt pin fires (plus one follow-up read to catch the finger lifting).
 * ------------------------------------------------------------------------------------------- */

static void IRAM_ATTR touch_isr_cb(esp_lcd_touch_handle_t tp)
{
    s_touch_irq = true;
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool pressed;
    static int64_t last_irq_us;
    static uint16_t last_x, last_y;

    int64_t now = esp_timer_get_time();
    bool irq = s_touch_irq;
    if (irq) {
        s_touch_irq = false;
        last_irq_us = now;
    }

    /* Read on a fresh interrupt, or once more ~60 ms after the last one while a finger was down */
    if (irq || (pressed && now - last_irq_us > 60 * 1000)) {
        esp_lcd_touch_point_data_t point;
        uint8_t cnt = 0;
        pressed = false;
        if (esp_lcd_touch_read_data(s_touch) == ESP_OK &&
                esp_lcd_touch_get_data(s_touch, &point, &cnt, 1) == ESP_OK && cnt > 0) {
            pressed = true;
            last_x = point.x;
            last_y = point.y;
        }
        if (!irq) {
            last_irq_us = now;  /* don't hammer the chip if it keeps reporting a press */
        }
    }

    data->point.x = last_x;
    data->point.y = last_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static esp_err_t board_touch_init(lv_display_t *disp)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_KNOB_PIN_TP_SDA,
        .scl_io_num = CONFIG_KNOB_PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "touch I2C bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch IO");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = CONFIG_KNOB_PIN_TP_RST,
        .int_gpio_num = CONFIG_KNOB_PIN_TP_INT,
        .flags = {
            .mirror_x = CONFIG_KNOB_TP_MIRROR_X,
            .mirror_y = CONFIG_KNOB_TP_MIRROR_Y,
        },
        .interrupt_callback = touch_isr_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(io, &tp_cfg, &s_touch), TAG, "CST816");

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------------------------
 * LVGL plumbing
 * ------------------------------------------------------------------------------------------- */

void board_lvgl_lock(void)
{
    _lock_acquire(&s_lvgl_lock);
}

void board_lvgl_unlock(void)
{
    _lock_release(&s_lvgl_lock);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    while (1) {
        board_lvgl_lock();
        uint32_t wait_ms = lv_timer_handler();
        board_lvgl_unlock();
        wait_ms = MAX(wait_ms, LVGL_TASK_MIN_DELAY_MS);
        wait_ms = MIN(wait_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * wait_ms);
    }
}

esp_err_t board_init(void)
{
    board_power_on();
    ESP_RETURN_ON_ERROR(board_backlight_init(), TAG, "backlight");
    if (board_leds_init() != ESP_OK) {
        ESP_LOGW(TAG, "LED ring not available, continuing without it");
    }

    lv_init();
    lv_display_t *disp = NULL;
    ESP_RETURN_ON_ERROR(board_lcd_init(&disp), TAG, "LCD");
    if (board_touch_init(disp) != ESP_OK) {
        ESP_LOGW(TAG, "touch not available, the knob still works");
    }

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "LVGL tick");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_PERIOD_MS * 1000), TAG, "LVGL tick");

    BaseType_t ok = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "LVGL task");
    return ESP_OK;
}
