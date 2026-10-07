/*
 * ui.h - the round on-screen interface.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Build the screen and start reading knob events. Must be called with the LVGL lock held. */
void ui_start(QueueHandle_t knob_events);
