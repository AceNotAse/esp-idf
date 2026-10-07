/*
 * ui.c - the round screen and what the knob does.
 *
 *   BROWSE mode (default)
 *     turn knob / swipe   -> pick the previous / next entity
 *     click / tap         -> toggle it (lights, switches, fans...), run it (scenes, scripts), lock it
 *     long press          -> ADJUST mode if it has a level, or unlock if it is a lock
 *
 *   ADJUST mode (ring turns blue)
 *     turn knob           -> change brightness / volume / position / target temperature
 *     click or long press -> back to BROWSE (also happens after 5 s without touching the knob)
 *
 * Everything in this file runs inside the LVGL task (from an lv_timer or an LVGL event), so it can
 * call LVGL freely without extra locking.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "board.h"
#include "encoder.h"
#include "ha_client.h"
#include "ui.h"

static const char *TAG = "ui";

#define UI_TICK_MS             30
#define ADJUST_SEND_DELAY_MS   350   /* send the new level once the knob stops for this long */
#define ADJUST_EXIT_MS         5000
#define ARC_RANGE              1000

#define COLOR_BG        lv_color_hex(0x000000)
#define COLOR_TRACK     lv_color_hex(0x262626)
#define COLOR_ON        lv_color_hex(0xFFB300)
#define COLOR_OFF       lv_color_hex(0x5A5A5A)
#define COLOR_ADJUST    lv_color_hex(0x00B0FF)
#define COLOR_ERROR     lv_color_hex(0xE53935)
#define COLOR_TEXT_DIM  lv_color_hex(0x9E9E9E)

typedef enum {
    MODE_BROWSE,
    MODE_ADJUST,
} ui_mode_t;

static struct {
    QueueHandle_t knob;
    int sel;
    ui_mode_t mode;
    float adj_value;
    bool adj_pending;
    int64_t adj_changed_ms;
    int64_t last_input_ms;
    bool screen_on;
    bool dirty;
    uint32_t seen_change;
    uint32_t led_rgb;

    lv_obj_t *arc;
    lv_obj_t *index;
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *hint;
    lv_obj_t *status;
} s_ui;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *domain_icon(const ha_entity_t *e)
{
    switch (e->domain) {
    case HA_DOMAIN_LIGHT:         return LV_SYMBOL_CHARGE;
    case HA_DOMAIN_SWITCH:        return LV_SYMBOL_POWER;
    case HA_DOMAIN_INPUT_BOOLEAN: return LV_SYMBOL_OK;
    case HA_DOMAIN_FAN:           return LV_SYMBOL_REFRESH;
    case HA_DOMAIN_COVER:         return LV_SYMBOL_BARS;
    case HA_DOMAIN_MEDIA_PLAYER:  return LV_SYMBOL_VOLUME_MAX;
    case HA_DOMAIN_CLIMATE:       return LV_SYMBOL_HOME;
    case HA_DOMAIN_LOCK:          return e->is_on ? LV_SYMBOL_OK : LV_SYMBOL_WARNING;
    case HA_DOMAIN_SCENE:         return LV_SYMBOL_IMAGE;
    case HA_DOMAIN_SCRIPT:        return LV_SYMBOL_PLAY;
    default:                      return LV_SYMBOL_SETTINGS;
    }
}

static void format_level(char *buf, size_t len, const ha_entity_t *e, float v)
{
    if (e->domain == HA_DOMAIN_CLIMATE) {
        snprintf(buf, len, "%.1f%s", v, e->unit);
    } else {
        snprintf(buf, len, "%d%s", (int)lroundf(v), e->unit);
    }
}

/* Capitalise HA's raw state for display: "playing" -> "Playing" */
static void format_state(char *buf, size_t len, const char *state)
{
    snprintf(buf, len, "%s", state);
    if (buf[0] >= 'a' && buf[0] <= 'z') {
        buf[0] -= 'a' - 'A';
    }
}

/* ---------------------------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------------------------- */

static void ui_set_leds(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t rgb = (r << 16) | (g << 8) | b;
    if (rgb != s_ui.led_rgb) {
        s_ui.led_rgb = rgb;
        board_leds_set(r, g, b);
    }
}

static void ui_redraw(void)
{
    static const char *const link_text[] = {
        [HA_LINK_WIFI_CONNECTING] = "Connecting to Wi-Fi...",
        [HA_LINK_HA_CONNECTING] = "Connecting to HA...",
        [HA_LINK_OK] = "",
        [HA_LINK_AUTH_ERROR] = "Bad HA token",
        [HA_LINK_ERROR] = "HA unreachable",
    };
    ha_link_t link = ha_link_state();
    lv_label_set_text(s_ui.status, link_text[link]);

    int count = ha_entity_count();
    ha_entity_t e;
    if (!ha_get_entity(s_ui.sel, &e)) {
        lv_label_set_text(s_ui.index, "");
        lv_label_set_text(s_ui.icon, LV_SYMBOL_WARNING);
        lv_label_set_text(s_ui.name, "No entities");
        lv_label_set_text(s_ui.value, "");
        lv_label_set_text(s_ui.hint, "Set them in menuconfig");
        lv_arc_set_value(s_ui.arc, 0);
        ui_set_leds(0, 0, 0);
        return;
    }

    lv_label_set_text_fmt(s_ui.index, "%d / %d", s_ui.sel + 1, count);
    lv_label_set_text(s_ui.icon, domain_icon(&e));
    lv_label_set_text(s_ui.name, e.name);

    char text[32];
    lv_color_t color;
    float frac;  /* how much of the ring to fill, 0..1 */
    const char *hint;

    if (s_ui.mode == MODE_ADJUST) {
        format_level(text, sizeof(text), &e, s_ui.adj_value);
        color = COLOR_ADJUST;
        frac = (s_ui.adj_value - e.level_min) / (e.level_max - e.level_min);
        hint = "Turn to adjust, press when done";
    } else if (!e.available) {
        format_state(text, sizeof(text), strcmp(e.state, "not found") == 0 ? "Not found" : "Offline");
        color = COLOR_ERROR;
        frac = 0;
        hint = "";
    } else if (e.domain == HA_DOMAIN_SCENE || e.domain == HA_DOMAIN_SCRIPT) {
        snprintf(text, sizeof(text), "Run");
        color = COLOR_ON;
        frac = 1;
        hint = "Press to run";
    } else if (e.has_level && e.is_on) {
        format_level(text, sizeof(text), &e, e.level);
        color = COLOR_ON;
        frac = (e.level - e.level_min) / (e.level_max - e.level_min);
        hint = "Press: on/off   Hold: adjust";
    } else {
        format_state(text, sizeof(text), e.state);
        color = e.is_on ? COLOR_ON : COLOR_OFF;
        frac = e.is_on ? 1 : 0;
        if (e.domain == HA_DOMAIN_LOCK) {
            hint = "Press: lock   Hold: unlock";
        } else if (e.has_level) {
            hint = "Press: on/off   Hold: adjust";
        } else {
            hint = "Press: on/off";
        }
    }

    lv_label_set_text(s_ui.value, text);
    lv_label_set_text(s_ui.hint, hint);
    lv_arc_set_value(s_ui.arc, (int32_t)(fminf(fmaxf(frac, 0), 1) * ARC_RANGE));
    lv_obj_set_style_arc_color(s_ui.arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_ui.icon, color, 0);

    /* The LED ring mirrors the screen: blue while adjusting, amber when on, dim red if offline */
    if (!s_ui.screen_on) {
        ui_set_leds(0, 0, 0);
    } else if (s_ui.mode == MODE_ADJUST) {
        ui_set_leds(0, 30, 60);
    } else if (!e.available) {
        ui_set_leds(20, 0, 0);
    } else if (e.is_on) {
        float k = (e.has_level && e.domain == HA_DOMAIN_LIGHT) ? fmaxf(frac, 0.1f) : 1.0f;
        ui_set_leds((uint8_t)(60 * k), (uint8_t)(30 * k), 0);
    } else {
        ui_set_leds(0, 0, 0);
    }
}

/* ---------------------------------------------------------------------------------------------
 * Input handling
 * ------------------------------------------------------------------------------------------- */

/* Note user activity. Returns true if the screen was asleep: that input only wakes it up. */
static bool ui_wake(void)
{
    s_ui.last_input_ms = now_ms();
    if (s_ui.screen_on) {
        return false;
    }
    s_ui.screen_on = true;
    s_ui.dirty = true;
    board_backlight_set(100);
    return true;
}

static void ui_flush_adjust(void)
{
    if (s_ui.adj_pending) {
        s_ui.adj_pending = false;
        ha_request_level(s_ui.sel, s_ui.adj_value);
    }
}

static void ui_exit_adjust(void)
{
    ui_flush_adjust();
    s_ui.mode = MODE_BROWSE;
    s_ui.dirty = true;
}

static void ui_on_rotate(int steps)
{
    int count = ha_entity_count();
    if (count == 0) {
        return;
    }
    if (s_ui.mode == MODE_BROWSE) {
        s_ui.sel = ((s_ui.sel + steps) % count + count) % count;
    } else {
        ha_entity_t e;
        ha_get_entity(s_ui.sel, &e);
        float v = s_ui.adj_value + steps * e.level_step;
        s_ui.adj_value = fminf(fmaxf(v, e.level_min), e.level_max);
        s_ui.adj_pending = true;
        s_ui.adj_changed_ms = now_ms();
    }
    s_ui.dirty = true;
}

static void ui_on_click(void)
{
    if (s_ui.mode == MODE_ADJUST) {
        ui_exit_adjust();
        return;
    }
    ESP_LOGI(TAG, "toggle entity %d", s_ui.sel);
    ha_request_toggle(s_ui.sel);
}

static void ui_on_long_press(void)
{
    if (s_ui.mode == MODE_ADJUST) {
        ui_exit_adjust();
        return;
    }
    ha_entity_t e;
    if (!ha_get_entity(s_ui.sel, &e) || !e.available) {
        return;
    }
    if (e.has_level) {
        s_ui.mode = MODE_ADJUST;
        s_ui.adj_value = e.is_on || e.domain == HA_DOMAIN_CLIMATE ? e.level : e.level_min;
        s_ui.adj_pending = false;
        s_ui.adj_changed_ms = now_ms();
        s_ui.dirty = true;
    } else if (e.domain == HA_DOMAIN_LOCK) {
        ESP_LOGI(TAG, "unlock entity %d", s_ui.sel);
        ha_request_alt_action(s_ui.sel);
    }
}

static void ui_screen_event_cb(lv_event_t *ev)
{
    lv_event_code_t code = lv_event_get_code(ev);
    if (code == LV_EVENT_SHORT_CLICKED) {
        if (!ui_wake()) {
            ui_on_click();
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (!ui_wake()) {
            ui_on_long_press();
        }
    } else if (code == LV_EVENT_GESTURE && s_ui.mode == MODE_BROWSE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (!ui_wake()) {
            if (dir == LV_DIR_LEFT) {
                ui_on_rotate(1);
            } else if (dir == LV_DIR_RIGHT) {
                ui_on_rotate(-1);
            }
        }
    }
}

static void ui_timer_cb(lv_timer_t *t)
{
    knob_evt_t evt;
    while (xQueueReceive(s_ui.knob, &evt, 0) == pdTRUE) {
        if (ui_wake()) {
            continue;
        }
        switch (evt.type) {
        case KNOB_EVT_ROTATE:
            ui_on_rotate(evt.steps);
            break;
        case KNOB_EVT_CLICK:
            ui_on_click();
            break;
        case KNOB_EVT_LONG_PRESS:
            ui_on_long_press();
            break;
        }
    }

    int64_t now = now_ms();
    if (s_ui.mode == MODE_ADJUST) {
        if (s_ui.adj_pending && now - s_ui.adj_changed_ms >= ADJUST_SEND_DELAY_MS) {
            ui_flush_adjust();
        }
        if (now - s_ui.last_input_ms >= ADJUST_EXIT_MS) {
            ui_exit_adjust();
        }
    }

#if CONFIG_KNOB_SCREEN_TIMEOUT_S > 0
    if (s_ui.screen_on && now - s_ui.last_input_ms >= CONFIG_KNOB_SCREEN_TIMEOUT_S * 1000LL) {
        s_ui.screen_on = false;
        board_backlight_set(0);
        s_ui.dirty = true;
    }
#endif

    uint32_t change = ha_change_counter();
    if (s_ui.dirty || change != s_ui.seen_change) {
        s_ui.dirty = false;
        s_ui.seen_change = change;
        ui_redraw();
    }
}

/* ---------------------------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------------------------- */

static lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

void ui_start(QueueHandle_t knob_events)
{
    s_ui.knob = knob_events;
    s_ui.screen_on = true;
    s_ui.last_input_ms = now_ms();
    s_ui.dirty = true;
    s_ui.led_rgb = UINT32_MAX;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* The ring around the edge: a 270 degree gauge with the gap at the bottom */
    s_ui.arc = lv_arc_create(scr);
    lv_obj_set_size(s_ui.arc, BOARD_LCD_H_RES - 8, BOARD_LCD_V_RES - 8);
    lv_obj_center(s_ui.arc);
    lv_arc_set_bg_angles(s_ui.arc, 135, 45);
    lv_arc_set_range(s_ui.arc, 0, ARC_RANGE);
    lv_arc_set_value(s_ui.arc, 0);
    lv_obj_remove_style(s_ui.arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ui.arc, LV_OBJ_FLAG_CLICKABLE);  /* touches go to the screen instead */
    lv_obj_set_style_arc_width(s_ui.arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ui.arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ui.arc, COLOR_TRACK, LV_PART_MAIN);

    s_ui.index = ui_label(scr, &lv_font_montserrat_14, COLOR_TEXT_DIM, -84);
    s_ui.icon = ui_label(scr, &lv_font_montserrat_28, COLOR_OFF, -54);
    s_ui.name = ui_label(scr, &lv_font_montserrat_20, lv_color_white(), -18);
    lv_obj_set_width(s_ui.name, 176);
    lv_label_set_long_mode(s_ui.name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    s_ui.value = ui_label(scr, &lv_font_montserrat_40, lv_color_white(), 26);
    s_ui.hint = ui_label(scr, &lv_font_montserrat_14, COLOR_TEXT_DIM, 64);
    lv_obj_set_width(s_ui.hint, 160);
    lv_label_set_long_mode(s_ui.hint, LV_LABEL_LONG_WRAP);
    s_ui.status = ui_label(scr, &lv_font_montserrat_14, COLOR_ERROR, 92);

    lv_obj_add_event_cb(scr, ui_screen_event_cb, LV_EVENT_ALL, NULL);
    lv_timer_create(ui_timer_cb, UI_TICK_MS, NULL);
}
